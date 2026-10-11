#include "k230/inspector/nonai2d_converter.hpp"

#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace k230::inspector {
namespace {
constexpr auto kOutput = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto kCapture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
using Coefficients = std::array<std::int16_t, 12>;
constexpr Coefficients k601Full{256, 0, 359, -180, 256, -88, -183, 135, 256, 453, 0, -227};
constexpr Coefficients k601Limited{298, 0, 409, -223, 298, -100, -208, 135, 298, 516, 0, -277};
constexpr Coefficients k709Full{256, 0, 403, -202, 256, -48, -120, 84, 256, 475, 0, -238};
constexpr Coefficients k709Limited{298, 0, 459, -248, 298, -55, -136, 77, 298, 541, 0, -289};

class Nonai2dConverter final : public FrameConverter {
 public:
  Nonai2dConverter(std::string device, std::uint32_t timeout_ms)
      : device_(std::move(device)), timeout_ms_(timeout_ms) {
    if (!timeout_ms || timeout_ms > 60000) throw std::invalid_argument("invalid CSC timeout");
  }
  ~Nonai2dConverter() override { release(); }

  VideoFrame convert(const VideoFrame& input) override {
    if (input.format != PixelFormat::NV12 || !input.width || !input.height ||
        input.width > 1920 || input.height > 1080 || (input.width & 1) || (input.height & 1) ||
        input.data.size() != input.luma_size() * 3 / 2)
      throw std::runtime_error("nonai2d requires tightly packed, even-sized NV12");
    try {
      if (width_ != input.width || height_ != input.height ||
          matrix_ != input.yuv_matrix || range_ != input.yuv_range) initialize(input);
      copy_rows(input.data.data(), input.width, input.height, input_.planes[0], output_format_.plane_fmt[0].bytesperline);
      copy_rows(input.data.data() + input.luma_size(), input.width, input.height / 2,
                input_.planes[1], output_format_.plane_fmt[1].bytesperline);
      queue(kCapture, capture_, false);
      queue(kOutput, input_, true);
      dequeue(kCapture, capture_);
      dequeue(kOutput, input_);
      VideoFrame result;
      result.width = input.width; result.height = input.height;
      result.pts_us = input.pts_us; result.key_frame = input.key_frame;
      result.yuv_matrix = input.yuv_matrix; result.yuv_range = input.yuv_range;
      result.format = PixelFormat::RGBPlanar;
      result.data.resize(input.luma_size() * 3);
      for (unsigned p = 0; p < 3; ++p) {
        const auto stride = capture_format_.plane_fmt[p].bytesperline;
        for (unsigned row = 0; row < input.height; ++row)
          std::memcpy(result.data.data() + p * input.luma_size() + std::size_t(row) * input.width,
                      static_cast<const std::uint8_t*>(capture_.planes[p].data) + std::size_t(row) * stride,
                      input.width);
      }
      return result;
    } catch (...) {
      release();
      throw;
    }
  }

