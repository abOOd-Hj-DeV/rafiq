#include "k230/bridge/force_stop_executor.hpp"

#include <gtest/gtest.h>

using k230::bridge::ForceStopExecutor;

TEST(ForceStopExecutor, ExtractsResumedApplication) {
  const auto package = ForceStopExecutor::foreground_package(
      "  mResumedActivity: ActivityRecord{123 u0 com.example.video/.Player t42}\n");
  ASSERT_TRUE(package);
  EXPECT_EQ(*package, "com.example.video");
}

TEST(ForceStopExecutor, RejectsMalformedAndProtectedPackages) {
  EXPECT_FALSE(ForceStopExecutor::foreground_package("mResumedActivity: none"));
  EXPECT_FALSE(ForceStopExecutor::safe_target("com.example;rm"));
  EXPECT_FALSE(ForceStopExecutor::safe_target("com.android.systemui"));
  EXPECT_FALSE(ForceStopExecutor::safe_target("dev.k230.mentor_app"));
  EXPECT_TRUE(ForceStopExecutor::safe_target("com.example.video"));
  EXPECT_FALSE(ForceStopExecutor::safe_target("com.bbk.launcher2"));
}

namespace {
struct FakePhone {
  std::string foreground = "com.example.video";
  bool stopped = false;
  bool fail_stop = false;
  bool confirm_stop = true;
  bool fail_intervention = false;
  int stop_calls = 0, intervention_calls = 0;
  k230::bridge::CommandResult shell(const std::vector<std::string>& args, int) {
    if (args[0] == "dumpsys" && args[1] == "activity")
      return {0, "mResumedActivity: ActivityRecord{123 u0 " + foreground + "/.Main}"};
    if (args[0] == "dumpsys" && args[1] == "package")
      return {0, std::string("User 0: installed=true stopped=") + (stopped ? "true" : "false")};
    if (args[1] == "get-current-user") return {0, "0\n"};
    if (args[1] == "force-stop") {
      ++stop_calls;
      EXPECT_EQ(args.back(), "com.example.video");
      if (fail_stop) return {1, "Error"};
      stopped = confirm_stop;
      foreground = "com.android.launcher3";
      return {0, ""};
    }
    ++intervention_calls;
    return {0, fail_intervention ? "Error: Activity not found" : "Status: ok"};
  }
  ForceStopExecutor executor() {
    return ForceStopExecutor([this](const auto& args, int timeout) { return shell(args, timeout); });
  }
};
}

TEST(ForceStopExecutor, ChangedForegroundRejectsWithoutStoppingAnyApp) {
  FakePhone phone;
  phone.foreground = "com.example.other";
  const auto result = phone.executor().execute("com.example.video");
  EXPECT_EQ(result.error, "foreground_changed");
  EXPECT_EQ(phone.stop_calls, 0);
  EXPECT_EQ(phone.intervention_calls, 0);
}

TEST(ForceStopExecutor, FailedCommandNeverClaimsExecutionOrOpensIntervention) {
  FakePhone phone;
  phone.fail_stop = true;
  const auto result = phone.executor().execute("com.example.video");
  EXPECT_FALSE(result.stopped);
  EXPECT_EQ(result.error, "force_stop_failed");
  EXPECT_EQ(phone.intervention_calls, 0);
}

TEST(ForceStopExecutor, ChangingFocusAloneDoesNotProveForceStop) {
  FakePhone phone;
  phone.confirm_stop = false;
  const auto result = phone.executor().execute("com.example.video");
  EXPECT_FALSE(result.stopped);
  EXPECT_EQ(result.error, "force_stop_unconfirmed");
  EXPECT_EQ(phone.intervention_calls, 0);
}

TEST(ForceStopExecutor, ConfirmedStopRequestsIntervention) {
  FakePhone phone;
  const auto result = phone.executor().execute("com.example.video");
  EXPECT_TRUE(result.stopped);
  EXPECT_TRUE(result.intervention_started);
  EXPECT_EQ(phone.stop_calls, 1);
  EXPECT_EQ(phone.intervention_calls, 1);
}

