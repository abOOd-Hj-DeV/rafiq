#include "k230/inspector/kpu_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <utility>

#include "k230/log.hpp"

#ifdef __linux__
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace k230::inspector {
namespace {
void put(std::uint8_t* destination, std::uint64_t value, unsigned bytes = 4) {
  for (unsigned i = 0; i < bytes; ++i) destination[bytes - 1 - i] = value >> (i * 8);
}
std::uint64_t get(const std::uint8_t* source, unsigned bytes = 4) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < bytes; ++i) value = (value << 8) | source[i];
  return value;
}
}

struct KpuRuntime::Impl {
  explicit Impl(KpuRuntimeConfig value) : config(std::move(value)) {}
  KpuRuntimeConfig config;
#ifdef __linux__
  int fd = -1;
  pid_t pid = -1;
  using Clock = std::chrono::steady_clock;

  void close() {
    if (fd >= 0) { ::close(fd); fd = -1; }
    if (pid <= 0) return;
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    int status = 0;
    while (Clock::now() < deadline) {
      const auto result = waitpid(pid, &status, WNOHANG);
      if (result == pid || (result < 0 && errno == ECHILD)) { pid = -1; return; }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(pid, SIGKILL);
    const auto kill_deadline = Clock::now() + std::chrono::seconds(1);
    while (Clock::now() < kill_deadline) {
      const auto result = waitpid(pid, &status, WNOHANG);
      if (result == pid || (result < 0 && errno == ECHILD)) { pid = -1; return; }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    K230_LOG_ERROR("kpu") << "runtime process did not exit after SIGKILL";
    pid = -1;
  }
  ~Impl() { close(); }

  void transfer(void* buffer, std::size_t size, bool writing, Clock::time_point deadline) {
    auto* data = static_cast<std::uint8_t*>(buffer);
    while (size) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (remaining <= 0) throw std::runtime_error("KPU runtime timeout; no fallback");
      pollfd descriptor{fd, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
      const auto ready = poll(&descriptor, 1, static_cast<int>(remaining));
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0 || (descriptor.revents & POLLNVAL)) throw std::runtime_error("KPU runtime unavailable");
      const auto count = writing ? send(fd, data, size, MSG_NOSIGNAL) : recv(fd, data, size, 0);
      if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (count <= 0) throw std::runtime_error("KPU runtime socket closed");
      data += count; size -= count;
    }
  }

  void start() {
    close();
    if (config.model_path.empty() || config.timeout_ms < 1 || config.timeout_ms > 60000)
      throw std::invalid_argument("invalid KPU runtime configuration");
    if (config.input_size == 0 || config.input_size > 4096 || config.output_count == 0 ||
        config.output_count > 1024 * 1024)
      throw std::invalid_argument("invalid KPU tensor bounds");
    if (config.audio && (config.input_size != 96 || config.output_count != 521 ||
                         config.letterbox || config.probabilities))
      throw std::invalid_argument("invalid YAMNet KPU contract");
    if (config.worker_path.empty()) {
      std::array<char, 4096> executable{};
      const auto length = readlink("/proc/self/exe", executable.data(), executable.size());
      if (length <= 0 || std::size_t(length) == executable.size()) throw std::runtime_error("cannot locate KPU worker");
      config.worker_path = (std::filesystem::path(std::string(executable.data(), length)).parent_path() /
                            "kpu_worker.py").string();
    }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets) < 0)
      throw std::runtime_error("cannot create KPU runtime socket");
    // Keep the inherited endpoint above fd 3 so dup2 always clears CLOEXEC.
    const int child_fd = fcntl(sockets[1], F_DUPFD_CLOEXEC, 4);
    ::close(sockets[1]);
    if (child_fd < 0) { ::close(sockets[0]); throw std::runtime_error("cannot duplicate KPU socket"); }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) { ::close(sockets[0]); ::close(child_fd); throw std::runtime_error("cannot initialize KPU spawn"); }
    if (!error) error = posix_spawn_file_actions_addclose(&actions, sockets[0]);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, child_fd, 3);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, child_fd);
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    auto input_size = std::to_string(config.input_size);
    auto output_count = std::to_string(config.output_count);
    std::vector<char*> argv{config.python.data(), config.worker_path.data(),
                            const_cast<char*>("--model"), config.model_path.data(),
                            const_cast<char*>("--input-size"), input_size.data(),
                            const_cast<char*>("--outputs"), output_count.data()};
    if (config.letterbox) argv.push_back(const_cast<char*>("--letterbox"));
    if (config.audio) argv.push_back(const_cast<char*>("--audio"));
    argv.push_back(nullptr);
    if (!error) error = posix_spawnp(&pid, config.python.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(child_fd);
    if (error) { ::close(sockets[0]); pid = -1; throw std::runtime_error("cannot start KPU worker: " + std::string(std::strerror(error))); }
    fd = sockets[0];
    std::array<std::uint8_t, 16> hello{};
    transfer(hello.data(), hello.size(), false, Clock::now() + std::chrono::milliseconds(config.timeout_ms));
    if (std::memcmp(hello.data(), "RFR1", 4) || get(hello.data() + 4) != config.input_size ||
        get(hello.data() + 8) != (config.audio ? 64 : config.input_size) ||
        get(hello.data() + 12) != config.output_count)
      throw std::runtime_error("KPU runtime model shape mismatch");
  }

  KpuResult infer(const VideoFrame& frame, const ImageRegion& region) {
    if (fd < 0) throw std::runtime_error("KPU runtime is not open");
    if (config.audio) throw std::invalid_argument("audio KPU cannot accept RGB input");
    if (frame.format != PixelFormat::RGBPlanar || !frame.width || !frame.height ||
        frame.width > 4096 || frame.height > 4096 || frame.data.size() != frame.luma_size() * 3 ||
        frame.data.size() > 12 * 1024 * 1024 || frame.pts_us < 0 ||
        !region.width || !region.height || region.x >= frame.width || region.y >= frame.height ||
        region.width > frame.width - region.x || region.height > frame.height - region.y)
      throw std::invalid_argument("KPU runtime requires bounded RGB planar data and a valid crop");
    std::array<std::uint8_t, 40> header{};
    std::memcpy(header.data(), "RFI1", 4);
    const std::array<std::uint32_t, 6> fields{frame.width, frame.height, region.x, region.y, region.width, region.height};
    for (unsigned i = 0; i < fields.size(); ++i) put(header.data() + 4 + 4 * i, fields[i]);
    put(header.data() + 28, frame.pts_us, 8);
    put(header.data() + 36, frame.data.size());
    return exchange(header, frame.data.data(), frame.data.size(), frame.pts_us);
  }

  KpuResult infer_audio(const std::vector<std::int16_t>& samples, std::uint32_t rate,
                        std::uint16_t channels, std::int64_t end_pts_us) {
    if (fd < 0) throw std::runtime_error("KPU runtime is not open");
    if (!config.audio || (rate != 16000 && rate != 48000) || !channels || channels > 2 ||
        samples.size() % channels || samples.size() / channels < rate * 975 / 1000 ||
        samples.size() / channels > rate * 10 || end_pts_us < 975000)
      throw std::invalid_argument("YAMNet requires bounded 16/48kHz mono/stereo s16 PCM");
    const auto count = std::min(samples.size(), std::size_t(rate) * channels);
    const auto offset = samples.size() - count;
    std::vector<std::uint8_t> data(count * 2);
    for (std::size_t i = 0; i < count; ++i)
      put(data.data() + i * 2, static_cast<std::uint16_t>(samples[offset + i]), 2);
    std::array<std::uint8_t, 40> header{};
    std::memcpy(header.data(), "RFA1", 4);
    put(header.data() + 4, rate);
    put(header.data() + 8, channels);
    put(header.data() + 12, count / channels);
    put(header.data() + 28, end_pts_us, 8);
    put(header.data() + 36, data.size());
    return exchange(header, data.data(), data.size(), end_pts_us);
  }

  KpuResult exchange(std::array<std::uint8_t, 40>& header, const std::uint8_t* data,
                      std::size_t size, std::int64_t pts_us) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(config.timeout_ms);
    transfer(header.data(), header.size(), true, deadline);
    transfer(const_cast<std::uint8_t*>(data), size, true, deadline);
    std::array<std::uint8_t, 36> response{};
    transfer(response.data(), response.size(), false, deadline);
    const auto status = get(response.data() + 4), count = get(response.data() + 16);
    if (std::memcmp(response.data(), "RFO1", 4) || get(response.data() + 8, 8) != std::uint64_t(pts_us) ||
        status > 1 || (status ? count > 4096 : count != config.output_count) || count > 1024 * 1024)
      throw std::runtime_error("invalid KPU runtime response");
    if (status) {
      std::string error(count, '\0');
      transfer(error.data(), error.size(), false, deadline);
      throw std::runtime_error("KPU inference failed: " + error);
    }
    std::vector<std::uint8_t> values(std::size_t(count) * sizeof(float));
    transfer(values.data(), values.size(), false, deadline);
    KpuResult result;
    for (unsigned i = 0; i < count; ++i) {
      const auto bits = static_cast<std::uint32_t>(get(values.data() + i * 4));
      float value;
      std::memcpy(&value, &bits, sizeof(value));
      if (!std::isfinite(value) || ((config.probabilities || config.audio) && (value < 0 || value > 1)))
        throw std::runtime_error("invalid KPU output");
      result.values.push_back(value);
    }
    if (config.probabilities) {
      float sum = 0;
      for (auto value : result.values) sum += value;
      if (std::abs(sum - 1.0f) > 0.01f) throw std::runtime_error("KPU probabilities do not sum to one");
    }
    result.preparation_ms = get(response.data() + 20) / 1000.0;
    result.transform_ms = get(response.data() + 24) / 1000.0;
    result.inference_ms = get(response.data() + 28) / 1000.0;
    return result;
  }