 private:
  struct Plane { void* data = MAP_FAILED; std::size_t size = 0; };
  struct Buffer { std::array<Plane, 3> planes; unsigned count = 0; unsigned index = 0; };
  int call(unsigned long command, void* argument) {
    int result;
    do { result = ioctl(fd_, command, argument); } while (result < 0 && errno == EINTR);
    return result;
  }
  [[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string("nonai2d hardware CSC: ") + operation + ": " + std::strerror(errno));
  }
  void checked(unsigned long command, void* argument, const char* operation) {
    if (call(command, argument) < 0) fail(operation);
  }
  v4l2_pix_format_mplane format(v4l2_buf_type type, std::uint32_t fourcc, unsigned planes) {
    v4l2_format fmt{};
    fmt.type = type;
    fmt.fmt.pix_mp.width = width_; fmt.fmt.pix_mp.height = height_;
    fmt.fmt.pix_mp.pixelformat = fourcc; fmt.fmt.pix_mp.num_planes = planes;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    checked(VIDIOC_S_FMT, &fmt, "set format");
    if (fmt.fmt.pix_mp.width != width_ || fmt.fmt.pix_mp.height != height_ ||
        fmt.fmt.pix_mp.pixelformat != fourcc || fmt.fmt.pix_mp.num_planes != planes)
      throw std::runtime_error("nonai2d rejected the exact CSC format");
    return fmt.fmt.pix_mp;
  }
  void allocate(v4l2_buf_type type, Buffer& target, const v4l2_pix_format_mplane& fmt) {
    v4l2_requestbuffers request{};
    request.type = type; request.memory = V4L2_MEMORY_MMAP; request.count = 1;
    checked(VIDIOC_REQBUFS, &request, "allocate buffers");
    if (!request.count) throw std::runtime_error("nonai2d allocated no buffers");
    std::array<v4l2_plane, 3> planes{};
    v4l2_buffer buffer{};
    buffer.type = type; buffer.memory = V4L2_MEMORY_MMAP;
    buffer.length = planes.size(); buffer.m.planes = planes.data();
    checked(VIDIOC_QUERYBUF, &buffer, "query buffer");
    if (buffer.length != fmt.num_planes) throw std::runtime_error("invalid nonai2d plane count");
    target.count = buffer.length;
    for (unsigned p = 0; p < target.count; ++p) {
      const unsigned rows = type == kOutput && p == 1 ? height_ / 2 : height_;
      if (fmt.plane_fmt[p].bytesperline < width_ ||
          planes[p].length < std::size_t(fmt.plane_fmt[p].bytesperline) * rows)
        throw std::runtime_error("invalid nonai2d plane size");
      target.planes[p].size = planes[p].length;
      target.planes[p].data = mmap(nullptr, planes[p].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, planes[p].m.mem_offset);
      if (target.planes[p].data == MAP_FAILED) fail("map buffer");
    }
  }
  void initialize(const VideoFrame& frame) {
    release();
    width_ = frame.width; height_ = frame.height;
    matrix_ = frame.yuv_matrix; range_ = frame.yuv_range;
    fd_ = ::open(device_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) fail("open converter");
    v4l2_capability capability{};
    checked(VIDIOC_QUERYCAP, &capability, "query converter");
    const auto capabilities = capability.capabilities & V4L2_CAP_DEVICE_CAPS ?
                                  capability.device_caps : capability.capabilities;
    if (std::strncmp(reinterpret_cast<const char*>(capability.driver), "nonai-2d", sizeof(capability.driver)) != 0 ||
        !(capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) || !(capabilities & V4L2_CAP_STREAMING))
      throw std::runtime_error("CSC device is not K230 nonai2d");
    output_format_ = format(kOutput, V4L2_PIX_FMT_NV12, 2);
    capture_format_ = format(kCapture, V4L2_PIX_FMT_RGB24, 3);
    v4l2_control mode{};
    mode.id = V4L2_CID_USER_BASE + 0x1200; mode.value = 0;
    checked(VIDIOC_S_CTRL, &mode, "select CSC");
    auto coefficients = matrix_ == YuvMatrix::BT709 ?
        (range_ == YuvRange::Full ? k709Full : k709Limited) :
        (range_ == YuvRange::Full ? k601Full : k601Limited);
    v4l2_ext_control coefficient_control{};
    coefficient_control.id = V4L2_CID_USER_BASE + 0x1202;
    coefficient_control.size = sizeof(coefficients);
    coefficient_control.ptr = coefficients.data();
    v4l2_ext_controls controls{};
    controls.count = 1; controls.controls = &coefficient_control;
    checked(VIDIOC_S_EXT_CTRLS, &controls, "set CSC coefficients");
    allocate(kOutput, input_, output_format_);
    allocate(kCapture, capture_, capture_format_);
    auto type = kCapture;
    checked(VIDIOC_STREAMON, &type, "start capture"); capture_streaming_ = true;
    type = kOutput;
    checked(VIDIOC_STREAMON, &type, "start output"); output_streaming_ = true;
  }
  static void copy_rows(const std::uint8_t* source, unsigned width, unsigned height,
                        const Plane& plane, unsigned stride) {
    for (unsigned row = 0; row < height; ++row)
      std::memcpy(static_cast<std::uint8_t*>(plane.data) + std::size_t(row) * stride,
                  source + std::size_t(row) * width, width);
  }
  void queue(v4l2_buf_type type, const Buffer& target, bool input) {
    std::array<v4l2_plane, 3> planes{};
    for (unsigned p = 0; p < target.count; ++p) {
      planes[p].length = target.planes[p].size;
      planes[p].bytesused = input ? output_format_.plane_fmt[p].sizeimage : 0;
    }
    v4l2_buffer buffer{};
    buffer.type = type; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = target.index;
    buffer.length = target.count; buffer.m.planes = planes.data();
    checked(VIDIOC_QBUF, &buffer, "queue frame");
  }
  void dequeue(v4l2_buf_type type, Buffer& target) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
    for (;;) {
      std::array<v4l2_plane, 3> planes{};
      v4l2_buffer buffer{};
      buffer.type = type; buffer.memory = V4L2_MEMORY_MMAP;
      buffer.length = target.count; buffer.m.planes = planes.data();
      if (call(VIDIOC_DQBUF, &buffer) == 0) {
        if (buffer.index != target.index || buffer.flags & V4L2_BUF_FLAG_ERROR)
          throw std::runtime_error("nonai2d produced an invalid frame");
        return;
      }
      if (errno != EAGAIN) fail("receive frame");
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("nonai2d hardware CSC timed out");
      pollfd descriptor{fd_, static_cast<short>(type == kCapture ? POLLIN : POLLOUT), 0};
      int result;
      do { result = poll(&descriptor, 1, 10); } while (result < 0 && errno == EINTR);
      if (result < 0) fail("poll converter");
      if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
        throw std::runtime_error("nonai2d hardware converter failed");
    }
  }
  static void unmap(Buffer& buffer) noexcept {
    for (auto& plane : buffer.planes) {
      if (plane.data != MAP_FAILED) munmap(plane.data, plane.size);
      plane = {};
    }
    buffer.count = 0;
  }
  void release() noexcept {
    if (fd_ >= 0) {
      if (output_streaming_) { auto type = kOutput; ioctl(fd_, VIDIOC_STREAMOFF, &type); }
      if (capture_streaming_) { auto type = kCapture; ioctl(fd_, VIDIOC_STREAMOFF, &type); }
    }
    unmap(input_); unmap(capture_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1; width_ = height_ = 0; output_streaming_ = capture_streaming_ = false;
  }
  std::string device_;
  std::uint32_t timeout_ms_;
  int fd_ = -1;
  unsigned width_ = 0, height_ = 0;
  bool output_streaming_ = false, capture_streaming_ = false;
  YuvMatrix matrix_ = YuvMatrix::BT601;
  YuvRange range_ = YuvRange::Limited;
  v4l2_pix_format_mplane output_format_{}, capture_format_{};
  Buffer input_, capture_;
};
}  // namespace

