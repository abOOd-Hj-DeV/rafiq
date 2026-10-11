#pragma once

#include <optional>
#include <functional>
#include <string>

#include "k230/bridge/adb_controller.hpp"
#include "k230/layout.hpp"

namespace k230::bridge {

class FrameAttribution {
 public:
  std::optional<std::string> observe(const std::optional<LayoutSnapshot>& layout,
                                     std::int64_t pts, std::int64_t latest) {
    const bool foreground_only = layout &&
        layout->flags == (kLayoutForegroundOnly | kLayoutPartial) &&
        layout->nodes.empty() && layout->window_id >= 0;
    if (!layout || layout->package.empty() || (layout->flags & kLayoutInvalidate) ||
        ((layout->flags & kLayoutPartial) && !foreground_only) ||
        latest < pts || latest - pts > 5'000'000) {
      package_.clear();
      confirmations_ = 0;
      return std::nullopt;
    }
    if (pts <= last_pts_) return std::nullopt;
    last_pts_ = pts;
    if (package_ != layout->package || session_ != layout->session ||
        window_id_ != layout->window_id) {
      package_ = layout->package;
      session_ = layout->session;
      window_id_ = layout->window_id;
      confirmations_ = 0;
    }
    if (confirmations_ < 3) ++confirmations_;
    return confirmations_ >= 3 ? std::optional<std::string>(package_) : std::nullopt;
  }
 private:
  std::string package_;
  std::int64_t last_pts_ = -1;
  std::uint64_t session_ = 0;
  std::int32_t window_id_ = -1;
  int confirmations_ = 0;
};

struct ForceStopResult {
  bool stopped = false;
  bool intervention_started = false;
  std::string package;
  std::string error;
};

class ForceStopExecutor {
 public:
  using Shell = std::function<CommandResult(const std::vector<std::string>&, int)>;
  ForceStopExecutor(AdbController adb, std::string serial)
      : shell_([adb = std::move(adb), serial = std::move(serial)](const auto& args, int timeout) {
          return adb.shell(serial, args, timeout);
        }) {}
  explicit ForceStopExecutor(Shell shell) : shell_(std::move(shell)) {}

  ForceStopResult execute(const std::string& expected_package);
  static std::optional<std::string> foreground_package(const std::string& dumpsys);
  static bool safe_target(const std::string& package);

 private:
  Shell shell_;
};

}  // namespace k230::bridge
