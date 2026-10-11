#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <future>
#include <limits>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

#include "k230/ipc/shared_memory_channel.hpp"
#include "k230/bridge/protection_source.hpp"

namespace {
using namespace k230;
using namespace k230::ipc;
using namespace std::chrono_literals;

class SharedMemoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/rafiq-shmem-XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, 4096), 0);
    close(fd);
    config = {path, 0, 4, 128};
  }
  void TearDown() override { unlink(config.device.c_str()); }
  MediaPacket packet(std::uint32_t seq) {
    MediaPacket p;
    p.codec = CodecId::H264;
    p.seq = seq;
    p.pts_us = 9876543 + seq;
    p.is_config = seq == 0;
    p.is_key_frame = seq == 1;
    p.data = {1, 2, static_cast<std::uint8_t>(seq)};
    return p;
  }
  SharedMemoryConfig config;
};

TEST_F(SharedMemoryTest, RoundTripPreservesFlagsPayloadAndPts) {
  SharedPacketSink sink(config);
  SharedPacketSource source(config);
  for (std::uint32_t seq = 0; seq < 40; ++seq) {
    ASSERT_TRUE(sink.push(packet(seq)));
    const auto p = source.pop(10ms);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->seq, seq);
    EXPECT_EQ(p->pts_us, 9876543 + seq);
    EXPECT_EQ(p->is_config, seq == 0);
    EXPECT_EQ(p->is_key_frame, seq == 1);
    EXPECT_EQ(p->data, packet(seq).data);
  }
}

TEST_F(SharedMemoryTest, NativeControlPreservesAllProtocolShapesAndDisconnectWithoutEviction) {
  config.slot_size = 32768;
  const int fd = open(config.device.c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  const auto bytes = 64 + config.slots * config.slot_size;
  ASSERT_EQ(ftruncate(fd, (bytes + 4095) / 4096 * 4096), 0);
  close(fd);
  SharedControlSink sink(config);
  SharedControlSource source(config);
  for (const char* name : {"hello", "bind", "bound", "state", "decision", "ack", "released"}) {
    std::ifstream input(std::string(K230_V2_FIXTURES) + "/" + name + ".json");
    const auto expected = companion::Json::parse(input);
    ASSERT_TRUE(sink.push(companion::Json(expected)));
    const auto actual = source.pop(1ms);
    ASSERT_TRUE(actual);
    EXPECT_EQ(*actual, expected);
  }
  for (std::uint32_t index = 0; index < config.slots; ++index)
    ASSERT_TRUE(sink.push(companion::Json()));
  EXPECT_FALSE(sink.push(companion::Json()));
  sink.close();
  for (std::uint32_t index = 0; index < config.slots; ++index) {
    const auto disconnected = source.pop(1ms);
    ASSERT_TRUE(disconnected);
    EXPECT_TRUE(disconnected->is_null());
  }
  EXPECT_TRUE(source.closed());
}

TEST_F(SharedMemoryTest, NativeControlRejectsInvalidJsonRatherThanSendingUnvalidatedRecords) {
  SharedControlSink sink(config);
  EXPECT_FALSE(sink.push(companion::Json{{"v", 1}, {"type", "decision"}}));
  EXPECT_FALSE(sink.push(companion::Json("invalid")));
}

TEST(SharedMemoryLayout, NativeControlsFitReservedMemoryWithoutOverlappingMediaOrVerdicts) {
  const auto media = SharedMemoryConfig{};
  const auto up = shared_control_up_config();
  const auto down = shared_control_down_config();
  const auto verdict = shared_verdict_config();
  const auto end = [](const auto& c) { return c.address + 64 + c.slots * c.slot_size; };
  EXPECT_LE(end(media), up.address);
  EXPECT_LE(end(up), down.address);
  EXPECT_LE(end(down), verdict.address);
  EXPECT_LE(end(verdict), 0x0d200000u);
  EXPECT_GT(up.slot_size - 4, 16391u);
}

TEST(ProtectionSource, RequiresPinnedScrcpyFourServer) {
  bridge::ScrcpyConfig config;
  config.server_jar = std::string(K230_SOURCE_DIR) + "/assets/scrcpy-server";
  EXPECT_TRUE(bridge::verified_protection_source(config));
  config.server_version = "3.3";
  EXPECT_FALSE(bridge::verified_protection_source(config));
  config.server_version = "4.0";
  config.server_jar = "/dev/null";
  EXPECT_FALSE(bridge::verified_protection_source(config));
}

TEST_F(SharedMemoryTest, FullRingDropsNewPacketWithoutOverwritingConfiguration) {
  SharedPacketSink sink(config);
  SharedPacketSource source(config);
  for (std::uint32_t i = 0; i < 4; ++i) ASSERT_TRUE(sink.push(packet(i)));
  EXPECT_FALSE(sink.push(packet(99)));
  for (std::uint32_t i = 0; i < 4; ++i) {
    const auto p = source.pop(0ms);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->seq, i);
  }
  EXPECT_FALSE(source.pop(1ms));
  EXPECT_FALSE(source.closed());
}

TEST_F(SharedMemoryTest, ClosedWriterDrainsBeforeEndOfStream) {
  SharedPacketSink sink(config);
  SharedPacketSource source(config);
  ASSERT_TRUE(sink.push(packet(1)));
  sink.close();
  EXPECT_FALSE(sink.push(packet(2)));
  ASSERT_TRUE(source.pop(1ms));
  EXPECT_TRUE(source.closed());
  EXPECT_FALSE(source.pop(1ms));
}

