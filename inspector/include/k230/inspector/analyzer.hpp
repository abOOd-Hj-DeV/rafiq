#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "k230/inspector/sync_engine.hpp"
#include "k230/inspector/age_policy.hpp"

namespace k230::inspector {

struct NsfwjsScores {
  float drawing = 0.0f;
  float hentai = 0.0f;
  float neutral = 0.0f;
  float porn = 0.0f;
  float sexy = 0.0f;
};

struct AudioEvent {
  std::uint32_t class_index = 0;
  float probability = 0;
  std::int64_t end_pts_us = 0;
  double inference_ms = 0;
};

// Per-sample scores in [0, 1]. Produced by the model, consumed by Policy.
struct Scores {
  float nudity = 0.0f;
  float violence = 0.0f;
  float profanity = 0.0f;  // from the audio window
  float audio_level = 0.0f;  // RMS of the window, diagnostic only
  std::optional<NsfwjsScores> nsfwjs;
  std::optional<AudioEvent> audio_event;
  std::uint32_t analysis_region = 0;
  std::uint32_t analysis_regions = 1;
  std::uint64_t analysis_layout = 0;
  double analysis_ms = 0.0;
  bool analysis_complete = true;
  std::uint64_t layout_sequence = 0;
  std::uint64_t layout_session = 0;
  std::uint32_t source_region = 0;
  double frame_ms = 0.0;
  double preparation_ms = 0.0;
  double inference_ms = 0.0;
  double transform_ms = 0.0;
  std::optional<ImageRegion> crop;
  std::optional<AnalysisBatch> protection;
};

std::string describe_scores(const Scores& scores);

class Analyzer {
 public:
  virtual ~Analyzer() = default;
  virtual bool open() = 0;
  virtual Scores analyze(const SyncedSample& sample) = 0;
  virtual const char* name() const = 0;
};

// Development stand-in: skin-tone ratio in YCbCr for `nudity`, RMS for
// `audio_level`. Deterministic and dependency-free, so the whole pipeline can
// be exercised and unit-tested before a real model exists. NOT a classifier.
class HeuristicAnalyzer final : public Analyzer {
 public:
  bool open() override { return true; }
  Scores analyze(const SyncedSample& sample) override;
  const char* name() const override { return "heuristic"; }

  static float skin_ratio(const VideoFrame& frame);
  static float rms(const std::vector<std::int16_t>& samples);
};

// K230 KPU backend: runs a .kmodel produced by nncase through the nncase
// official nncase Linux runtime with hardware-only CSC/crop/resize.
struct KpuConfig {
  std::string kmodel_path = "/sdcard/models/nsfw.kmodel";
  std::uint32_t input_width = 224;
  std::uint32_t input_height = 224;
  std::uint32_t max_regions = 9;
  std::string worker_path;
  std::string python = "python3";
  std::uint32_t timeout_ms = 30000;
  std::string ui_kmodel_path;
  bool ui_logits = false;
  std::string audio_kmodel_path;
  float ui_confidence = 0.25f;
  float ui_iou = 0.7f;
  std::uint32_t ui_min_side = 64;
  float ui_min_area = 0.01f;
  std::uint32_t ui_max_regions = 8;
};
std::unique_ptr<Analyzer> make_kpu_analyzer(const KpuConfig& config);

}  // namespace k230::inspector
