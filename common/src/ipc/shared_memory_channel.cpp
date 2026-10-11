#include "k230/ipc/shared_memory_channel.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "k230/byteorder.hpp"
#include "k230/ipc/wire.hpp"

namespace k230::ipc {
namespace {
constexpr std::uint32_t kMagic = 0x52465131;
constexpr std::size_t kHeaderSize = 64;
enum Word { Magic, Version, Slots, SlotSize, Epoch, Head, Tail, WriterClosed, ReaderState };

void barrier() {
#if defined(__riscv)
  asm volatile("fence iorw,iorw" ::: "memory");
#else
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

bool reserved(std::uint64_t address, std::uint64_t size) {
  const std::filesystem::path root("/sys/firmware/devicetree/base/reserved-memory");
  if (!std::filesystem::exists(root)) return false;
  for (const auto& node : std::filesystem::directory_iterator(root)) {
    if (!std::filesystem::exists(node.path() / "no-map")) continue;
    std::ifstream reg(node.path() / "reg", std::ios::binary);
    std::array<std::uint8_t, 16> cells{};
    while (reg.read(reinterpret_cast<char*>(cells.data()), cells.size())) {
      const auto base = read64be(cells.data()), length = read64be(cells.data() + 8);
      if (address >= base && size <= length && address - base <= length - size) return true;
    }
  }
  return false;
}

class Ring {
 public:
  Ring(SharedMemoryConfig config, bool writer) : config_(std::move(config)), writer_(writer) {
    if (config_.slots < 2 || config_.slots > 1024 || config_.slot_size < 32 ||
        config_.slot_size > 1024 * 1024 || config_.address % 4096 != 0)
      throw std::runtime_error("invalid shared-memory ring layout");
    size_ = (kHeaderSize + std::size_t{config_.slots} * config_.slot_size + 4095) & ~std::size_t{4095};
    if (config_.address > INT64_MAX - size_) throw std::runtime_error("shared-memory address overflow");
    if (config_.device == "/dev/mem" && !reserved(config_.address, size_))
      throw std::runtime_error("shared-memory ring is not inside DT no-map reserved memory");
    const int fd = ::open(config_.device.c_str(), O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open shared-memory device");
    struct stat st{};
    if (fstat(fd, &st) != 0 || (config_.device != "/dev/mem" &&
        (!S_ISREG(st.st_mode) || std::uint64_t(st.st_size) < config_.address + size_))) {
      ::close(fd);
      throw std::runtime_error("invalid shared-memory backing file");
    }
    void* mapping = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, config_.address);
    ::close(fd);
    if (mapping == MAP_FAILED) throw std::runtime_error("cannot map shared-memory ring");
    memory_ = static_cast<std::uint8_t*>(mapping);
    if (writer_) {
      if (load(Magic) == kMagic && load(ReaderState) == 1) {
        munmap(memory_, size_);
        throw std::runtime_error("shared-memory reader still attached; stop both endpoints before restart");
      }
      if (load(Magic) == kMagic && !load(WriterClosed) && load(ReaderState) != 2) {
        munmap(memory_, size_);
        throw std::runtime_error("shared-memory writer already active");
      }
      epoch_ = load(Magic) == kMagic ? load(Epoch) + 1 : 1;
      if (!epoch_) epoch_ = 1;
      store(Magic, 0);
      store(Version, 1); store(Slots, config_.slots); store(SlotSize, config_.slot_size);
      store(Epoch, epoch_); store(Head, 0); store(Tail, 0);
      store(WriterClosed, 0); store(ReaderState, 0);
      store(Magic, kMagic);
    }
  }
  ~Ring() { close(); munmap(memory_, size_); }

  bool push(const std::uint8_t* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || !same_epoch() || load(WriterClosed) || load(ReaderState) == 2 ||
        size > config_.slot_size - 4) return false;
    const auto head = load(Head), tail = load(Tail);
    if (head - tail > config_.slots) throw std::runtime_error("corrupt shared-memory cursors");
    if (head - tail == config_.slots) return false;
    auto* slot = memory_ + kHeaderSize + std::size_t{head % config_.slots} * config_.slot_size;
    write32be(slot, static_cast<std::uint32_t>(size));
    std::memcpy(slot + 4, data, size);
    store(Head, head + 1);
    return true;
  }

  std::size_t capacity() const { return config_.slot_size - 4; }

  std::optional<std::vector<std::uint8_t>> pop(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return std::nullopt;
        if (attach()) {
          if (!same_epoch()) { closed_ = true; return std::nullopt; }
          const auto head = load(Head), tail = load(Tail);
          if (head - tail > config_.slots) throw std::runtime_error("corrupt shared-memory cursors");
          if (head != tail) {
            const auto* slot = memory_ + kHeaderSize + std::size_t{tail % config_.slots} * config_.slot_size;
            const auto length = read32be(slot);
            if (length > config_.slot_size - 4) throw std::runtime_error("corrupt shared-memory item size");
            std::vector<std::uint8_t> value(slot + 4, slot + 4 + length);
            store(Tail, tail + 1);
            return value;
          }
          if (load(WriterClosed)) return std::nullopt;
        }
      }
      if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_ || (epoch_ && (!same_epoch() || (load(WriterClosed) && load(Head) == load(Tail))));
  }
  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    if (epoch_ && same_epoch()) store(writer_ ? WriterClosed : ReaderState, writer_ ? 1 : 2);
    closed_ = true;
  }
 private:
  std::uint32_t load(Word word) const {
    const auto value = *reinterpret_cast<volatile std::uint32_t*>(memory_ + 4 * word);
    barrier();
    return value;
  }
  void store(Word word, std::uint32_t value) {
    barrier();
    *reinterpret_cast<volatile std::uint32_t*>(memory_ + 4 * word) = value;
    barrier();
  }
  bool same_epoch() const { return load(Magic) == kMagic && load(Epoch) == epoch_; }
  bool attach() {
    if (epoch_) return true;
    if (load(Magic) != kMagic) return false;
    if (load(Version) != 1 || load(Slots) != config_.slots || load(SlotSize) != config_.slot_size)
      throw std::runtime_error("shared-memory endpoints disagree on layout");
    const auto reader_state = load(ReaderState);
    if (reader_state == 2 || (load(WriterClosed) && load(Head) == load(Tail))) return false;
    if (reader_state != 0) throw std::runtime_error("shared-memory reader already attached");
    epoch_ = load(Epoch);
    store(ReaderState, 1);
    return true;
  }
  SharedMemoryConfig config_;
  bool writer_, closed_ = false;
  std::uint32_t epoch_ = 0;
  std::size_t size_ = 0;
  std::uint8_t* memory_ = nullptr;
  mutable std::mutex mutex_;
};
}  // namespace

