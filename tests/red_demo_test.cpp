#include "k230/inspector/red_demo.hpp"
#include <gtest/gtest.h>

using namespace k230::inspector;

TEST(RedDemo, HandlesRgbAndRejectsNonRedColors) {
  VideoFrame f;
  f.width = f.height = 8;
  f.format = PixelFormat::RGBPlanar;
  f.data.assign(64 * 3, 0);
  std::fill_n(f.data.begin(), 64, 255);
  EXPECT_EQ(demo_red_fraction(f), 1.0);
  std::fill_n(f.data.begin() + 64, 64, 255);
  EXPECT_EQ(demo_red_fraction(f), 0.0);
  std::fill(f.data.begin(), f.data.end(), 255);
  EXPECT_EQ(demo_red_fraction(f), 0.0);
  std::fill(f.data.begin(), f.data.end(), 0);
  EXPECT_EQ(demo_red_fraction(f), 0.0);
}

TEST(RedDemo, HandlesNv12AndI420IncludingOddDimensions) {
  for (const auto format : {PixelFormat::NV12, PixelFormat::I420}) {
    for (const auto range : {YuvRange::Limited, YuvRange::Full}) {
      for (const auto matrix : {YuvMatrix::BT601, YuvMatrix::BT709}) {
        VideoFrame f;
        f.width = 7; f.height = 9;
        f.format = format; f.yuv_range = range; f.yuv_matrix = matrix;
        const bool limited = range == YuvRange::Limited;
        const bool bt709 = matrix == YuvMatrix::BT709;
        const auto y = limited ? (bt709 ? 63 : 81) : (bt709 ? 54 : 76);
        const auto u = limited ? (bt709 ? 102 : 90) : (bt709 ? 99 : 85);
        const auto v = limited ? 240 : 255;
        f.data.assign(63 + 20 * 2, y);
        for (int i = 0; i < 20; ++i) {
          if (format == PixelFormat::NV12) {
            f.data[63 + i * 2] = u; f.data[64 + i * 2] = v;
          } else {
            f.data[63 + i] = u; f.data[83 + i] = v;
          }
        }
        EXPECT_EQ(demo_red_fraction(f), 1.0);
      }
    }
  }
}

TEST(RedDemo, RejectsInvalidFrames) {
  VideoFrame f;
  EXPECT_FALSE(demo_red_fraction(f));
  f.width = f.height = 8;
  EXPECT_FALSE(demo_red_fraction(f));
  f.format = PixelFormat::RGBPlanar;
  f.data.resize(191);
  EXPECT_FALSE(demo_red_fraction(f));
}

TEST(RedDemo, RequiresNeutralBaselineThenTriggersOnFirstRedFrameOnlyOnce) {
  RedDemoTrigger t;
  using E = RedDemoTrigger::Event;
  EXPECT_EQ(t.observe(1, 1.0), E::None);
  EXPECT_EQ(t.observe(2, 0.0), E::Ready);
  EXPECT_EQ(t.observe(3, 0.799), E::None);
  EXPECT_EQ(t.observe(4, 0.80), E::Trigger);
  EXPECT_EQ(t.observe(5, 1.0), E::None);
  EXPECT_EQ(t.observe(6, 0.0), E::Ready);
  EXPECT_EQ(t.observe(7, 1.0), E::Trigger);
}

TEST(RedDemo, InvalidOrRepeatedFramesCannotTrigger) {
  RedDemoTrigger t;
  using E = RedDemoTrigger::Event;
  EXPECT_EQ(t.observe(2, 0.0), E::Ready);
  EXPECT_EQ(t.observe(1, 1.0), E::None);
  EXPECT_EQ(t.observe(2, 1.0), E::None);
  EXPECT_EQ(t.observe(3, std::nullopt), E::None);
  EXPECT_EQ(t.observe(4, 1.0), E::None);
  EXPECT_EQ(t.observe(5, 0.0), E::Ready);
  EXPECT_EQ(t.observe(6, -1.0), E::None);
  EXPECT_EQ(t.observe(7, 1.0), E::None);
}
