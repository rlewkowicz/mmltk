#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/controller/subsystems/system/tests/application_data_test_support.h"
#include "src/controller/subsystems/system/tests/prediction_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/controller/subsystems/system/predict_system.h"
#include "src/controller/subsystems/system/detail/prediction_preview.h"
#include "src/controller/subsystems/system/detail/predict_revision.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/data/catalog/class_catalog.h"
#include "src/common/system/numa_topology.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_authority.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
using namespace mmltk::controller::test_support;
namespace mmltk::controller {
namespace {
TEST_CASE("Predict revision capacity preserves cancellation and terminal observations", "[controller][systems][predict]") {
 using Revision = detail::PredictRevision;
 constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
 for (std::uint64_t reserved = 0U; reserved <= 5U; ++reserved) CHECK_THROWS_AS(Revision::Admit(maximum - reserved), contracts::FailedError);
 const auto admitted = Revision::Admit(maximum - 7U);
 REQUIRE(admitted == maximum - 6U);
 const auto progressed = Revision::Progress(admitted, false);
 REQUIRE(progressed == maximum - 5U);
 CHECK_FALSE(Revision::Progress(*progressed, false));
 const auto cancelled = Revision::Cancel(*progressed);
 REQUIRE(cancelled == maximum - 4U);
 CHECK_FALSE(Revision::Progress(*cancelled, true));
 const auto settled = Revision::Complete(*cancelled);
 REQUIRE(settled == maximum - 3U);
 CHECK_FALSE(Revision::Complete(*settled));
 const auto copied = Revision::Frame(*settled, false, false);
 REQUIRE(copied == maximum - 2U);
 const auto latest = Revision::Frame(*copied, false, false);
 REQUIRE(latest == maximum - 1U);
 CHECK_FALSE(Revision::Frame(*latest, false, false));
 CHECK(Revision::Fail(*latest) == maximum);
 CHECK_FALSE(Revision::Fail(maximum));
 CHECK(Revision::Fail(*cancelled) == maximum - 3U);
 CHECK(Revision::Complete(Revision::Admit(maximum - 6U)) == maximum - 4U);
 const auto earlier_cancel = Revision::Cancel(admitted);
 REQUIRE(earlier_cancel == maximum - 5U);
 CHECK(Revision::Progress(*earlier_cancel, true) == maximum - 4U);
}
class UnsafePredictRuntime final : public PredictRuntime {
public:
 UnsafePredictRuntime(bool on_close, std::shared_ptr<int> custody, bool preview_terminal = false) : on_close_(on_close), preview_terminal_(preview_terminal), custody_(std::move(custody)) {}
 void Close() noexcept override {
  ++*custody_;
  unsafe_ = true;
 }
 [[nodiscard]] bool HasUnsafeCustody() const noexcept override { return unsafe_; }
 contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::PredictRequest, std::stop_token, const ComputeProgressSink&, const ProductSink&, const PlaybackGate&, VisualExtent,
  const ContextProvider&, const PreviewRetirement& retirement) override {
  if (preview_terminal_) {
   auto lease = mmltk::frameworks::gpu::ReserveTerminalCudaLease(*retirement);
   auto retained = custody_;
   std::move(lease).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(retained)), cudaErrorUnknown);
   throw mmltk::frameworks::gpu::CudaContextFailure(true);
  }
  unsafe_ = !on_close_;
  throw std::runtime_error("test prediction failure");
 }

