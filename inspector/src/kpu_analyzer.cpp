#include "k230/inspector/analyzer.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "k230/inspector/kpu_runtime.hpp"
#include "k230/inspector/nonai2d_converter.hpp"
#include "k230/inspector/nsfwjs_analyzer.hpp"
#include "k230/inspector/ui_detector.hpp"
#include "k230/log.hpp"

namespace k230::inspector {

namespace {

class KpuAnalyzer final : public Analyzer {
 public:
  explicit KpuAnalyzer(KpuConfig config)
      : config_(std::move(config)),
        runtime_(KpuRuntimeConfig{config_.kmodel_path, config_.worker_path, config_.python, config_.timeout_ms}) {
    if (!config_.ui_kmodel_path.empty()) {
      ui_runtime_ = std::make_unique<KpuRuntime>(KpuRuntimeConfig{
          config_.ui_kmodel_path, config_.worker_path, config_.python, config_.timeout_ms,
          kUiSize, static_cast<std::uint32_t>((kUiClasses + 4) * kUiBoxes), false, true});
    }
    if (!config_.audio_kmodel_path.empty()) {
      audio_runtime_ = std::make_unique<KpuRuntime>(KpuRuntimeConfig{
          config_.audio_kmodel_path, config_.worker_path, config_.python, config_.timeout_ms,
          96, 521, false, false, true});
    }
  }

  bool open() override {
    try {
      if (config_.input_width != 224 || config_.input_height != 224 ||
          config_.max_regions < 1 || config_.max_regions > 9)
        throw std::invalid_argument("NSFWJS KPU requires 224x224 input and 1..9 regions");
#ifdef K230_HAS_V4L2_API
      converter_ = make_nonai2d_converter();
      verify_nonai2d_converter(*converter_);
      if (!runtime_.open()) { converter_.reset(); return false; }
      if (ui_runtime_ && !ui_runtime_->open()) { converter_.reset(); return false; }
      if (audio_runtime_ && !audio_runtime_->open()) { converter_.reset(); return false; }
      K230_LOG_INFO("kpu") << "hardware-only Non-AI2D -> AI2D -> KPU; "
                           << (ui_runtime_ ? "Android UI + NSFWJS" : "NSFWJS") << " output";
      return true;
#else
      throw std::runtime_error("K230 Linux hardware backend unavailable");
#endif
    } catch (const std::exception& error) {
      K230_LOG_ERROR("kpu") << error.what() << "; no fallback";
      return false;
    }
  }

  Scores analyze(const SyncedSample& sample) override {
#ifdef K230_HAS_V4L2_API
    if (!converter_) throw std::runtime_error("KPU analyzer is not open");
    const auto started = std::chrono::steady_clock::now();
    auto rgb = converter_->convert(sample.frame);
    const auto converted = std::chrono::steady_clock::now();
    std::vector<ImageRegion> regions;
    std::vector<std::string> region_kinds;
    bool complete = true;
    KpuResult ui_result;
    if (ui_runtime_) {
      ui_result = ui_runtime_->infer(rgb, ImageRegion{0, 0, rgb.width, rgb.height});
      const auto detections = ui_quantized_regions(sample.frame, std::move(ui_result.values),
          UiDetectorConfig{"", 1, config_.ui_confidence, config_.ui_iou, config_.ui_min_side,
                           config_.ui_min_area, config_.ui_max_regions}, config_.ui_logits);
      regions = detections.regions;
      region_kinds = detections.kinds;
      if (regions.size() != region_kinds.size())
        throw std::runtime_error("KPU UI region kinds do not match detections");
      const bool detected_media = !regions.empty() && regions.size() == region_kinds.size();
      if (regions.empty()) {
        regions.push_back({0, 0, sample.frame.width, sample.frame.height});
        region_kinds.push_back("Image");
      }
      complete = detections.complete && detected_media;
    } else {
      regions = nsfwjs_regions(sample.frame, config_.max_regions);
    }
    Scores scores;
    scores.audio_level = HeuristicAnalyzer::rms(sample.audio);
    scores.analysis_regions = static_cast<std::uint32_t>(regions.size());
    scores.analysis_complete = complete;
    if (ui_runtime_) {
      scores.protection = AnalysisBatch{sample.frame.pts_us, static_cast<int>(sample.frame.width),
          static_cast<int>(sample.frame.height), {}, {}, complete, false, {}};
    }
    if (!regions.empty() && ui_runtime_) {
      float maximum = -1;
      for (std::size_t index = 0; index < regions.size(); ++index) {
        const auto result = runtime_.infer(rgb, regions[index]);
        const auto risk = std::max(result.values[1], result.values[3]);
        const auto& crop = regions[index];
        scores.protection->regions.push_back({{static_cast<int>(crop.x), static_cast<int>(crop.y),
            static_cast<int>(crop.width), static_cast<int>(crop.height)}, region_kinds[index],
            {result.values[3], result.values[1], result.values[4]}, complete, false});
        scores.preparation_ms += result.preparation_ms;
        scores.transform_ms += result.transform_ms;
        scores.inference_ms += result.inference_ms;
        if (risk > maximum) {
          maximum = risk;
          scores.nsfwjs = NsfwjsScores{result.values[0], result.values[1], result.values[2],
                                      result.values[3], result.values[4]};
          scores.nudity = risk;
          scores.analysis_region = static_cast<std::uint32_t>(index);
          scores.crop = regions[index];
        }
      }
    } else if (!regions.empty()) {
      const auto index = next_region_++ % regions.size();
      const auto result = runtime_.infer(rgb, regions[index]);
      scores.nsfwjs = NsfwjsScores{result.values[0], result.values[1], result.values[2],
                                  result.values[3], result.values[4]};
      scores.nudity = std::max(result.values[1], result.values[3]);
      scores.analysis_region = static_cast<std::uint32_t>(index);
      scores.crop = regions[index];
      scores.preparation_ms = result.preparation_ms;
      scores.transform_ms = result.transform_ms;
      scores.inference_ms = result.inference_ms;
    }
    scores.preparation_ms += ui_result.preparation_ms;
    scores.transform_ms += ui_result.transform_ms +
        std::chrono::duration<double, std::milli>(converted - started).count();
    scores.inference_ms += ui_result.inference_ms;
    if (audio_runtime_ && sample.audio_complete && sample.audio_end_us - sample.audio_start_us >= 975000) {
      const auto audio = audio_runtime_->infer_audio(sample.audio, sample.sample_rate, sample.channels,
                                                     sample.audio_end_us);
      const auto best = std::max_element(audio.values.begin(), audio.values.end());
      scores.audio_event = AudioEvent{static_cast<std::uint32_t>(best - audio.values.begin()), *best,
                                      sample.audio_end_us, audio.inference_ms};
      scores.preparation_ms += audio.preparation_ms;
      scores.inference_ms += audio.inference_ms;
    }
    scores.analysis_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    return scores;
#else
    (void)sample;
    throw std::runtime_error("K230 Linux hardware backend unavailable");
#endif
  }
  const char* name() const override { return "k230-kpu-nsfwjs"; }

 private:
  KpuConfig config_;
  KpuRuntime runtime_;
  std::unique_ptr<KpuRuntime> ui_runtime_;
  std::unique_ptr<KpuRuntime> audio_runtime_;
  std::size_t next_region_ = 0;
#ifdef K230_HAS_V4L2_API
  std::unique_ptr<FrameConverter> converter_;
#endif
};

}  // namespace

std::unique_ptr<Analyzer> make_kpu_analyzer(const KpuConfig& config) {
  return std::make_unique<KpuAnalyzer>(config);
}

}  // namespace k230::inspector