std::unique_ptr<FrameConverter> make_nonai2d_converter(const std::string& device, std::uint32_t timeout_ms) {
  return std::make_unique<Nonai2dConverter>(device, timeout_ms);
}

void verify_nonai2d_converter(FrameConverter& converter) {
  struct ColorCase { YuvMatrix matrix; YuvRange range; std::array<int, 3> yuv, rgb; };
  const std::array<ColorCase, 12> cases{{
      {YuvMatrix::BT601, YuvRange::Limited, {16,128,128}, {0,0,0}},
      {YuvMatrix::BT601, YuvRange::Limited, {235,128,128}, {255,255,255}},
      {YuvMatrix::BT601, YuvRange::Limited, {81,90,240}, {254,0,0}},
      {YuvMatrix::BT709, YuvRange::Limited, {16,128,128}, {0,0,0}},
      {YuvMatrix::BT709, YuvRange::Limited, {235,128,128}, {255,255,255}},
      {YuvMatrix::BT709, YuvRange::Limited, {63,102,240}, {255,0,0}},
      {YuvMatrix::BT601, YuvRange::Full, {0,128,128}, {0,0,0}},
      {YuvMatrix::BT601, YuvRange::Full, {255,128,128}, {255,255,255}},
      {YuvMatrix::BT601, YuvRange::Full, {81,90,240}, {238,14,13}},
      {YuvMatrix::BT709, YuvRange::Full, {0,128,128}, {0,0,0}},
      {YuvMatrix::BT709, YuvRange::Full, {255,128,128}, {255,255,255}},
      {YuvMatrix::BT709, YuvRange::Full, {54,99,255}, {255,0,0}},
  }};
  for (const auto& test : cases) {
    VideoFrame input;
    input.width = 32; input.height = 32; input.pts_us = 123456;
    input.format = PixelFormat::NV12; input.yuv_matrix = test.matrix; input.yuv_range = test.range;
    input.data.resize(input.luma_size() * 3 / 2);
    std::fill_n(input.data.begin(), input.luma_size(), test.yuv[0]);
    for (std::size_t p = input.luma_size(); p < input.data.size(); p += 2) {
      input.data[p] = test.yuv[1]; input.data[p + 1] = test.yuv[2];
    }
    const auto output = converter.convert(input);
    if (output.format != PixelFormat::RGBPlanar || output.data.size() != input.luma_size() * 3 ||
        output.pts_us != input.pts_us) throw std::runtime_error("nonai2d CSC self-test layout failed");
    for (unsigned channel = 0; channel < 3; ++channel)
      for (std::size_t p = 0; p < input.luma_size(); ++p)
        if (std::abs(int(output.data[channel * input.luma_size() + p]) - test.rgb[channel]) > 2)
          throw std::runtime_error("nonai2d CSC color self-test failed; correct range/matrix support is required");
  }
}
}  // namespace k230::inspector
