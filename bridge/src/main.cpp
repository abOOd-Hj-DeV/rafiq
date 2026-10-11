// k230-bridge: little-core (Linux) side.
//
//   phone --USB/adb--> scrcpy-server sockets --> ScrcpyDemuxer --> PacketSink
//                                                                   |
//                        DATAFIFO to the big core (K230)  <---------+
//                        or a .k230rec file (PC development)
//
//   VerdictSource (IPCMSG from the big core) --> VerdictDispatcher --> companion app

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <thread>

#include "k230/bridge/adb_controller.hpp"
#include "k230/bridge/scrcpy_session.hpp"
#include "k230/bridge/companion_dispatcher.hpp"
#include "k230/bridge/protection_source.hpp"
#include "k230/bridge/verdict_dispatcher.hpp"
#include "k230/bridge/layout_receiver.hpp"
#include "k230/cli.hpp"
#include "k230/ipc/datafifo_channel.hpp"
#include "k230/ipc/shared_memory_channel.hpp"
#include "k230/log.hpp"
#include "k230/recording.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

class RecordingSink final : public k230::ipc::PacketSink {
 public:
  explicit RecordingSink(const std::string& path) : writer_(path) {}
  bool ok() const { return writer_.ok(); }
  bool push(k230::MediaPacket&& p) override { return writer_.write(p); }
  void close() override {}

 private:
  k230::RecordingWriter writer_;
};

class NullSink final : public k230::ipc::PacketSink {
 public:
  bool push(k230::MediaPacket&&) override { return true; }
  void close() override {}
};

void usage() {
  std::puts(
      "k230-bridge [options]\n"
      "  --sink shmem|datafifo|record|null  Linux/Linux shared memory, SDK, file, or discard\n"
      "  --record FILE                 shorthand for --sink record, writes FILE (.k230rec)\n"
      "  --adb PATH                    adb binary (default: adb)\n"
      "  --serial SERIAL               device serial (default: first authorised device)\n"
      "  --server-jar PATH             scrcpy-server jar (default: assets/scrcpy-server)\n"
      "  --max-size N  --max-fps N  --bitrate N  --video-codec h264|h265\n"
      "  --audio-codec raw|opus  --no-audio\n"
      "  --duration SEC                stop after SEC seconds (default: run until Ctrl-C)\n"
      "  --diagnostic-only             log verdicts without contacting a companion\n"
      "  --force-stop                  stop the foreground app on a confirmed block\n"
      "  --verbose");
}

}  // namespace

