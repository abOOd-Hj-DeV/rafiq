#include "k230/inspector/decoder.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "k230/log.hpp"

namespace k230::inspector {
namespace {
constexpr auto kOutput = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto kCapture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
constexpr std::uint32_t kMvxCodecConfig = 0xc1000000;
constexpr std::uint32_t kMvxNeedReallocate = 0x20000000;
constexpr std::uint32_t kMvxRgbPlanar = v4l2_fourcc('R', 'G', 'B', 'M');

class V4l2VideoDecoder final : public VideoDecoder {
 public:
  explicit V4l2VideoDecoder(V4l2DecoderConfig config) : config_(std::move(config)) {}
  ~V4l2VideoDecoder() override { release(); }
  const char* name() const override { return "k230-vpu-v4l2"; }

  bool open(CodecId codec) override {
    release();
    codec_ = codec;
    if (codec != CodecId::H264 && codec != CodecId::H265) return fail("only H.264/HEVC are supported");
    if (config_.input_buffer_size < 4096 || config_.input_buffer_size > 16 * 1024 * 1024 ||
        config_.timeout_ms == 0 || config_.timeout_ms > 60000) return fail("invalid VPU configuration");
    try {
      fd_ = ::open(config_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
      if (fd_ < 0) error("open VPU");
      v4l2_capability caps{};
      checked(VIDIOC_QUERYCAP, &caps, "query VPU");
      const auto capabilities = caps.capabilities & V4L2_CAP_DEVICE_CAPS ? caps.device_caps : caps.capabilities;
      if (std::strncmp(reinterpret_cast<const char*>(caps.driver), "mvx", sizeof(caps.driver)) != 0 ||
          !(capabilities & V4L2_CAP_STREAMING) ||
          !(capabilities & V4L2_CAP_VIDEO_M2M_MPLANE))
        throw std::runtime_error("device is not the K230 mvx hardware decoder");
      v4l2_format format{};
      format.type = kOutput;
      format.fmt.pix_mp.pixelformat = codec == CodecId::H264 ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_HEVC;
      format.fmt.pix_mp.num_planes = 1;
      format.fmt.pix_mp.plane_fmt[0].sizeimage = config_.input_buffer_size;
      checked(VIDIOC_S_FMT, &format, "set compressed VPU format");
      if (format.fmt.pix_mp.pixelformat != (codec == CodecId::H264 ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_HEVC) ||
          format.fmt.pix_mp.num_planes != 1) throw std::runtime_error("VPU rejected compressed format");
      if (config_.output_format == PixelFormat::RGBPlanar) {
        capture_pixel_format_ = kMvxRgbPlanar;
      } else if (config_.output_format == PixelFormat::NV12) {
        capture_pixel_format_ = supported(kCapture, V4L2_PIX_FMT_NV12) ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_NV12M;
      } else {
        throw std::runtime_error("unsupported VPU output format");
      }
      if (!supported(kCapture, capture_pixel_format_)) throw std::runtime_error("VPU has no requested linear output");
      for (auto type : {V4L2_EVENT_SOURCE_CHANGE, V4L2_EVENT_EOS}) {
        v4l2_event_subscription subscription{};
        subscription.type = type;
        checked(VIDIOC_SUBSCRIBE_EVENT, &subscription, "subscribe VPU event");
      }
      allocate(kOutput, output_, 4);
      stream(kOutput, true);
      output_streaming_ = true;
      configure_capture(true);
      K230_LOG_INFO("vpu") << "hardware-only mvx " << to_string(codec) << " on " << config_.device;
      return true;
    } catch (const std::exception& e) { release(); return fail(e.what()); }
  }

  bool decode(const MediaPacket& packet, std::vector<VideoFrame>& frames) override {
    if (fd_ < 0 || packet.stream != StreamType::Video || packet.codec != codec_ || packet.data.empty())
      return fail("invalid compressed packet");
    try {
      service(frames);
      const auto index = available_output(frames);
      auto& buffer = output_[index];
      if (packet.data.size() > buffer.planes[0].size) throw std::runtime_error("access unit exceeds VPU input buffer");
      std::memcpy(buffer.planes[0].data, packet.data.data(), packet.data.size());
      queue_output(index, packet.data.size(), packet.pts_us, packet.is_config);
      if (!packet.is_config) {
        pending_frames_.push_back({packet.pts_us, packet.is_key_frame});
        ++submitted_frames_;
      }
      poll_device(2);
      service(frames);
      return true;
    } catch (const std::exception& e) { release(); return fail(e.what()); }
  }

  bool poll_frames(std::vector<VideoFrame>& frames) override {
    if (fd_ < 0) return fail("VPU is not open");
    try {
      service(frames);
      return true;
    } catch (const std::exception& e) { release(); return fail(e.what()); }
  }

  void flush(std::vector<VideoFrame>& frames) override {
    if (fd_ < 0 || !submitted_frames_) return;
    v4l2_decoder_cmd command{};
    command.cmd = V4L2_DEC_CMD_STOP;
    if (call(VIDIOC_DECODER_CMD, &command) < 0) {
      if (errno != EINVAL && errno != ENOTTY) error("stop VPU");
      queue_output(available_output(frames), 0, kNoPts);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.timeout_ms);
    while (!eos_) {
      service(frames);
      if (eos_) break;
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("VPU drain timed out; no software fallback");
      poll_device(10);
    }
    if (!pending_frames_.empty()) throw std::runtime_error("VPU dropped decoded frames; no software fallback");
  }

 private:
  struct Plane { void* data = MAP_FAILED; std::size_t size = 0; };
  struct Buffer { std::array<Plane, VIDEO_MAX_PLANES> planes{}; unsigned count = 0; bool queued = false; };
  struct PendingFrame { std::int64_t pts; bool key_frame; };

  int call(unsigned long request, void* argument) {
    int result;
    do { result = ioctl(fd_, request, argument); } while (result < 0 && errno == EINTR);
    return result;
  }
  [[noreturn]] void error(const char* operation) { throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno)); }
  void checked(unsigned long request, void* argument, const char* operation) {
    if (call(request, argument) < 0) error(operation);
  }
  bool fail(const char* message) {
    K230_LOG_ERROR("vpu") << message << "; hardware required, no software fallback";
    return false;
  }
  bool supported(v4l2_buf_type type, std::uint32_t pixel_format) {
    for (std::uint32_t index = 0;; ++index) {
      v4l2_fmtdesc description{};
      description.type = type; description.index = index;
      if (call(VIDIOC_ENUM_FMT, &description) < 0) {
        if (errno != EINVAL) error("enumerate VPU format");
        return false;
      }
      if (description.pixelformat == pixel_format) return true;
    }
  }
  void stream(v4l2_buf_type type, bool enabled) {
    checked(enabled ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type, "VPU streaming");
  }
  void allocate(v4l2_buf_type type, std::vector<Buffer>& buffers, unsigned count) {
    v4l2_requestbuffers request{};
    request.type = type; request.memory = V4L2_MEMORY_MMAP; request.count = count;
    checked(VIDIOC_REQBUFS, &request, "allocate VPU buffers");
    if (!request.count || request.count > 64) throw std::runtime_error("invalid VPU buffer count");
    buffers.resize(request.count);
    for (unsigned i = 0; i < request.count; ++i) {
      std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
      v4l2_buffer buffer{};
      buffer.type = type; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = i;
      buffer.length = planes.size(); buffer.m.planes = planes.data();
      checked(VIDIOC_QUERYBUF, &buffer, "query VPU buffer");
      if (!buffer.length || buffer.length > VIDEO_MAX_PLANES || (type == kOutput && buffer.length != 1))
        throw std::runtime_error("invalid VPU plane count");
      auto& target = buffers[i];
      target.count = buffer.length;
      for (unsigned p = 0; p < buffer.length; ++p) {
        if (!planes[p].length) throw std::runtime_error("empty VPU plane");
        target.planes[p].size = planes[p].length;
        target.planes[p].data = mmap(nullptr, planes[p].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, planes[p].m.mem_offset);
        if (target.planes[p].data == MAP_FAILED) error("map VPU plane");
      }
    }
  }
  void queue_capture(unsigned index) {
    auto& target = capture_[index];
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
    v4l2_buffer buffer{};
    buffer.type = kCapture; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
    buffer.length = target.count; buffer.m.planes = planes.data();
    for (unsigned p = 0; p < target.count; ++p) planes[p].length = target.planes[p].size;
    checked(VIDIOC_QBUF, &buffer, "queue VPU capture");
    target.queued = true;
  }
  void queue_output(unsigned index, std::size_t size, std::int64_t pts, bool config = false) {
    auto& target = output_[index];
    v4l2_plane plane{};
    plane.length = target.planes[0].size; plane.bytesused = size;
    v4l2_buffer buffer{};
    buffer.type = kOutput; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
    buffer.length = 1; buffer.m.planes = &plane;
    buffer.flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
    if (!size) buffer.flags |= V4L2_BUF_FLAG_LAST;
    else if (config) buffer.flags |= kMvxCodecConfig;
    // MVX uses KEYFRAME as end-of-access-unit on the bitstream input.
    else buffer.flags |= V4L2_BUF_FLAG_KEYFRAME;
    if (pts >= 0) { buffer.timestamp.tv_sec = pts / 1000000; buffer.timestamp.tv_usec = pts % 1000000; }
    checked(VIDIOC_QBUF, &buffer, "queue compressed VPU packet");
    target.queued = true;
  }
  void configure_capture(bool bootstrap = false) {
    const bool reallocate = capture_streaming_;
    if (capture_streaming_) {
      stream(kCapture, false); capture_streaming_ = false;
      unmap(capture_);
    }
    v4l2_format format{};
    format.type = kCapture;
    checked(VIDIOC_G_FMT, &format, "read decoded VPU format");
    if (reallocate) {
      v4l2_requestbuffers request{};
      request.type = kCapture; request.memory = V4L2_MEMORY_MMAP;
      checked(VIDIOC_REQBUFS, &request, "release VPU capture buffers");
    }
    if (bootstrap) {
      format.fmt.pix_mp.pixelformat = capture_pixel_format_;
      checked(VIDIOC_S_FMT, &format, "select linear VPU output");
    }
    format_ = format.fmt.pix_mp;
    if (format_.pixelformat != capture_pixel_format_ || (!bootstrap && (format_.width == 0 || format_.height == 0)) ||
        format_.width > 8192 || format_.height > 8192 ||
        (config_.output_format == PixelFormat::RGBPlanar ? format_.num_planes != 3 :
                                                         (format_.num_planes != 1 && format_.num_planes != 2)))
      throw std::runtime_error("invalid VPU capture layout");
    visible_ = {0, 0, format_.width, format_.height};
    v4l2_selection selection{};
    selection.type = kCapture; selection.target = V4L2_SEL_TGT_COMPOSE;
    if (call(VIDIOC_G_SELECTION, &selection) == 0) visible_ = selection.r;
    else if (errno != EINVAL && errno != ENOTTY) error("query visible VPU region");
    if (visible_.left < 0 || visible_.top < 0 || (visible_.left & 1) || (visible_.top & 1) ||
        (!bootstrap && (!visible_.width || !visible_.height)) || std::uint64_t(visible_.left) + visible_.width > format_.width ||
        std::uint64_t(visible_.top) + visible_.height > format_.height)
      throw std::runtime_error("invalid visible VPU region");
    unsigned count = bootstrap ? 1 : 8;
    v4l2_control minimum{};
    minimum.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    if (!bootstrap && call(VIDIOC_G_CTRL, &minimum) == 0 && minimum.value > 0) {
      if (minimum.value > 30) throw std::runtime_error("VPU requires too many capture buffers");
      count = std::max(count, unsigned(minimum.value) + 2);
    }
    allocate(kCapture, capture_, count);
    for (unsigned i = 0; i < capture_.size(); ++i) queue_capture(i);
    K230_LOG_INFO("vpu") << (config_.output_format == PixelFormat::RGBPlanar ? "RGB planar " : "NV12 ")
                         << visible_.width << "x" << visible_.height
                         << " stride=" << format_.plane_fmt[0].bytesperline
                         << " buffers=" << capture_.size();
    stream(kCapture, true); capture_streaming_ = true; changing_ = false;
  }
  void events() {
    for (;;) {
      v4l2_event event{};
      if (call(VIDIOC_DQEVENT, &event) < 0) {
        if (errno == EAGAIN || errno == ENOENT) break;
        error("dequeue VPU event");
      }
      if (event.type == V4L2_EVENT_EOS) eos_ = true;
      if (event.type == V4L2_EVENT_SOURCE_CHANGE && (event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION)) {
        if (capture_streaming_) changing_ = true;
        else configure_capture();
      }
    }
  }
  void service(std::vector<VideoFrame>& frames) {
    events();
    for (;;) {
      std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
      v4l2_buffer buffer{};
      buffer.type = kOutput; buffer.memory = V4L2_MEMORY_MMAP;
      buffer.length = planes.size(); buffer.m.planes = planes.data();
      if (call(VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN || errno == EPIPE) break;
        error("dequeue VPU input");
      }
      if (buffer.index >= output_.size() || (buffer.flags & V4L2_BUF_FLAG_ERROR))
        throw std::runtime_error("VPU rejected compressed input");
      output_[buffer.index].queued = false;
    }
    if (!capture_streaming_) return;
    for (;;) {
      std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
      v4l2_buffer buffer{};
      buffer.type = kCapture; buffer.memory = V4L2_MEMORY_MMAP;
      buffer.length = planes.size(); buffer.m.planes = planes.data();
      if (call(VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN || errno == EPIPE) break;
        error("dequeue decoded VPU frame");
      }
      if (buffer.index >= capture_.size() || buffer.length != capture_[buffer.index].count ||
          (buffer.flags & V4L2_BUF_FLAG_ERROR)) throw std::runtime_error("VPU reported a damaged frame");
      if (buffer.flags & kMvxNeedReallocate) {
        K230_LOG_INFO("vpu") << "capture reallocation flags=" << buffer.flags
                             << " used=" << planes[0].bytesused;
        configure_capture();
        break;
      }
      if (planes[0].bytesused) frames.push_back(copy_frame(buffer, planes));
      if (buffer.flags & V4L2_BUF_FLAG_LAST) {
        if (changing_) configure_capture();
        else eos_ = true;
        break;
      }
      queue_capture(buffer.index);
    }
  }
  VideoFrame copy_frame(const v4l2_buffer& buffer, const std::array<v4l2_plane, VIDEO_MAX_PLANES>& planes) {
    VideoFrame frame;
    frame.width = visible_.width; frame.height = visible_.height; frame.format = config_.output_format;
    const auto device_pts = std::int64_t(buffer.timestamp.tv_sec) * 1000000 + buffer.timestamp.tv_usec;
    auto pending = std::find_if(pending_frames_.begin(), pending_frames_.end(),
                                [&](const PendingFrame& item) { return item.pts == device_pts; });
    if (pending == pending_frames_.end()) {
      if (pending_frames_.empty()) throw std::runtime_error("VPU produced an unexpected frame");
      pending = pending_frames_.begin();
      if (!stale_pts_reported_) {
        K230_LOG_WARN("vpu") << "mvx returned a stale PTS; restoring access-unit PTS order";
        stale_pts_reported_ = true;
      }
    }
    frame.pts_us = pending->pts;
    frame.key_frame = pending->key_frame || (buffer.flags & V4L2_BUF_FLAG_KEYFRAME);
    pending_frames_.erase(pending);
    const auto uv_width = 2 * ((frame.width + 1) / 2), uv_height = (frame.height + 1) / 2;
    frame.data.resize(frame.format == PixelFormat::RGBPlanar ? frame.luma_size() * 3 :
                      frame.luma_size() + std::size_t(uv_width) * uv_height);
    const auto& mapped = capture_[buffer.index];
    const auto y_stride = format_.plane_fmt[0].bytesperline;
    const bool single = format_.num_planes == 1;
    const auto uv_stride = single ? y_stride : format_.plane_fmt[1].bytesperline;
    auto rows = [&](unsigned plane, std::size_t offset, std::size_t stride, unsigned count, unsigned width, std::uint8_t* output) {
      if (stride < width || planes[plane].bytesused > mapped.planes[plane].size ||
          planes[plane].data_offset > planes[plane].bytesused)
        throw std::runtime_error("invalid decoded plane bounds");
      offset += planes[plane].data_offset;
      if (offset > planes[plane].bytesused || std::size_t(count - 1) * stride + width > planes[plane].bytesused - offset)
        throw std::runtime_error("truncated decoded VPU plane");
      const auto* source = static_cast<const std::uint8_t*>(mapped.planes[plane].data) + offset;
      for (unsigned row = 0; row < count; ++row) std::memcpy(output + std::size_t(row) * width, source + std::size_t(row) * stride, width);
    };
    if (frame.format == PixelFormat::RGBPlanar) {
      for (unsigned plane = 0; plane < 3; ++plane) {
        const auto stride = format_.plane_fmt[plane].bytesperline;
        rows(plane, std::size_t(visible_.top) * stride + visible_.left, stride,
             frame.height, frame.width, frame.data.data() + plane * frame.luma_size());
      }
      return frame;
    }
    rows(0, std::size_t(visible_.top) * y_stride + visible_.left, y_stride, frame.height, frame.width, frame.data.data());
    rows(single ? 0 : 1, (single ? std::size_t(y_stride) * format_.height : 0) +
         std::size_t(visible_.top / 2) * uv_stride + visible_.left, uv_stride, uv_height, uv_width, frame.data.data() + frame.luma_size());
    return frame;
  }
  unsigned available_output(std::vector<VideoFrame>& frames) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.timeout_ms);
    for (;;) {
      for (unsigned i = 0; i < output_.size(); ++i) if (!output_[i].queued) return i;
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("VPU input timed out");
      poll_device(10); service(frames);
    }
  }
  void poll_device(int timeout) {
    // mvx polls both vb2 queues; CAPTURE is initialized only after SOURCE_CHANGE.
    if (!capture_streaming_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
      return;
    }
    pollfd descriptor{fd_, POLLIN | POLLPRI, 0};
    int result;
    do { result = poll(&descriptor, 1, timeout); } while (result < 0 && errno == EINTR);
    if (result < 0) error("poll VPU");
    if (descriptor.revents & (POLLHUP | POLLNVAL)) throw std::runtime_error("VPU disconnected");
  }
  static void unmap(std::vector<Buffer>& buffers) noexcept {
    for (auto& buffer : buffers) for (auto& plane : buffer.planes)
      if (plane.data != MAP_FAILED) munmap(plane.data, plane.size);
    buffers.clear();
  }
  void release() noexcept {
    if (fd_ >= 0) {
      if (capture_streaming_) { auto type = kCapture; ioctl(fd_, VIDIOC_STREAMOFF, &type); }
      if (output_streaming_) { auto type = kOutput; ioctl(fd_, VIDIOC_STREAMOFF, &type); }
    }
    unmap(capture_); unmap(output_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1; output_streaming_ = capture_streaming_ = changing_ = eos_ = stale_pts_reported_ = false;
    submitted_frames_ = 0; pending_frames_.clear();
  }
  V4l2DecoderConfig config_;
  CodecId codec_ = CodecId::Unknown;
  int fd_ = -1;
  bool output_streaming_ = false, capture_streaming_ = false, changing_ = false, eos_ = false;
  bool stale_pts_reported_ = false;
  std::uint32_t capture_pixel_format_ = 0;
  std::uint64_t submitted_frames_ = 0;
  v4l2_pix_format_mplane format_{};
  v4l2_rect visible_{};
  std::vector<Buffer> output_, capture_;
  std::deque<PendingFrame> pending_frames_;
};
}  // namespace

std::unique_ptr<VideoDecoder> make_v4l2_video_decoder(const V4l2DecoderConfig& config) {
  return std::make_unique<V4l2VideoDecoder>(config);
}
}  // namespace k230::inspector