#else
  void close() {}
  void start() { throw std::runtime_error("KPU Linux runtime unavailable"); }
  KpuResult infer(const VideoFrame&, const ImageRegion&) { throw std::runtime_error("KPU Linux runtime unavailable"); }
  KpuResult infer_audio(const std::vector<std::int16_t>&, std::uint32_t, std::uint16_t, std::int64_t) {
    throw std::runtime_error("KPU Linux runtime unavailable");
  }
#endif
};

KpuRuntime::KpuRuntime(KpuRuntimeConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
KpuRuntime::~KpuRuntime() = default;
bool KpuRuntime::open() {
  try { impl_->start(); return true; }
  catch (const std::exception& e) {
    impl_->close();
    K230_LOG_ERROR("kpu") << e.what() << "; hardware required, no fallback";
    return false;
  }
}
KpuResult KpuRuntime::infer(const VideoFrame& frame, const ImageRegion& region) {
  try { return impl_->infer(frame, region); }
  catch (...) { impl_->close(); throw; }
}
KpuResult KpuRuntime::infer_audio(const std::vector<std::int16_t>& samples,
                                 std::uint32_t rate, std::uint16_t channels,
                                 std::int64_t end_pts_us) {
  try { return impl_->infer_audio(samples, rate, channels, end_pts_us); }
  catch (...) { impl_->close(); throw; }
}
}  // namespace k230::inspector
