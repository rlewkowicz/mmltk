#include "src/common/system/tests/numa_topology_test_support.h"
#include <algorithm>
#include <chrono>
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/controller/subsystems/validate/validation_runtime.h"
#include "src/controller/subsystems/system/predict_system.h"
#include "src/controller/subsystems/export/export_system.h"
#include <sched.h>
#include "src/controller/subsystems/system/tests/application_data_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/controller/runtime/local_run.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/controller/subsystems/system/detail/cuda_runtime_resources.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/frameworks/gpu/cuda_context_scope.h"
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
#include <type_traits>
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
   return contracts::make_compute_terminal(scenario == 2 ? contracts::ComputeOperationOutcome::Running : contracts::ComputeOperationOutcome::Succeeded, 0U, 2U);
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
    const DirectComputeConfiguration configuration{.execution = mmltk::frameworks::gpu::DeviceExecution{.device = 999999, .placement = {.numa_node = node, .cpus = {cpu}}}};
    for (const auto& construct : std::array<std::function<void()>, 3>{
          [&] { CudaValidationRuntime runtime(configuration); }, [&] { CudaExportRuntime runtime(configuration); }, [&] { CudaPredictRuntime runtime(configuration); }}) {
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

namespace mmltk::controller {
namespace {
TEST_CASE("direct CUDA resources preserve the exact session and stream on unproved settlement", "[controller][compute][custody]") {
 using Resources = detail::CudaRuntimeResources;
 STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Resources>);
 STATIC_REQUIRE_FALSE(std::is_move_constructible_v<Resources>);
 struct Probe {
  int current = 9;
  unsigned created = 0, destroyed = 0, closed = 0, sessions_destroyed = 0, synchronized = 0;
  bool restore_failure = false, construction_restore_failure = false, sync_failure = false, close_failure = false, destroy_failure = false;
 };
 struct Session {
  std::shared_ptr<Probe> probe;
  ~Session() { ++probe->sessions_destroyed; }
 };
 const auto failure = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
 const auto cpu = mmltk::common::system::test_support::first_permitted_cpu(mmltk::common::system::NumaTopology::Capture());
 const DirectComputeConfiguration config{.execution = mmltk::frameworks::gpu::DeviceExecution{.device = 3, .placement = {.numa_node = cpu.node, .cpus = {cpu.cpu}}}};
 auto probe = std::make_shared<Probe>();
 auto session = std::make_shared<Session>(probe);
 std::weak_ptr<Session> physical = session;
 const Resources::Operations operations{
  .device = {probe.get(), [](void* value, int* device) noexcept { *device = static_cast<Probe*>(value)->current; return cudaSuccess; },
   [](void* value, int device) noexcept {
    auto& state = *static_cast<Probe*>(value);
    if (device == 9 && state.restore_failure) return cudaErrorUnknown;
    state.current = device;
    return cudaSuccess;
   }},
  .context = probe.get(),
  .create = [](void* value, cudaStream_t* stream, unsigned) {
   auto& state = *static_cast<Probe*>(value);
   ++state.created;
   *stream = reinterpret_cast<cudaStream_t>(value);
   if (state.construction_restore_failure) state.restore_failure = true;
   return cudaSuccess;
  },
  .synchronize = [](void* value, cudaStream_t) { auto& state = *static_cast<Probe*>(value); ++state.synchronized; return state.sync_failure ? cudaErrorUnknown : cudaSuccess; },
  .destroy = [](void* value, cudaStream_t) {
   auto& state = *static_cast<Probe*>(value);
   if (state.destroy_failure) return cudaErrorUnknown;
   ++state.destroyed;
   return cudaSuccess;
  },
 };
 auto close = [session = std::move(session)] {
  ++session->probe->closed;
  if (session->probe->close_failure) throw std::runtime_error("injected session close failure");
 };
 std::unique_ptr<Resources> resources;
 if (failure == 1) {
  probe->construction_restore_failure = true;
  CHECK_THROWS(resources = std::make_unique<Resources>(config, std::move(close), operations));
 } else {
  resources = std::make_unique<Resources>(config, std::move(close), operations);
  probe->sync_failure = failure == 2 || failure == 4;
  probe->close_failure = failure == 3;
  probe->restore_failure = failure == 5 || failure == 6;
  probe->destroy_failure = failure == 7;
  if (failure == 8) {
   CHECK_THROWS(resources->Run([](auto) -> contracts::ComputeTerminal { throw mmltk::frameworks::gpu::CudaContextFailure(true); }, {}));
   CHECK(probe->synchronized == 0U);
  } else if (failure == 2 || failure == 5) {
   CHECK_THROWS(resources->Run([](auto) { return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded); }, {}));
  } else if (failure == 3 || failure == 6) {
   CHECK_THROWS(resources->CloseSession());
  } else if (failure == 0) {
   CHECK_THROWS(resources->Run([](auto) -> contracts::ComputeTerminal { throw std::invalid_argument("ordinary work refusal"); }, {}));
   CHECK_FALSE(resources->HasUnsafeCustody());
   CHECK(resources->Run([](auto) { return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded); }, {}).outcome == contracts::ComputeOperationOutcome::Succeeded);
   std::stop_source stopped;
   stopped.request_stop();
   CHECK(resources->Run([](auto) -> contracts::ComputeTerminal { FAIL("cancelled work ran"); return {}; }, stopped.get_token()).outcome == contracts::ComputeOperationOutcome::Cancelled);
  }
  if (failure != 0 && failure != 4 && failure != 7) {
   CHECK(resources->HasUnsafeCustody());
   CHECK_THROWS_AS(resources->CloseSession(), contracts::UnavailableError);
   CHECK_THROWS_AS(resources->Run([](auto) { return contracts::ComputeTerminal{}; }, {}), contracts::UnavailableError);
  }
 }
 resources.reset();
 CHECK(probe->created == 1U);
 CHECK(probe->destroyed == (failure == 0 ? 1U : 0U));
 CHECK(probe->sessions_destroyed == (failure == 0 ? 1U : 0U));
 CHECK(physical.expired() == (failure == 0));
}
class SelectedWorkflowRuntime final : public ValidationRuntime, public ExportRuntime, public PredictRuntime {
public:
 SelectedWorkflowRuntime(DirectComputeConfiguration configuration, std::vector<int>& runs, unsigned& closes, std::function<void()> before)
     : configuration_(std::move(configuration)), runs_(runs), closes_(closes), before_(std::move(before)) {}
 void Close() noexcept override { ++closes_; }
 ValidationRuntimeResult Run(mmltk::backend::models::rfdetr::ValidateRequest request, std::stop_token, const ComputeProgressSink&,
  const mmltk::backend::models::rfdetr::ValidationDelivery&, std::uint64_t generation) override {
  CHECK(request.compile_cuda_device_id == request.device_id);
  return {.terminal = Record(request.device_id, generation)};
 }
 contracts::ComputeTerminal Run(ExportRunRequest request, std::stop_token, const ComputeProgressSink&, const ComputeArtifactSink&, std::uint64_t generation) override { return Record(request.onnx.device_id, generation); }
 contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::PredictRequest request, std::stop_token, const ComputeProgressSink&, const ProductSink&, const PlaybackGate&, VisualExtent,
  const ContextProvider&, const PreviewRetirement&, const ComputeArtifactSink&, const PredictionRunOutput&, std::uint64_t generation) override { return Record(request.device_id, generation); }
