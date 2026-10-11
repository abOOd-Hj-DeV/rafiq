// k230-inspector: big-core (RT-Smart) side.
//
//   PacketSource (DATAFIFO on K230 | .k230rec replay on PC)
//     --> VideoDecoder (VDEC | FFmpeg) --> SyncEngine --> Analyzer (KPU | heuristic)
//     --> Policy --> VerdictSink (IPCMSG on K230 | stdout on PC)

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <thread>
#include <array>
#include <fstream>

#include "k230/cli.hpp"
#include "k230/inspector/pipeline.hpp"
#include "k230/inspector/nsfwjs_analyzer.hpp"
#include "k230/inspector/protection_session.hpp"
#include "k230/ipc/channel.hpp"
#include "k230/ipc/datafifo_channel.hpp"
#include "k230/ipc/shared_memory_channel.hpp"
#include "k230/log.hpp"
#include "k230/recording.hpp"

namespace {

constexpr std::size_t kReplayQueueCapacity = 256;
constexpr std::size_t kReplayHighWater = kReplayQueueCapacity / 2;

std::atomic<bool> g_stop{false};
k230::inspector::InspectorPipeline* g_pipeline = nullptr;
void on_signal(int) {
  g_stop = true;
  if (g_pipeline) g_pipeline->request_stop();
}

class StdoutVerdictSink final : public k230::ipc::VerdictSink {
 public:
  bool push(k230::Verdict&& v) override {
    std::fputs(k230::to_json_line(v).c_str(), stdout);
    std::fflush(stdout);
    return true;
  }
  void close() override {}
};
class NullVerdictSink final : public k230::ipc::VerdictSink {
 public:
  bool push(k230::Verdict&&) override { return true; }
  void close() override {}
};

// Feeds a recording into an in-process queue, optionally at the original
// pace (so the SyncEngine timeouts behave as they would live).
std::optional<k230::LayoutSnapshot> next_layout(std::ifstream& input) {
  std::array<std::uint8_t, 4> prefix{};
  if (!input.read(reinterpret_cast<char*>(prefix.data()), prefix.size())) return std::nullopt;
  const auto size = k230::layout_payload_size(prefix.data());
  if (size < k230::kLayoutHeaderSize || size > k230::kLayoutMaxPayload) {
    K230_LOG_WARN("replay") << "invalid layout length";
    return std::nullopt;
  }
  std::vector<std::uint8_t> payload(size);
  if (!input.read(reinterpret_cast<char*>(payload.data()), payload.size())) {
    K230_LOG_WARN("replay") << "truncated layout payload";
    return std::nullopt;
  }
  auto snapshot = k230::decode_layout(payload.data(), payload.size());
  if (!snapshot) K230_LOG_WARN("replay") << "malformed layout payload";
  return snapshot;
}

void replay_thread(const std::string& path, std::shared_ptr<k230::ipc::InProcessQueue<k230::MediaPacket>> queue,
                   bool realtime, std::shared_ptr<k230::LayoutCache> layouts, const std::string& layout_path) {
  using namespace k230;
  RecordingReader reader(path);
  if (!reader.ok()) {
    K230_LOG_ERROR("replay") << reader.error();
    queue->close();
    return;
  }
  std::int64_t first_pts = -1;
  auto t0 = std::chrono::steady_clock::now();
  std::ifstream layout_input;
  std::optional<LayoutSnapshot> pending_layout;
  std::uint64_t layout_session = 0;
  if (!layout_path.empty()) {
    layout_input.open(layout_path, std::ios::binary);
    if (!layout_input) K230_LOG_WARN("replay") << "cannot open layout replay; using visual fallback";
    else pending_layout = next_layout(layout_input);
  }
  while (!g_stop) {
    auto pkt = reader.next();
    if (!pkt) break;
    if (realtime && pkt->pts_us >= 0) {
      if (first_pts < 0) first_pts = pkt->pts_us;
      const auto due = t0 + std::chrono::microseconds(pkt->pts_us - first_pts);
      std::this_thread::sleep_until(due);
    }
    // Offline replay must not lose packets to the lossy queue: throttle instead.
    while (!g_stop && queue->size() >= kReplayHighWater) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (pending_layout && pkt->pts_us >= pending_layout->sampled_at_us) {
      if (layout_session != pending_layout->session) {
        layouts->clear();
        layout_session = pending_layout->session;
      }
      layouts->push(*pending_layout);
      pending_layout = next_layout(layout_input);
    }
    queue->push(std::move(*pkt));
  }
  if (!reader.error().empty()) K230_LOG_WARN("replay") << reader.error();
  queue->close();
}

void usage() {
  std::puts(
      "k230-inspector [options]\n"
      "  --source shmem|datafifo|replay  Linux/Linux shared memory, SDK, or recording\n"
      "  --replay FILE                 .k230rec produced by `k230-bridge --record`\n"
      "  --realtime                    pace the replay by PTS instead of as fast as possible\n"
      "  --layout-replay FILE          fast vision using recorded Mentor snapshots and --nsfwjs-model\n"
      "  --ui-model PATH               Android UI YOLOv8 Nano ONNX crops, without Mentor\n"
      "  --ui-confidence X             detector confidence threshold (default: 0.25)\n"
      "  --ui-iou X --ui-max-regions N  overlap threshold (0.7) and crop limit (8)\n"
      "  --ui-min-side N               minimum crop width AND height in decoded pixels (default: 64)\n"
      "  --ui-min-area X               minimum crop fraction of frame area (default: 0.01)\n"
      "  --verdicts shmem|ipcmsg|stdout  where verdicts go\n"
      "  --kmodel PATH                 use the KPU analyzer with this model (K230 only)\n"
      "  --ui-kmodel PATH              Android UI KModel feeding regions to --kmodel\n"
      "  --ui-logits                   UI KModel emits class logits; exact sigmoid after KPU\n"
      "  --audio-kmodel PATH           YAMNet audio events on KPU (not profanity detection)\n"
      "  --kpu-worker PATH             nncase Python worker (default: beside executable)\n"
      "  --nsfwjs-model PATH           use NSFWJS MobileNetV2 ONNX (PC, warning-only evaluation)\n"
      "  --onnx-threads N              CPU inference threads (default: 1)\n"
      "  --nsfwjs-regions N            rotate through up to N screen regions (default: 9)\n"
      "  --dump-dir DIR                PNGs up to 10 fps + continuous WAV segments, aligned by phone PTS\n"
      "  --no-audio                    ignore audio, release frames immediately\n"
      "  --warn X --block X --confirm N --cooldown-ms N   policy tuning\n"
      "  --audio-before-ms N --audio-after-ms N --max-wait-ms N  sync tuning\n"
      "  --analysis-fps N              sample decoded frames for analysis (0: all)\n"
      "  --verbose");
}

}  // namespace

