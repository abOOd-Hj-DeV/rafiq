#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

#include "k230/inspector/frame.hpp"

namespace k230::inspector {

inline std::optional<double> demo_red_fraction(const VideoFrame& frame) {
  if (!frame.width || !frame.height || frame.width > 4096 || frame.height > 4096) return std::nullopt;
  if (frame.format != PixelFormat::RGBPlanar && frame.format != PixelFormat::NV12 &&
      frame.format != PixelFormat::I420) return std::nullopt;
  const auto pixels = frame.luma_size();
  const auto chroma = std::size_t{(frame.width + 1) / 2} * ((frame.height + 1) / 2);
  const auto required = frame.format == PixelFormat::RGBPlanar ? pixels * 3 : pixels + chroma * 2;
  if (frame.data.size() < required) return std::nullopt;
  std::size_t red = 0, total = 0;
  for (std::uint32_t y = 0; y < frame.height; y += 4) {
    for (std::uint32_t x = 0; x < frame.width; x += 4) {
      const auto index = std::size_t{y} * frame.width + x;
      double r, g, b;
      if (frame.format == PixelFormat::RGBPlanar) {
        r = frame.data[index];
        g = frame.data[pixels + index];
        b = frame.data[pixels * 2 + index];
      } else {
        const bool limited = frame.yuv_range == YuvRange::Limited;
        const double luma = (frame.data[index] - (limited ? 16.0 : 0.0)) * (limited ? 255.0 / 219.0 : 1.0);
        const double u = (frame.cb(x / 2, y / 2) - 128.0) * (limited ? 255.0 / 224.0 : 1.0);
        const double v = (frame.cr(x / 2, y / 2) - 128.0) * (limited ? 255.0 / 224.0 : 1.0);
        const bool bt709 = frame.yuv_matrix == YuvMatrix::BT709;
        r = luma + (bt709 ? 1.5748 : 1.402) * v;
        g = luma - (bt709 ? 0.187324 : 0.344136) * u - (bt709 ? 0.468124 : 0.714136) * v;
        b = luma + (bt709 ? 1.8556 : 1.772) * u;
      }
      ++total;
      if (r >= 200 && g <= 60 && b <= 60) ++red;
    }
  }
  return static_cast<double>(red) / total;
}

class RedDemoTrigger {
 public:
  enum class Event { None, Ready, Trigger };
  Event observe(std::int64_t pts_us, std::optional<double> fraction) {
    if (!fraction || !std::isfinite(*fraction) || *fraction < 0 || *fraction > 1 || pts_us < 0) {
      armed_ = false;
      return Event::None;
    }
    if (pts_us <= previous_pts_) return Event::None;
    previous_pts_ = pts_us;
    if (!armed_ && *fraction <= 0.20) {
      armed_ = true;
      return Event::Ready;
    }
    if (armed_ && *fraction >= 0.80) {
      armed_ = false;
      return Event::Trigger;
    }
    return Event::None;
  }
 private:
  bool armed_ = false;
  std::int64_t previous_pts_ = -1;
};

}  // namespace k230::inspector
