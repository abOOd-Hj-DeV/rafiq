#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace k230::inspector {

// Decoded video frame. Tightly packed planes, no padding:
//   I420: Y (w*h), U (w/2*h/2), V (w/2*h/2)
//   NV12: Y (w*h), interleaved UV (w*h/2)          <- native K230 VDEC output
//   RGBPlanar: R (w*h), G (w*h), B (w*h)
enum class PixelFormat : std::uint8_t { I420, NV12, RGBPlanar };
enum class YuvMatrix : std::uint8_t { BT601, BT709 };
enum class YuvRange : std::uint8_t { Limited, Full };

struct ImageRegion {
  std::uint32_t x = 0, y = 0, width = 0, height = 0;
};

struct VideoFrame {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::int64_t pts_us = -1;
  bool key_frame = false;
  PixelFormat format = PixelFormat::I420;
  std::vector<std::uint8_t> data;
  YuvMatrix yuv_matrix = YuvMatrix::BT601;
  YuvRange yuv_range = YuvRange::Limited;

  std::size_t luma_size() const { return std::size_t{width} * height; }
  const std::uint8_t* y() const { return data.data(); }
  // Chroma sample at (cx, cy) in the half-resolution grid.
  std::uint8_t cb(std::uint32_t cx, std::uint32_t cy) const {
    if (format == PixelFormat::RGBPlanar) throw std::invalid_argument("RGB has no chroma plane");
    const std::uint32_t cw = (width + 1) / 2;
    if (format == PixelFormat::I420) return data[luma_size() + cy * cw + cx];
    return data[luma_size() + (cy * cw + cx) * 2];
  }
  std::uint8_t cr(std::uint32_t cx, std::uint32_t cy) const {
    if (format == PixelFormat::RGBPlanar) throw std::invalid_argument("RGB has no chroma plane");
    const std::uint32_t cw = (width + 1) / 2;
    const std::uint32_t ch = (height + 1) / 2;
    if (format == PixelFormat::I420) return data[luma_size() + cw * ch + cy * cw + cx];
    return data[luma_size() + (cy * cw + cx) * 2 + 1];
  }
};

// Interleaved signed 16-bit PCM with a PTS for the first sample.
struct PcmChunk {
  std::int64_t pts_us = -1;
  std::uint32_t sample_rate = 48000;
  std::uint16_t channels = 2;
  std::vector<std::int16_t> samples;  // frames * channels
  // Source PTS stays untouched; raw PCM may additionally use its bounded sample clock.
  std::int64_t sample_clock_pts_us = -1;
  bool timing_discontinuity = false;

  std::int64_t timeline_pts_us() const { return sample_clock_pts_us < 0 ? pts_us : sample_clock_pts_us; }

  std::size_t frames() const { return channels ? samples.size() / channels : 0; }
  std::int64_t duration_us() const {
    return sample_rate ? static_cast<std::int64_t>(frames()) * 1'000'000 / sample_rate : 0;
  }
  std::int64_t end_pts_us() const { return timeline_pts_us() + duration_us(); }
};

}  // namespace k230::inspector