struct SharedPacketSink::Impl {
  Impl(const SharedMemoryConfig& c, std::chrono::milliseconds timeout) : ring(c, true), push_timeout(timeout) {}
  Ring ring;
  std::chrono::milliseconds push_timeout;
};
struct SharedPacketSource::Impl { explicit Impl(const SharedMemoryConfig& c) : ring(c, false) {} Ring ring; };
struct SharedVerdictSink::Impl { explicit Impl(const SharedMemoryConfig& c) : ring(c, true) {} Ring ring; };
struct SharedVerdictSource::Impl { explicit Impl(const SharedMemoryConfig& c) : ring(c, false) {} Ring ring; };
struct SharedControlSink::Impl { explicit Impl(const SharedMemoryConfig& c) : ring(c, true) {} Ring ring; };
struct SharedControlSource::Impl { explicit Impl(const SharedMemoryConfig& c) : ring(c, false) {} Ring ring; };

SharedControlSink::SharedControlSink(const SharedMemoryConfig& c) : impl_(std::make_unique<Impl>(c)) {}
SharedControlSink::~SharedControlSink() = default;
bool SharedControlSink::push(companion::Json&& message) {
  try {
    const auto bytes = message.is_null() ? std::vector<std::uint8_t>{'D', 2, 0, 0, 0, 0, 0, 0}
                                         : companion::control_record(message);
    if (bytes.size() > impl_->ring.capacity()) return false;
    return impl_->ring.push(bytes.data(), bytes.size());
  } catch (const std::exception&) {
    return false;
  }
}
void SharedControlSink::close() { impl_->ring.close(); }
SharedControlSource::SharedControlSource(const SharedMemoryConfig& c) : impl_(std::make_unique<Impl>(c)) {}
SharedControlSource::~SharedControlSource() = default;
std::optional<companion::Json> SharedControlSource::pop(std::chrono::milliseconds timeout) {
  const auto bytes = impl_->ring.pop(timeout);
  if (!bytes) return std::nullopt;
  if (*bytes == std::vector<std::uint8_t>{'D', 2, 0, 0, 0, 0, 0, 0}) return companion::Json();
  auto message = companion::decode_control(*bytes);
  if (!message) throw std::runtime_error("invalid shared control record");
  return message;
}
bool SharedControlSource::closed() const { return impl_->ring.closed(); }