private:
 contracts::ComputeTerminal Record(int device, std::uint64_t generation) {
  CHECK(generation == runs_.size() + 1U);
  before_();
  CHECK(configuration_.execution->device == device);
  runs_.push_back(device);
  return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded);
 }
 DirectComputeConfiguration configuration_;
 std::vector<int>& runs_;
 unsigned& closes_;
 std::function<void()> before_;
};
TEST_CASE("Validate and Export seal replacement retry and shutdown after unsafe runtime custody", "[controller][compute][custody]") {
 struct UnsafeRuntime final : ValidationRuntime, ExportRuntime {
  std::shared_ptr<unsigned> destroyed;
  bool fail_run, unsafe = false;
  UnsafeRuntime(std::shared_ptr<unsigned> count, bool fail) : destroyed(std::move(count)), fail_run(fail) {}
  ~UnsafeRuntime() override { ++*destroyed; }
  bool HasUnsafeCustody() const noexcept override { return unsafe; }
  void Close() override { unsafe = true; throw std::runtime_error("injected unproved close"); }
  contracts::ComputeTerminal Result() {
   if (fail_run) { unsafe = true; throw std::runtime_error("injected unproved run"); }
   return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded);
  }
  ValidationRuntimeResult Run(mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token, const ComputeProgressSink&,
   const mmltk::backend::models::rfdetr::ValidationDelivery&, std::uint64_t) override { return {.terminal = Result()}; }
  contracts::ComputeTerminal Run(ExportRunRequest, std::stop_token, const ComputeProgressSink&, const ComputeArtifactSink&, std::uint64_t) override { return Result(); }
 };
 const auto feature = GENERATE(contracts::FeatureId::Validate, contracts::FeatureId::Export);
 const auto failure = GENERATE(0, 1, 2, 3);
 const bool fail_run = failure == 1;
 mmltk::testsupport::ScopedTempDir root{"unsafe-selected-execution"};
 ApplicationDataFixture fixture(root.path());
 fixture.PrepareModel(feature);
 auto [settings, dataset, model] = fixture.systems();
 const auto cpu = mmltk::common::system::test_support::first_permitted_cpu(mmltk::common::system::NumaTopology::Capture());
 const DirectComputeResolver resolver = [&](int device, int numa) {
  return DirectComputeConfiguration{.execution = mmltk::frameworks::gpu::DeviceExecution{.device = device, .placement = {.numa_node = cpu.node, .cpus = {cpu.cpu}}}, .numa_node = numa};
 };
 auto destroyed = std::make_shared<unsigned>(0U);
 unsigned constructed = 0U;
 const auto factory = [&](DirectComputeConfiguration) {
  ++constructed;
  if (constructed == 1U && failure >= 2) {
   const auto cause = std::make_exception_ptr(std::runtime_error("injected constructor failure"));
   if (failure == 3) throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(cause);
   std::rethrow_exception(cause);
  }
  return std::make_unique<UnsafeRuntime>(destroyed, fail_run);
 };
 std::array<std::promise<contracts::ComputeUiState>, 2> terminal;
 const auto observe = [&](const contracts::ComputeUiState& value) {
  if (!value.active) terminal.at(value.generation_frontier - 1U).set_value(value);
 };
 ValidationSystem validation(settings, dataset, model, factory, [&](const ValidationSystem::event_type& event) {
  if (const auto* changed = std::get_if<ValidationChanged>(&event)) observe(changed->snapshot.operation);
 }, resolver);
 ExportSystem exporter(settings, dataset, model, factory, [&](const ExportSystem::event_type& event) {
  if (const auto* changed = std::get_if<ComputeChanged>(&event)) observe(changed->snapshot);
 }, resolver);
 const auto start = [&] {
  if (feature == contracts::FeatureId::Validate) static_cast<void>(validation.Start({}));
  else static_cast<void>(exporter.Start({}));
 };
 start();
 auto result = mmltk::testsupport::await_test_promise(terminal[0], "unsafe compute first run");
 if (failure >= 2) {
  CHECK(result.terminal.outcome == contracts::ComputeOperationOutcome::Failed);
  if (failure == 3) {
   CHECK_THROWS_AS(start(), contracts::UnavailableError);
   CHECK(constructed == 1U);
  } else {
   start();
   result = mmltk::testsupport::await_test_promise(terminal[1], "ordinary construction retry");
   CHECK(result.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
   CHECK(constructed == 2U);
  }
  validation.Shutdown();
  exporter.Shutdown();
  return;
 }
 if (!fail_run) {
  REQUIRE(result.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
  contracts::SettingsUpdateRequest edit;
  edit.updates.push_back({.path = feature == contracts::FeatureId::Validate ? "workflows.validate.request.device_id" : "workflows.export_state.device_id",
   .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{7}}});
  static_cast<void>(settings.Update(std::move(edit)));
  start();
  result = mmltk::testsupport::await_test_promise(terminal[1], "unsafe selected-device replacement");
 }
 CHECK(result.terminal.outcome == contracts::ComputeOperationOutcome::Failed);
 CHECK_THROWS_AS(start(), contracts::UnavailableError);
 CHECK(constructed == 1U);
 CHECK(*destroyed == 0U);
 validation.Shutdown();
 exporter.Shutdown();
 CHECK(*destroyed == 0U);
}
TEST_CASE("workflow factories receive captured nonzero GPUs and reuse only complete execution policy", "[controller][systems][compute][placement]") {
 const auto feature = GENERATE(contracts::FeatureId::Validate, contracts::FeatureId::Export, contracts::FeatureId::Predict);
 mmltk::testsupport::ScopedTempDir root{"workflow-selected-execution"};
 ApplicationDataFixture fixture(root.path());
 fixture.PrepareModel(feature);
 auto [settings, dataset, model] = fixture.systems();
 const auto topology = mmltk::common::system::NumaTopology::Capture();
 const auto cpu = mmltk::common::system::test_support::first_permitted_cpu(topology);
 std::vector<int> runs;
 std::vector<DirectComputeConfiguration> constructions;
 unsigned closes = 0;
 const DirectComputeResolver resolver = [&](int device, int numa) {
  if (device == 99) throw contracts::UnavailableError("selected CUDA GPU 99 is unavailable");
  return DirectComputeConfiguration{.execution = mmltk::frameworks::gpu::DeviceExecution{.device = device, .placement = {.numa_node = cpu.node, .cpus = {cpu.cpu}}}, .numa_node = numa};
 };
 std::promise<void> entered, release;
 const auto released = release.get_future().share();
 bool first_run = true;
 const auto before = [&] {
  if (!std::exchange(first_run, false)) return;
  entered.set_value();
  released.wait();
 };
 const auto factory = [&](DirectComputeConfiguration configuration) {
  constructions.push_back(configuration);
  return std::make_unique<SelectedWorkflowRuntime>(std::move(configuration), runs, closes, before);
 };
 std::mutex mutex;
 std::condition_variable condition;
 std::uint64_t settled = 0;
 const auto terminal = [&](const contracts::ComputeUiState& operation) {
  if (operation.active) return;
  std::scoped_lock lock(mutex);
  settled = operation.generation_frontier;
  condition.notify_all();
 };
 ValidationSystem validation(settings, dataset, model, factory,
  [&](const auto& event) { std::visit([&](const auto& value) {
   if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, ValidationProgress>) terminal(value.operation);
   else terminal(value.snapshot.operation);
  }, event); }, resolver);
 ExportSystem exporter(settings, dataset, model, factory,
  [&](const auto& event) { std::visit([&](const auto& value) { terminal(value.snapshot); }, event); }, resolver);
 PredictSystem prediction(settings, dataset, model, {.device = 0, .maximum_width = 8U, .maximum_height = 8U}, factory,
  [&](const auto& event) { std::visit([&](const auto& value) { terminal(value.snapshot.operation); }, event); }, resolver);
 mmltk::testsupport::ScopedTestCleanup unblock([&] { try { release.set_value(); } catch (const std::future_error&) {} });
 const auto start = [&] {
  switch (feature) {
   case contracts::FeatureId::Validate: static_cast<void>(validation.Start({})); break;
   case contracts::FeatureId::Export: static_cast<void>(exporter.Start({})); break;
   case contracts::FeatureId::Predict: static_cast<void>(prediction.Start({})); break;
   default: throw std::logic_error("unexpected workflow");
  }
 };
 const auto path = feature == contracts::FeatureId::Validate ? "workflows.validate.request.device_id" : feature == contracts::FeatureId::Predict ? "workflows.predict.request.device_id" : "workflows.export_state.device_id";
 for (const auto device : {7, 7, 3}) {
  const auto expected = settled + 1;
  contracts::SettingsUpdateRequest edit;
  edit.updates.push_back({.path = path, .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{device}}});
  static_cast<void>(settings.Update(std::move(edit)));
  start();
  if (expected == 1U) {
   mmltk::testsupport::await_test_promise(entered, "selected workflow entered");
   CHECK_THROWS_AS(start(), contracts::BusyError);
   contracts::SettingsUpdateRequest active_edit;
   active_edit.updates.push_back({.path = path, .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{99}}});
   static_cast<void>(settings.Update(std::move(active_edit)));
   CHECK_THROWS_AS(start(), contracts::BusyError);
   release.set_value();
  }
  std::unique_lock lock(mutex);
  REQUIRE(condition.wait_for(lock, std::chrono::seconds(10), [&] { return settled == expected; }));
 }
 CHECK(runs == std::vector<int>{7, 7, 3});
 REQUIRE(constructions.size() == 2U);
 CHECK(closes == 1U);
 if (feature != contracts::FeatureId::Export) {
  contracts::SettingsUpdateRequest edit;
  edit.updates.push_back({.path = feature == contracts::FeatureId::Validate ? "workflows.validate.request.numa_node" : "workflows.predict.request.numa_node",
   .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{cpu.node}}});
  static_cast<void>(settings.Update(std::move(edit)));
  start();
  std::unique_lock lock(mutex);
  REQUIRE(condition.wait_for(lock, std::chrono::seconds(10), [&] { return settled == 4U; }));
  CHECK(constructions.size() == 3U);
  CHECK(closes == 2U);
 }
 contracts::SettingsUpdateRequest missing;
 missing.updates.push_back({.path = path, .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{99}}});
 static_cast<void>(settings.Update(std::move(missing)));
 const auto count = constructions.size();
 CHECK_THROWS_AS(start(), contracts::UnavailableError);
 CHECK(constructions.size() == count);
 prediction.Shutdown();
 exporter.Shutdown();
 validation.Shutdown();
}
}
}
