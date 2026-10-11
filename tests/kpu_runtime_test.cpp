#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

#include "k230/inspector/kpu_runtime.hpp"

namespace k230::inspector {
#ifdef __linux__
namespace {
KpuRuntimeConfig config(const std::string& mode) {
  return {mode, K230_TEST_KPU_WORKER, "python3", 1000};
}
VideoFrame frame() {
  VideoFrame value;
  value.width = 320; value.height = 640; value.pts_us = 12345678;
  value.format = PixelFormat::RGBPlanar;
  value.data.assign(value.luma_size() * 3, 73);
  return value;
}
}

TEST(KpuRuntime, PersistentSocketPreservesLargeRgbPayloadAndPts) {
  KpuRuntime runtime(config("good"));
  ASSERT_TRUE(runtime.open());
  auto rgb = frame();
  for (unsigned i = 0; i < 3; ++i) {
    rgb.pts_us += 33333;
    const auto result = runtime.infer(rgb, {0, 0, rgb.width, rgb.height});
    ASSERT_EQ(result.values.size(), 5);
    EXPECT_FLOAT_EQ(result.values[3], 0.3f);
    EXPECT_DOUBLE_EQ(result.preparation_ms, 1);
    EXPECT_DOUBLE_EQ(result.transform_ms, 2);
    EXPECT_DOUBLE_EQ(result.inference_ms, 3);
  }
}

TEST(KpuRuntime, RejectsWrongModelShape) {
  KpuRuntime runtime(config("wrong-shape"));
  EXPECT_FALSE(runtime.open());
}

TEST(KpuRuntime, AcceptsBoundedGenericUiTensorWithoutProbabilityValidation) {
  auto cfg = config("good");
  cfg.input_size = 640;
  cfg.output_count = 25 * 8400;
  cfg.probabilities = false;
  cfg.letterbox = true;
  KpuRuntime runtime(cfg);
  ASSERT_TRUE(runtime.open());
  auto rgb = frame();
  const auto result = runtime.infer(rgb, {0, 0, rgb.width, rgb.height});
  ASSERT_EQ(result.values.size(), 25u * 8400u);
  EXPECT_FLOAT_EQ(result.values.front(), 0);
  EXPECT_FLOAT_EQ(result.values.back(), 0);
}

TEST(KpuRuntime, StartupTimeoutKillsAndReapsWorker) {
  auto cfg = config("startup-stall");
  cfg.timeout_ms = 100;
  const auto start = std::chrono::steady_clock::now();
  KpuRuntime runtime(cfg);
  EXPECT_FALSE(runtime.open());
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
}

TEST(KpuRuntime, AudioPreservesPcmEndianPtsAndIndependentProbabilities) {
  auto cfg = config("good");
  cfg.input_size = 96; cfg.output_count = 521;
  cfg.probabilities = false; cfg.audio = true;
  KpuRuntime runtime(cfg);
  ASSERT_TRUE(runtime.open());
  for (unsigned i = 0; i < 3; ++i) {
    const auto result = runtime.infer_audio(std::vector<std::int16_t>(64000, -1234),
                                           16000, 2, 12345678 + i * 33333);
    ASSERT_EQ(result.values.size(), 521u);
    EXPECT_FLOAT_EQ(result.values[12], .8f);
    EXPECT_FLOAT_EQ(result.values[0], .2f);
  }
}

TEST(KpuRuntime, AudioRejectsInvalidContractAndOutputWithoutFallback) {
  auto cfg = config("good");
  cfg.input_size = 96; cfg.output_count = 521;
  cfg.probabilities = false; cfg.audio = true;
  auto invalid = cfg;
  invalid.input_size = 224;
  EXPECT_FALSE(KpuRuntime(invalid).open());
  for (const auto& mode : {"nan", "bad-range", "wrong-pts"}) {
    cfg.model_path = mode;
    KpuRuntime runtime(cfg);
    ASSERT_TRUE(runtime.open());
    EXPECT_THROW(runtime.infer_audio(std::vector<std::int16_t>(32000, -1234), 16000, 2, 12345678),
                 std::runtime_error);
  }
}

TEST(KpuRuntime, AudioRejectsUnsupportedRateChannelsShortPcmAndNegativePts) {
  for (unsigned test = 0; test < 4; ++test) {
    auto cfg = config("good");
    cfg.input_size = 96; cfg.output_count = 521;
    cfg.probabilities = false; cfg.audio = true;
    KpuRuntime runtime(cfg);
    ASSERT_TRUE(runtime.open());
    EXPECT_THROW(runtime.infer_audio(std::vector<std::int16_t>(test == 2 ? 100 : 32000, -1234),
                                     test == 0 ? 8000 : 16000, test == 1 ? 0 : 2,
                                     test == 3 ? -1 : 12345678), std::invalid_argument);
  }
}

TEST(KpuRuntime, InvalidConfigurationFailsClosed) {
  auto cfg = config("good");
  cfg.timeout_ms = 0;
  KpuRuntime runtime(cfg);
  EXPECT_FALSE(runtime.open());
  EXPECT_THROW(runtime.infer(frame(), {0, 0, 320, 640}), std::runtime_error);
}

TEST(KpuRuntime, MissingPythonFailsClosed) {
  auto cfg = config("good");
  cfg.python = "/nonexistent/rafiq-python";
  KpuRuntime runtime(cfg);
  EXPECT_FALSE(runtime.open());
}

TEST(KpuRuntime, RejectsInvalidFrameAndCrop) {
  for (unsigned test = 0; test < 4; ++test) {
    KpuRuntime runtime(config("good"));
    ASSERT_TRUE(runtime.open());
    auto rgb = frame();
    ImageRegion crop{0, 0, rgb.width, rgb.height};
    if (test == 0) rgb.data.pop_back();
    if (test == 1) rgb.format = PixelFormat::NV12;
    if (test == 2) rgb.pts_us = -1;
    if (test == 3) crop.x = 1;
    EXPECT_THROW(runtime.infer(rgb, crop), std::invalid_argument);
    EXPECT_THROW(runtime.infer(frame(), {0, 0, 320, 640}), std::runtime_error);
  }
}

class KpuResponseFailure : public ::testing::TestWithParam<const char*> {};
TEST_P(KpuResponseFailure, FailsWithoutFallbackAndClosesRuntime) {
  auto cfg = config(GetParam());
  cfg.timeout_ms = 200;
  KpuRuntime runtime(cfg);
  ASSERT_TRUE(runtime.open());
  EXPECT_THROW(runtime.infer(frame(), {0, 0, 320, 640}), std::runtime_error);
  EXPECT_THROW(runtime.infer(frame(), {0, 0, 320, 640}), std::runtime_error);
}
INSTANTIATE_TEST_SUITE_P(Protocol, KpuResponseFailure,
    ::testing::Values("eof", "infer-stall", "error", "nan", "wrong-pts", "bad-sum"));
#endif
}  // namespace k230::inspector