TEST_F(SharedMemoryTest, BackpressurePreservesCompressedPacketsWhenRingIsFull) {
  SharedPacketSink sink(config, 1s);
  SharedPacketSource source(config);
  for (std::uint32_t i = 0; i < config.slots; ++i) ASSERT_TRUE(sink.push(packet(i)));
  auto pending = std::async(std::launch::async, [&] { return sink.push(packet(config.slots)); });
  EXPECT_EQ(pending.wait_for(10ms), std::future_status::timeout);
  const auto first = source.pop(0ms);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->seq, 0u);
  ASSERT_TRUE(pending.get());
  for (std::uint32_t i = 1; i <= config.slots; ++i) {
    const auto next = source.pop(1ms);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->seq, i);
    EXPECT_EQ(next->data, packet(i).data);
  }
}

TEST_F(SharedMemoryTest, BackpressureRejectsStalledRingWithoutOverwritingPackets) {
  SharedPacketSink sink(config, 5ms);
  SharedPacketSource source(config);
  for (std::uint32_t i = 0; i < config.slots; ++i) ASSERT_TRUE(sink.push(packet(i)));
  EXPECT_FALSE(sink.push(packet(99)));
  for (std::uint32_t i = 0; i < config.slots; ++i) {
    const auto next = source.pop(0ms);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->seq, i);
  }
}

TEST_F(SharedMemoryTest, CloseInterruptsBackpressureWithoutWaitingForTimeout) {
  SharedPacketSink sink(config, 5s);
  for (std::uint32_t i = 0; i < config.slots; ++i) ASSERT_TRUE(sink.push(packet(i)));
  auto pending = std::async(std::launch::async, [&] { return sink.push(packet(99)); });
  EXPECT_EQ(pending.wait_for(10ms), std::future_status::timeout);
  sink.close();
  EXPECT_EQ(pending.wait_for(500ms), std::future_status::ready);
  EXPECT_FALSE(pending.get());
}

TEST_F(SharedMemoryTest, ReaderCanStartBeforeWriter) {
  SharedPacketSource source(config);
  EXPECT_FALSE(source.pop(1ms));
  EXPECT_FALSE(source.closed());
  SharedPacketSink sink(config);
  ASSERT_TRUE(sink.push(packet(5)));
  ASSERT_TRUE(source.pop(1ms));
}

TEST_F(SharedMemoryTest, ActiveEndpointsCannotBeReinitializedOrDuplicated) {
  SharedPacketSink sink(config);
  EXPECT_THROW(SharedPacketSink duplicate(config), std::runtime_error);
  SharedPacketSource source(config);
  ASSERT_TRUE(sink.push(packet(1)));
  ASSERT_TRUE(source.pop(1ms));
  SharedPacketSource duplicate(config);
  EXPECT_THROW(duplicate.pop(0ms), std::runtime_error);
  sink.close();
  EXPECT_THROW(SharedPacketSink replacement(config), std::runtime_error);
}

TEST_F(SharedMemoryTest, OversizedPacketIsRejected) {
  SharedPacketSink sink(config);
  auto p = packet(1);
  p.data.resize(config.slot_size);
  EXPECT_FALSE(sink.push(std::move(p)));
}

TEST_F(SharedMemoryTest, RestartAfterReaderExitRejectsOldWriter) {
  SharedPacketSink old_writer(config);
  {
    SharedPacketSource reader(config);
    ASSERT_TRUE(old_writer.push(packet(1)));
    ASSERT_TRUE(reader.pop(1ms));
  }
  SharedPacketSink new_writer(config);
  EXPECT_FALSE(old_writer.push(packet(2)));
  old_writer.close();
  SharedPacketSource reader(config);
  ASSERT_TRUE(new_writer.push(packet(3)));
  const auto p = reader.pop(1ms);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->seq, 3u);
}

TEST_F(SharedMemoryTest, ReaderRestartWaitsForNewWriterRatherThanAttachingOldEpoch) {
  {
    SharedPacketSink writer(config);
    SharedPacketSource reader(config);
    ASSERT_TRUE(writer.push(packet(1)));
    ASSERT_TRUE(reader.pop(1ms));
    writer.close();
  }
  SharedPacketSource reader(config);
  EXPECT_FALSE(reader.pop(1ms));
  EXPECT_FALSE(reader.closed());
  SharedPacketSink writer(config);
  ASSERT_TRUE(writer.push(packet(2)));
  auto value = reader.pop(1ms);
  ASSERT_TRUE(value);
  EXPECT_EQ(value->seq, 2u);
}

TEST_F(SharedMemoryTest, VerdictRoundTripAndInvalidConfidence) {
  SharedVerdictSink sink(config);
  SharedVerdictSource source(config);
  Verdict v;
  v.sequence = 77; v.pts_us = 987654; v.action = Action::Warn;
  v.category = Category::Nudity; v.confidence = 0.75f;
  ASSERT_TRUE(sink.push(Verdict(v)));
  const auto result = source.pop(1ms);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->sequence, v.sequence);
  EXPECT_EQ(result->pts_us, v.pts_us);
  EXPECT_EQ(result->action, v.action);
  EXPECT_FLOAT_EQ(result->confidence, v.confidence);
  v.confidence = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(sink.push(std::move(v)));
}

TEST_F(SharedMemoryTest, LayoutMismatchAndInvalidBackingFailExplicitly) {
  SharedPacketSink sink(config);
  auto wrong = config; wrong.slots = 3;
  SharedPacketSource source(wrong);
  EXPECT_THROW(source.pop(0ms), std::runtime_error);
  wrong.slots = 0;
  EXPECT_THROW(SharedPacketSource invalid(wrong), std::runtime_error);
  wrong = config; wrong.address = 4096;
  EXPECT_THROW(SharedPacketSource invalid(wrong), std::runtime_error);
}
}  // namespace
