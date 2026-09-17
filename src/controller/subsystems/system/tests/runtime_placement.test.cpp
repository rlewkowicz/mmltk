#include "src/common/system/tests/numa_topology_test_support.h"
#include <algorithm>
#include "src/controller/subsystems/validate/validation_runtime.h"
#include "src/controller/subsystems/system/predict_system.h"
#include "src/controller/subsystems/export/export_system.h"
#include <sched.h>
#include "src/controller/subsystems/system/tests/application_data_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/controller/runtime/local_run.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/common/system/numa_topology.h"
#include "src/common/system/execution_policy.h"
#include "src/common/system/tests/denied_syscall.h"
#include "src/frameworks/gpu/device_execution.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <condition_variable>
#include <cstdint>
#include <cuda.h>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <sys/syscall.h>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
using namespace mmltk::controller::test_support;
namespace mmltk::controller {
namespace {
TEST_CASE("compute runtime admission preserves valid progress and reports malformed or failed work", "[controller][systems][compute]") {
    const auto scenario = GENERATE(0, 1, 2, 3);
    std::vector<std::uint64_t> delivered;
    const auto terminal = run_checked_compute(
        [&](const ComputeProgressSink& progress) {
            std::jthread reporter([&] {
                progress({1U, 1U, 2U, "first"});
                if (scenario == 1) progress({2U, 3U, 2U, "invalid"});
                progress({3U, 2U, 2U, "last"});
            });
            reporter.join();
            if (scenario == 3) throw std::runtime_error("runtime failure detail");
            return contracts::make_compute_terminal(scenario == 2 ? contracts::ComputeOperationOutcome::Running : contracts::ComputeOperationOutcome::Succeeded,
                                                    0U, 2U);
        },
        [&](const contracts::ComputeProgress& progress) { delivered.push_back(progress.sequence); });
    CHECK(delivered == std::vector<std::uint64_t>{1U, 3U});
    CHECK(terminal.valid_worker_terminal());
    if (scenario == 0) {
        CHECK(terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
        CHECK(terminal.completed == 2U);
    } else {
        CHECK(terminal.outcome == contracts::ComputeOperationOutcome::Failed);
        CHECK(terminal.detail == (scenario == 3 ? "runtime failure detail" : "compute runtime returned an invalid terminal"));
    }
}
TEST_CASE("local run linearizes Stop with admission and installed worker", "[controller][systems][run]") {
    direct::LocalRun run;
    std::promise<void> preparing;
    std::promise<void> release_prepare;
    std::promise<bool> observed_stop;
    std::condition_variable_any stop_condition;
    std::mutex stop_mutex;
    auto release = release_prepare.get_future().share();
    auto launch = std::async(std::launch::async, [&] {
        run.Start({
            .prepare =
                [&] {
                    preparing.set_value();
                    release.wait();
                },
            .work = [&](const std::stop_token stop) -> direct::LocalRun::Notification {
                std::unique_lock lock(stop_mutex);
                const bool completed = stop_condition.wait(lock, stop, [] { return false; });
                observed_stop.set_value(!completed && stop.stop_requested());
                return {};
            },
        });
    });
    preparing.get_future().wait();
    auto admitted = run.CurrentStopSource();
    REQUIRE(admitted.stop_possible());
    CHECK(run.Stop());
    release_prepare.set_value();
    launch.get();
    CHECK(observed_stop.get_future().get());
    run.StopAndJoin();
    auto second_gate = std::make_shared<mmltk::testsupport::StopGate>();
    std::promise<void> second_started;
    std::promise<bool> second_cancelled;
    run.Start({
        .work = [&](const std::stop_token stop) -> direct::LocalRun::Notification {
            second_started.set_value();
            static_cast<void>(second_gate->Wait(stop));
            second_cancelled.set_value(stop.stop_requested());
            return {};
        },
    });
    second_started.get_future().wait();
    CHECK_FALSE(admitted.request_stop());
    CHECK_FALSE(run.CurrentStopSource().stop_requested());
    second_gate->Release();
    CHECK_FALSE(second_cancelled.get_future().get());
}
TEST_CASE("local run preserves admission after prepare throws", "[controller][systems][run]") {
    direct::LocalRun run;
    direct::LocalRun::Job rejected{
        .prepare = [] { throw contracts::FailedError("admission failed"); },
        .work = [](std::stop_token) -> direct::LocalRun::Notification { return {}; },
    };
    CHECK_THROWS_AS(run.Start(std::move(rejected)), contracts::FailedError);
    CHECK_FALSE(run.active());
    CHECK_FALSE(run.CurrentStopSource().stop_possible());
    std::promise<void> completed;
    run.Start({
        .work = [&](std::stop_token) -> direct::LocalRun::Notification {
            completed.set_value();
            return {};
        },
    });
    completed.get_future().wait();
}
TEST_CASE("checked operation identity advancement refuses exhaustion", "[controller][systems][identity]") {
    CHECK_FALSE(contracts::next_compute_generation(std::numeric_limits<std::uint64_t>::max()));
    CHECK(contracts::next_compute_generation(0U) == 1U);
}
TEST_CASE("Local compute admission waits for required worker placement", "[controller][compute][placement]") {
    using namespace mmltk::common::system;
    using namespace mmltk::controller;
    // CLEANUP-IGNORE: Worker-admission evidence must capture its own caller placement; the topology selector already owns lookup.
    const auto topology = NumaTopology::Capture();
    const auto selected = mmltk::common::system::test_support::first_permitted_cpu(topology);
    const auto cpu = selected.cpu;
    const auto node = selected.node;
    const auto before = capture_execution_policy_snapshot();
    direct::LocalRun run;
    std::optional<ExecutionPolicySnapshot> observed;
    run.Start({
        .policy = ExecutionPolicyRequest{{cpu}, {}, 0, node, -10, false},
        .work = [&](std::stop_token) -> direct::LocalRun::Notification {
            observed = capture_execution_policy_snapshot();
            return {};
        },
    });
    run.StopAndJoin();
    REQUIRE(observed);
    CHECK(observed->affinity == std::vector<int>{cpu});
    CHECK(observed->numa_node == node);
    CHECK(observed->nice_value <= -10);
    CHECK(observed->scheduler_policy == SCHED_OTHER);
    CHECK(capture_execution_policy_snapshot().affinity == before.affinity);
}
TEST_CASE("Compute policy denial precedes admission and CUDA construction", "[controller][compute][placement]") {
    using namespace mmltk::common::system;
    using namespace mmltk::controller;
    for (const auto call : {SYS_setpriority, SYS_set_mempolicy}) {
        CHECK(mmltk::common::system::test_support::with_denied_syscall(call, [] {
                  const auto topology = NumaTopology::Capture();
                  const auto selected = mmltk::common::system::test_support::first_permitted_cpu(topology);
                  const auto cpu = selected.cpu;
                  const auto node = selected.node;
                  direct::LocalRun run;
                  bool admitted = false;
                  try {
                      run.Start({
                          .policy = ExecutionPolicyRequest{{cpu}, {}, 0, node, -10, false},
                          .prepare = [&] { admitted = true; },
                          .work = [](std::stop_token) -> direct::LocalRun::Notification { return {}; },
                      });
                  } catch (const std::system_error& error) {
                      if (admitted || run.active() || error.code().value() != EPERM) return false;
                      const DirectComputeConfiguration configuration{
                          .execution = mmltk::frameworks::gpu::DeviceExecution{.device = 999999, .placement = {.numa_node = node, .cpus = {cpu}}}};
                      for (const auto& construct : std::array<std::function<void()>, 3>{[&] { CudaValidationRuntime runtime(configuration); },
                                                                                        [&] { CudaExportRuntime runtime(configuration); },
                                                                                        [&] { CudaPredictRuntime runtime(configuration); }}) {
                          try {
                              construct();
                              return false;
                          } catch (const std::system_error& denied) {
                              if (denied.code().value() != EPERM) return false;
                          }
                      }
                      return true;
                  }
                  return false;
              }) == 0);
    }
}
}  // namespace
}  // namespace mmltk::controller