SharedPacketSink::SharedPacketSink(const SharedMemoryConfig& c, std::chrono::milliseconds timeout)
    : impl_(std::make_unique<Impl>(c, timeout)) {}
SharedPacketSink::~SharedPacketSink() = default;
bool SharedPacketSink::push(MediaPacket&& p) {
  if (p.data.size() > impl_->ring.capacity() - kPacketHeaderSize) return false;
  const auto bytes = encode(p);
  const auto deadline = std::chrono::steady_clock::now() + impl_->push_timeout;
  while (!impl_->ring.push(bytes.data(), bytes.size())) {
    if (impl_->ring.closed() || std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}
void SharedPacketSink::close() { impl_->ring.close(); }
SharedPacketSource::SharedPacketSource(const SharedMemoryConfig& c) : impl_(std::make_unique<Impl>(c)) {}
SharedPacketSource::~SharedPacketSource() = default;
std::optional<MediaPacket> SharedPacketSource::pop(std::chrono::milliseconds timeout) {
  const auto bytes = impl_->ring.pop(timeout);
  if (!bytes) return std::nullopt;
  MediaPacket packet;
  if (bytes->size() < kPacketHeaderSize) throw std::runtime_error("short shared media header");
  const auto length = decode_header(bytes->data(), packet);
  if (!length || *length != bytes->size() - kPacketHeaderSize) throw std::runtime_error("invalid shared media packet");
  packet.data.assign(bytes->begin() + kPacketHeaderSize, bytes->end());
  return packet;
}
bool SharedPacketSource::closed() const { return impl_->ring.closed(); }
SharedVerdictSink::SharedVerdictSink(const SharedMemoryConfig& c) : impl_(std::make_unique<Impl>(c)) {}
SharedVerdictSink::~SharedVerdictSink() = default;
bool SharedVerdictSink::push(Verdict&& v) {
  if (!(v.confidence >= 0 && v.confidence <= 1)) return false;
  std::array<std::uint8_t, kVerdictSize> bytes{};
  encode(v, bytes.data());
  return impl_->ring.push(bytes.data(), bytes.size());
}
void SharedVerdictSink::close() { impl_->ring.close(); }
SharedVerdictSource::SharedVerdictSource(const SharedMemoryConfig& c) : impl_(std::make_unique<Impl>(c)) {}
SharedVerdictSource::~SharedVerdictSource() = default;
std::optional<Verdict> SharedVerdictSource::pop(std::chrono::milliseconds timeout) {
  const auto bytes = impl_->ring.pop(timeout);
  if (!bytes) return std::nullopt;
  if (bytes->size() != kVerdictSize) throw std::runtime_error("invalid shared verdict length");
  auto verdict = decode_verdict(bytes->data());
  if (!verdict || !(verdict->confidence >= 0 && verdict->confidence <= 1))
    throw std::runtime_error("invalid shared verdict");
  return verdict;
}
bool SharedVerdictSource::closed() const { return impl_->ring.closed(); }
}  // namespace k230::ipc
