#pragma once

#include <string>
#include <sys/stat.h>

#include "k230/bridge/scrcpy_session.hpp"
#include "k230/bridge/subprocess.hpp"

namespace k230::bridge {

inline bool verified_protection_source(const ScrcpyConfig& config) {
  struct stat info{};
  if (config.server_version != "4.0" || ::stat(config.server_jar.c_str(), &info) != 0 ||
      !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 1048576)
    return false;
  const auto result = run_command({"sha256sum", config.server_jar}, 5000);
  constexpr const char* expected = "84924bd564a1eb6089c872c7521f968058977f91f5ff02514a8c74aff3210f3a";
  return result.ok() && result.output.compare(0, 64, expected) == 0 &&
         result.output.size() > 64 && (result.output[64] == ' ' || result.output[64] == '\t');
}

}  // namespace k230::bridge