TEST(ForceStopExecutor, ActivityErrorWithZeroExitIsNotSuccessfulIntervention) {
  FakePhone phone;
  phone.fail_intervention = true;
  const auto result = phone.executor().execute("com.example.video");
  EXPECT_TRUE(result.stopped);
  EXPECT_FALSE(result.intervention_started);
  EXPECT_EQ(result.error, "intervention_failed");
}

TEST(FrameAttribution, RequiresDistinctFreshFramesFromTheSameApplication) {
  k230::bridge::FrameAttribution attribution;
  k230::LayoutSnapshot layout;
  layout.package = "com.example.video";
  EXPECT_FALSE(attribution.observe(layout, 100, 100));
  EXPECT_FALSE(attribution.observe(layout, 100, 100));
  EXPECT_FALSE(attribution.observe(layout, 200, 200));
  EXPECT_EQ(attribution.observe(layout, 300, 300), layout.package);
  layout.package = "com.example.other";
  EXPECT_FALSE(attribution.observe(layout, 400, 400));
  EXPECT_FALSE(attribution.observe(layout, 500, 500));
  EXPECT_EQ(attribution.observe(layout, 600, 600), layout.package);
}

TEST(FrameAttribution, StalePartialMissingAndFutureSamplesInvalidateConfirmation) {
  k230::bridge::FrameAttribution attribution;
  k230::LayoutSnapshot layout;
  layout.package = "com.example.video";
  attribution.observe(layout, 100, 100);
  attribution.observe(layout, 200, 200);
  EXPECT_FALSE(attribution.observe(layout, 300, 5'000'301));
  EXPECT_FALSE(attribution.observe(layout, 400, 400));
  EXPECT_FALSE(attribution.observe(std::nullopt, 500, 500));
  layout.flags = k230::kLayoutPartial;
  EXPECT_FALSE(attribution.observe(layout, 600, 600));
  layout.flags = 0;
  EXPECT_FALSE(attribution.observe(layout, 700, 699));
}

TEST(FrameAttribution, ExplicitForegroundOnlyConfirmsPackageWithoutClaimingCompleteGeometry) {
  k230::bridge::FrameAttribution attribution;
  k230::LayoutSnapshot layout;
  layout.flags = k230::kLayoutPartial | k230::kLayoutForegroundOnly;
  layout.session = 7;
  layout.window_id = 42;
  layout.package = "com.example.video";
  EXPECT_FALSE(attribution.observe(layout, 100, 100));
  EXPECT_FALSE(attribution.observe(layout, 200, 200));
  EXPECT_EQ(attribution.observe(layout, 300, 300), layout.package);
  ++layout.window_id;
  EXPECT_FALSE(attribution.observe(layout, 400, 400));
  EXPECT_FALSE(attribution.observe(layout, 500, 500));
  EXPECT_EQ(attribution.observe(layout, 600, 600), layout.package);
  ++layout.session;
  EXPECT_FALSE(attribution.observe(layout, 700, 700));
  layout.flags |= k230::kLayoutInvalidate;
  EXPECT_FALSE(attribution.observe(layout, 800, 800));
}

TEST(FrameAttribution, MalformedForegroundOnlyCannotAuthorizeForceStop) {
  for (const int invalid : {0, 1, 2}) {
    k230::bridge::FrameAttribution attribution;
    k230::LayoutSnapshot layout;
    layout.flags = k230::kLayoutPartial | k230::kLayoutForegroundOnly;
    layout.window_id = 42;
    layout.package = "com.example.video";
    if (invalid == 0) layout.package.clear();
    if (invalid == 1) layout.window_id = -1;
    if (invalid == 2) layout.nodes.push_back({0, 0, 1, 1, 1, 1});
    EXPECT_FALSE(attribution.observe(layout, 100, 100));
    EXPECT_FALSE(attribution.observe(layout, 200, 200));
    EXPECT_FALSE(attribution.observe(layout, 300, 300));
  }
}
