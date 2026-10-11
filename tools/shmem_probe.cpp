#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "k230/cli.hpp"
#include "k230/ipc/shared_memory_channel.hpp"

int main(int argc, char** argv) {
  using namespace k230;
  using namespace std::chrono_literals;
  Cli cli(argc, argv);
  const auto role = cli.get("role", "");
  const auto count = cli.get_int("count", 100);
  if ((role != "little" && role != "big") || count < 1 || count > 100000) {
    std::puts("k230-shmem-probe --role little|big [--count 100]");
    return 1;
  }
  try {
    if (cli.has("send-file") || cli.has("receive-file")) {
      constexpr std::size_t kChunk = 240 * 1024;
      ipc::SharedMemoryConfig transfer_config;
      if (cli.has("device")) {
        transfer_config.device = cli.get("device");
        transfer_config.address = 0;
      }
      if (role == "big" && cli.has("send-file") && !cli.has("receive-file")) {
        std::ifstream input(cli.get("send-file"), std::ios::binary);
        if (!input) throw std::runtime_error("cannot open input file");
        input.seekg(0, std::ios::end);
        const auto size = input.tellg();
        if (size <= 0 || size > 256 * 1024 * 1024)
          throw std::runtime_error("input size must be 1..256 MiB");
        input.seekg(0);
        ipc::SharedPacketSink media(transfer_config);
        std::vector<char> buffer(kChunk);
        std::uint32_t sequence = 0;
        while (input) {
          input.read(buffer.data(), buffer.size());
          const auto bytes = input.gcount();
          if (bytes <= 0) break;
          MediaPacket packet;
          packet.codec = CodecId::Raw;
          packet.stream = StreamType::Audio;
          packet.seq = sequence++;
          packet.pts_us = static_cast<std::int64_t>(size);
          packet.is_config = packet.seq == 0;
          packet.is_key_frame = input.peek() == std::ifstream::traits_type::eof();
          packet.data.assign(buffer.begin(), buffer.begin() + bytes);
          const auto deadline = std::chrono::steady_clock::now() + 5min;
          while (!media.push(MediaPacket(packet))) {
            if (std::chrono::steady_clock::now() >= deadline)
              throw std::runtime_error("file transfer stalled");
            std::this_thread::sleep_for(1ms);
          }
        }
        media.close();
        std::printf("PASS big: sent %lld bytes in %u shared-memory records\n",
                    static_cast<long long>(size), sequence);
        return 0;
      }
      if (role == "little" && cli.has("receive-file") && !cli.has("send-file")) {
        const auto destination = cli.get("receive-file");
        const auto temporary = destination + ".rafiq-part";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot create output file");
        ipc::SharedPacketSource media(transfer_config);
        std::uint32_t sequence = 0;
        std::int64_t expected_size = -1;
        while (true) {
          const auto packet = media.pop(30s);
          if (!packet || packet->seq != sequence++ || packet->codec != CodecId::Raw ||
              packet->stream != StreamType::Audio || packet->is_config != (packet->seq == 0) ||
              packet->data.empty() || packet->data.size() > kChunk)
            throw std::runtime_error("file record missing or corrupt");
          if (expected_size < 0) expected_size = packet->pts_us;
          if (packet->pts_us != expected_size) throw std::runtime_error("file size changed in stream");
          output.write(reinterpret_cast<const char*>(packet->data.data()), packet->data.size());
          if (packet->is_key_frame) break;
        }
        output.close();
        std::ifstream check(temporary, std::ios::binary | std::ios::ate);
        if (!check || check.tellg() != expected_size) throw std::runtime_error("received size mismatch");
        if (std::rename(temporary.c_str(), destination.c_str()) != 0)
          throw std::runtime_error("cannot publish output file");
        std::printf("PASS little: received %lld bytes in %u shared-memory records\n",
                    static_cast<long long>(expected_size), sequence);
        return 0;
      }
      throw std::runtime_error("--send-file is big-only and --receive-file is little-only");
    }
    if (cli.has("control")) {
      ipc::SharedControlSink outgoing(role == "little" ? ipc::shared_control_up_config() :
                                                       ipc::shared_control_down_config());
      ipc::SharedControlSource incoming(role == "little" ? ipc::shared_control_down_config() :
                                                          ipc::shared_control_up_config());
      const char* names[] = {"hello", "bind", "bound", "state", "decision", "ack", "released"};
      for (int i = 0; i <= count; ++i) {
        companion::Json expected;
        if (role == "little" && i < count) {
          std::ifstream file(cli.get("fixtures", "protocol/v2/fixtures") + "/" + names[i % 7] + ".json");
          expected = companion::Json::parse(file);
        }
        if (role == "little" && !outgoing.push(companion::Json(expected)))
          throw std::runtime_error("control ring rejected probe");
        const auto message = incoming.pop(30s);
        if (!message || (role == "little" && *message != expected) ||
            (i == count && !message->is_null()))
          throw std::runtime_error("control record missing or corrupt");
        if (role == "big" && !outgoing.push(companion::Json(*message)))
          throw std::runtime_error("control echo rejected");
      }
      outgoing.close();
      std::printf("PASS %s: %lld native control transport records + disconnect marker (not phone execution)\n",
                  role.c_str(), static_cast<long long>(count));
      return 0;
    }
    if (role == "little") {
      ipc::SharedPacketSink media;
      ipc::SharedVerdictSource verdicts;
      for (int i = 0; i < count; ++i) {
        MediaPacket packet;
        packet.codec = i % 2 ? CodecId::Raw : CodecId::H264;
        packet.stream = i % 2 ? StreamType::Audio : StreamType::Video;
        packet.pts_us = std::int64_t{i} * 16667;
        packet.seq = static_cast<std::uint32_t>(i);
        packet.is_config = i == 0;
        packet.is_key_frame = i % 30 == 0;
        packet.data.resize(65536, static_cast<std::uint8_t>(i));
        if (!media.push(std::move(packet))) throw std::runtime_error("media ring rejected probe");
        const auto ack = verdicts.pop(30s);
        if (!ack || ack->sequence != std::uint32_t(i) || ack->pts_us != std::int64_t{i} * 16667)
          throw std::runtime_error("verdict missing or corrupt");
      }
      media.close();
    } else {
      ipc::SharedVerdictSink verdicts;
      ipc::SharedPacketSource media;
      for (int i = 0; i < count; ++i) {
        const auto packet = media.pop(30s);
        if (!packet || packet->seq != std::uint32_t(i) || packet->pts_us != std::int64_t{i} * 16667 ||
            packet->stream != (i % 2 ? StreamType::Audio : StreamType::Video) ||
            packet->codec != (i % 2 ? CodecId::Raw : CodecId::H264) ||
            packet->is_config != (i == 0) || packet->is_key_frame != (i % 30 == 0) ||
            packet->data.size() != 65536)
          throw std::runtime_error("media metadata missing or corrupt");
        for (auto byte : packet->data)
          if (byte != static_cast<std::uint8_t>(i)) throw std::runtime_error("media payload corrupt");
        Verdict ack;
        ack.sequence = packet->seq; ack.pts_us = packet->pts_us;
        ack.action = Action::Log; ack.confidence = 1;
        if (!verdicts.push(std::move(ack))) throw std::runtime_error("verdict ring rejected probe");
      }
      verdicts.close();
    }
    std::printf("PASS %s: %lld x 64KiB media packets + verdicts, payload/PTS/flags verified\n",
                role.c_str(), static_cast<long long>(count));
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL %s: %s\n", role.c_str(), e.what());
    return 1;
  }
}
