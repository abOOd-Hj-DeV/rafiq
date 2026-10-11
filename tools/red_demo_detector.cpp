#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>

#include "k230/inspector/decoder.hpp"
#include "k230/inspector/red_demo.hpp"
#include "k230/ipc/shared_memory_channel.hpp"

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
}

int main() {
  using namespace k230;
  using namespace k230::inspector;
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  try {
    ipc::SharedPacketSource source;
    auto decoder = make_v4l2_video_decoder();
    RedDemoTrigger trigger;
    bool opened = false;
    std::uint32_t next_sequence = 0;
    std::int64_t latest_pts = -1;
    auto last_packet = std::chrono::steady_clock::now();
    std::cerr << "DEMO ONLY: VPU decoded red screen; no AI classification\n";
    while (!stopped) {
      auto packet = source.pop(std::chrono::milliseconds(20));
      std::vector<VideoFrame> frames;
      if (packet && packet->stream == StreamType::Video) {
        last_packet = std::chrono::steady_clock::now();
        if (!opened) {
          if (!decoder->open(packet->codec)) throw std::runtime_error("hardware VPU open failed");
          opened = true;
        } else if (packet->seq != next_sequence) {
          throw std::runtime_error("video sequence gap; restart demo before continuing");
        }
        next_sequence = packet->seq + 1;
        if (packet->pts_us >= 0) latest_pts = packet->pts_us;
        if (!decoder->decode(*packet, frames)) throw std::runtime_error("hardware VPU decode failed");
      } else if (source.closed()) {
        throw std::runtime_error("capture disconnected");
      }
      if (opened && !decoder->poll_frames(frames)) throw std::runtime_error("hardware VPU polling failed");
      if (!opened && std::chrono::steady_clock::now() - last_packet > std::chrono::seconds(30))
        throw std::runtime_error("capture did not start; refusing intervention");
      for (const auto& frame : frames) {
        if (frame.pts_us < 0 || latest_pts < frame.pts_us || latest_pts - frame.pts_us > 1'000'000) {
          trigger.observe(frame.pts_us, std::nullopt);
          continue;
        }
        const auto fraction = demo_red_fraction(frame);
        const auto event = trigger.observe(frame.pts_us, fraction);
        if (event != RedDemoTrigger::Event::None) {
          std::cout << "{\"event\":\"" << (event == RedDemoTrigger::Event::Ready ? "ready" : "red")
                    << "\",\"pts_us\":" << frame.pts_us << ",\"red_fraction\":" << *fraction
                    << ",\"detected_at_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now().time_since_epoch()).count()
                    << "}" << std::endl;
        }
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "demo stopped: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
