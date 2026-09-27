#include "src/frameworks/gpu/tests/pinned_host_fault.h"
#include "src/common/system/tests/numa_topology_test_support.h"
#include <algorithm>
#include <chrono>
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/controller/subsystems/validate/validation_runtime.h"
#include "src/controller/subsystems/validate/validation_system.h"
#include "src/backend/models/rfdetr/core/tests/class_artifact_fixture.h"
#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include "src/controller/subsystems/system/predict_system.h"
#include "src/controller/subsystems/export/export_system.h"
#include <sched.h>
#include "src/controller/subsystems/system/tests/application_data_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/controller/runtime/local_run.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/controller/subsystems/system/detail/cuda_runtime_resources.h"
#include "src/frameworks/gpu/image/image_failure.h"
#include "src/frameworks/gpu/cuda/cuda_context_scope.h"
#include "src/common/system/numa_topology.h"
#include "src/common/system/execution_policy.h"
#include "src/common/system/tests/denied_syscall.h"
#include "src/frameworks/gpu/cuda/device_execution.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <condition_variable>
#include <cstdint>
#include <cuda.h>
#include <functional>
#include <future>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <stop_token>
#include <sys/syscall.h>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
import mmltk.backend.models.rfdetr.inference.prediction;
using namespace mmltk::controller::test_support;
namespace mmltk::controller {
namespace {
TEST_CASE("compute runtime admission preserves valid progress and reports malformed or failed work", "[controller][systems][compute]") {
 const auto scenario = GENERATE(0, 1, 2, 3);
 std::vector<std::uint64_t> delivered;
 const auto terminal = run_checked_compute([&](const ComputeProgressSink& progress) {
  std::jthread reporter([&] {
   progress({1U, 1U, 2U, "first"});
   if (scenario == 1) progress({2U, 3U, 2U, "invalid"});
   progress({3U, 2U, 2U, "last"});
  });
  reporter.join();
  if (scenario == 3) throw std::runtime_error("runtime failure detail");
  return contracts::make_compute_terminal(scenario == 2 ? contracts::ComputeOperationOutcome::Running : contracts::ComputeOperationOutcome::Succeeded, 0U, 2U);
 }, [&](const contracts::ComputeProgress& progress) { delivered.push_back(progress.sequence); });
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
    for (const auto& construct : std::array<std::function<void()>, 3>{[&] { CudaValidationRuntime runtime(configuration); }, [&] { CudaExportRuntime runtime(configuration); }, [&] {
     CudaPredictRuntime runtime(configuration);
    }}) {
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
struct RuntimeRetirementProbe {
 int current = 9;
 unsigned created = 0, destroyed = 0, closed = 0, sessions_destroyed = 0, synchronized = 0;
 bool restore_failure = false, construction_restore_failure = false, sync_failure = false, close_failure = false, destroy_failure = false;
};
struct RuntimeRetirementSession {
 std::shared_ptr<RuntimeRetirementProbe> probe;
 ~RuntimeRetirementSession() { ++probe->sessions_destroyed; }
};
detail::CudaRuntimeResources::Operations retirement_operations(const std::shared_ptr<RuntimeRetirementProbe>& probe) {
 return detail::CudaRuntimeResources::Operations{
  .device = {
   probe.get(),
   [](void* value, int* device) noexcept {
  *device = static_cast<RuntimeRetirementProbe*>(value)->current;
  return cudaSuccess;
 },
   [](void* value, int device) noexcept {
  auto& state = *static_cast<RuntimeRetirementProbe*>(value);
  if (device == 9 && state.restore_failure) return cudaErrorUnknown;
  state.current = device;
  return cudaSuccess;
 }
  },
  .context = probe.get(),
  .create =
   [](void* value, cudaStream_t* stream, unsigned) {
  auto& state = *static_cast<RuntimeRetirementProbe*>(value);
  ++state.created;
  *stream = reinterpret_cast<cudaStream_t>(value);
  if (state.construction_restore_failure) state.restore_failure = true;
  return cudaSuccess;
 },
  .synchronize =
   [](void* value, cudaStream_t) {
  auto& state = *static_cast<RuntimeRetirementProbe*>(value);
  ++state.synchronized;
  return state.sync_failure ? cudaErrorUnknown : cudaSuccess;
 },
  .destroy = [](void* value, cudaStream_t) {
  auto& state = *static_cast<RuntimeRetirementProbe*>(value);
  if (state.destroy_failure) return cudaErrorUnknown;
  ++state.destroyed;
  return cudaSuccess;
 },
 };
}
TEST_CASE("direct CUDA resources preserve the exact session and stream on unproved settlement", "[controller][compute][custody]") {
 using Resources = detail::CudaRuntimeResources;
 STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Resources>);
 STATIC_REQUIRE_FALSE(std::is_move_constructible_v<Resources>);
 const auto failure = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
 const auto cpu = mmltk::common::system::test_support::first_permitted_cpu(mmltk::common::system::NumaTopology::Capture());
 const DirectComputeConfiguration config{.execution = mmltk::frameworks::gpu::DeviceExecution{.device = 3, .placement = {.numa_node = cpu.node, .cpus = {cpu.cpu}}}};
 auto probe = std::make_shared<RuntimeRetirementProbe>();
 auto session = std::make_shared<RuntimeRetirementSession>(probe);
 std::weak_ptr<RuntimeRetirementSession> physical = session;
 const auto operations = retirement_operations(probe);
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
  } else if (failure == 3 || failure == 6 || failure == 7) {
   CHECK_THROWS(resources->Retire());
  } else if (failure == 0) {
   CHECK_THROWS(resources->Run([](auto) -> contracts::ComputeTerminal { throw std::invalid_argument("ordinary work refusal"); }, {}));
   CHECK_FALSE(resources->HasUnsafeCustody());
   CHECK(resources->Run([](auto) { return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded); }, {}).outcome == contracts::ComputeOperationOutcome::Succeeded);
   std::stop_source stopped;
   stopped.request_stop();
   const auto cancelled = resources->Run([](auto) -> contracts::ComputeTerminal {
    FAIL("cancelled work ran");
    return {};
   }, stopped.get_token());
   CHECK(cancelled.outcome == contracts::ComputeOperationOutcome::Cancelled);
   resources->Retire();
   const auto synchronized = probe->synchronized;
   resources->Retire();
   CHECK(probe->synchronized == synchronized);
   CHECK(probe->closed == 1U);
   CHECK(probe->destroyed == 1U);
   CHECK(probe->current == 9);
   CHECK_THROWS_AS(resources->Run([](auto) { return contracts::ComputeTerminal{}; }, {}), contracts::UnavailableError);
  }
  if (failure != 0 && failure != 4) {
   CHECK(resources->HasUnsafeCustody());
   CHECK_THROWS_AS(resources->Retire(), contracts::UnavailableError);
   CHECK_THROWS_AS(resources->Run([](auto) { return contracts::ComputeTerminal{}; }, {}), contracts::UnavailableError);
  }
 }
 resources.reset();
 CHECK(probe->created == 1U);
 CHECK(probe->destroyed == (failure == 0 || failure == 6 ? 1U : 0U));
 CHECK(probe->sessions_destroyed == (failure == 0 ? 1U : 0U));
 CHECK(physical.expired() == (failure == 0));
}
class SelectedWorkflowRuntime : public ValidationRuntime, public ExportRuntime, public PredictRuntime {
public:
 SelectedWorkflowRuntime(DirectComputeConfiguration configuration, std::vector<int>& runs, unsigned& closes, std::function<void()> before)
     : configuration_(std::move(configuration)), runs_(runs), closes_(closes), before_(std::move(before)) {}
 void Close() noexcept override { ++closes_; }
 ValidationRuntimeResult Run(
  mmltk::backend::models::rfdetr::ValidateRequest request, std::stop_token, const ComputeProgressSink&, const mmltk::backend::models::rfdetr::ValidationDelivery&, std::uint64_t generation) override {
  CHECK(request.compile_cuda_device_id == request.device_id);
  return {.terminal = Record(request.device_id, generation)};
 }
 contracts::ComputeTerminal Run(ExportRunRequest request, std::stop_token, const ComputeProgressSink&, const ComputeArtifactSink&, std::uint64_t generation) override {
  return Record(request.onnx.device_id, generation);
 }
 contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::PredictRequest request, std::stop_token, const ComputeProgressSink&, const ProductSink&, const PlaybackGate&, VisualExtent,
  const ContextProvider&, const PreviewRetirement&, const ComputeArtifactSink&, const PredictionRunOutput&, std::uint64_t generation, const ExecutionSink&) override {
  return Record(request.device_id, generation);
 }

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
class RetiringWorkflowRuntime final : public SelectedWorkflowRuntime {
public:
 RetiringWorkflowRuntime(DirectComputeConfiguration configuration, std::vector<int>& runs, unsigned& closes, std::shared_ptr<RuntimeRetirementProbe> probe)
     : SelectedWorkflowRuntime(configuration, runs, closes, [] {}),
       resources_(configuration, [session = std::make_shared<RuntimeRetirementSession>(probe)] { ++session->probe->closed; }, retirement_operations(probe)) {}
 void Close() noexcept override {
  try {
   resources_.Retire();
  } catch (...) {}
 }
 bool HasUnsafeCustody() const noexcept override { return resources_.HasUnsafeCustody(); }

private:
 detail::CudaRuntimeResources resources_;
};
// CPU placement is real; the synthetic device identity belongs to the runtime under test.
DirectComputeResolver test_compute_resolver(int unavailable = -1) {
 const auto cpu = mmltk::common::system::test_support::first_permitted_cpu(mmltk::common::system::NumaTopology::Capture());
 return [cpu, unavailable](int device, int numa) {
  if (device == unavailable) throw contracts::UnavailableError("selected CUDA GPU " + std::to_string(device) + " is unavailable");
  return DirectComputeConfiguration{.execution = mmltk::frameworks::gpu::DeviceExecution{.device = device, .placement = {.numa_node = cpu.node, .cpus = {cpu.cpu}}}, .numa_node = numa};
 };
}
class WorkflowSystems final {
public:
 struct Factories {
  ValidationRuntimeFactory validation{};
  ExportRuntimeFactory exporter{};
  PredictRuntimeFactory prediction{};
 };
 WorkflowSystems(contracts::FeatureId feature, ApplicationDataFixture& fixture, Factories factories, DirectComputeResolver resolver, std::function<void(const contracts::ComputeUiState&)> observe,
  VisualExtent extent = {64U, 64U})
     : feature_(feature), settings_(std::get<0>(fixture.systems())), observe_(std::move(observe)) {
  auto [settings, dataset, model] = fixture.systems();
  if (factories.validation)
   validation_.emplace(settings, dataset, model, std::move(factories.validation), [this](const ValidationSystem::event_type& event) {
    if (const auto* changed = std::get_if<ValidationChanged>(&event)) observe_(changed->snapshot.operation);
   }, resolver);
  if (factories.exporter)
   exporter_.emplace(settings, dataset, model, std::move(factories.exporter), [this](const ExportSystem::event_type& event) {
    if (const auto* changed = std::get_if<ComputeChanged>(&event)) observe_(changed->snapshot);
   }, resolver);
  if (factories.prediction)
   prediction_.emplace(settings, dataset, model, VisualDeviceSettings{.device = 0, .maximum_width = extent.width, .maximum_height = extent.height}, std::move(factories.prediction),
    [this](const PredictSystem::event_type& event) { std::visit([this](const auto& value) { observe_(value.snapshot.operation); }, event); }, resolver);
 }
 void Start() {
  switch (feature_) {
   case contracts::FeatureId::Validate: static_cast<void>(validation_.value().Start({})); break;
   case contracts::FeatureId::Export: static_cast<void>(exporter_.value().Start({})); break;
   case contracts::FeatureId::Predict: static_cast<void>(prediction_.value().Start({})); break;
   default: throw std::logic_error("unexpected workflow");
  }
 }
 void SetDevice(int device) {
  const auto path = feature_ == contracts::FeatureId::Validate  ? "workflows.validate.request.device_id"
                    : feature_ == contracts::FeatureId::Predict ? "workflows.predict.request.device_id"
                                                                : "workflows.export_state.device_id";
  Update(path, device);
 }
 void SetNuma(int node) {
  if (feature_ == contracts::FeatureId::Export) throw std::logic_error("export has no explicit NUMA preference");
  Update(feature_ == contracts::FeatureId::Validate ? "workflows.validate.request.numa_node" : "workflows.predict.request.numa_node", node);
 }
 void Stop() {
  if (validation_) static_cast<void>(validation_->Stop());
  if (exporter_) static_cast<void>(exporter_->Stop());
  if (prediction_) static_cast<void>(prediction_->Stop({}));
 }
 void Shutdown() {
  if (validation_) validation_->Shutdown();
  if (exporter_) exporter_->Shutdown();
  if (prediction_) prediction_->Shutdown();
 }

private:
 void Update(std::string_view path, int value) {
  contracts::SettingsUpdateRequest edit;
  edit.updates.push_back({.path = std::string(path), .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{value}}});
  static_cast<void>(settings_.Update(std::move(edit)));
 }
 contracts::FeatureId feature_;
 SettingsSystem& settings_;
 std::function<void(const contracts::ComputeUiState&)> observe_;
 std::optional<ValidationSystem> validation_;
 std::optional<ExportSystem> exporter_;
 std::optional<PredictSystem> prediction_;
};
TEST_CASE("workflow replacement observes complete stream retirement before another factory", "[controller][compute][custody]") {
 const auto feature = GENERATE(contracts::FeatureId::Validate, contracts::FeatureId::Export, contracts::FeatureId::Predict);
 const bool restore = GENERATE(false, true);
 mmltk::testsupport::ScopedTempDir root{"complete-workflow-retirement"};
 ApplicationDataFixture fixture(root.path());
 fixture.PrepareModel(feature);
 const auto resolver = test_compute_resolver();
 auto probe = std::make_shared<RuntimeRetirementProbe>();
 unsigned constructed = 0, closes = 0;
 std::vector<int> runs;
 const auto factory = [&](DirectComputeConfiguration configuration) {
  ++constructed;
  return std::make_unique<RetiringWorkflowRuntime>(configuration, runs, closes, probe);
 };
 std::array<std::promise<contracts::ComputeUiState>, 2> terminals;
 const auto observe = [&](const contracts::ComputeUiState& value) {
  if (!value.active && value.generation_frontier) terminals.at(value.generation_frontier - 1U).set_value(value);
 };
 WorkflowSystems systems(feature, fixture, {.validation = factory, .exporter = factory, .prediction = factory}, resolver, observe);
 systems.Start();
 REQUIRE(mmltk::testsupport::await_test_promise(terminals[0], "initial workflow").terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
 probe->restore_failure = restore;
 probe->destroy_failure = !restore;
 systems.SetDevice(7);
 systems.Start();
 REQUIRE(mmltk::testsupport::await_test_promise(terminals[1], "complete retirement refusal").terminal.outcome == contracts::ComputeOperationOutcome::Failed);
 CHECK(probe->closed == 1U);
 CHECK(probe->synchronized == 1U);
 CHECK(probe->destroyed == (restore ? 1U : 0U));
 CHECK(probe->sessions_destroyed == 0U);
 for (unsigned attempt = 0; attempt < 3; ++attempt) CHECK_THROWS_AS(systems.Start(), contracts::UnavailableError);
 CHECK(constructed == 1U);
 systems.Stop();
 systems.Shutdown();
 CHECK(probe->closed == 1U);
 CHECK(probe->synchronized == 1U);
 CHECK(probe->sessions_destroyed == 0U);
}
TEST_CASE("workflow replacement observes production session registered-page retirement", "[controller][compute][custody]") {
 namespace rfdetr = mmltk::backend::models::rfdetr;
 namespace gpu = mmltk::frameworks::gpu;
 const auto feature = GENERATE(contracts::FeatureId::Validate, contracts::FeatureId::Predict);
 const int failure = GENERATE(1, 2, 3);
 struct Probe {
  explicit Probe(int scenario) : pages({.unregister_call = static_cast<unsigned>(scenario), .restore = scenario == 3}) {}
  gpu::test_support::PinnedHostFault pages;
  unsigned streams_destroyed = 0;
 };
 auto probe = std::make_shared<Probe>(failure);
 const mmltk::testsupport::ScopedTempDir root("workflow-session-retirement");
 ApplicationDataFixture fixture(root.path());
 fixture.PrepareModel(feature);
 const auto request = rfdetr::test_support::prediction_image_fixture(root.path());
 class SessionRuntime final : public ValidationRuntime, public PredictRuntime {
 public:
  SessionRuntime(DirectComputeConfiguration configuration, std::shared_ptr<Probe> probe, rfdetr::PredictRequest request, std::shared_ptr<rfdetr::PredictionSession> session)
      : session_(std::move(session)), probe_(std::move(probe)), request_(std::move(request)), resources_(configuration, [session = session_, probe = probe_] {
         static_cast<void>(probe);
         if (session->Close() != mmltk::backend::ml::runtime::kRuntimeSuccess) throw std::runtime_error("production session close failed");
        }, StreamOperations(probe_.get())) {}
  void Close() noexcept override {
   try {
    resources_.Retire();
   } catch (...) {}
  }
  bool HasUnsafeCustody() const noexcept override { return resources_.HasUnsafeCustody() || session_->HasUnsafeCustody(); }
  ValidationRuntimeResult Run(rfdetr::ValidateRequest, std::stop_token, const ComputeProgressSink&, const rfdetr::ValidationDelivery& delivery, std::uint64_t) override {
   return {.terminal = Execute(delivery.retirement)};
  }
  contracts::ComputeTerminal Run(rfdetr::PredictRequest, std::stop_token, const ComputeProgressSink&, const ProductSink&, const PlaybackGate&, VisualExtent, const ContextProvider&,
   const PreviewRetirement& retirement, const ComputeArtifactSink&, const PredictionRunOutput&, std::uint64_t, const ExecutionSink&) override {
   return Execute(retirement);
  }

 private:
  static detail::CudaRuntimeResources::Operations StreamOperations(Probe* probe) {
   detail::CudaRuntimeResources::Operations operations;
   operations.context = probe;
   operations.destroy = [](void* value, cudaStream_t stream) {
    ++static_cast<Probe*>(value)->streams_destroyed;
    return cudaStreamDestroy(stream);
   };
   return operations;
  }
  contracts::ComputeTerminal Execute(const PredictRuntime::PreviewRetirement& retirement) {
   const auto operations = probe_->pages.operations();
   return resources_.Run([&](auto stream) {
    const auto result = session_->Run(request_, stream, {.retirement = retirement, .registered_host_operations = operations});
    return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded, 0U, result.processed_images);
   }, {});
  }
  std::shared_ptr<rfdetr::PredictionSession> session_;
  std::shared_ptr<Probe> probe_;
  rfdetr::PredictRequest request_;
  detail::CudaRuntimeResources resources_;
 };
 const auto resolver = test_compute_resolver();
 unsigned factories = 0;
 std::weak_ptr<rfdetr::PredictionSession> retained;
 const auto factory = [&](DirectComputeConfiguration configuration) {
  ++factories;
  auto session = std::make_shared<rfdetr::PredictionSession>();
  retained = session;
  return std::make_unique<SessionRuntime>(configuration, probe, request, std::move(session));
 };
 std::array<std::promise<contracts::ComputeUiState>, 2> settled;
 const auto observe = [&](const contracts::ComputeUiState& value) {
  if (!value.active && value.generation_frontier) settled.at(value.generation_frontier - 1U).set_value(value);
 };
 WorkflowSystems systems(feature, fixture, {.validation = factory, .prediction = factory}, resolver, observe);
 systems.Start();
 REQUIRE(mmltk::testsupport::await_test_promise(settled[0], "production session run").terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
 probe->pages.Arm();
 systems.SetDevice(7);
 systems.Start();
 CHECK(mmltk::testsupport::await_test_promise(settled[1], "production session close refusal").terminal.outcome == contracts::ComputeOperationOutcome::Failed);
 for (unsigned attempt = 0; attempt < 3; ++attempt) CHECK_THROWS_AS(systems.Start(), contracts::UnavailableError);
 CHECK(factories == 1U);
 CHECK_FALSE(retained.expired());
 CHECK(probe->streams_destroyed == 0U);
 systems.Stop();
 systems.Shutdown();
 CHECK_FALSE(retained.expired());
 CHECK(probe->streams_destroyed == 0U);
}
TEST_CASE("Validate and Export seal replacement retry and shutdown after unsafe runtime custody", "[controller][compute][custody]") {
 struct UnsafeRuntime final : ValidationRuntime, ExportRuntime {
  std::shared_ptr<unsigned> destroyed;
  bool fail_run, unsafe = false;
  UnsafeRuntime(std::shared_ptr<unsigned> count, bool fail) : destroyed(std::move(count)), fail_run(fail) {}
  ~UnsafeRuntime() override { ++*destroyed; }
  bool HasUnsafeCustody() const noexcept override { return unsafe; }
  void Close() override {
   unsafe = true;
   throw std::runtime_error("injected unproved close");
  }
  contracts::ComputeTerminal Result() {
   if (fail_run) {
    unsafe = true;
    throw std::runtime_error("injected unproved run");
   }
   return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded);
  }
  ValidationRuntimeResult Run(
   mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token, const ComputeProgressSink&, const mmltk::backend::models::rfdetr::ValidationDelivery&, std::uint64_t) override {
   return {.terminal = Result()};
  }
  contracts::ComputeTerminal Run(ExportRunRequest, std::stop_token, const ComputeProgressSink&, const ComputeArtifactSink&, std::uint64_t) override { return Result(); }
 };
 const auto feature = GENERATE(contracts::FeatureId::Validate, contracts::FeatureId::Export);
 const auto failure = GENERATE(0, 1, 2, 3);
 const bool fail_run = failure == 1;
 mmltk::testsupport::ScopedTempDir root{"unsafe-selected-execution"};
 ApplicationDataFixture fixture(root.path());
 fixture.PrepareModel(feature);
 const auto resolver = test_compute_resolver();
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
 WorkflowSystems systems(feature, fixture, {.validation = factory, .exporter = factory}, resolver, observe);
 systems.Start();
 auto result = mmltk::testsupport::await_test_promise(terminal[0], "unsafe compute first run");
 if (failure >= 2) {
  CHECK(result.terminal.outcome == contracts::ComputeOperationOutcome::Failed);
  if (failure == 3) {
   CHECK_THROWS_AS(systems.Start(), contracts::UnavailableError);
   CHECK(constructed == 1U);
  } else {
   systems.Start();
   result = mmltk::testsupport::await_test_promise(terminal[1], "ordinary construction retry");
   CHECK(result.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
   CHECK(constructed == 2U);
  }
  systems.Shutdown();
  return;
 }
 if (!fail_run) {
  REQUIRE(result.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
  systems.SetDevice(7);
  systems.Start();
  result = mmltk::testsupport::await_test_promise(terminal[1], "unsafe selected-device replacement");
 }
 CHECK(result.terminal.outcome == contracts::ComputeOperationOutcome::Failed);
 CHECK_THROWS_AS(systems.Start(), contracts::UnavailableError);
 CHECK(constructed == 1U);
 CHECK(*destroyed == 0U);
 systems.Shutdown();
 CHECK(*destroyed == 0U);
}
TEST_CASE("workflow factories receive captured nonzero GPUs and reuse only complete execution policy", "[controller][systems][compute][placement]") {
 const auto feature = GENERATE(contracts::FeatureId::Validate, contracts::FeatureId::Export, contracts::FeatureId::Predict);
 mmltk::testsupport::ScopedTempDir root{"workflow-selected-execution"};
 ApplicationDataFixture fixture(root.path());
 fixture.PrepareModel(feature);
 std::vector<int> runs;
 std::vector<DirectComputeConfiguration> constructions;
 unsigned closes = 0;
 const auto resolver = test_compute_resolver(99);
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
 WorkflowSystems systems(feature, fixture, {.validation = factory, .exporter = factory, .prediction = factory}, resolver, terminal, {8U, 8U});
 mmltk::testsupport::ScopedTestCleanup unblock([&] {
  try {
   release.set_value();
  } catch (const std::future_error&) {}
 });
 for (const auto device : {7, 7, 3}) {
  const auto expected = settled + 1;
  systems.SetDevice(device);
  systems.Start();
  if (expected == 1U) {
   mmltk::testsupport::await_test_promise(entered, "selected workflow entered");
   CHECK_THROWS_AS(systems.Start(), contracts::BusyError);
   systems.SetDevice(99);
   CHECK_THROWS_AS(systems.Start(), contracts::BusyError);
   release.set_value();
  }
  std::unique_lock lock(mutex);
  REQUIRE(condition.wait_for(lock, std::chrono::seconds(10), [&] { return settled == expected; }));
 }
 CHECK(runs == std::vector<int>{7, 7, 3});
 REQUIRE(constructions.size() == 2U);
 CHECK(closes == 1U);
 if (feature != contracts::FeatureId::Export) {
  systems.SetNuma(resolver(3, -1).execution->placement.numa_node);
  systems.Start();
  std::unique_lock lock(mutex);
  REQUIRE(condition.wait_for(lock, std::chrono::seconds(10), [&] { return settled == 4U; }));
  CHECK(constructions.size() == 3U);
  CHECK(closes == 2U);
 }
 systems.SetDevice(99);
 const auto count = constructions.size();
 CHECK_THROWS_AS(systems.Start(), contracts::UnavailableError);
 CHECK(constructions.size() == count);
 systems.Shutdown();
}
}  // namespace
}  // namespace mmltk::controller
