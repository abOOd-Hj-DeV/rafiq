#include "k230/bridge/force_stop_executor.hpp"

#include <chrono>
#include <regex>
#include <thread>

namespace k230::bridge {

std::optional<std::string> ForceStopExecutor::foreground_package(const std::string& output) {
  static const std::regex resumed(
      R"((?:mResumedActivity|topResumedActivity|mCurrentFocus)[^\n]*[ /}]([A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)+)/)");
  std::smatch match;
  if (!std::regex_search(output, match, resumed)) return std::nullopt;
  return match[1].str();
}

bool ForceStopExecutor::safe_target(const std::string& package) {
  static const std::regex valid(R"([A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)+)");
  if (!std::regex_match(package, valid)) return false;
  for (const char* protected_package : {
           "android", "com.android.systemui", "com.android.settings",
           "dev.k230.mentor_app", "com.genymobile.scrcpy", "com.bbk.launcher2",
           "com.google.android.apps.nexuslauncher", "com.android.phone",
           "com.android.dialer", "com.android.permissioncontroller",
           "com.google.android.permissioncontroller", "com.vivo.launcher"}) {
    if (package == protected_package) return false;
  }
  return package.rfind("com.android.launcher", 0) != 0;
}

ForceStopResult ForceStopExecutor::execute(const std::string& expected_package) {
  ForceStopResult result;
  const auto before = shell_({"dumpsys", "activity", "activities"}, 5000);
  const auto package = before.ok() ? foreground_package(before.output) : std::nullopt;
  if (!package || !safe_target(*package)) {
    result.error = package ? "protected_package" : "foreground_unavailable";
    return result;
  }
  result.package = *package;
  if (*package != expected_package) {
    result.error = "foreground_changed";
    return result;
  }
  const auto user = shell_({"am", "get-current-user"}, 5000);
  std::smatch user_match;
  if (!user.ok() || !std::regex_match(user.output, user_match, std::regex(R"(\s*([0-9]+)\s*)"))) {
    result.error = "current_user_unavailable";
    return result;
  }
  const auto stopped_user = std::regex("User " + user_match[1].str() + R"(:[^\n]*\bstopped=true\b)");
  if (!shell_({"am", "force-stop", "--user", "current", *package}, 5000).ok()) {
    result.error = "force_stop_failed";
    return result;
  }
  for (int attempt = 0; attempt < 5; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto after = shell_({"dumpsys", "activity", "activities"}, 5000);
    const auto foreground = after.ok() ? foreground_package(after.output) : std::nullopt;
    const auto state = shell_({"dumpsys", "package", *package}, 5000);
    if (foreground && *foreground != *package && state.ok() &&
        std::regex_search(state.output, stopped_user)) {
      result.stopped = true;
      break;
    }
  }
  if (!result.stopped) {
    result.error = "force_stop_unconfirmed";
    return result;
  }
  const auto intervention = shell_(
      {"am", "start", "-W", "-n", "dev.k230.mentor_app/.MvpBreakActivity",
       "--es", "reason", "content", "--es", "blocked_package", *package,
       "--ez", "force_stop_verified", "true"}, 10000);
  result.intervention_started = intervention.ok() &&
      intervention.output.find("Status: ok") != std::string::npos;
  if (!result.intervention_started) result.error = "intervention_failed";
  return result;
}

}  // namespace k230::bridge
