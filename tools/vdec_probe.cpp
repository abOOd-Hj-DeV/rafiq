#include <cstdio>
#include <algorithm>
#include <stdexcept>

#include "k230/cli.hpp"
#include "k230/inspector/decoder.hpp"
#include "k230/inspector/nonai2d_converter.hpp"
#include "k230/recording.hpp"

int main(int argc, char** argv) {
  using namespace k230;
  using namespace k230::inspector;
  Cli cli(argc, argv);
  const auto path = cli.get("input", "");
  const auto expected = cli.get_int("expected-frames", 0);
  if (path.empty() || cli.has("help") || expected < 0) {
    std::puts("k230-vdec-probe --input FILE.k230rec [--expected-frames N] [--device /dev/video0] [--csc] [--dump-frame FILE]");
    return 1;
  }
  try {
    RecordingReader input(path);
    if (!input.ok()) throw std::runtime_error(input.error());
    V4l2DecoderConfig config;
    config.device = cli.get("device", config.device);
    if (cli.has("rgb")) config.output_format = PixelFormat::RGBPlanar;
    auto decoder = make_v4l2_video_decoder(config);
    auto converter = cli.has("csc") ? make_nonai2d_converter() : nullptr;
    if (converter) {
      verify_nonai2d_converter(*converter);
      std::puts("PASS hardware CSC: BT.601/BT.709, full/limited range; 12 color cases");
    }
    bool opened = false;
    std::size_t count = 0;
    std::vector<std::int64_t> expected_pts, decoded_pts;
    std::int64_t last_pts = -1;
    auto check = [&](std::vector<VideoFrame>& frames) {
      for (auto& frame : frames) {
        if (converter) frame = converter->convert(frame);
        const auto expected_size = frame.format == PixelFormat::RGBPlanar ? frame.luma_size() * 3 :
            frame.luma_size() + std::size_t(2 * ((frame.width + 1) / 2)) * ((frame.height + 1) / 2);
        if (frame.format != (converter ? PixelFormat::RGBPlanar : config.output_format) || !frame.width || !frame.height ||
            frame.pts_us < last_pts ||
            frame.data.size() != expected_size)
          throw std::runtime_error("decoded NV12 layout or original PTS mismatch at frame " +
                                   std::to_string(count) + ": pts=" + std::to_string(frame.pts_us) +
                                   ", previous=" + std::to_string(last_pts));
        last_pts = frame.pts_us;
        decoded_pts.push_back(frame.pts_us);
        if (!count) {
          std::printf("VPU%s %s %ux%u first_pts=%lld\n", converter ? "+nonai2d" : "",
                      frame.format == PixelFormat::RGBPlanar ? "RGB planar" : "NV12",
                      frame.width, frame.height, static_cast<long long>(frame.pts_us));
          if (cli.has("dump-frame")) {
            auto* file = std::fopen(cli.get("dump-frame").c_str(), "wb");
            if (!file) throw std::runtime_error("cannot create decoded frame dump");
            const auto written = std::fwrite(frame.data.data(), 1, frame.data.size(), file);
            std::fclose(file);
            if (written != frame.data.size()) throw std::runtime_error("incomplete decoded frame dump");
          }
        }
        ++count;
      }
      frames.clear();
    };
    std::vector<VideoFrame> frames;
    while (auto packet = input.next()) {
      if (packet->stream != StreamType::Video) continue;
      if (!opened) {
        if (!decoder->open(packet->codec)) throw std::runtime_error("VPU open failed");
        opened = true;
      }
      if (!packet->is_config && packet->pts_us >= 0) expected_pts.push_back(packet->pts_us);
      if (!decoder->decode(*packet, frames)) throw std::runtime_error("VPU decode failed");
      check(frames);
    }
    if (!input.error().empty()) throw std::runtime_error(input.error());
    decoder->flush(frames);
    check(frames);
    std::sort(expected_pts.begin(), expected_pts.end());
    if (!count || expected_pts != decoded_pts || (expected && count != std::size_t(expected))) {
      const auto mismatch = std::mismatch(expected_pts.begin(), expected_pts.end(),
                                          decoded_pts.begin(), decoded_pts.end());
      if (mismatch.first != expected_pts.end() && mismatch.second != decoded_pts.end())
        throw std::runtime_error("VPU PTS mismatch at frame " +
                                 std::to_string(mismatch.first - expected_pts.begin()) +
                                 ": expected=" + std::to_string(*mismatch.first) +
                                 ", actual=" + std::to_string(*mismatch.second));
      throw std::runtime_error("VPU frame count mismatch");
    }
    std::printf("PASS hardware-only mvx: %zu %s frames; all original PTS verified; last_pts=%lld\n",
                count, converter || cli.has("rgb") ? "RGB planar" : "NV12", static_cast<long long>(last_pts));
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL VPU: %s; no software fallback\n", e.what());
    return 1;
  }
}
