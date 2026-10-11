#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "k230/ipc/channel.hpp"

namespace k230::ipc {

// Linux/Linux AMP transport. Both mappings must be uncached reserved memory.
struct SharedMemoryConfig {
  std::string device = "/dev/mem";
  std::uint64_t address = 0x0c000000;
  std::uint32_t slots = 64;
  std::uint32_t slot_size = 256 * 1024;
};

inline SharedMemoryConfig shared_verdict_config() {
  return {"/dev/mem", 0x0d100000, 32, 256};
}

inline SharedMemoryConfig shared_control_up_config() {
  return {"/dev/mem", 0x0d010000, 8, 32768};
}

inline SharedMemoryConfig shared_control_down_config() {
  return {"/dev/mem", 0x0d060000, 8, 32768};
}

class SharedControlSink final : public ControlSink {
 public:
  explicit SharedControlSink(const SharedMemoryConfig& config);
  ~SharedControlSink() override;
  bool push(companion::Json&& message) override;
  void close() override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class SharedControlSource final : public ControlSource {
 public:
  explicit SharedControlSource(const SharedMemoryConfig& config);
  ~SharedControlSource() override;
  std::optional<companion::Json> pop(std::chrono::milliseconds timeout) override;
  bool closed() const override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class SharedPacketSink final : public PacketSink {
 public:
  explicit SharedPacketSink(const SharedMemoryConfig& config = {},
                            std::chrono::milliseconds push_timeout = std::chrono::milliseconds{0});
  ~SharedPacketSink() override;
  bool push(MediaPacket&& packet) override;
  void close() override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class SharedPacketSource final : public PacketSource {
 public:
  explicit SharedPacketSource(const SharedMemoryConfig& config = {});
  ~SharedPacketSource() override;
  std::optional<MediaPacket> pop(std::chrono::milliseconds timeout) override;
  bool closed() const override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class SharedVerdictSink final : public VerdictSink {
 public:
  explicit SharedVerdictSink(const SharedMemoryConfig& config = shared_verdict_config());
  ~SharedVerdictSink() override;
  bool push(Verdict&& verdict) override;
  void close() override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class SharedVerdictSource final : public VerdictSource {
 public:
  explicit SharedVerdictSource(const SharedMemoryConfig& config = shared_verdict_config());
  ~SharedVerdictSource() override;
  std::optional<Verdict> pop(std::chrono::milliseconds timeout) override;
  bool closed() const override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace k230::ipc