int main(int argc, char** argv) {
  using namespace k230;
  using namespace k230::inspector;
  Cli cli(argc, argv);
  if (cli.has("help")) {
    usage();
    return 0;
  }
  if (cli.has("verbose")) log::threshold() = log::Level::Debug;
#ifndef K230_LINUX_PROTECTION_V2
  if (cli.has("protection-v2")) {
    K230_LOG_ERROR("inspector") << "standalone v2 enforcement unsupported: SDK bidirectional control/KPU adapter unverified; use PC k230-monitor";
    return 1;
  }
#endif

  PipelineConfig cfg;
  cfg.sync.audio_enabled = !cli.has("no-audio");
  cfg.sync.audio_before_us = cli.get_int("audio-before-ms", 2500) * 1000;
  cfg.sync.audio_after_us = cli.get_int("audio-after-ms", 500) * 1000;
  cfg.sync.max_wait_us = cli.get_int("max-wait-ms", 1000) * 1000;
  const auto analysis_fps = cli.get_int("analysis-fps", 0);
  if (analysis_fps < 0 || analysis_fps > 120) {
    K230_LOG_ERROR("inspector") << "analysis-fps must be 0..120";
    return 1;
  }
  cfg.sync.min_frame_interval_us = analysis_fps ? 1000000 / analysis_fps : 0;
  cfg.policy.warn_threshold = static_cast<float>(cli.get_double("warn", 0.60));
  cfg.policy.block_threshold = static_cast<float>(cli.get_double("block", 0.85));
  cfg.policy.confirm_frames = static_cast<std::uint32_t>(cli.get_int("confirm", 3));
  cfg.policy.cooldown_us = cli.get_int("cooldown-ms", 5000) * 1000;
  cfg.dump_dir = cli.get("dump-dir", "");
  cfg.idle_timeout_ms = cli.get_int("idle-timeout-ms", 0);

  if (cli.has("kmodel") && cli.has("nsfwjs-model")) {
    K230_LOG_ERROR("inspector") << "choose either --kmodel or --nsfwjs-model";
    return 1;
  }
  if (cli.has("ui-kmodel") && !cli.has("kmodel")) {
    K230_LOG_ERROR("inspector") << "--ui-kmodel requires --kmodel";
    return 2;
  }
  if (cli.has("ui-logits") && !cli.has("ui-kmodel")) {
    K230_LOG_ERROR("inspector") << "--ui-logits requires --ui-kmodel";
    return 2;
  }
  if (cli.has("audio-kmodel") && (!cli.has("kmodel") || cli.has("no-audio"))) {
    K230_LOG_ERROR("inspector") << "--audio-kmodel requires --kmodel and enabled audio";
    return 2;
  }
  std::unique_ptr<Analyzer> analyzer;
  NsfwjsConfig nsfw;
  std::unique_ptr<RegionDetector> detector;
  if (cli.has("ui-model")) {
    if (!cli.has("nsfwjs-model") || cli.has("layout-replay")) {
      K230_LOG_ERROR("inspector") << "--ui-model requires --nsfwjs-model and cannot be combined with --layout-replay";
      return 1;
    }
    UiDetectorConfig ui;
    ui.model_path = cli.get("ui-model");
    ui.min_side = static_cast<std::uint32_t>(cli.get_int("ui-min-side", 64));
    ui.min_area = static_cast<float>(cli.get_double("ui-min-area", 0.01));
    ui.confidence = static_cast<float>(cli.get_double("ui-confidence", 0.25));
    ui.iou = static_cast<float>(cli.get_double("ui-iou", 0.7));
    ui.max_regions = static_cast<std::uint32_t>(cli.get_int("ui-max-regions", 8));
    detector = make_ui_detector(ui);
    if (!detector || !detector->open()) return 1;
  }
  auto layouts = std::make_shared<LayoutCache>();
  if (cli.has("layout-replay") && (!cli.has("nsfwjs-model") || !cli.has("replay"))) {
    K230_LOG_ERROR("inspector") << "--layout-replay requires --replay and --nsfwjs-model";
    return 1;
  }
  if (cli.has("nsfwjs-model")) {
    nsfw.model_path = cli.get("nsfwjs-model");
    nsfw.threads = static_cast<int>(cli.get_int("onnx-threads", 1));
    nsfw.max_regions = static_cast<std::uint32_t>(cli.get_int("nsfwjs-regions", 9));
    if (cli.has("layout-replay") || cli.has("ui-model")) {
      if (nsfw.threads != 1) {
        K230_LOG_ERROR("inspector") << "fast vision requires --onnx-threads 1";
        return 1;
      }
      analyzer = std::make_unique<HeuristicAnalyzer>();
    } else {
      analyzer = make_nsfwjs_analyzer(nsfw);
      if (!analyzer || !analyzer->open()) return 1;
    }
    cfg.allow_analyzer_fallback = false;
    cfg.policy.escalated_action = Action::Warn;
  } else if (cli.has("kmodel")) {
    KpuConfig kpu;
    kpu.kmodel_path = cli.get("kmodel");
    kpu.ui_kmodel_path = cli.get("ui-kmodel");
    kpu.ui_logits = cli.has("ui-logits");
    kpu.audio_kmodel_path = cli.get("audio-kmodel");
    kpu.worker_path = cli.get("kpu-worker", "");
    kpu.max_regions = static_cast<std::uint32_t>(cli.get_int("nsfwjs-regions", 9));
    analyzer = make_kpu_analyzer(kpu);
    cfg.allow_analyzer_fallback = false;
    cfg.policy.escalated_action = Action::Warn;
  } else {
    analyzer = std::make_unique<HeuristicAnalyzer>();
  }
  if (cli.has("mvp-force-stop")) cfg.policy.escalated_action = Action::Block;

  const std::string source_kind = cli.get("source", cli.has("replay") ? "replay" : "datafifo");
  const std::string verdict_kind = cli.get("verdicts", source_kind == "replay" ? "stdout" :
                                                      source_kind == "shmem" ? "shmem" : "ipcmsg");
  const bool protection_v2 = cli.has("protection-v2");
  if (protection_v2 && (source_kind != "shmem" || !cli.has("kmodel") || !cli.has("ui-kmodel"))) {
    K230_LOG_ERROR("inspector") << "v2 requires shared memory plus KPU content and UI models";
    return 1;
  }

  std::shared_ptr<ipc::PacketSource> source;
  std::shared_ptr<ipc::VerdictSink> verdicts;
  std::thread replay;
  try {
    if (source_kind == "replay") {
      auto queue = std::make_shared<ipc::InProcessQueue<MediaPacket>>(kReplayQueueCapacity, ipc::keep_config_packets);
      source = queue;
      replay = std::thread(replay_thread, cli.get("replay", "capture.k230rec"), queue, cli.has("realtime"),
                           layouts, cli.get("layout-replay", ""));
    } else if (source_kind == "shmem") {
      source = std::make_shared<ipc::SharedPacketSource>();
    } else {
      source = std::make_shared<ipc::DataFifoPacketSource>(ipc::DataFifoConfig{});
    }
    if (protection_v2) {
      verdicts = std::make_shared<NullVerdictSink>();
    } else if (verdict_kind == "stdout") {
      verdicts = std::make_shared<StdoutVerdictSink>();
    } else if (verdict_kind == "shmem") {
      verdicts = std::make_shared<ipc::SharedVerdictSink>();
    } else {
      verdicts = std::make_shared<ipc::IpcMsgVerdictSink>(ipc::IpcMsgConfig{});
    }
  } catch (const std::exception& e) {
    K230_LOG_ERROR("inspector") << e.what();
    g_stop = true;
    if (replay.joinable()) replay.join();
    return 1;
  }

  InspectorPipeline pipeline(cfg, source, verdicts, make_default_video_decoder(), std::move(analyzer));
  std::shared_ptr<ipc::ControlSink> control_down;
  std::shared_ptr<ipc::ControlSource> control_up;
  std::unique_ptr<ProtectionSession> protection;
  std::thread control_receiver;
  std::atomic<bool> control_failed{false};
  if (protection_v2) {
    try {
      control_down = std::make_shared<ipc::SharedControlSink>(ipc::shared_control_down_config());
      control_up = std::make_shared<ipc::SharedControlSource>(ipc::shared_control_up_config());
      protection = std::make_unique<ProtectionSession>(control_down, true);
      pipeline.set_capture_pts_observer([&protection](std::int64_t pts) { protection->observe_capture_pts(pts); });
      control_receiver = std::thread([&] {
        try {
          while (!g_stop) {
            auto message = control_up->pop(std::chrono::milliseconds(50));
            if (!message) {
              if (control_up->closed()) {
                if (!g_stop) { control_failed = true; pipeline.request_stop(); }
                break;
              }
              continue;
            }
            if (message->is_null()) protection->disconnected();
            else protection->receive(*message);
          }
          protection->disconnected();
        } catch (const std::exception& error) {
          K230_LOG_ERROR("protection") << "control channel failed: " << error.what();
          control_failed = true;
          pipeline.request_stop();
        }
      });
    } catch (const std::exception& error) {
      K230_LOG_ERROR("protection") << error.what();
      return 1;
    }
  }
  std::unique_ptr<FastVision> vision;
  if (cli.has("ui-model")) {
    vision = std::make_unique<FastVision>(
        [nsfw] { return make_nsfwjs_region_analyzer(nsfw); }, std::move(detector), cfg.policy, verdicts);
  }
  if (cli.has("layout-replay")) {
    vision = std::make_unique<FastVision>(
        [nsfw] { return make_nsfwjs_region_analyzer(nsfw); }, layouts, cfg.policy, verdicts);
  }
  if (vision) {
    vision->set_observer([](const auto& frame, const auto& scores, const auto&) {
      K230_LOG_INFO("vision") << "pts=" << frame.pts_us << " " << describe_scores(scores);
    });
    pipeline.set_vision(std::move(vision));
  }
  g_pipeline = &pipeline;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  pipeline.set_observer([&](const SyncedSample& s, const Scores& scores, const Verdict& v) {
    if (!g_stop && !control_failed && protection && scores.protection) protection->analyze(*scores.protection);
    if (v.action >= Action::Warn || log::threshold() <= log::Level::Debug) {
      K230_LOG_INFO("sample") << "pts=" << s.frame.pts_us << " " << s.frame.width << "x" << s.frame.height
                              << " audio=" << s.audio_filled_us / 1000 << "/" << (s.audio_end_us - s.audio_start_us) / 1000
                              << "ms" << (s.audio_complete ? "" : " (incomplete)") << " " << describe_scores(scores)
                              << " -> " << to_string(v.action);
    }
  });

  const bool ok = pipeline.run();
  g_pipeline = nullptr;
  g_stop = true;
  if (control_down) control_down->close();
  if (control_receiver.joinable()) control_receiver.join();
  if (replay.joinable()) replay.join();

  const auto& st = pipeline.stats();
  const auto& ss = pipeline.sync().stats();
  K230_LOG_INFO("inspector") << "sync: emitted=" << ss.emitted << " incomplete=" << ss.emitted_incomplete
                             << " sampled_out=" << ss.frames_skipped
                             << " backpressure=" << ss.emitted_backpressure << " last_av_lead_ms=" << ss.last_av_lead_us / 1000
                             << " | verdicts escalated=" << st.verdicts_escalated;
  return !ok || control_failed || (st.video_packets > 0 && st.frames_decoded == 0) ? 1 : 0;
}
