#include <iostream>
#include <string>

#include "k230/bridge/adb_controller.hpp"
#include "k230/bridge/force_stop_executor.hpp"

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: k230-mvp-force-stop-probe <adb-path> <serial> <expected-package>\n";
    return 2;
  }
  k230::bridge::AdbController adb(argv[1]);
  const std::string serial = argv[2];
  k230::bridge::ForceStopExecutor executor(
      [&adb, &serial](const auto& args, int timeout) { return adb.shell(serial, args, timeout); });
  const auto result = executor.execute(argv[3]);
  std::cout << "package=" << result.package << " stopped=" << result.stopped
            << " intervention_started=" << result.intervention_started
            << " error=" << result.error << '\n';
  return result.stopped && result.intervention_started ? 0 : 1;
}
