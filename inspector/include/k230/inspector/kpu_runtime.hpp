#pragma once

#include <memory>
#include <string>
#include <vector>

#include "k230/inspector/frame.hpp"

namespace k230::inspector {

struct KpuRuntimeConfig {
  std::string model_path;
  std::string worker_path;
  std::string python = "python3";
  std::uint32_t timeout_ms = 30000;
  std::uint32_t input_size = 224;
  std::uint32_t output_count = 5;
  bool probabilities = true;
  bool letterbox = false;
  bool audio = false;
};

struct KpuResult {
  std::vector<float> values;
  double preparation_ms = 0, transform_ms = 0, inference_ms = 0;
};

// The official nncase 2.11 Linux binding owns AI2D/KPU tensors in a persistent
// process. Only RGB/PCM bytes and model outputs cross this bounded local socket.
class KpuRuntime {
 public:
  explicit KpuRuntime(KpuRuntimeConfig config);
  ~KpuRuntime();
  KpuRuntime(const KpuRuntime&) = delete;
  KpuRuntime& operator=(const KpuRuntime&) = delete;
  bool open();
  KpuResult infer(const VideoFrame& rgb, const ImageRegion& region);
  KpuResult infer_audio(const std::vector<std::int16_t>& samples,
                        std::uint32_t sample_rate, std::uint16_t channels,
                        std::int64_t end_pts_us);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace k230::inspector