private:
 bool on_close_;
 bool preview_terminal_;
 bool unsafe_ = false;
 std::shared_ptr<int> custody_;
};
TEST_CASE("Predict seals unsafe execution and close custody across repeated admission", "[controller][systems][predict][custody]") {
 const bool on_close = GENERATE(false, true);
 const bool preview_terminal = GENERATE(false, true);
 ApplicationDataFixture fixture{mmltk::testsupport::make_temp_root("predict-unsafe-admission")};
 fixture.PrepareModel(contracts::FeatureId::Predict);
 auto [settings, dataset, model] = fixture.systems();
 auto custody = std::make_shared<int>(7);
 std::weak_ptr<int> retained = custody;
 std::atomic_size_t constructions = 0U;
 std::promise<void> failed;
 {
  PredictSystem prediction{settings, dataset, model, {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
   [&] {
    ++constructions;
    return std::make_unique<UnsafePredictRuntime>(on_close, custody, preview_terminal);
   },
   [&](PredictSystem::event_type event) {
    if (const auto* failure = std::get_if<PredictFailed>(&event); failure && !failure->snapshot.operation.active) mmltk::testsupport::release_test_promise(failed);
   }};
  static_cast<void>(prediction.Start({}));
  mmltk::testsupport::await_test_promise(failed, "unsafe Predict settlement");
  REQUIRE_FALSE(prediction.snapshot().operation.active);
  for (unsigned attempt = 0U; attempt < 4U; ++attempt) CHECK_THROWS_AS(prediction.Start({}), contracts::UnavailableError);
  CHECK(constructions == 1U);
  CHECK(*custody == (on_close && !preview_terminal ? 8 : 7));
  custody.reset();
 }
 CHECK_FALSE(retained.expired());
}
TEST_CASE("receiver retirement outlives concurrent preview pool destruction", "[controller][gpu][custody]") {
 namespace gpu = mmltk::frameworks::gpu;
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
 auto authority = std::make_shared<gpu::TerminalCudaRetirementOwner>(detail::PredictionPreviewPool::kSlotCapacity + 2U);
 PredictionReceiverFault fault;
 fault.enabled = fault.terminal = true;
 const ScopedPredictionReceiverFault registration{fault};
 const auto operations = PredictionReceiverFault::Operations();
 auto pool = std::make_unique<detail::PredictionPreviewPool>(execution, context, operations, authority);
 std::array<std::uint8_t, 48U> pixels;
 pixels.fill(127U);
 auto input = PredictionSource::Decoded({4U, 4U}, pixels);
 auto& decoded = input.custody();
 std::weak_ptr<void> retained = decoded;
 auto raw = pool->Capture(nullptr, input.extent(), 0U, {}, {}, input.classes(), 1, input.rgb8(), decoded);
 REQUIRE(raw);
 auto draw = std::async(std::launch::async, [raw, context] {
  try {
   gpu::SystemImageRuntime runtime({.device = 0, .context_mode = gpu::DeviceContextMode::Isolated, .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .adopted_context = context});
   auto candidate = runtime.AcquireOutput();
   raw->Draw(runtime, candidate);
  } catch (...) { return std::current_exception(); }
  return std::exception_ptr{};
 });
 const mmltk::testsupport::ScopedTestCleanup release{[&] {
  fault.upload.Release();
  if (draw.valid()) draw.wait();
 }};
 REQUIRE(fault.upload.WaitEntered(std::chrono::seconds{2}));
 CHECK_FALSE(pool->HasUnsafeSourceCustody());
 raw.reset();
 decoded.reset();
 pool.reset();  // the receiver transaction, not this replaceable shell, owns the fact
 CHECK(authority->admission_open());
 // The source frame and the in-flight composition reserve independent
 // custody. Terminal composition retains the real frame, including its lease.
 CHECK(authority->fact().reservations == 2U);
 fault.upload.Release();
 const auto failure = mmltk::testsupport::await_test_future(draw, "receiver completion after pool destruction");
 CHECK(gpu::is_image_execution_failure(failure));
 CHECK_FALSE(authority->admission_open());
 CHECK(authority->fact().occupancy == 1U);
 CHECK(authority->fact().reservations == 1U);
 CHECK_FALSE(retained.expired());
 for (unsigned attempt = 0U; attempt < 4U; ++attempt) CHECK_THROWS(detail::PredictionPreviewPool(execution, context, operations, authority));
 CHECK(authority->fact().occupancy == 1U);
}
TEST_CASE("late receiver custody seals Predict admission while optional visual failure preserves semantic success", "[controller][systems][predict][custody]") {
 const bool terminal = GENERATE(false, true);
 ApplicationDataFixture fixture{mmltk::testsupport::make_temp_root("predict-late-receiver")};
 fixture.PrepareModel(contracts::FeatureId::Predict);
 auto [settings, dataset, model] = fixture.systems();
 auto gate = std::make_shared<mmltk::testsupport::StopGate>();
 gate->Release();
 auto fault = std::make_shared<PredictionReceiverFault>();
 fault->terminal = terminal;
 const ScopedPredictionReceiverFault registration{*fault};
 std::atomic_size_t constructions = 0U;
 std::promise<void> first_frame, first_done, second_done, visual_failure, recovered;
 {
  PredictSystem prediction{settings, dataset, model, {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
   [&] {
    ++constructions;
    return std::make_unique<FakePredictRuntime>(PredictionScenario{.compute = {.gate = gate}, .receiver_fault = fault});
   },
   [&](PredictSystem::event_type event) {
    if (const auto* changed = std::get_if<PredictChanged>(&event)) {
     const auto& state = changed->snapshot;
     if (state.frame.valid() && state.operation.generation_frontier == 1U) mmltk::testsupport::release_test_promise(first_frame);
     if (!state.operation.active) {
      if (state.operation.generation_frontier == 1U) mmltk::testsupport::release_test_promise(first_done);
      if (state.operation.generation_frontier == 2U) mmltk::testsupport::release_test_promise(second_done);
     }
     if (state.operation.generation_frontier == 3U && state.frame.revision > 1U) mmltk::testsupport::release_test_promise(recovered);
    }
    if (std::holds_alternative<PredictFailed>(event)) mmltk::testsupport::release_test_promise(visual_failure);
   }};
  const mmltk::testsupport::ScopedTestCleanup stop{[&] {
   fault->upload.Release();
   prediction.Shutdown();
  }};
  static_cast<void>(prediction.Start({}));
  mmltk::testsupport::await_test_promise(first_frame, "initial retained Predict image");
  mmltk::testsupport::await_test_promise(first_done, "initial semantic completion");
  const auto retained = prediction.snapshot().frame;
  fault->enabled = true;
  static_cast<void>(prediction.Start({}));
  REQUIRE(fault->upload.WaitEntered(std::chrono::seconds{2}));
  mmltk::testsupport::await_test_promise(second_done, "semantic completion before receiver outcome");
  CHECK(prediction.snapshot().operation.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
  CHECK(prediction.snapshot().frame == retained);
  REQUIRE(fault->retirement);
  CHECK(fault->retirement->admission_open());
  fault->upload.Release();
  mmltk::testsupport::await_test_promise(visual_failure, "late receiver failure");
  CHECK(prediction.snapshot().operation.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
  CHECK(prediction.snapshot().frame == retained);
  CHECK(fault->retirement->admission_open() == !terminal);
  if (terminal) {
   CHECK(fault->retirement->fact().occupancy == 1U);
   for (unsigned attempt = 0U; attempt < 4U; ++attempt) CHECK_THROWS_AS(prediction.Start({}), contracts::UnavailableError);
   CHECK_FALSE(fault->decoded.expired());
  } else {
   fault->enabled = false;
   static_cast<void>(prediction.Start({}));
   mmltk::testsupport::await_test_promise(recovered, "safely settled visual recovery");
   CHECK(prediction.snapshot().frame.revision > retained.revision);
   CHECK(fault->retirement->fact().occupancy == 0U);
  }
  CHECK(constructions == 1U);
 }
 CHECK(fault->decoded.expired() == !terminal);
 if (terminal) {
  const auto facts = fault->retirement->fact();
  CHECK(facts.occupancy == 1U);
  CHECK(facts.occupancy + facts.reservations <= detail::PredictionPreviewPool::kSlotCapacity + 4U);
 }
}
TEST_CASE("prediction preview refusal preserves successful inference completion", "[controller][systems][compute]") {
 const auto root = mmltk::testsupport::make_temp_root("prediction-preview-refusal");
 ApplicationDataFixture fixture{root};
 fixture.PrepareModel(contracts::FeatureId::Predict);
 auto [settings, dataset, model] = fixture.systems();
 auto gate = std::make_shared<mmltk::testsupport::StopGate>();
 gate->Release();
 std::promise<PredictSnapshot> completed;
 std::promise<PredictFailed> preview_failed;
 PredictSystem prediction{settings, dataset, model, {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
  [gate] { return std::make_unique<FakePredictRuntime>(PredictionScenario{.compute = {.gate = gate}, .refuse_preview = true}); },
  [&](PredictSystem::event_type event) {
   if (auto* failure = std::get_if<PredictFailed>(&event)) preview_failed.set_value(std::move(*failure));
   if (auto* changed = std::get_if<PredictChanged>(&event); changed && !changed->snapshot.operation.active) completed.set_value(std::move(changed->snapshot));
  }};
 static_cast<void>(prediction.Start({}));
 const auto failure = mmltk::testsupport::await_test_promise(preview_failed, "prediction preview refusal");
 CHECK(failure.snapshot.operation.terminal.outcome != contracts::ComputeOperationOutcome::Failed);
 const auto terminal = mmltk::testsupport::await_test_promise(completed, "prediction completion after preview refusal");
 CHECK(terminal.operation.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
 CHECK(terminal.operation.terminal.completed == 2U);
 CHECK(terminal.operation.terminal.output == "result");
}
TEST_CASE("prediction raw custody is bounded under retained readers and preserves pixels", "[controller][gpu]") {
 using namespace mmltk::controller;
 // CLEANUP-IGNORE: Raw prediction custody and concurrent receiver retirement own independent device-context lifetimes.
 namespace gpu = mmltk::frameworks::gpu;
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 const auto classes = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"first", "last"});
 const std::array<float, 12U> pixels{1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
 auto input = PredictionSource::Device(execution, {2U, 2U}, pixels, {}, classes);
 const auto* source = input.pixels();
 auto& custody = input.custody();
 std::optional<detail::PredictionPreviewPool> pool;
 gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
 pool.emplace(execution, context);
 std::array<std::shared_ptr<const detail::PredictionPreviewFrame>, 3U> readers;
 for (auto& reader : readers) {
  reader = pool->Capture(source, {2U, 2U}, 0U, {}, {}, classes, 2, nullptr, custody);
  REQUIRE(reader);
 }
 CHECK_FALSE(pool->Capture(source, {2U, 2U}, 0U, {}, {}, classes, 2, nullptr, custody));
 gpu::SystemImageRuntime runtime({.device = 0, .context_mode = gpu::DeviceContextMode::Isolated, .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .adopted_context = context});
 auto candidate = runtime.AcquireOutput();
 readers[0]->Draw(runtime, candidate);
 const auto complete = runtime.CommitOutput(std::move(candidate));
 REQUIRE(complete.valid());
 gpu::SystemImageRuntime::OutputCandidate invalid_candidate;
 CHECK_THROWS_AS(readers[0]->Draw(runtime, invalid_candidate), std::invalid_argument);
 CHECK(runtime.Completed().revision() == complete.revision());
 auto image = complete.Borrow();
 const auto plane = image.plane(0U).plane();
 std::array<std::uint8_t, 16U> rgba{};
 REQUIRE(cudaMemcpy2D(rgba.data(), 8U, reinterpret_cast<const void*>(plane.data), plane.descriptor.pitch_bytes, 8U, 2U, cudaMemcpyDeviceToHost) == cudaSuccess);
 CHECK(rgba == std::array<std::uint8_t, 16U>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255});
 // A replacement renderer rejects a pending old-context frame before touching its candidate.
 gpu::DeviceContext replacement_context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
 gpu::SystemImageRuntime replacement(
  {.device = 0, .context_mode = gpu::DeviceContextMode::Isolated, .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .adopted_context = replacement_context});
 CHECK(readers[0]->CompatibleWith(runtime));
 CHECK_FALSE(readers[0]->CompatibleWith(replacement));
 gpu::SystemImageRuntime::OutputCandidate untouched;
 CHECK_THROWS_AS(readers[0]->Draw(replacement, untouched), std::runtime_error);
 CHECK_FALSE(replacement.Completed().valid());
 CHECK(runtime.Completed().revision() == complete.revision());
 detail::PredictionPreviewPool replacement_pool(execution, replacement_context);
 auto next = replacement_pool.Capture(source, {2U, 2U}, 0U, {}, {}, classes, 2, nullptr, custody);
 REQUIRE(next);
 CHECK(next->CompatibleWith(replacement));
 auto next_candidate = replacement.AcquireOutput();
 next->Draw(replacement, next_candidate);
 CHECK(replacement.CommitOutput(std::move(next_candidate)).valid());
 readers[1].reset();
 auto latest = pool->Capture(source, {2U, 2U}, 0U, {}, {}, classes, 2, nullptr, custody);
 REQUIRE(latest);
 auto invalid = execution;
 invalid.device = std::numeric_limits<int>::max();
 CHECK_THROWS_AS(detail::PredictionPreviewPool(invalid, context), std::invalid_argument);
 std::vector<mmltk::backend::models::rfdetr::Prediction> excessive(contracts::kAnnotationObjectCapacity + 1U);
 readers[2].reset();
 CHECK_THROWS_AS(pool->Capture(source, {2U, 2U}, 0U, excessive, {}, classes, 2, nullptr, custody), std::invalid_argument);
 gpu::DeviceContext other(0, gpu::cuda_image_copy_backend());
 other.Bind();
 CUcontext before = nullptr;
 REQUIRE(cuCtxGetCurrent(&before) == CUDA_SUCCESS);
 latest.reset();
 readers = {};
 pool.reset();
 CUcontext after = nullptr;
 REQUIRE(cuCtxGetCurrent(&after) == CUDA_SUCCESS);
 CHECK(after == before);
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
}
TEST_CASE("prediction transfer faults settle or retain exact source custody", "[controller][gpu]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace controller = mmltk::controller;
 namespace runtime = mmltk::backend::ml::runtime;
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 const auto classes = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"object"});
 const std::array<float, 12U> pixels{};
 auto input = PredictionSource::Device(execution, {2U, 2U}, pixels, {{.class_reference = 0, .score = .7F}}, classes);
 const auto* source = input.pixels();
 auto& custody = input.custody();
 const std::weak_ptr<void> lifetime = custody;
 const auto& annotations = input.annotations();
 const auto& predictions = input.detections();
 gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
 PredictionTransferFault fault;
 const auto operations = PredictionTransferFault::Operations();
 auto pool = std::make_unique<controller::detail::PredictionPreviewPool>(execution, context, operations);
 for (int stage : {1, 2, 3, 4}) {
  fault.Reset({.fail_copy = stage < 4 ? stage : 0, .fail_record = stage == 4});
  CHECK_THROWS_AS(pool->Capture(source, {2, 2}, 0, predictions, annotations, classes, 1, nullptr, custody), std::runtime_error);
  CHECK(fault.settlements == 1);
  CHECK(custody.use_count() == 1);
  fault.Reset();
  auto recovered = pool->Capture(source, {2, 2}, 0, predictions, annotations, classes, 1, nullptr, custody);
  REQUIRE(recovered);
  CHECK(fault.settlements == 0);
 }
 fault.Reset({.fail_copy = 2, .fail_settle = true});
 int stopped = 0;
 CHECK_THROWS_AS(pool->Capture(source, {2, 2}, 0, predictions, annotations, classes, 1, nullptr, custody, &CountPredictionSourceStop, &stopped), runtime::CudaOperationError);
 CHECK(stopped == 1);
 CHECK(pool->HasUnsafeSourceCustody());
 const auto attempted = fault.copies;
 CHECK_THROWS(pool->Capture(source, {2, 2}, 0, predictions, annotations, classes, 1, nullptr, custody));
 CHECK(fault.copies == attempted);
 custody.reset();
 CHECK_FALSE(lifetime.expired());
 pool.reset();
 CHECK_FALSE(lifetime.expired());
 fault.Reset();
}
TEST_CASE("preview slot reuse orders cross-stream writes and preserves fault custody", "[controller][gpu]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace runtime = mmltk::backend::ml::runtime;
 const bool decoded = GENERATE(false, true);
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 cudaStream_t first_raw = nullptr, second_raw = nullptr;
 REQUIRE(cudaStreamCreateWithFlags(&first_raw, cudaStreamNonBlocking) == cudaSuccess);
 std::unique_ptr<std::remove_pointer_t<cudaStream_t>, decltype(&cudaStreamDestroy)> first(first_raw, &cudaStreamDestroy);
 REQUIRE(cudaStreamCreateWithFlags(&second_raw, cudaStreamNonBlocking) == cudaSuccess);
 std::unique_ptr<std::remove_pointer_t<cudaStream_t>, decltype(&cudaStreamDestroy)> second(second_raw, &cudaStreamDestroy);
 const auto classes = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"object"});
 const std::array<float, 12> pixels{};
 const std::array<std::uint8_t, 12> rgb{};
 auto input = PredictionSource::Device(execution, {2U, 2U}, pixels, {{.class_reference = 0}}, classes);
 auto bytes = PredictionSource::Decoded({2U, 2U}, rgb, classes);
 auto custody = std::make_shared<std::pair<std::shared_ptr<void>, std::shared_ptr<void>>>(input.custody(), bytes.custody());
 gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
 PredictionTransferFault fault;
 for (int stage : {0, 1, 2, 3, 4}) {
  fault.Reset();
  mmltk::controller::detail::PredictionPreviewPool pool(execution, context, PredictionTransferFault::Operations(), {}, 1U);
  auto capture = [&](cudaStream_t stream) {
   return pool.Capture(
    decoded ? nullptr : input.pixels(), {2U, 2U}, reinterpret_cast<std::uintptr_t>(stream), input.detections(), input.annotations(), classes, 1, decoded ? bytes.rgb8() : nullptr, custody);
  };
  auto previous = capture(first.get());
  REQUIRE(previous);
  previous.reset();  // The peer copy need not have completed or been drawn.
  fault.Reset({.fail_copy = stage == 2 || stage == 4 ? 1 : 0, .fail_record = stage == 3, .fail_settle = stage == 4, .fail_wait = stage == 1});
  if (stage == 0) {
   REQUIRE(capture(second.get()));
   CHECK(fault.settlements == 0);
  } else if (stage == 4) {
   CHECK_THROWS_AS(capture(second.get()), runtime::CudaOperationError);
   CHECK(pool.HasUnsafeSourceCustody());
  } else {
   CHECK_THROWS_AS(capture(second.get()), std::runtime_error);
   CHECK(fault.settlements == 1);
   CHECK_FALSE(pool.HasUnsafeCustody());
  }
  CHECK(fault.waits == 1);
  CHECK(fault.waited_stream == second.get());
  fault.Reset();
  if (stage != 4) REQUIRE(capture(first.get()));
 }
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
}
TEST_CASE("preview recapture invalidates retained scratch and destination regions", "[controller][gpu]") {
 namespace gpu = mmltk::frameworks::gpu;
 // CLEANUP-IGNORE: An alias and ordinary policy/context construction precede independently owned test resources.
 using Composition = detail::PredictionPreviewComposition;
 const bool decoded = GENERATE(false, true);
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
 PredictionReceiverFault fault;
 ScopedPredictionReceiverFault receiver(fault);
 detail::PredictionPreviewPool pool(execution, context, PredictionReceiverFault::Operations(), {}, 1U);
 gpu::SystemImageRuntime runtime({.device = 0, .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .output_buffer_count = 2U, .adopted_context = context});
 Composition retained;
 const auto classes = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"sample"});
 const detail::PredictionPreviewFrame* slot = nullptr;
 for (std::uint32_t generation = 0U; generation < 3U; ++generation) {
  const auto side = generation == 1U ? 3U : 2U;
  const auto count = side * side;
  std::vector<float> pixels(count * 3U, 0.0F);
  std::fill_n(pixels.begin() + generation * count, count, 1.0F);
  std::vector<std::uint8_t> rgb(count * 3U, 0U);
  for (std::size_t pixel = 0U; pixel < count; ++pixel) rgb[pixel * 3U + generation] = 255U;
  const bool bytes = decoded != (generation == 1U);
  auto source = bytes ? PredictionSource::Decoded({side, side}, rgb, classes) : PredictionSource::Device(execution, {side, side}, pixels, {}, classes);
  auto frame = pool.Capture(source.pixels(), {side, side}, 0U, {}, source.annotations(), classes, 1, source.rgb8(), source.custody(), nullptr, nullptr, {}, true);
  REQUIRE(frame);
  if (slot) CHECK(frame.get() == slot);  // Weak preparation records cannot occupy a raw slot.
  slot = frame.get();
  const std::array regions{Composition::Region{frame, {0U, 0U, 4U, 4U}}};
  for (unsigned publication = 0U; publication < 2U; ++publication) {
   auto candidate = runtime.AcquireOutput();
   Composition::Draw(runtime, candidate, {4U, 4U}, regions, {}, &retained);
   auto completed = runtime.CommitOutput(std::move(candidate));
   auto image = completed.Borrow();
   context.Bind();
   const auto clean = image.plane(0U).plane();
   std::array<std::array<std::uint8_t, 4U>, 16U> actual{};
   REQUIRE(cudaMemcpy2D(actual.data(), 16U, reinterpret_cast<const void*>(clean.data), clean.descriptor.pitch_bytes, 16U, 4U, cudaMemcpyDeviceToHost) == cudaSuccess);
   std::array<std::uint8_t, 4U> expected{0, 0, 0, 255};
   expected[generation] = 255U;
   CHECK(std::ranges::all_of(actual, [&](auto pixel) { return pixel == expected; }));
   CHECK(fault.draws == generation + 1U);
  }
 }
}
TEST_CASE("decoded compact preview retains bytes through retry and release without widening admission", "[controller][gpu]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 using Composition = detail::PredictionPreviewComposition;
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
 PredictionReceiverFault fault;
 ScopedPredictionReceiverFault receiver(fault);
 PredictionTransferFault transfer;
 auto operations = PredictionReceiverFault::Operations();
 operations.copy = PredictionTransferFault::Operations().copy;
 detail::PredictionPreviewPool pool(execution, context, operations, {}, 1U);
 gpu::SystemImageRuntime runtime({.device = 0, .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .adopted_context = context});
 const auto classes = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"object"});
 // Odd 3P puts every typed annotation after required alignment padding.
 constexpr VisualExtent extent{3U, 3U};
 std::array<std::uint8_t, 27U> rgb{};
 for (std::size_t index = 0U; index < rgb.size(); ++index) rgb[index] = static_cast<std::uint8_t>(index * 9U);
 auto decoded = PredictionSource::Decoded(extent, rgb, classes);
 const std::array<float, 27U> unused{};
 const std::array<std::uint8_t, 9U> mask{1, 0, 0, 0, 1, 0, 0, 0, 1};
 auto annotations = PredictionSource::Device(execution, extent, unused, {{.class_reference = 0, .bbox_xyxy = {0, 0, 2, 2}}}, classes, mask);
 auto custody = std::make_shared<std::pair<std::shared_ptr<void>, std::shared_ptr<void>>>(decoded.custody(), annotations.custody());
 std::weak_ptr<void> lifetime = decoded.custody();
 rfdetr::Prediction gt{.class_reference = 0};
 gt.mask.runs.emplace_back(0U, 9U);
 const std::array ground_truth{gt};
 auto frame = pool.Capture(nullptr, extent, 0U, annotations.detections(), annotations.annotations(), classes, 1, decoded.rgb8(), custody, nullptr, nullptr, ground_truth, true);
 REQUIRE(frame);
 custody.reset();
 decoded.custody().reset();
 annotations.custody().reset();
 fault.draw_failures_remaining = 1U;
 {
  auto candidate = runtime.AcquireOutput();
  CHECK_THROWS(frame->Draw(runtime, candidate));
 }
 CHECK_FALSE(lifetime.expired());
 CHECK(fault.uploaded_bytes == rgb.size());
 const auto storage = fault.upload_destination.load();
 REQUIRE(transfer.copies == 3);
 CHECK(transfer.destinations[0] == storage + 28U);
 CHECK(transfer.destinations[1] == storage + 44U);
 CHECK(transfer.destinations[2] == storage + 48U);
 CHECK(transfer.copy_bytes == std::array<std::size_t, 4U>{16U, 4U, 9U, 0U});
 CHECK(transfer.destinations[0] % alignof(float) == 0U);
 CHECK(transfer.destinations[1] % alignof(std::int32_t) == 0U);
 const auto staging = fault.upload_staging.load();
 for (unsigned draw = 0U; draw < 3U; ++draw) {
  auto candidate = runtime.AcquireOutput();
  const std::array regions{Composition::Region{frame, {0U, 0U, 3U, 3U}}};
  Composition::Draw(runtime, candidate, extent, regions, {.prediction_boxes = false, .prediction_masks = true, .ground_truth_masks = true, .complementary_layers = true});
  auto complete = runtime.CommitOutput(std::move(candidate));
  CHECK(lifetime.expired());
  auto image = complete.Borrow();
  context.Bind();
  std::array<std::uint8_t, 36U> pixels{}, semantic{};
  for (std::size_t layer = 0U; layer < 2U; ++layer) {
   const auto plane = image.plane(layer).plane();
   REQUIRE(cudaMemcpy2D(layer == 0U ? pixels.data() : semantic.data(), 12U, reinterpret_cast<void*>(plane.data), plane.descriptor.pitch_bytes, 12U, 3U, cudaMemcpyDeviceToHost) == cudaSuccess);
  }
  for (std::size_t pixel = 0U; pixel < 9U; ++pixel) {
   for (std::size_t channel = 0U; channel < 3U; ++channel) CHECK(pixels[pixel * 4U + channel] == rgb[pixel * 3U + channel]);
   CHECK(pixels[pixel * 4U + 3U] == 255U);
   CHECK(semantic[pixel * 4U + 3U] == 96U);
   if (mask[pixel])
    for (std::size_t channel = 0U; channel < 3U; ++channel) CHECK(semantic[pixel * 4U + channel] == 255U);
  }
 }
 CHECK(fault.uploads == 3U);  // RGB failure, RGB retry, and once-only ground truth.
 CHECK(fault.uploaded_bytes == rgb.size() * 2U + 2U * sizeof(std::uint32_t));
 CHECK(fault.upload_destination == storage + 60U);  // 28 + 16 + 4 + 9 + 3, already word-aligned.
 CHECK(fault.upload_destination.load() % alignof(std::uint32_t) == 0U);
 // Pinned storage may grow for GT, but repeated settled draws never upload again.
 CHECK(staging != 0U);
 const auto retained_staging = fault.upload_staging.load() - rgb.size();
 frame.reset();
 auto tiny = PredictionSource::Decoded({1U, 1U}, std::array<std::uint8_t, 3U>{7U, 31U, 129U});
 auto next = pool.Capture(nullptr, {1U, 1U}, 0U, {}, {}, tiny.classes(), 0, tiny.rgb8(), tiny.custody());
 REQUIRE(next);
 {
  auto candidate = runtime.AcquireOutput();
  next->Draw(runtime, candidate);
 }
 CHECK(fault.upload_destination == storage);  // Existing raw high-water storage is reused.
 CHECK(fault.upload_staging == retained_staging);
 next.reset();
 // No GPU read is reachable for either former refusal: 12P first, then the
 // complete former 12P + annotations + scratch aggregate, even though RGB fits.
 CHECK_THROWS_AS(pool.Capture(nullptr, {89478486U, 1U}, 0U, {}, {}, tiny.classes(), 0, tiny.rgb8(), tiny.custody()), std::invalid_argument);
 CHECK_THROWS_AS(pool.Capture(nullptr, {60000000U, 1U}, 0U, {}, {}, tiny.classes(), 0, tiny.rgb8(), tiny.custody(), nullptr, nullptr, {}, true), std::invalid_argument);
 CHECK_THROWS_AS(pool.Capture(nullptr, {83000000U, 1U}, 0U, annotations.detections(), annotations.annotations(), classes, 1, tiny.rgb8(), tiny.custody()), std::invalid_argument);
 CHECK(fault.uploads == 4U);
}
TEST_CASE("ordinary preview allocation refusal leaves its decoded source intact", "[controller][gpu]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace controller = mmltk::controller;
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
 const auto operations = RefusePinnedRegistration();
 controller::detail::PredictionPreviewPool pool(execution, context, operations);
 const std::array<std::uint8_t, 12U> pixels{255, 0, 0};
 auto input = PredictionSource::Decoded({2U, 2U}, pixels);
 auto& source = input.custody();
 const auto& classes = input.classes();
 auto raw = pool.Capture(nullptr, input.extent(), 0, {}, {}, classes, 1, input.rgb8(), source);
 REQUIRE(raw);
 CHECK(source.use_count() == 2);
 gpu::SystemImageRuntime runtime({.device = 0, .context_mode = gpu::DeviceContextMode::Isolated, .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .adopted_context = context});
 auto candidate = runtime.AcquireOutput();
 CHECK_THROWS(raw->Draw(runtime, candidate));
 CHECK(input.rgb8()[0] == 255U);
 candidate = {};
 controller::detail::PredictionPreviewPool healthy(execution, context);
 auto recovered = healthy.Capture(nullptr, {2, 2}, 0, {}, {}, classes, 1, input.rgb8(), source);
 REQUIRE(recovered);
 auto output = runtime.AcquireOutput();
 recovered->Draw(runtime, output);
 const auto completed = runtime.CommitOutput(std::move(output));
 auto view = completed.Borrow();
 const auto plane = view.plane(0U).plane();
 std::array<std::uint8_t, 16> rgba{};
 REQUIRE(cudaMemcpy2D(rgba.data(), 8U, reinterpret_cast<const void*>(plane.data), plane.descriptor.pitch_bytes, 8U, 2U, cudaMemcpyDeviceToHost) == cudaSuccess);
 CHECK(rgba[0] == 255U);
 CHECK(rgba[1] == 0U);
 CHECK(rgba[2] == 0U);
 CHECK(rgba[3] == 255U);
}
TEST_CASE("preview context failure retains initialized state and source before returning", "[controller][gpu][context]") {
 namespace gpu = mmltk::frameworks::gpu;
 const bool query_failure = GENERATE(false, true);
 const bool terminal = GENERATE(false, true);
 enum class Stage { Capture, Draw, Destruction };
 const auto stage = GENERATE(Stage::Capture, Stage::Draw, Stage::Destruction);
 PredictionContextFault driver{query_failure ? PredictionContextFault::Failure::Query : terminal ? PredictionContextFault::Failure::RestoreAlways : PredictionContextFault::Failure::RestoreOnce};
 const auto api = driver.Api();
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
 auto authority = std::make_shared<gpu::TerminalCudaRetirementOwner>(detail::PredictionPreviewPool::kSlotCapacity);
 detail::PredictionPreviewPool::TransferOperations operations{&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister};
 operations.context_api = api;
 auto pool = std::make_unique<detail::PredictionPreviewPool>(execution, context, operations, authority);
 const std::array<std::uint8_t, 12U> pixels{};
 auto input = PredictionSource::Decoded({2U, 2U}, pixels);
 auto& decoded = input.custody();
 const std::weak_ptr<void> retained = decoded;
 const auto& classes = input.classes();
 int stopped = 0;
 const auto capture = [&] { return pool->Capture(nullptr, {2U, 2U}, 0U, {}, {}, classes, 1, input.rgb8(), decoded, &CountPredictionSourceStop, &stopped); };
 const mmltk::testsupport::ScopedTestCleanup disarm{[&] { driver.armed = false; }};
 if (stage == Stage::Destruction) {
  auto raw = capture();
  REQUIRE(raw);
 }
 const bool unsafe = query_failure || terminal;
 if (stage == Stage::Draw) {
  auto raw = capture();
  REQUIRE(raw);
  gpu::SystemImageRuntime runtime({.device = 0, .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .adopted_context = context});
  auto candidate = runtime.AcquireOutput();
  const mmltk::testsupport::ScopedTestCleanup disarm_draw{[&] { driver.armed = false; }};
  driver.armed = true;
  if (unsafe) {
   CHECK_THROWS_AS(raw->Draw(runtime, candidate), gpu::ImageStreamExecutionFailure);
   CHECK_THROWS(runtime.BeginWork());
   CHECK_FALSE(runtime.Retire().safe_to_destroy);
  } else {
   CHECK_THROWS_AS(raw->Draw(runtime, candidate), gpu::CudaContextFailure);
   CHECK_NOTHROW(runtime.BeginWork());
  }
  driver.armed = false;
  raw.reset();
  pool.reset();
 } else if (stage == Stage::Destruction) {
  driver.armed = true;
  pool.reset();
 } else {
  driver.armed = true;
  if (unsafe)
   CHECK_THROWS_AS(capture(), mmltk::backend::ml::runtime::CudaOperationError);
  else
   CHECK_THROWS_AS(capture(), gpu::CudaContextFailure);
  CHECK(stopped == (unsafe ? 1 : 0));
  if (unsafe) {
   const auto calls = driver.calls;
   CHECK_THROWS(capture());
   CHECK(driver.calls == calls);
  }
  pool.reset();
 }
 CHECK(authority->admission_open() == !unsafe);
 CHECK(authority->fact().occupancy == (unsafe ? 1U : 0U));
 decoded.reset();
 // A destructor restore failure occurs after settled source custody releases.
 if (stage == Stage::Capture || query_failure) CHECK(retained.expired() == !unsafe);
 driver.armed = false;
}
TEST_CASE("Predict replacement pressure coalesces without overwriting its selected image", "[controller][systems][predict][gpu]") {
 ApplicationDataFixture fixture{mmltk::testsupport::make_temp_root("predict-replacement-pressure")};
 fixture.PrepareModel(contracts::FeatureId::Predict);
 auto [settings, dataset, model] = fixture.systems();
 auto gate = std::make_shared<mmltk::testsupport::StopGate>();
 gate->Release();
 auto fault = std::make_shared<PredictionReceiverFault>();
 const ScopedPredictionReceiverFault registration{*fault};
 auto source_index = std::make_shared<std::atomic_int64_t>(0);
 std::array<std::promise<void>, 6U> done;
 std::array<std::promise<void>, 3U> images;
 std::promise<void> failed;
 std::atomic_size_t published = 0U;
 std::atomic_uint64_t last_revision = 0U;
 PredictSystem prediction{settings, dataset, model, {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
  [&] { return std::make_unique<FakePredictRuntime>(PredictionScenario{.compute = {.gate = gate}, .source_index = source_index, .labels = 1U}); },
  [&](PredictSystem::event_type event) {
   if (const auto* changed = std::get_if<PredictChanged>(&event)) {
    const auto& snapshot = changed->snapshot;
    if (!snapshot.operation.active && snapshot.operation.generation_frontier <= done.size()) mmltk::testsupport::release_test_promise(done[snapshot.operation.generation_frontier - 1U]);
    if (snapshot.frame.valid() && snapshot.frame.revision > last_revision.load()) {
     last_revision = snapshot.frame.revision;
     const auto index = published.fetch_add(1U);
     if (index < images.size()) mmltk::testsupport::release_test_promise(images[index]);
    }
   }
   if (std::holds_alternative<PredictFailed>(event)) mmltk::testsupport::release_test_promise(failed);
  }};
 const mmltk::testsupport::ScopedTestCleanup stop{[&] { prediction.Shutdown(); }};
 const auto run = [&](std::size_t index) {
  source_index->store(static_cast<std::int64_t>(index));
  static_cast<void>(prediction.Start({}));
  mmltk::testsupport::await_test_promise(done[index], "Predict semantic completion under display pressure");
 };
 run(0U);
 mmltk::testsupport::await_test_promise(images[0U], "first Predict slot");
 auto old_reader = prediction.BorrowFrame();
 REQUIRE(old_reader.valid());
 run(1U);
 mmltk::testsupport::await_test_promise(images[1U], "selected Predict slot");
 const auto selected = prediction.snapshot();
 const auto pixels = [&] {
  auto borrowed = prediction.BorrowFrame();
  REQUIRE(borrowed.valid());
  const auto plane = borrowed.plane(0U).plane();
  borrowed.plane(0U).context().Bind();
  std::array<std::uint8_t, 64U> result{};
  REQUIRE(cudaMemcpy2D(result.data(), 16U, reinterpret_cast<void*>(plane.data), plane.descriptor.pitch_bytes, 16U, 4U, cudaMemcpyDeviceToHost) == cudaSuccess);
  return result;
 };
 const auto selected_pixels = pixels();
 const auto check_metadata = [&] {
  const auto metadata = prediction.ImageSnapshot(selected.frame);
  REQUIRE(metadata);
  CHECK(metadata->frame == selected.frame);
  CHECK(metadata->content_identity == selected.content_identity);
  CHECK(metadata->image_id == selected.image_id);
  REQUIRE(metadata->labels.size() == selected.labels.size());
  for (std::size_t index = 0U; index < selected.labels.size(); ++index) {
   CHECK(metadata->labels[index].name == selected.labels[index].name);
   CHECK(metadata->labels[index].box == selected.labels[index].box);
   CHECK(metadata->labels[index].confidence == selected.labels[index].confidence);
   CHECK(metadata->labels[index].class_reference == selected.labels[index].class_reference);
   CHECK(metadata->labels[index].class_domain == selected.labels[index].class_domain);
   CHECK(metadata->labels[index].color == selected.labels[index].color);
  }
 };
 fault->partial_draw = true;
 run(2U);
 run(3U);  // bounded latest-product replacement while the other output is held
 CHECK(published == 2U);
 CHECK(prediction.snapshot().frame == selected.frame);
 check_metadata();
 CHECK(pixels() == selected_pixels);
 old_reader = {};
 mmltk::testsupport::await_test_promise(failed, "partial replacement draw failure after reader release");
 CHECK(prediction.snapshot().frame == selected.frame);
 check_metadata();
 CHECK(pixels() == selected_pixels);
 fault->partial_draw = false;
 old_reader = prediction.BorrowFrame();
 run(4U);
 mmltk::testsupport::await_test_promise(images[2U], "successful replacement recovery");
 CHECK(prediction.snapshot().content_identity > selected.content_identity);
 CHECK(prediction.snapshot().frame.revision > selected.frame.revision);
 CHECK(prediction.BorrowFrame().valid());
 gate->Reset();
 static_cast<void>(prediction.Start({}));
 auto stopping = std::async(std::launch::async, [&] { return prediction.Stop({}); });
 static_cast<void>(mmltk::testsupport::await_test_future(stopping, "Stop while old output remains borrowed"));
 mmltk::testsupport::await_test_promise(done[5U], "cancelled Predict execution with both display roles occupied");
 CHECK_FALSE(prediction.snapshot().operation.active);
 CHECK(prediction.snapshot().operation.terminal.outcome == contracts::ComputeOperationOutcome::Cancelled);
}
TEST_CASE("preview context construction publishes only after exact caller restoration", "[controller][gpu][context]") {
 namespace gpu = mmltk::frameworks::gpu;
 using Failure = PredictionContextFault::Failure;
 const auto failure = GENERATE(Failure::None, Failure::Query, Failure::RestoreOnce, Failure::RestoreAlways);
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 int devices = 0;
 REQUIRE(cudaGetDeviceCount(&devices) == cudaSuccess);
 gpu::DeviceContext caller(devices > 1 ? 1 : 0, gpu::cuda_image_copy_backend());
 caller.Bind();
 PredictionContextFault driver{failure};
 driver.armed = true;
 driver.observe_candidate = true;
 CUcontext previous{};
 REQUIRE(cuCtxGetCurrent(&previous) == CUDA_SUCCESS);
 const auto api = driver.Api();
 auto authority = std::make_shared<gpu::TerminalCudaRetirementOwner>(detail::PredictionPreviewPool::kSlotCapacity + 3U);
 // Existing current, in-flight and pending frames can reserve all five other slots.
 std::array<gpu::TerminalCudaRetirementLease, detail::PredictionPreviewPool::kSlotCapacity + 2U> frames;
 for (auto& lease : frames) lease = gpu::ReserveTerminalCudaLease(*authority);
 std::optional<gpu::DeviceContext> published;
 const auto create = [&] { published = detail::CreatePredictionPreviewContext(execution, authority, api); };
 if (failure == Failure::None)
  CHECK_NOTHROW(create());
 else
  CHECK_THROWS_AS(create(), gpu::CudaContextFailure);
 const bool terminal = failure == Failure::Query || failure == Failure::RestoreAlways;
 CHECK(published.has_value() == (failure == Failure::None));
 CHECK(authority->admission_open() == !terminal);
 CHECK(authority->fact().occupancy == (terminal ? 1U : 0U));
 CHECK(authority->fact().reservations == frames.size());
 CUcontext restored{};
 REQUIRE(cuCtxGetCurrent(&restored) == CUDA_SUCCESS);
 CHECK(restored == previous);
 if (failure == Failure::RestoreAlways) {
  REQUIRE(driver.candidate != previous);
  unsigned version{};
  CHECK(cuCtxGetApiVersion(driver.candidate, &version) == CUDA_SUCCESS);
 }
}
}  // namespace
}  // namespace mmltk::controller
