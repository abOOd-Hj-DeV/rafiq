#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "k230/inspector/frame.hpp"

namespace k230::inspector {

class FrameConverter {
 public:
  virtual ~FrameConverter() = default;
  virtual VideoFrame convert(const VideoFrame& input) = 0;
};

std::unique_ptr<FrameConverter> make_nonai2d_converter(
    const std::string& device = "/dev/video_nonai_2d", std::uint32_t timeout_ms = 2000);
void verify_nonai2d_converter(FrameConverter& converter);

}  // namespace k230::inspector