int main(int argc, char** argv) {
  using namespace k230;
  Cli cli(argc, argv);
  if (cli.has("help")) {
    usage();
    return 0;
  }
  if (cli.has("verbose")) log::threshold() = log::Level::Debug;
  bridge::ScrcpyConfig cfg;
  cfg.server_jar = cli.get("server-jar", cfg.server_jar);
  cfg.serial = cli.get("serial", "");
  cfg.max_size = static_cast<std::uint32_t>(cli.get_int("max-size", cfg.max_size));
  cfg.max_fps = static_cast<std::uint32_t>(cli.get_int("max-fps", cfg.max_fps));
  cfg.video_bit_rate = static_cast<std::uint32_t>(cli.get_int("bitrate", cfg.video_bit_rate));
  cfg.video_codec = cli.get("video-codec", cfg.video_codec);
  cfg.audio_codec = cli.get("audio-codec", cfg.audio_codec);
  cfg.audio = !cli.has("no-audio");
  const bool protection_v2 = cli.has("protection-v2");
  if (protection_v2 && !bridge::verified_protection_source(cfg)) {
    K230_LOG_ERROR("bridge") << "v2 requires the pinned scrcpy 4.0 capture source";
    return 1;
  }

  std::string sink_kind = cli.get("sink", cli.has("record") ? "record" : "null");
  std::shared_ptr<ipc::PacketSink> sink;
  std::shared_ptr<ipc::VerdictSource> verdicts;
  try {
    if (sink_kind == "record") {
      auto rec = std::make_shared<RecordingSink>(cli.get("record", "capture.k230rec"));
      if (!rec->ok()) {
        K230_LOG_ERROR("bridge") << "cannot open recording file";
        return 1;
      }
      sink = rec;
    } else if (sink_kind == "datafifo") {
      sink = std::make_shared<ipc::DataFifoPacketSink>(ipc::DataFifoConfig{});
      verdicts = std::make_shared<ipc::IpcMsgVerdictSource>(ipc::IpcMsgConfig{});
    } else if (sink_kind == "shmem") {
      sink = std::make_shared<ipc::SharedPacketSink>(ipc::SharedMemoryConfig{}, std::chrono::seconds(5));
      if (!protection_v2) verdicts = std::make_shared<ipc::SharedVerdictSource>();
    } else {
      sink = std::make_shared<NullSink>();
    }
  } catch (const std::exception& e) {
    K230_LOG_ERROR("bridge") << e.what();
    return 1;
  }

  bridge::AdbController adb(cli.get("adb", "adb"));
  if (!adb.available()) {
    K230_LOG_ERROR("bridge") << "adb not found or not working";
    return 1;
  }

  bridge::ScrcpySession session(cfg, adb, sink);
  if (!session.start()) return 1;

  std::unique_ptr<bridge::VerdictDispatcher> dispatcher;
  std::unique_ptr<bridge::LayoutReceiver> mvp_layout_receiver;
  std::unique_ptr<bridge::CompanionDispatcher> companion_dispatcher;
  std::shared_ptr<ipc::ControlSink> control_up;
  std::shared_ptr<ipc::ControlSource> control_down;
  std::atomic<std::uint64_t> verdicts_received{0};
  if (verdicts) {
    bridge::CompanionConfig companion;
    companion.force_stop = cli.has("force-stop");
    if (companion.force_stop) {
      companion.layouts = std::make_shared<LayoutCache>();
      companion.latest_capture_pts = [&session] { return session.latest_video_pts(); };
      mvp_layout_receiver = std::make_unique<bridge::LayoutReceiver>(
          bridge::LayoutReceiverConfig{}, adb, session.device_serial(), companion.layouts);
      mvp_layout_receiver->start();
    }
    companion.local_only = cli.has("diagnostic-only") || companion.force_stop;
    dispatcher = std::make_unique<bridge::VerdictDispatcher>(companion, adb,
                                                             session.device_serial(), verdicts);
    dispatcher->set_observer([&](const Verdict& verdict) {
      ++verdicts_received;
      if (cli.has("verbose")) K230_LOG_DEBUG("verdict") << to_json_line(verdict);
    });
    dispatcher->start();
  }
  if (protection_v2) {
    if (sink_kind != "shmem") {
      K230_LOG_ERROR("bridge") << "v2 requires Linux/Linux shared memory";
      return 1;
    }
    control_up = std::make_shared<ipc::SharedControlSink>(ipc::shared_control_up_config());
    control_down = std::make_shared<ipc::SharedControlSource>(ipc::shared_control_down_config());
    companion_dispatcher = std::make_unique<bridge::CompanionDispatcher>(
        bridge::CompanionConfig{}, adb, session.device_serial(), control_down,
        [control_up](const auto& message) {
          if (!control_up->push(companion::Json(message))) { control_up->close(); g_stop = true; }
        },
        [control_up] {
          if (!control_up->push(companion::Json())) { control_up->close(); g_stop = true; }
        });
    if (!companion_dispatcher->start()) return 1;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const long duration = cli.get_int("duration", 0);
  const auto t0 = std::chrono::steady_clock::now();
  auto next_report = t0 + std::chrono::seconds(5);
  while (!g_stop && session.running()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto now = std::chrono::steady_clock::now();
    if (duration > 0 && now - t0 >= std::chrono::seconds(duration)) break;
    if (now >= next_report) {
      next_report = now + std::chrono::seconds(5);
      const auto v = session.video_stats();
      const auto a = session.audio_stats();
      K230_LOG_INFO("bridge") << "video pkts=" << v.packets << " key=" << v.key_frames << " sessions=" << v.sessions
                              << " | audio pkts=" << a.packets << " | " << (v.bytes + a.bytes) / 1024 << " KiB";
    }
  }

  session.stop();
  sink->close();
  if (dispatcher) dispatcher->stop();
  if (companion_dispatcher) companion_dispatcher->stop();
  if (control_up) control_up->close();
  K230_LOG_INFO("bridge") << "stopped; verdicts received=" << verdicts_received
                         << " sent=" << (dispatcher ? dispatcher->sent() : 0)
                         << " dropped=" << (dispatcher ? dispatcher->dropped() : 0);
  return session.delivery_failed() ? 1 : 0;
}
