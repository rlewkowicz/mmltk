#include "src/controller/subsystems/system/tests/prediction_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/controller/subsystems/validate/detail/validation_samples.h"
#include "src/controller/subsystems/validate/detail/validation_sample_output.h"
#include "src/backend/imaging/raster/detail/checked_png.h"
#include <cstdio>
#include <iterator>
#include <stb_image.h>
#include <stb_image_write.h>
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <utility>
#include <catch2/generators/catch_generators.hpp>
#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/controller/subsystems/system/tests/application_data_test_support.h"
#include <future>
#include <fstream>
#include <limits>
#include <cmath>
using namespace mmltk::controller::test_support;
namespace {
void check_validation_png_pixels(const std::filesystem::path& path) {
 int width = 0, height = 0, channels = 0;
 std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load(path.c_str(), &width, &height, &channels, 4), stbi_image_free);
 REQUIRE(pixels);
 CHECK(width == 32);
 CHECK(height == 24);
 CHECK(pixels.get()[0] == 64U);
}
}  // namespace
namespace mmltk::controller {
namespace {
class AdmittedValidationRuntime final : public ValidationRuntime {
public:
 ValidationRuntimeResult Run(
  mmltk::backend::models::rfdetr::ValidateRequest request, std::stop_token, const ComputeProgressSink&, const mmltk::backend::models::rfdetr::ValidationDelivery& delivery, std::uint64_t) override {
  auto facts = mmltk::backend::models::rfdetr::derive_execution_facts(request, 0);
  facts.admitted_capacity = 1;
  delivery.admitted(facts);
  delivery.admitted(facts);
  return {.terminal = contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded)};
 }
};
TEST_CASE("Validation retains stable admitted capacity independently of visual revisions", "[controller][validation]") {
 ApplicationDataFixture fixture{mmltk::testsupport::make_temp_root("validation-capacity-facts")};
 fixture.PrepareModel(contracts::FeatureId::Validate);
 auto [settings, dataset, model] = fixture.systems();
 const auto revision = settings.snapshot().revision;
 std::promise<void> settled;
 ValidationSystem validation{settings, dataset, model, [](DirectComputeConfiguration) { return std::make_unique<AdmittedValidationRuntime>(); },
  [&](ValidationSystem::event_type event) {
   if (const auto* changed = std::get_if<ValidationChanged>(&event); changed && !changed->snapshot.operation.active && changed->snapshot.operation.generation_frontier)
    mmltk::testsupport::release_test_promise(settled);
  },
  [](int, int) { return DirectComputeConfiguration{}; }};
 const auto frame = validation.snapshot().frame;
 static_cast<void>(validation.Start({}));
 mmltk::testsupport::await_test_promise(settled, "validation admitted capacity");
 const auto snapshot = validation.snapshot();
 CHECK(snapshot.execution.admitted_capacity == 1);
 CHECK(snapshot.execution.settings_revision == revision);
 CHECK(snapshot.execution.operation_generation == snapshot.operation.generation_frontier);
 CHECK(snapshot.frame == frame);
 CHECK(snapshot.operation.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
}
template <class Predicate>
void await_validation(std::mutex& mutex, std::condition_variable& changed, Predicate predicate) {
 std::unique_lock lock(mutex);
 REQUIRE(changed.wait_for(lock, std::chrono::seconds(20), predicate));
}
auto borrow_validation_frame(detail::ValidationSamples& samples) {
 auto image = samples.BorrowFrame();
 REQUIRE(image.valid());
 auto metadata = samples.ImageSnapshot(samples.snapshot().frame);
 REQUIRE(metadata);
 return std::pair{std::move(image), std::move(metadata)};
}
std::array<std::uint8_t, 4U> validation_tile_pixel(const mmltk::frameworks::gpu::BorrowedImageProductReadView& image, VisualRegion crop) {
 const auto& read = image.plane(0U);
 read.context().Bind();
 const auto plane = read.plane();
 std::array<std::uint8_t, 4U> pixel{};
 REQUIRE(cudaMemcpy(pixel.data(), reinterpret_cast<const void*>(plane.data + (crop.y + crop.height / 2U) * plane.descriptor.pitch_bytes + (crop.x + crop.width / 2U) * 4U), 4U,
          cudaMemcpyDeviceToHost) == cudaSuccess);
 return pixel;
}
}  // namespace
// CLEANUP-IGNORE: Independent custody test preamble; aliases and one execution-policy call are not a shared algorithm.
TEST_CASE("composed preview retains every source after the outer draw callback", "[controller][gpu][validation]") {
 namespace gpu = mmltk::frameworks::gpu;
 using Composition = detail::PredictionPreviewComposition;
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 const bool outer = GENERATE(false, true);
 std::array<std::weak_ptr<void>, 6> sources;
 std::array<std::weak_ptr<const detail::PredictionPreviewFrame>, 6> frames;
 PredictionSettlementFault fault;
 {
  PredictionReceiverFault receiver_fault;
  receiver_fault.terminal = true;
  receiver_fault.fail_upload_at = 3U;
  ScopedPredictionReceiverFault receiver(receiver_fault);
  auto backend = fault.Backend();
  gpu::DeviceContext context(0, backend, gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
  auto retirement = std::make_shared<gpu::TerminalCudaRetirementOwner>(9U);
  detail::PredictionPreviewPool pool(execution, context, PredictionReceiverFault::Operations(), retirement, 7U);
  gpu::SystemImageRuntime runtime({.device = 0,
   .backend = backend,
   .output_layout = gpu::ImageProductLayout::CleanAndSemantic,
   .output_buffer_count = 2U,
   .numa_node = execution.placement.numa_node,
   .execution = execution,
   .adopted_context = context});
  const std::array<std::uint8_t, 12> pixels{255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0};
  std::array<Composition::Region, 6> regions;
  for (std::size_t index = 0U; index < regions.size(); ++index) {
   auto source = PredictionSource::Decoded({2U, 2U}, pixels);
   sources[index] = source.custody();
   auto frame = pool.Capture(nullptr, {2U, 2U}, 0U, {}, source.annotations(), source.classes(), 0, source.rgb8(), source.custody(), nullptr, nullptr, {}, true);
   REQUIRE(frame);
   frames[index] = frame;
   regions[index] = {std::move(frame), {static_cast<std::uint32_t>(index % 3U) * 2U, static_cast<std::uint32_t>(index / 3U) * 2U, 2U, 2U}};
  }
  auto candidate = runtime.AcquireOutput();
  Composition retained_preparation;
  fault.enabled = outer;
  receiver_fault.enabled = !outer;
  auto draw = std::async(std::launch::async, [&] { Composition::Draw(runtime, candidate, {6U, 4U}, regions, {}, &retained_preparation); });
  auto& failure_gate = outer ? fault.settlement : receiver_fault.upload;
  mmltk::testsupport::ScopedTestCleanup release([&] { failure_gate.Release(); });
  REQUIRE(failure_gate.WaitEntered(std::chrono::seconds(20)));
  CHECK(fault.callback_returned == outer);  // Event recording follows the complete outer callback.
  CHECK(receiver_fault.uploads == (outer ? 6U : 3U));
  for (const auto& source : sources) CHECK_FALSE(source.expired());
  auto extra = PredictionSource::Decoded({2U, 2U}, pixels);
  auto concurrent = pool.Capture(nullptr, {2U, 2U}, 0U, {}, extra.annotations(), extra.classes(), 0, extra.rgb8(), extra.custody(), nullptr, nullptr, {}, true);
  REQUIRE(concurrent);  // Source delivery continues while the independent draw owns its set.
  CHECK_FALSE(pool.Capture(nullptr, {2U, 2U}, 0U, {}, extra.annotations(), extra.classes(), 0, extra.rgb8(), extra.custody(), nullptr, nullptr, {}, true));
  failure_gate.Release();
  CHECK_THROWS(draw.get());
  CHECK(pool.HasUnsafeCustody());
  CHECK(retirement->fact().occupancy == 1U);
  regions = {};
  for (const auto& frame : frames) CHECK_FALSE(frame.expired());
  for (const auto& source : sources) CHECK_FALSE(source.expired());
  for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
   CHECK_THROWS(pool.Capture(nullptr, {2U, 2U}, 0U, {}, extra.annotations(), extra.classes(), 0, extra.rgb8(), extra.custody(), nullptr, nullptr, {}, true));
   CHECK(retirement->fact().occupancy == 1U);
  }
  // Replacement contexts cannot read the terminal set, even in the same run.
  gpu::DeviceContext replacement_context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
  gpu::SystemImageRuntime replacement({.device = 0, .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .adopted_context = replacement_context});
  for (const auto& frame : frames) CHECK_FALSE(frame.lock()->CompatibleWith(replacement));
 }
 // Check after pool, output runtime and source/context wrappers are destroyed.
 for (const auto& frame : frames) CHECK_FALSE(frame.expired());
 for (const auto& source : sources) CHECK_FALSE(source.expired());
 CHECK(fault.streams_created == 1U);
 CHECK(fault.streams_destroyed == 0U);
}
TEST_CASE("validation admits asynchronous selected-path inspection and cancels before compute", "[controller][systems][compute][admission]") {
 const auto root = mmltk::testsupport::make_temp_root("validation-inspection-admission");
 ApplicationDataFixture fixture{root};
 fixture.PrepareModel(contracts::FeatureId::Validate);
 auto [settings, unused_dataset, model] = fixture.systems();
 auto observation = std::make_shared<DatasetRuntimeObservation>();
 DatasetSystem dataset{settings, [observation] { return std::make_unique<BlockingInspectRuntime>(observation); }};
 auto gate = std::make_shared<mmltk::testsupport::StopGate>();
 std::atomic_size_t constructions = 0;
 std::promise<ValidationSystem::event_type> settled;
 ValidationSystem validation{settings, dataset, model,
  [&](DirectComputeConfiguration) {
   ++constructions;
   return std::make_unique<FakeNonvisualComputeRuntime>(ComputeScenario{.gate = gate});
  },
  [&](ValidationSystem::event_type event) {
   if (std::holds_alternative<ValidationChanged>(event)) settled.set_value(std::move(event));
  },
  [](int, int) { return DirectComputeConfiguration{}; }};
 const auto admitted = validation.Start({});
 CHECK(admitted.operation.active);
 observation->inspect_started.get_future().wait();
 CHECK(constructions == 0U);
 CHECK(validation.snapshot().operation.output.directory.empty());
 CHECK_FALSE(std::filesystem::exists(settings.snapshot().settings_state.workflows.validate.output.directory));
 CHECK_THROWS_AS(dataset.Compile({}), contracts::BusyError);
 static_cast<void>(validation.Stop());
 const auto terminal = settled.get_future().get();
 CHECK(std::get<ValidationChanged>(terminal).snapshot.operation.terminal.outcome == contracts::ComputeOperationOutcome::Cancelled);
 CHECK(constructions == 0U);
 CHECK(observation->compile_calls == 0);
}
TEST_CASE("display settings install quietly before any validation atlas or GPU device exists", "[controller][validation]") {
 std::atomic_size_t notifications = 0U;
 detail::ValidationSamples samples({}, [&] { ++notifications; });
 samples.SetDisplay({0.437F});
 samples.SetDisplay({1.0F});
 CHECK(samples.snapshot().frame.revision == 0U);
 CHECK(notifications == 0U);
 samples.Shutdown();
 CHECK(notifications == 0U);
}
TEST_CASE("validation retains the limited sample atlas and selects detail without a producer", "[controller][gpu][validation]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 const bool labelled = GENERATE(false, true);
 const auto sample_count = GENERATE(std::size_t{2U}, std::size_t{6U});
 const auto close_stage = GENERATE(0U, 1U, 2U);
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 std::mutex mutex;
 std::condition_variable changed;
 PredictionReceiverFault fault;
 ScopedPredictionReceiverFault receiver(fault);
 std::size_t notifications = 0U;
 detail::ValidationSamples samples(
  {.device = 0, .maximum_width = 768U, .maximum_height = 512U},
  [&] {
   std::scoped_lock lock(mutex);
   ++notifications;
   changed.notify_all();
  },
  PredictionReceiverFault::Operations());
 const std::array<std::uint32_t, 6> indices{1U, 3U, 4U, 5U, 8U, 9U};
 const auto selected_indices = std::span(indices).first(sample_count);
 samples.Begin(7U, selected_indices);
 const auto classes = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"original"});
 const std::array<float, 12> pixels{1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
 std::vector<rfdetr::Prediction> detections;
 if (labelled) detections.push_back({.class_reference = 0, .score = 0.5F, .bbox_xyxy = {0, 0, 1, 1}});
 auto source = PredictionSource::Device(execution, {2U, 2U}, pixels, detections, classes);
 const auto capture_initial = [&](std::uint32_t index) {
  const rfdetr::PredictionRecord record{.dataset_index = index, .detections = source.detections()};
  samples.Capture(7U, {record, {.chw = source.pixels(), .width = 2U, .height = 2U, .device = 0, .custody = source.custody()}, source.annotations(), source.detections()});
 };
 capture_initial(selected_indices.front());
 await_validation(mutex, changed, [&] { return samples.snapshot().sample_available[0]; });
 auto first_atlas_reader = samples.BorrowFrame();
 REQUIRE(first_atlas_reader.valid());
 samples.Select({7U, selected_indices.front()});
 await_validation(mutex, changed, [&] { return samples.snapshot().detail; });
 auto detail_reader = samples.BorrowFrame();
 REQUIRE(detail_reader.valid());
 const auto close_progressive = [&] {
  const auto closing = samples.snapshot();
  samples.CloseDetail();
  CHECK(samples.snapshot().frame == closing.frame);  // Both physical outputs are still borrowed.
  first_atlas_reader = {};
  await_validation(mutex, changed, [&] { return !samples.snapshot().detail; });
  detail_reader = {};
 };
 if (close_stage == 0U) close_progressive();
 for (const auto index : selected_indices.subspan(1)) capture_initial(index);
 if (close_stage == 2U) {
  samples.Settle(7U, true);
  // Refuse both automatic attempts after settlement, while the displayed
  // detail still has the first capture's immutable membership.
  first_atlas_reader = {};
  const auto applied = samples.snapshot();
  const auto refuse_edit = [&](auto edit) {
   std::size_t target = 0U;
   {
    std::scoped_lock lock(mutex);
    target = notifications + 2U;
   }
   const auto semantic_writes = fault.semantic_writes.load();
   fault.partial_semantic = true;
   edit();
   await_validation(mutex, changed, [&] { return notifications >= target; });
   CHECK(fault.semantic_writes == semantic_writes + 2U);
   CHECK(samples.snapshot().frame == applied.frame);
   CHECK(samples.snapshot().document == applied.document);
   REQUIRE(samples.ImageSnapshot(applied.frame));
   CHECK(samples.ImageSnapshot(applied.frame)->display.confidence_threshold == 0.4F);
   CHECK(samples.snapshot().overlays == applied.overlays);
  };
  refuse_edit([&] { samples.SetOverlays({false, false, false, false}); });
  CHECK(samples.snapshot().overlay_selection.value == applied.overlays);
  refuse_edit([&] { samples.SetDisplay({0.437F}); });
  fault.partial_semantic = false;
  samples.SetDisplay({0.437F});  // Explicit retry requires no new Capture.
  await_validation(mutex, changed, [&] { return samples.snapshot().frame.revision > applied.frame.revision; });
  CHECK(samples.ImageSnapshot(samples.snapshot().frame)->display.confidence_threshold == 0.437F);
  first_atlas_reader = std::move(detail_reader);
  detail_reader = samples.BorrowFrame();
  REQUIRE(detail_reader.valid());
 }
 if (close_stage != 0U) close_progressive();
 await_validation(mutex, changed, [&] {
  const auto snapshot = samples.snapshot();
  return std::cmp_equal(std::count(snapshot.sample_available.begin(), snapshot.sample_available.end(), true), sample_count);
 });
 if (close_stage == 2U) {
  const auto revision = samples.snapshot().frame.revision;
  samples.SetDisplay({0.4F});
  await_validation(mutex, changed, [&] { return samples.snapshot().frame.revision > revision; });
 }
 const auto atlas = samples.snapshot();
 const auto check_retained_atlas = [&] {
  const auto current = samples.snapshot();
  CHECK(current.content_identity == atlas.content_identity);
  CHECK(current.frame.clean_revision == atlas.frame.clean_revision);
 };
 CHECK_FALSE(atlas.detail);
 CHECK(atlas.frame.extent.width * 9U == atlas.frame.extent.height * 8U);
 const auto atlas_metadata = samples.ImageSnapshot(atlas.frame);
 REQUIRE(atlas_metadata);
 const auto cell_width = atlas.frame.extent.width / 2U;
 const auto cell_height = atlas.frame.extent.height / 3U;
 CHECK(cell_width * 3U == cell_height * 4U);
 for (std::size_t index = 0; index < sample_count; ++index) {
  const auto& crop = atlas_metadata->samples[index].crop;
  CHECK(crop.width == crop.height);
  CHECK(crop.x == (index % 2U) * cell_width + (cell_width - crop.width) / 2U);
  CHECK(crop.y == (index / 2U) * cell_height);
 }
 {
  auto image = samples.BorrowFrame();
  REQUIRE(image.valid());
  for (std::size_t index = 0; index < sample_count; ++index) CHECK(validation_tile_pixel(image, atlas_metadata->samples[index].crop) == std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U});
 }
 const auto unchanged_revision = samples.snapshot().frame.revision;
 samples.CloseDetail();
 CHECK(samples.snapshot().frame.revision == unchanged_revision);
 CHECK(std::cmp_equal(std::count(atlas.sample_available.begin(), atlas.sample_available.end(), true), sample_count));
 REQUIRE(samples.ImageSnapshot(atlas.frame));
 CHECK(samples.ImageSnapshot(atlas.frame)->samples[0].labels.empty() == !labelled);
 if (labelled) {
  const auto& labels = atlas_metadata->samples[0].labels;
  REQUIRE(labels.size() == 2U);
  CHECK(labels[0].name == "original");
  CHECK_FALSE(labels[0].ground_truth);
  CHECK(labels[1].ground_truth);
  for (std::size_t channel = 0; channel < 3U; ++channel) CHECK(unsigned(labels[0].rgb[channel]) + labels[1].rgb[channel] == 255U);
 }
 CHECK_THROWS_AS(samples.Select({6U, 1U}), contracts::InvalidIntentError);
 CHECK_THROWS_AS(samples.Select({7U, 2U}), contracts::InvalidIntentError);
 samples.Select({7U, 3U});
 await_validation(mutex, changed, [&] { return samples.snapshot().detail; });
 const auto detail = samples.snapshot();
 CHECK(detail.content_identity != atlas.content_identity);
 CHECK(detail.frame.extent == VisualExtent{2U, 2U});
 auto document = samples.BorrowDocument(detail.frame);
 REQUIRE(document.valid());
 CHECK(document.document->facts() == detail.document);
 CHECK(document.document->scene.frame_index == 3U);
 CHECK(document.document->scene.categories[0].value == "original");
 CHECK(document.document->scene.valid());
 for (const auto& object : document.document->scene.objects) {
  CHECK(object.name.valid());
  CHECK(object.name.view() == "object 1");
 }
 REQUIRE(document.image_metadata);
 document.pixels = {};
 auto retained = samples.BorrowFrame();
 REQUIRE(retained.valid());
 samples.Settle(7U, true);
 samples.Begin(8U, selected_indices);  // Empty newer run cannot replace the retained detail's atlas.
 source = PredictionSource::Device(execution, {2U, 2U}, pixels, detections, std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"replacement"}));
 samples.CloseDetail();
 await_validation(mutex, changed, [&] { return !samples.snapshot().detail; });
 check_retained_atlas();
 if (labelled) CHECK(samples.ImageSnapshot(samples.snapshot().frame)->samples[0].labels[0].name == "original");
 auto atlas_reader = samples.BorrowFrame();
 REQUIRE(atlas_reader.valid());
 const auto before = samples.snapshot();
 samples.SetOverlays({false, false, false, false});
 CHECK(samples.snapshot().frame == before.frame);  // Both outputs have physical readers.
 CHECK(samples.snapshot().overlays == before.overlays);
 CHECK(samples.snapshot().overlay_selection.value == ValidationOverlays{false, false, false, false});
 CHECK(samples.snapshot().overlay_selection.revision > before.overlay_selection.revision);
 retained = {};
 await_validation(mutex, changed, [&] { return samples.snapshot().frame.revision > before.frame.revision; });
 check_retained_atlas();
 CHECK(samples.snapshot().overlays == ValidationOverlays{false, false, false, false});
 CHECK(samples.ImageSnapshot(samples.snapshot().frame)->overlays == samples.snapshot().overlays);
 atlas_reader = {};
 samples.Select({7U, 1U});
 await_validation(mutex, changed, [&] { return samples.snapshot().detail; });
 samples.Begin(9U, selected_indices);
 const auto capture = [&](std::uint32_t index) {
  const rfdetr::PredictionRecord record{.dataset_index = index, .detections = source.detections()};
  samples.Capture(9U, {record, {.chw = source.pixels(), .width = 2U, .height = 2U, .device = 0, .custody = source.custody()}, source.annotations(), source.detections()});
 };
 capture(1U);  // A partial newer set stays pending while old detail is selected.
 samples.CloseDetail();
 await_validation(mutex, changed, [&] { return !samples.snapshot().detail; });
 check_retained_atlas();
 for (const auto overlays : {ValidationOverlays{true, true, true, true, false, true}, ValidationOverlays{true, true, true, true, true, false}, ValidationOverlays{true, true, true, true, false, false},
       ValidationOverlays{true, false, false, false}, ValidationOverlays{false, true, false, false}, ValidationOverlays{false, false, true, false}, ValidationOverlays{false, false, false, true}}) {
  const auto revision = samples.snapshot().frame.revision;
  samples.SetOverlays(overlays);
  await_validation(mutex, changed, [&] { return samples.snapshot().frame.revision > revision; });
  check_retained_atlas();
  CHECK(samples.snapshot().overlays == overlays);
 }
 const auto preserved = samples.snapshot();
 std::size_t failure_notifications = 0U;
 {
  std::scoped_lock lock(mutex);
  failure_notifications = notifications + 2U;
 }
 fault.partial_semantic = true;
 samples.SetOverlays({true, true, true, true});
 await_validation(mutex, changed, [&] { return notifications >= failure_notifications; });
 CHECK(samples.snapshot().frame == preserved.frame);
 CHECK(samples.snapshot().overlays == preserved.overlays);
 CHECK(samples.snapshot().overlay_selection.value == preserved.overlays);
 CHECK(samples.snapshot().overlay_selection.revision > preserved.overlay_selection.revision);
 CHECK(samples.ImageSnapshot(preserved.frame)->overlays == preserved.overlays);
 {
  std::scoped_lock lock(mutex);
  failure_notifications = notifications + 2U;
 }
 samples.SetDisplay({0.437F});
 await_validation(mutex, changed, [&] { return notifications >= failure_notifications; });
 CHECK(samples.snapshot().frame == preserved.frame);
 CHECK(samples.ImageSnapshot(preserved.frame)->display.confidence_threshold == 0.4F);
 fault.partial_semantic = false;
 samples.SetOverlays({true, true, true, true});  // Repeating a refused request is an explicit retry.
 await_validation(mutex, changed, [&] { return samples.snapshot().frame.revision > preserved.frame.revision; });
 check_retained_atlas();
 CHECK(samples.ImageSnapshot(samples.snapshot().frame)->display.confidence_threshold == 0.437F);
 fault.draw_failures_remaining = 1U;
 capture(3U);  // Last capture's ordinary draw failure retries without another producer callback.
 await_validation(mutex, changed, [&] { return samples.snapshot().sample_identities[0].generation == 9U; });
 CHECK(samples.snapshot().sample_available[0]);
 CHECK(samples.snapshot().sample_available[1]);
 if (labelled) CHECK(samples.ImageSnapshot(samples.snapshot().frame)->samples[0].labels[0].name == "replacement");
 CHECK_THROWS_AS(samples.Select({7U, 1U}), contracts::InvalidIntentError);
 auto final_reader = samples.BorrowFrame();
 REQUIRE(final_reader.valid());
 samples.Shutdown();
}
TEST_CASE("validation preview generations settle to retained source custody with fresh products", "[controller][gpu][validation]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 const bool detail_open = GENERATE(false, true);
 // Failure and cancellation share unsuccessful terminal settlement. Refusal
 // rejects capture independently of an otherwise successful metric result.
 const auto outcome = GENERATE(contracts::ComputeOperationOutcome::Succeeded, contracts::ComputeOperationOutcome::Failed, contracts::ComputeOperationOutcome::Cancelled);
 const bool refuse = GENERATE(false, true);
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 std::mutex mutex;
 std::condition_variable changed;
 detail::ValidationSamples samples({.device = 0, .maximum_width = 512U, .maximum_height = 576U}, [&] {
  std::scoped_lock lock(mutex);
  changed.notify_all();
 });
 const std::array<std::uint32_t, 2U> indices{3U, 7U};
 samples.Begin(1U, indices);
 await_validation(mutex, changed, [&] { return samples.snapshot().frame.valid(); });
 const auto empty = samples.snapshot();
 CHECK(empty.frame.extent == VisualExtent{512U, 576U});
 CHECK(empty.frame.content == VisualRegion{0U, 0U, 512U, 576U});
 CHECK(empty.frame.clean_revision != 0U);
 CHECK_FALSE(empty.detail);
 CHECK(std::ranges::none_of(empty.sample_available, [](bool value) { return value; }));
 {
  auto image = samples.BorrowFrame();
  REQUIRE(image.valid());
  for (std::size_t index = 0U; index < image.plane_count(); ++index) {
   const auto& read = image.plane(index);
   read.context().Bind();
   const auto plane = read.plane();
   std::vector<std::array<std::uint8_t, 4U>> pixels(512U * 576U);
   REQUIRE(cudaMemcpy2D(pixels.data(), 512U * 4U, reinterpret_cast<const void*>(plane.data), plane.descriptor.pitch_bytes, 512U * 4U, 576U, cudaMemcpyDeviceToHost) == cudaSuccess);
   const auto expected = index == 0U ? std::array<std::uint8_t, 4U>{24U, 18U, 35U, 255U} : std::array<std::uint8_t, 4U>{};
   CHECK(std::ranges::all_of(pixels, [&](const auto& value) { return value == expected; }));
  }
 }
 const auto classes = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"retained"});
 const std::array<float, 12U> red{1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
 auto source = PredictionSource::Device(execution, {2U, 2U}, red, {}, classes);
 const auto capture = [&](std::uint64_t generation, std::uint32_t index) {
  const rfdetr::PredictionRecord record{.dataset_index = index};
  samples.Capture(generation, {record, {.chw = source.pixels(), .width = 2U, .height = 2U, .device = 0, .custody = source.custody()}, source.annotations(), {}});
 };
 capture(1U, 3U);
 capture(1U, 7U);
 samples.Settle(1U, true);
 await_validation(mutex, changed, [&] { return samples.snapshot().sample_available[0] && samples.snapshot().sample_available[1]; });
 const auto original_atlas = samples.snapshot();
 if (detail_open) {
  samples.Select({1U, 3U});
  await_validation(mutex, changed, [&] { return samples.snapshot().detail; });
 }
 const auto incumbent = samples.snapshot();
 const auto incumbent_metadata = samples.ImageSnapshot(incumbent.frame);
 REQUIRE(incumbent_metadata);
 samples.Begin(2U, indices);
 const std::array<float, 12U> green{0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
 source = PredictionSource::Device(execution, {2U, 2U}, green, {}, std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"replacement"}));
 samples.Begin(1U, indices);  // A stale selection callback cannot restart an older generation.
 capture(1U, 7U);             // A stale producer cannot fill the new set's matching slot.
 capture(2U, 7U);
 capture(2U, 3U);
 if (!detail_open)
  await_validation(mutex, changed, [&] {
   const auto state = samples.snapshot();
   return state.sample_identities[1].generation == 2U && state.sample_available[0] && state.sample_available[1];
  });
 const auto preview = samples.snapshot();
 if (!detail_open) CHECK(preview.frame.clean_revision != incumbent.frame.clean_revision);
 samples.Settle(1U, false);  // A stale terminal cannot roll back this generation.
 CHECK(samples.snapshot().frame == preview.frame);
 if (refuse) {
  const rfdetr::PredictionRecord record{.dataset_index = 3U};
  samples.Capture(2U, {record, {.preview_failure = "optional preview refused"}, source.annotations(), {}});
 }
 samples.Settle(2U, outcome == contracts::ComputeOperationOutcome::Succeeded);
 const bool restored = refuse || outcome != contracts::ComputeOperationOutcome::Succeeded || detail_open;
 if (restored) {
  const bool republished = !detail_open || refuse || outcome != contracts::ComputeOperationOutcome::Succeeded;
  await_validation(mutex, changed, [&] { return samples.snapshot().content_identity == incumbent.content_identity && (!republished || samples.snapshot().frame.revision > preview.frame.revision); });
  const auto after = samples.snapshot();
  CHECK(after.frame.clean_revision == incumbent.frame.clean_revision);
  CHECK(after.selected == incumbent.selected);
  CHECK(after.document == incumbent.document);
  CHECK(after.sample_identities == incumbent.sample_identities);
  CHECK(after.sample_available == incumbent.sample_available);
  const auto metadata = samples.ImageSnapshot(after.frame);
  REQUIRE(metadata);
  CHECK(metadata->samples[0].pixel_extent == incumbent_metadata->samples[0].pixel_extent);
  if (detail_open) {
   auto document = samples.BorrowDocument(after.frame);
   REQUIRE(document.valid());
   CHECK(document.document->facts() == incumbent.document);
   CHECK(document.document->scene.categories[0].value == "retained");
  }
 } else {
  await_validation(mutex, changed, [&] { return samples.snapshot().frame.revision > preview.frame.revision; });
  CHECK(samples.snapshot().frame.clean_revision == preview.frame.clean_revision);
  CHECK(samples.snapshot().sample_identities[1].generation == 2U);
  CHECK(samples.snapshot().sample_available[0]);
  CHECK(samples.snapshot().sample_available[1]);
 }
 auto [image, metadata] = borrow_validation_frame(samples);
 const auto& sample = metadata->samples[metadata->samples[0].available ? 0U : 1U];
 CHECK(validation_tile_pixel(image, sample.crop) == (restored ? std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U} : std::array<std::uint8_t, 4U>{0U, 255U, 0U, 255U}));
 image = {};
 if (detail_open) {
  // A successful newer set survives another Begin while incumbent detail is
  // still open; unsuccessful/stale work cannot discard that retained atlas.
  const auto next_outcome = GENERATE(contracts::ComputeOperationOutcome::Succeeded, contracts::ComputeOperationOutcome::Failed, contracts::ComputeOperationOutcome::Cancelled);
  samples.Begin(3U, indices);
  samples.Settle(2U, false);
  samples.Select({1U, 7U});  // Navigating frozen A must not demote retained B.
  await_validation(mutex, changed, [&] { return samples.snapshot().selected == ValidationSampleIdentity{1U, 7U}; });
  auto old_detail = samples.BorrowFrame();
  REQUIRE(old_detail.valid());
  capture(3U, 3U);
  samples.CloseDetail();
  const bool replacement = !refuse && outcome == contracts::ComputeOperationOutcome::Succeeded;
  await_validation(mutex, changed, [&] {
   const auto state = samples.snapshot();
   return !state.detail && state.sample_identities[replacement ? 1U : 0U].generation == (replacement ? 2U : 1U);
  });
  auto [closed, closed_metadata] = borrow_validation_frame(samples);
  for (const auto& tile : std::span(closed_metadata->samples).first(2)) {
   CHECK(tile.identity.generation == (replacement ? 2U : 1U));
   CHECK(validation_tile_pixel(closed, tile.crop) == (replacement ? std::array<std::uint8_t, 4U>{0U, 255U, 0U, 255U} : std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U}));
  }
  const auto returned = samples.snapshot();
  CHECK(std::ranges::all_of(std::span(returned.sample_available).first(2), [](bool available) { return available; }));
  if (replacement)
   CHECK(returned.frame.clean_revision != original_atlas.frame.clean_revision);
  else
   CHECK(returned.frame.clean_revision == original_atlas.frame.clean_revision);
  closed = {};
  old_detail = {};
  const auto generation = replacement ? 2U : 1U;
  samples.Select({generation, 7U});
  await_validation(mutex, changed, [&] { return samples.snapshot().detail; });
  {
   auto document = samples.BorrowDocument(samples.snapshot().frame);
   REQUIRE(document.valid());
   CHECK(document.document->scene.frame_index == 7U);
   CHECK(document.document->scene.categories[0].value == (replacement ? "replacement" : "retained"));
  }
  samples.Settle(3U, next_outcome == contracts::ComputeOperationOutcome::Succeeded);
  samples.CloseDetail();
  await_validation(mutex, changed, [&] { return !samples.snapshot().detail; });
  const auto final = samples.snapshot();
  if (next_outcome == contracts::ComputeOperationOutcome::Succeeded) {
   CHECK(final.sample_identities[0].generation == 3U);
   CHECK(final.sample_available[0]);
   CHECK_FALSE(final.sample_available[1]);
  } else {
   CHECK(final.content_identity == returned.content_identity);
   CHECK(final.frame.clean_revision == returned.frame.clean_revision);
   CHECK(final.sample_available == returned.sample_available);
   CHECK(final.sample_identities == returned.sample_identities);
  }
 }
 samples.Shutdown();
}
TEST_CASE("validation presentation refusals preserve settled populations and explicit retry", "[controller][gpu][validation]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 const bool navigate = GENERATE(false, true);
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 std::mutex mutex;
 std::condition_variable changed;
 std::size_t notifications = 0U;
 PredictionReceiverFault fault;
 ScopedPredictionReceiverFault receiver(fault);
 static std::atomic<mmltk::testsupport::TestGate*> pending_draw = nullptr;
 mmltk::testsupport::TestGate draw_gate("validation close composition");
 auto operations = PredictionReceiverFault::Operations();
 operations.clear_semantic = +[](void* destination, std::size_t pitch, int value, std::size_t width, std::size_t height, cudaStream_t stream) {
  if (auto* gate = pending_draw.exchange(nullptr)) gate->receipt().ArriveAndWait();
  return PredictionReceiverFault::Operations().clear_semantic(destination, pitch, value, width, height, stream);
 };
 detail::ValidationSamples samples(
  {.device = 0, .maximum_width = 512U, .maximum_height = 576U},
  [&] {
   std::scoped_lock lock(mutex);
   ++notifications;
   changed.notify_all();
  },
  operations);
 mmltk::testsupport::ScopedTestCleanup release([&] {
  pending_draw = nullptr;
  draw_gate.Release();
 });
 const std::array<std::uint32_t, 2U> indices{3U, 7U};
 const auto classes = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"retained"});
 const std::array<float, 12U> red{1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
 auto source = PredictionSource::Device(execution, {2U, 2U}, red, {}, classes);
 const auto capture = [&](std::uint64_t generation) {
  for (const auto index : indices) {
   const rfdetr::PredictionRecord record{.dataset_index = index};
   samples.Capture(generation, {record, {.chw = source.pixels(), .width = 2U, .height = 2U, .device = 0, .custody = source.custody()}, source.annotations(), {}});
  }
 };
 samples.Begin(1U, indices);
 capture(1U);
 samples.Settle(1U, true);
 await_validation(mutex, changed, [&] { return samples.snapshot().sample_available[0] && samples.snapshot().sample_available[1]; });
 samples.Begin(2U, indices);
 capture(2U);
 await_validation(mutex, changed, [&] {
  const auto state = samples.snapshot();
  return state.sample_identities[0].generation == 2U && state.sample_available[0] && state.sample_available[1];
 });
 const auto population = samples.snapshot();
 auto atlas_reader = samples.BorrowFrame();
 REQUIRE(atlas_reader.valid());
 samples.Select({2U, 3U});
 await_validation(mutex, changed, [&] { return samples.snapshot().detail; });
 samples.Settle(2U, true);
 const auto applied = samples.snapshot();
 auto detail_reader = samples.BorrowFrame();
 REQUIRE(detail_reader.valid());
 const auto request = [&] {
  if (navigate)
   samples.Select({2U, 7U});
  else
   samples.CloseDetail();
 };
 std::size_t target = 0U;
 {
  std::scoped_lock lock(mutex);
  target = notifications + 2U;
 }
 fault.partial_semantic = true;
 // Hold both physical outputs while composing the presentation request. The
 // changed confidence forces semantic work without introducing raw content.
 samples.SetDisplay({0.437F});
 request();
 CHECK(samples.snapshot().frame == applied.frame);
 atlas_reader = {};
 await_validation(mutex, changed, [&] { return notifications >= target; });
 CHECK(samples.snapshot().frame == applied.frame);
 CHECK(samples.snapshot().selected == applied.selected);
 CHECK(samples.snapshot().document == applied.document);
 REQUIRE(samples.ImageSnapshot(applied.frame));
 CHECK(samples.ImageSnapshot(applied.frame)->display.confidence_threshold == 0.4F);
 fault.partial_semantic = false;
 request();  // Selection and CloseDetail both explicitly retry refused draws.
 await_validation(mutex, changed, [&] { return samples.snapshot().frame.revision > applied.frame.revision; });
 detail_reader = {};
 if (navigate) {
  auto document = samples.BorrowDocument(samples.snapshot().frame);
  REQUIRE(document.valid());
  CHECK(document.document->scene.frame_index == 7U);
  CHECK(document.document->scene.categories[0].value == "retained");
  CHECK(samples.snapshot().selected == ValidationSampleIdentity{2U, 7U});
  document = {};
  samples.CloseDetail();
  await_validation(mutex, changed, [&] { return !samples.snapshot().detail; });
 }
 // One close must survive duplicate intents while outputs are borrowed
 // and while its actual semantic composition is executing.
 atlas_reader = samples.BorrowFrame();
 REQUIRE(atlas_reader.valid());
 samples.Select({2U, 3U});
 await_validation(mutex, changed, [&] { return samples.snapshot().detail; });
 detail_reader = samples.BorrowFrame();
 REQUIRE(detail_reader.valid());
 const auto before_close = samples.snapshot();
 const auto writes_before_close = fault.semantic_writes.load();
 pending_draw = &draw_gate;
 samples.SetDisplay({0.75F});
 samples.CloseDetail();
 samples.CloseDetail();
 samples.CloseDetail();
 CHECK(samples.snapshot().frame == before_close.frame);
 CHECK(fault.semantic_writes == writes_before_close);
 atlas_reader = {};
 REQUIRE(draw_gate.WaitEntered(std::chrono::seconds(20)));
 samples.CloseDetail();
 samples.CloseDetail();
 CHECK(samples.snapshot().frame == before_close.frame);
 draw_gate.Release();
 await_validation(mutex, changed, [&] { return !samples.snapshot().detail; });
 // Each published composition consumes one product revision, including a
 // superseded candidate: this exact successor proves no discarded redraw.
 CHECK(samples.snapshot().frame.revision == before_close.frame.revision + 1U);
 CHECK(fault.semantic_writes == writes_before_close + indices.size());
 detail_reader = {};
 const auto restored = samples.snapshot();
 CHECK_FALSE(restored.detail);
 CHECK(restored.content_identity == population.content_identity);
 CHECK(restored.frame.clean_revision == population.frame.clean_revision);
 CHECK(restored.sample_identities == population.sample_identities);
 CHECK(restored.sample_available == population.sample_available);
 {
  const auto metadata = samples.ImageSnapshot(restored.frame);
  REQUIRE(metadata);
  auto image = samples.BorrowFrame();
  REQUIRE(image.valid());
  for (const auto& tile : std::span(metadata->samples).first(2)) CHECK(validation_tile_pixel(image, tile.crop) == std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U});
 }
 // A genuinely new raw population still rolls back after both refused draws.
 samples.Begin(3U, indices);
 fault.partial_draw = true;
 capture(3U);
 await_validation(mutex, changed, [&] {
  const auto state = samples.snapshot();
  return state.frame.revision > restored.frame.revision && state.sample_identities == restored.sample_identities;
 });
 CHECK(samples.snapshot().frame.clean_revision == restored.frame.clean_revision);
 CHECK(samples.snapshot().sample_available == restored.sample_available);
 fault.partial_draw = false;
 samples.Settle(3U, true);
 CHECK(samples.snapshot().sample_identities == restored.sample_identities);
 samples.Shutdown();
}
TEST_CASE("Validation documents preserve off-box empty and missing masks through crop and upscale", "[controller][gpu][validation]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 std::mutex mutex;
 std::condition_variable changed;
 detail::ValidationSamples samples({.device = 0, .maximum_width = 96, .maximum_height = 96}, [&] {
  std::scoped_lock lock(mutex);
  changed.notify_all();
 });
 const std::array<std::uint32_t, 1> indices{0};
 samples.Begin(1, indices);
 const auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"object"});
 std::array<float, 8 * 8 * 3> pixels{};
 auto source = PredictionSource::Device(execution, {8, 8}, pixels, {}, catalog);
 std::array<rfdetr::Prediction, 3> truth{};
 for (auto& object : truth) {
  object.class_reference = 0;
  object.bbox_xyxy = {4.25F, 3.25F, 7.75F, 5.75F};
 }
 truth[0].has_mask = truth[1].has_mask = true;
 truth[0].mask = {.height = 8, .width = 8, .area = 1, .runs = {{16, 1}}};
 truth[1].mask = {.height = 8, .width = 8, .runs = {}};
 const rfdetr::PredictionRecord record{.dataset_index = 0, .detections = source.detections()};
 samples.Capture(1,
  {record, {.chw = source.pixels(), .width = 8, .height = 8, .device = 0, .custody = source.custody()}, source.annotations(), truth, {.resized_width = 8, .resized_height = 4, .offset_y = 2}, 8, 4});
 await_validation(mutex, changed, [&] { return samples.snapshot().sample_available[0]; });
 samples.Select({1, 0});
 await_validation(mutex, changed, [&] { return samples.snapshot().detail; });
 const auto frame = samples.snapshot().frame;
 auto borrowed = samples.BorrowDocument(frame);
 REQUIRE(borrowed.valid());
 const auto imported = materialize_visual_document(*borrowed.document, frame.extent, frame.content);
 REQUIRE(imported.objects.size() == 3);
 CHECK(imported.objects[0].box == contracts::AnnotationBox{{4.25F, 1.25F}, {7.75F, 3.75F}});
 CHECK(imported.objects[0].mask.runs == std::vector<contracts::AnnotationMaskRun>{{0, 0, 0}});
 CHECK(imported.objects[1].mask.present);
 CHECK(imported.objects[1].mask.runs.empty());
 CHECK_FALSE(imported.objects[2].mask.present);
 const auto scaled = scale_visual_document(borrowed.document, {1U, 1U}, {4U, 4U});
 const auto enlarged = materialize_visual_document(*scaled, {32, 32}, {0, 8, 32, 16});
 CHECK(enlarged.objects[0].mask.runs == std::vector<contracts::AnnotationMaskRun>{{0, 0, 3}, {1, 0, 3}, {2, 0, 3}, {3, 0, 3}});
 borrowed = {};
 samples.Shutdown();
}
TEST_CASE("validation requires a nonzero rectangular two by three atlas envelope", "[controller][validation]") {
 CHECK_THROWS_AS(detail::ValidationSamples({.device = 0, .maximum_width = 2U, .maximum_height = 2U}, {}), contracts::InvalidIntentError);
 CHECK_THROWS_AS(detail::ValidationSamples({.device = 0, .maximum_width = 3U, .maximum_height = 1U}, {}), contracts::InvalidIntentError);
 detail::ValidationSamples minimum({.device = 0, .maximum_width = 8U, .maximum_height = 9U}, {});
 CHECK_FALSE(minimum.snapshot().frame.valid());
 CHECK(std::ranges::none_of(minimum.snapshot().sample_available, [](bool available) { return available; }));
 minimum.Shutdown();
}
TEST_CASE("validation composition preserves independent nonempty box and mask pixels", "[controller][gpu][validation]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 using Composition = detail::PredictionPreviewComposition;
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 const bool complementary = GENERATE(false, true);
 const bool same_class = GENERATE(false, true);
 const auto threshold = GENERATE(0.0F, 0.4F, 0.437F, 0.75F, 1.0F);
 const auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"prediction", "truth"});
 const std::array<float, 12U * 12U * 3U> pixels{};
 std::array<std::uint8_t, 12U * 12U> mask{};
 mask[3U * 12U + 3U] = 1U;
 const rfdetr::Prediction prediction{
  .class_reference = 0, .class_domain = mmltk::backend::data::catalog::ClassReferenceDomain::Foreground, .score = 0.75F, .bbox_xyxy = {2, 2, 4, 4}, .has_mask = true};
 auto truth = prediction;
 truth.class_reference = same_class ? 0 : 1;
 truth.bbox_xyxy = {8, 8, 10, 10};
 truth.mask = {.height = 12U, .width = 12U, .area = 2U, .runs = {{3U * 12U + 3U, 1U}, {8U * 12U + 8U, 1U}}};
 truth.has_mask = true;
 auto source = PredictionSource::Device(execution, {12U, 12U}, pixels, {prediction}, catalog, mask);
 gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
 detail::PredictionPreviewPool pool(execution, context);
 const std::array ground_truth{truth};
 auto frame = pool.Capture(source.pixels(), source.extent(), 0U, source.detections(), source.annotations(), catalog, 2, nullptr, source.custody(), nullptr, nullptr, ground_truth, true);
 REQUIRE(frame);
 const std::array regions{Composition::Region{frame, {0U, 0U, 12U, 12U}}};
 gpu::SystemImageRuntime runtime({.device = 0, .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .output_buffer_count = 2U, .adopted_context = context});
 for (unsigned flags = 0U; flags < 16U; ++flags) {
  const Composition::Options options{bool(flags & 1U), bool(flags & 2U), bool(flags & 4U), bool(flags & 8U), complementary, false, threshold};
  const bool prediction_visible = threshold <= prediction.score;
  auto candidate = runtime.AcquireOutput();
  Composition::Draw(runtime, candidate, {12U, 12U}, regions, options);
  auto completed = runtime.CommitOutput(std::move(candidate));
  auto image = completed.Borrow();
  REQUIRE(image.valid());
  std::array<std::uint8_t, 12U * 12U * 4U> rgba{};
  context.Bind();
  const auto semantic = image.plane(1U).plane();
  REQUIRE(cudaMemcpy2D(rgba.data(), 12U * 4U, reinterpret_cast<const void*>(semantic.data), semantic.descriptor.pitch_bytes, 12U * 4U, 12U, cudaMemcpyDeviceToHost) == cudaSuccess);
  const auto pixel = [&](std::size_t x, std::size_t y) {
   const auto offset = (y * 12U + x) * 4U;
   return std::array{rgba[offset], rgba[offset + 1U], rgba[offset + 2U], rgba[offset + 3U]};
  };
  // Disjoint known probes: red prediction, cyan truth, and untouched background.
  // These expected values do not call the production palette or rasterizer.
  const std::array<std::uint8_t, 4U> empty{};
  CHECK(pixel(1U, 3U) == (options.prediction_boxes && prediction_visible ? std::array<std::uint8_t, 4U>{255, 0, 0, 255} : empty));
  const bool cyan = same_class == complementary;
  const std::array<std::uint8_t, 4U> truth_mask{static_cast<std::uint8_t>(cyan ? 0 : 255), static_cast<std::uint8_t>(cyan ? 255 : 0), static_cast<std::uint8_t>(cyan ? 255 : 0), 96};
  auto truth_box = truth_mask;
  truth_box[3] = 255;
  auto overlap = options.prediction_masks && prediction_visible ? std::array<std::uint8_t, 4U>{255, 0, 0, 96} : empty;
  if (options.ground_truth_masks) {
   overlap = truth_mask;
   if (complementary && options.prediction_masks && prediction_visible) overlap[0] = 255;
  }
  CHECK(pixel(3U, 3U) == overlap);
  CHECK(pixel(7U, 8U) == (options.ground_truth_boxes ? truth_box : empty));
  CHECK(pixel(8U, 8U) == (options.ground_truth_masks ? truth_mask : empty));
  CHECK(pixel(11U, 0U) == empty);
  const auto clean = image.plane(0U).plane();
  REQUIRE(cudaMemcpy2D(rgba.data(), 12U * 4U, reinterpret_cast<const void*>(clean.data), clean.descriptor.pitch_bytes, 12U * 4U, 12U, cudaMemcpyDeviceToHost) == cudaSuccess);
  CHECK(pixel(3U, 3U) == std::array<std::uint8_t, 4U>{0, 0, 0, 255});
  CHECK(frame->classes()[0] == "prediction");
  CHECK(frame->classes()[1] == "truth");
 }
}
TEST_CASE("retained validation preparation follows physical storage and changed regions", "[controller][gpu][validation]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 using Composition = detail::PredictionPreviewComposition;
 PredictionReceiverFixture receiver;
 const auto& execution = receiver.device.execution;
 auto& context = receiver.device.context;
 auto& fault = receiver.fault;
 detail::PredictionPreviewPool pool(execution, context, PredictionReceiverFault::Operations(), {}, 6U);
 gpu::SystemImageRuntime runtime({.device = 0, .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .output_buffer_count = 2U, .adopted_context = context});
 Composition retained;
 const auto classes = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"sample"});
 const std::array<float, 12U> red{1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
 auto source = PredictionSource::Device(execution, {2U, 2U}, red, {}, classes);
 const std::array truth{rfdetr::Prediction{.class_reference = 0, .bbox_xyxy = {-0.5F, -0.5F, 0.5F, 0.5F}, .mask = {.height = 2U, .width = 2U, .area = 1U, .runs = {{0U, 1U}}}, .has_mask = true}};
 std::array<Composition::Region, 6U> regions;
 std::array<std::uint64_t, 2U> allocations{};
 std::size_t publications = 0U;
 const auto draw = [&](std::size_t count, Composition::Options options, VisualExtent extent = {12U, 8U}) {
  auto candidate = runtime.AcquireOutput();
  Composition::Draw(runtime, candidate, extent, std::span(regions).first(count), options, &retained);
  auto completed = runtime.CommitOutput(std::move(candidate));
  auto image = completed.Borrow();
  context.Bind();
  const auto clean = image.plane(0U).plane();
  if (extent == VisualExtent{12U, 8U}) {
   const auto slot = publications++ % 2U;
   if (allocations[slot]) CHECK(clean.allocation.identity == allocations[slot]);
   allocations[slot] = clean.allocation.identity;
  }
  for (std::size_t index = 0U; index < count; ++index) {
   std::array<std::uint8_t, 4U> pixel{};
   const auto crop = regions[index].crop;
   REQUIRE(cudaMemcpy(pixel.data(), reinterpret_cast<const void*>(clean.data + crop.y * clean.descriptor.pitch_bytes + crop.x * 4U), 4U, cudaMemcpyDeviceToHost) == cudaSuccess);
   CHECK(pixel == std::array<std::uint8_t, 4U>{255, 0, 0, 255});
   const auto semantic = image.plane(1U).plane();
   REQUIRE(cudaMemcpy(pixel.data(), reinterpret_cast<const void*>(semantic.data + crop.y * semantic.descriptor.pitch_bytes + crop.x * 4U), 4U, cudaMemcpyDeviceToHost) == cudaSuccess);
   CHECK(pixel == (options.ground_truth_masks ? std::array<std::uint8_t, 4U>{0, 255, 255, 96} : std::array<std::uint8_t, 4U>{}));
   REQUIRE(cudaMemcpy(pixel.data(), reinterpret_cast<const void*>(semantic.data + crop.y * semantic.descriptor.pitch_bytes + (crop.x + 2U) * 4U), 4U, cudaMemcpyDeviceToHost) == cudaSuccess);
   CHECK(pixel == (options.ground_truth_boxes ? std::array<std::uint8_t, 4U>{0, 255, 255, 255} : std::array<std::uint8_t, 4U>{}));
  }
 };
 Composition::Options options{false, false, false, false, true, true};
 draw(0U, options);
 for (std::size_t index = 0U; index < regions.size(); ++index) {
  auto frame = pool.Capture(source.pixels(), {2U, 2U}, 0U, {}, source.annotations(), classes, 1, nullptr, source.custody(), nullptr, nullptr, truth, true);
  REQUIRE(frame);
  regions[index] = {std::move(frame), {static_cast<std::uint32_t>(index % 3U) * 4U, static_cast<std::uint32_t>(index / 3U) * 4U, 4U, 4U}};
  draw(index + 1U, options);
  CHECK(fault.draws == index + 1U);  // One source conversion, including both destination allocations.
  CHECK(fault.uploads == 0U);        // GT boxes/disabled masks do not upload RLE.
 }
 draw(6U, options);  // Both allocations now hold all six unchanged tiles.
 REQUIRE(allocations[0] != allocations[1]);
 const auto unchanged_semantics = fault.semantic_writes.load();
 draw(6U, options);
 draw(6U, options);
 CHECK(fault.semantic_writes == unchanged_semantics);
 for (unsigned iteration = 0U; iteration < 6U; ++iteration) {
  options.ground_truth_masks = iteration % 2U == 0U;
  options.confidence_threshold = iteration % 2U == 0U ? 0.437F : 1.0F;
  draw(6U, options);
  CHECK(fault.draws == 6U);
 }
 CHECK(fault.uploads == 6U);
 options.ground_truth_boxes = true;
 fault.partial_semantic = true;
 {
  auto candidate = runtime.AcquireOutput();
  CHECK_THROWS(Composition::Draw(runtime, candidate, {12U, 8U}, regions, options, &retained));
 }
 fault.partial_semantic = false;
 // Allocation rotation after a refused candidate is unspecified; validate pixels directly.
 draw(6U, options, {16U, 12U});
 CHECK(fault.draws == 6U);  // Failure and output growth preserve untouched source clean preparation.
 options.ground_truth_masks = true;
 draw(6U, options, {16U, 12U});
 CHECK(fault.uploads == 6U);
 // Detail uses the original extent; returning to scaled layout retains the scratch clean plane.
 auto detail = runtime.AcquireOutput();
 const std::array selected{Composition::Region{regions[0].frame, {0U, 0U, 2U, 2U}}};
 Composition::Draw(runtime, detail, {2U, 2U}, selected, options, &retained);
 static_cast<void>(runtime.CommitOutput(std::move(detail)));
 CHECK(fault.draws == 6U);
 draw(6U, options, {16U, 12U});
 CHECK(fault.draws == 6U);
}
namespace {
class RefusedValidationPreview final : public ValidationRuntime {
public:
 explicit RefusedValidationPreview(std::atomic_size_t& runs, unsigned failure) : runs_(runs), failure_(failure) {}
 ValidationRuntimeResult Run(mmltk::backend::models::rfdetr::ValidateRequest request, std::stop_token, const ComputeProgressSink& progress,
  const mmltk::backend::models::rfdetr::ValidationDelivery& delivery, std::uint64_t) override {
  namespace rfdetr = mmltk::backend::models::rfdetr;
  ++runs_;
  const std::array<std::uint32_t, 2U> selected{3U, 5U};
  const auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"measured"});
  delivery.samples_selected(std::span(selected).first(failure_ == 2U ? 2U : 1U), catalog);
  const rfdetr::PredictionRecord prediction{.dataset_index = 3U};
  const mmltk::backend::ml::runtime::AnalysisAnnotationStorage annotations{.class_catalog = catalog};
  if (failure_ != 0U) {
   const auto execution = mmltk::frameworks::gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
   auto source = PredictionSource::Device(execution, {32U, 24U}, std::vector<float>(32U * 24U * 3U, 0.25F), {}, catalog);
   const auto directory = request.report_json_path.parent_path() / "samples";
   if (failure_ == 1U) {
    std::ofstream blocked(directory);
    blocked << "occupied";
   } else {
    std::filesystem::create_directories(directory);
   }
   delivery.sample({prediction, {.chw = source.pixels(), .width = 32U, .height = 24U, .device = 0, .custody = source.custody()}, source.annotations(), {}});
   if (failure_ == 2U) {
    const rfdetr::PredictionRecord second{.dataset_index = 5U};
    delivery.sample({second, {.chw = source.pixels(), .width = 32U, .height = 24U, .device = 0, .custody = source.custody()}, source.annotations(), {}});
   }
  } else
   delivery.sample({prediction, {.preview_failure = "preview storage refused"}, annotations, {}});
  const auto completed = failure_ == 2U ? 2U : 1U;
  progress({completed, completed, completed, "Validating"});
  ValidationRuntimeResult result{.terminal = contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded, 0U, completed)};
  result.evaluation.emplace();
  result.evaluation->summary.bbox.available = true;
  result.evaluation->summary.bbox.ap = 0.625;
  result.evaluation->class_catalog = catalog;
  return result;
 }

private:
 std::atomic_size_t& runs_;
 unsigned failure_;
};
}  // namespace
TEST_CASE("validation semantic metrics survive required sample refusal without reinference", "[controller][systems][gpu][validation]") {
 const unsigned failure = GENERATE(0U, 1U, 2U);
 CAPTURE(failure);
 const auto root = mmltk::testsupport::make_temp_root("validation-preview-refusal");
 ApplicationDataFixture fixture{root};
 fixture.PrepareModel(contracts::FeatureId::Validate);
 auto [settings, dataset, model] = fixture.systems();
 contracts::SettingsUpdateRequest initial_display;
 initial_display.updates.push_back({.path = "workflows.validate.display.confidence_threshold", .value = mmltk::frameworks::serialization::wire::FlatValue{0.437}});
 static_cast<void>(settings.Update(std::move(initial_display)));
 std::mutex display_mutex;
 std::condition_variable display_changed;
 std::atomic_size_t runs = 0U;
 std::promise<ValidationSnapshot> finished;
 unsigned encoded_samples = 0;
 ValidationSystem validation(
  settings, dataset, model, [&](DirectComputeConfiguration) { return std::make_unique<RefusedValidationPreview>(runs, failure); },
  [&](auto event) {
   std::scoped_lock lock(display_mutex);
   display_changed.notify_all();
   if (auto* changed = std::get_if<ValidationChanged>(&event);
    changed && !changed->snapshot.operation.active && changed->snapshot.operation.terminal.outcome == contracts::ComputeOperationOutcome::Failed) {
    try {
     finished.set_value(changed->snapshot);
    } catch (const std::future_error&) {}
   }
  },
  [](int, int) { return DirectComputeConfiguration{}; }, {.device = 0, .maximum_width = 768U, .maximum_height = 512U},
  [&](const char* path, int width, int height, int channels, const void* pixels, int stride) {
   if (failure == 2U && ++encoded_samples == 2U) {
    auto* full = std::fopen("/dev/full", "wb");
    if (!full) throw std::runtime_error("open validation PNG failure stream");
    return mmltk::backend::imaging::raster::detail::write_png_stream(full, path, width, height, channels, pixels, stride);
   }
   return mmltk::backend::imaging::raster::detail::write_png_file(path, width, height, channels, pixels, stride);
  });
 static_cast<void>(validation.Start({}));
 const auto result = finished.get_future().get();
 // Operation settlement schedules preview rollback; its terminal snapshot may
 // still describe the previously displayed image until the renderer publishes.
 await_validation(display_mutex, display_changed, [&] {
  const auto displayed = validation.snapshot();
  return displayed.frame.revision != 0U && std::ranges::none_of(displayed.sample_available, [](bool available) { return available; });
 });
 const auto displayed = validation.snapshot();
 REQUIRE(validation.ImageSnapshot(validation.snapshot().frame));
 CHECK(validation.ImageSnapshot(validation.snapshot().frame)->display.confidence_threshold == 0.437F);
 REQUIRE(result.metrics);
 CHECK(result.metrics->bbox.ap == 0.625);
 CHECK(result.operation.terminal.outcome == contracts::ComputeOperationOutcome::Failed);
 CHECK(result.operation.terminal.completed == (failure == 2U ? 2U : 1U));
 CHECK(result.operation.output.completed_samples == (failure == 2U ? 1U : 0U));
 if (failure == 2U) {
  const auto directory = std::filesystem::path(result.operation.output.directory) / "samples";
  CHECK(result.operation.output.recent_sample == (directory / "sample-3.png").string());
  CHECK(result.operation.terminal.detail.find("rendered PNG") != std::string::npos);
  CHECK(result.operation.terminal.detail.find("sample-5.png.partial") != std::string::npos);
  CHECK_FALSE(std::filesystem::exists(directory / "sample-5.png"));
  CHECK_FALSE(std::filesystem::exists(directory / "sample-5.png.partial"));
  CHECK_FALSE(std::filesystem::exists(directory / "sample-3.png.partial"));
  CHECK(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}) == 1);
  int width = 0, height = 0, channels = 0;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load((directory / "sample-3.png").c_str(), &width, &height, &channels, 4), stbi_image_free);
  REQUIRE(pixels);
  CHECK(width == 32);
  CHECK(height == 24);
  CHECK(pixels.get()[0] == 64U);
 } else {
  CHECK(std::ranges::none_of(displayed.sample_available, [](bool available) { return available; }));
  CHECK_THROWS_AS(validation.SelectSample({result.operation.generation_frontier, 3U}), contracts::InvalidIntentError);
 }
 static_cast<void>(validation.CloseDetail());
 for (const double threshold : {0.0, 1.0, 0.437, 0.4}) {
  contracts::SettingsUpdateRequest edit;
  edit.updates.push_back({.path = "workflows.validate.display.confidence_threshold", .value = mmltk::frameworks::serialization::wire::FlatValue{threshold}});
  static_cast<void>(settings.Update(std::move(edit)));
  validation.DisplaySettingsChanged();
  CHECK(validation.snapshot().metrics->bbox.ap == result.metrics->bbox.ap);
  CHECK(validation.snapshot().operation.terminal.completed == result.operation.terminal.completed);
  CHECK(runs == 1U);
 }
 validation.Shutdown();
 CHECK(runs == 1U);
}
}  // namespace mmltk::controller
TEST_CASE("validation reports honor enablement and propagate file settlement failures", "[controller][validation][output]") {
 namespace r = mmltk::backend::models::rfdetr;
 mmltk::testsupport::ScopedTempDir root{"validation-report-output"};
 r::ValidateRequest request;
 request.report_json_path = root.path() / "report.json";
 request.write_report_json = false;
 r::ValidationRunResult result;
 r::write_validation_report(request, result);
 CHECK_FALSE(std::filesystem::exists(request.report_json_path));
 request.write_report_json = true;
 r::write_validation_report(request, result);
 CHECK(std::filesystem::file_size(request.report_json_path) > 0U);
 request.report_json_path = "/dev/full";
 CHECK_THROWS(r::write_validation_report(request, result));
}
TEST_CASE("validation output saves the selected identities without presentation and retains receiver pixels", "[controller][validation][gpu][output]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 using namespace mmltk::controller;
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 const mmltk::testsupport::ScopedTempDir directory("validation-native-output");
 const auto count = GENERATE(0U, 2U, 6U);
 const bool cancelled = GENERATE(false, true);
 const auto saved_count = cancelled ? count / 2U : count;
 const std::array<std::uint32_t, 6> selected{11U, 2U, 8U, 4U, 21U, 3U};
 std::vector<std::filesystem::path> published;
 detail::ValidationSampleOutput output({execution}, {}, [&](const auto& path) { published.push_back(path); });
 contracts::ValidationRunPreview options;
 options.overlays = {false, false, false, false, false, false};
 output.Begin(directory.path(), options, std::span(selected).first(count));
 auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"café"});
 for (std::size_t index = 0; index < saved_count; ++index) {
  std::vector<float> pixels(32U * 24U * 3U, 0.25F);
  auto source = PredictionSource::Device(execution, {32U, 24U}, pixels, {}, catalog);
  rfdetr::PredictionRecord record{.dataset_index = selected[index]};
  auto captured = output.Capture({record, {.chw = source.pixels(), .width = 32U, .height = 24U, .device = 0, .custody = source.custody()}, source.annotations(), {}});
  REQUIRE(captured);
  REQUIRE(cudaMemset(const_cast<float*>(source.pixels()), 0, pixels.size() * sizeof(float)) == cudaSuccess);
 }
 output.Finish(!cancelled);
 REQUIRE(published.size() == saved_count);
 for (std::size_t index = 0; index < saved_count; ++index) {
  CHECK(published[index] == directory.path() / "samples" / ("sample-" + std::to_string(selected[index]) + ".png"));
  int width = 0, height = 0, channels = 0;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load(published[index].c_str(), &width, &height, &channels, 4), stbi_image_free);
  REQUIRE(pixels);
  REQUIRE(width == 32);
  REQUIRE(height == 24);
  for (int pixel = 0; pixel < width * height; ++pixel) {
   CHECK(pixels.get()[pixel * 4] == 64U);
   CHECK(pixels.get()[pixel * 4 + 3] == 255U);
  }
  CHECK_FALSE(std::filesystem::exists(published[index].string() + ".partial"));
 }
 // A new run can reuse the owner and its bounded transfer resource.
 output.Begin(directory.path() / "restart", options, {});
 output.Finish(true);
}
TEST_CASE("headless validation receivers follow selected execution while graphics receivers stay fixed", "[controller][validation][gpu][output]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 using namespace mmltk::controller;
 const auto topology = mmltk::common::system::NumaTopology::Capture();
 const auto first = gpu::resolve_device_execution(0, topology);
 int count = 0;
 REQUIRE(cudaGetDeviceCount(&count) == cudaSuccess);
 const auto second = count > 1 ? gpu::resolve_device_execution(1, topology) : first;
 if (count < 2) WARN("Only one CUDA GPU is visible; headless cross-device output remains unverified");
 const bool graphics = GENERATE(false, true);
 const mmltk::testsupport::ScopedTempDir directory("validation-receiver-switch");
 std::vector<std::filesystem::path> published;
 const VisualDeviceSettings visual = graphics ? VisualDeviceSettings{.device = 0, .maximum_width = 32U, .maximum_height = 24U} : VisualDeviceSettings{};
 detail::ValidationSampleOutput output({first}, visual, [&](const auto& path) { published.push_back(path); });
 contracts::ValidationRunPreview options;
 options.overlays = {false, false, false, false, false, false};
 auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"sample"});
 std::vector<std::shared_ptr<const detail::PredictionPreviewFrame>> retained;
 const std::array<std::uint32_t, 1> selected{0U};
 for (const auto& execution : {first, second, second}) {
  const auto run = retained.size();
  output.Begin(directory.path() / std::to_string(run), options, selected, {execution});
  auto source = PredictionSource::Device(execution, {32U, 24U}, std::vector<float>(32U * 24U * 3U, .25F), {}, catalog);
  rfdetr::PredictionRecord record{.dataset_index = 0};
  auto captured = output.Capture({record, {.chw = source.pixels(), .width = 32U, .height = 24U, .device = execution.device, .custody = source.custody()}, source.annotations(), {}});
  REQUIRE(captured);
  CHECK(captured->receiver_device() == (graphics ? first.device : execution.device));
  retained.push_back(std::move(captured));
  // The next Begin must settle this write before replacing a headless group.
 }
 output.Finish(true);
 REQUIRE(published.size() == 3U);
 CHECK(retained.front()->receiver_device() == first.device);
 CHECK(retained.front()->classes().front() == "sample");
 for (const auto& path : published) { check_validation_png_pixels(path); }
 retained.clear();
}
TEST_CASE("validation receiver authority survives group replacement and late borrowed release", "[controller][validation][gpu][custody]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 using namespace mmltk::controller;
 const bool borrowed = GENERATE(false, true);
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 auto replacement = execution;
 // A changed execution descriptor exercises receiver replacement on one GPU too.
 replacement.placement.cpus.push_back(replacement.placement.cpus.front());
 bool fail_restore = false;
 const gpu::CudaContextApi context_api{&fail_restore, [](void*, CUcontext* value) noexcept { return cuCtxGetCurrent(value); },
  [](void* fault, CUcontext value) noexcept { return *static_cast<bool*>(fault) ? CUDA_ERROR_CONTEXT_IS_DESTROYED : cuCtxSetCurrent(value); }};
 auto retirement = std::make_shared<gpu::TerminalCudaRetirementOwner>(21U);
 const mmltk::testsupport::ScopedTempDir root("validation-late-receiver");
 ApplicationDataFixture fixture(root.path());
 fixture.PrepareModel(contracts::FeatureId::Validate);
 auto [settings, dataset, model] = fixture.systems();
 unsigned factories = 0;
 ValidationSystem validation(
  settings, dataset, model,
  [&](DirectComputeConfiguration) {
   ++factories;
   return std::make_unique<FakeNonvisualComputeRuntime>(ComputeScenario{});
  },
  {}, [](int, int) { return DirectComputeConfiguration{}; }, {}, {}, retirement);
 std::vector<std::filesystem::path> published;
 detail::ValidationSampleOutput output({execution}, {}, [&](const auto& path) { published.push_back(path); }, {}, retirement, context_api);
 const std::array<std::uint32_t, 1> selected{0U};
 contracts::ValidationRunPreview options;
 options.overlays = {false, false, false, false, false, false};
 auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"retained"});
 auto source = PredictionSource::Device(execution, {32U, 24U}, std::vector<float>(32U * 24U * 3U, .25F), {}, catalog);
 rfdetr::PredictionRecord record{.dataset_index = 0};
 const auto sample = [&] {
  return rfdetr::ValidationSampleView{record, {.chw = source.pixels(), .width = 32U, .height = 24U, .device = execution.device, .custody = source.custody()}, source.annotations(), {}};
 };
 output.Begin(root.path() / "first", options, selected, {execution});
 auto old = output.Capture(sample());
 REQUIRE(old);
 if (!borrowed) old.reset();
 fail_restore = !borrowed;
 if (borrowed) {
  output.Begin(root.path() / "second", options, selected, {replacement});
  REQUIRE_FALSE(output.HasUnsafeCustody());
  REQUIRE(published.size() == 1U);
  CHECK(old->receiver_device() == execution.device);
  CHECK(old->classes().front() == "retained");
  // The old image remains usable after its receiver pool has been released.
  int width = 0, height = 0, channels = 0;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load(published.front().c_str(), &width, &height, &channels, 4), stbi_image_free);
  REQUIRE(pixels);
  CHECK(width == 32);
  CHECK(height == 24);
  CHECK(pixels.get()[0] == 64U);
  fail_restore = true;
  old.reset();
 } else {
  CHECK_THROWS_AS(output.Begin(root.path() / "second", options, selected, {replacement}), contracts::UnavailableError);
  CHECK(published.size() == 1U);
 }
 REQUIRE(output.HasUnsafeCustody());
 CHECK_THROWS_AS(output.Capture(sample()), contracts::UnavailableError);
 CHECK_THROWS_AS(output.Begin(root.path() / "third", options, selected, {execution}), contracts::UnavailableError);
 for (unsigned attempt = 0; attempt < 3; ++attempt) CHECK_THROWS_AS(validation.Start({}), contracts::UnavailableError);
 CHECK(factories == 0U);
 static_cast<void>(validation.Stop());
 validation.Shutdown();
}
TEST_CASE("validation rendered output applies captured layers masks boxes and inclusive confidence", "[controller][validation][gpu][output]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 using namespace mmltk::controller;
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 const mmltk::testsupport::ScopedTempDir directory("validation-output-layers");
 const auto choice = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U);
 CAPTURE(choice);
 contracts::ValidationRunPreview options;
 options.ground_truth_labels = options.prediction_labels = false;
 options.display.confidence_threshold = choice == 7U ? 0.438F : 0.437F;
 options.overlays.prediction_layer = choice != 1U;
 options.overlays.ground_truth_layer = choice != 2U;
 options.overlays.prediction_masks = choice != 3U;
 options.overlays.ground_truth_masks = choice != 4U;
 options.overlays.prediction_boxes = choice != 5U;
 options.overlays.ground_truth_boxes = choice != 6U;
 auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"object"});
 rfdetr::Prediction detection{.class_reference = 0, .score = 0.437F, .bbox_xyxy = {2, 26, 16, 38}};
 std::vector<std::uint8_t> masks(32U * 40U);
 masks[30U * 32U + 8U] = 1U;
 auto source = PredictionSource::Device(execution, {32U, 40U}, std::vector<float>(32U * 40U * 3U, 0.25F), {detection}, catalog, masks);
 rfdetr::Prediction truth = detection;
 truth.has_mask = true;
 truth.mask.width = 32U;
 truth.mask.height = 40U;
 truth.mask.runs = {{30U * 32U + 8U, 1U}};
 truth.mask.area = 1U;
 const std::array truths{truth};
 const std::array<std::uint32_t, 1> selection{29U};
 std::filesystem::path published;
 detail::ValidationSampleOutput output({execution}, {}, [&](const auto& path) { published = path; });
 output.Begin(directory.path(), options, selection);
 rfdetr::PredictionRecord record{.dataset_index = 29U, .detections = {detection}};
 REQUIRE(output.Capture({record, {.chw = source.pixels(), .width = 32U, .height = 40U, .device = 0, .custody = source.custody()}, source.annotations(), truths}));
 options.overlays = {false, false, false, false, false, false};  // Later viewer policy cannot mutate the admitted save.
 output.Finish(true);
 int width = 0, height = 0, channels = 0;
 std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load(published.c_str(), &width, &height, &channels, 4), stbi_image_free);
 REQUIRE(pixels);
 REQUIRE(width == 32);
 REQUIRE(height == 40);
 const bool prediction = choice != 1U && choice != 7U;
 const bool truth_visible = choice != 2U;
 const bool pm = prediction && choice != 3U, gm = truth_visible && choice != 4U;
 const auto* mask = pixels.get() + (30U * 32U + 8U) * 4U;
 const auto blend = [](unsigned color) { return static_cast<unsigned>((64U * 159U + color * 96U + 127U) / 255U); };
 CHECK(mask[0] == (pm || gm ? blend(pm ? 255U : 0U) : 64U));
 CHECK(mask[1] == (pm || gm ? blend(gm ? 255U : 0U) : 64U));
 CHECK(mask[2] == mask[1]);
 // The shared raster places the outline outside the continuous box extent.
 const auto* box = pixels.get() + (25U * 32U + 8U) * 4U;
 const bool pb = prediction && choice != 5U, gb = truth_visible && choice != 6U;
 CHECK(box[0] == (pb || gb ? (pb ? 255U : 0U) : 64U));
 CHECK(box[1] == (pb || gb ? (gb ? 255U : 0U) : 64U));
}
TEST_CASE("validation PNG captions preserve names independent flags opaque order and native geometry", "[controller][validation][gpu][output]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 using namespace mmltk::controller;
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 const mmltk::testsupport::ScopedTempDir directory("validation-caption-png");
 std::filesystem::path published;
 // This deliberately admits a much smaller optional browser envelope.
 detail::ValidationSampleOutput output({execution}, {.device = 0, .maximum_width = 8U, .maximum_height = 9U}, [&](const auto& path) { published = path; });
 unsigned serial = 0;
 const auto save = [&](bool gt, bool det, std::string name, float x) {
  contracts::ValidationRunPreview options;
  options.overlays.prediction_boxes = options.overlays.prediction_masks = false;
  options.overlays.ground_truth_boxes = options.overlays.ground_truth_masks = false;
  options.ground_truth_labels = gt;
  options.prediction_labels = det;
  auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{std::move(name)});
  rfdetr::Prediction detection{.class_reference = 0, .score = 0.8F, .bbox_xyxy = {x, 26, 64, 40}};
  auto source = PredictionSource::Device(execution, {64U, 48U}, std::vector<float>(64U * 48U * 3U, 0.25F), {detection}, catalog);
  const std::array truth{detection};
  const std::array<std::uint32_t, 1> selected{33U};
  output.Begin(directory.path() / std::to_string(serial++), options, selected);
  const rfdetr::PredictionRecord record{.dataset_index = 33, .detections = {detection}};
  REQUIRE(output.Capture({record, {.chw = source.pixels(), .width = 64U, .height = 48U, .device = 0, .custody = source.custody()}, source.annotations(), truth}));
  output.Finish(true);
  int width = 0, height = 0, channels = 0;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(stbi_load(published.c_str(), &width, &height, &channels, 4), stbi_image_free);
  REQUIRE(decoded);
  REQUIRE(width == 64);
  REQUIRE(height == 48);
  CHECK(published.filename() == "sample-33.png");
  CHECK_FALSE(std::filesystem::exists(published.string() + ".partial"));
  return std::vector<std::uint8_t>(decoded.get(), decoded.get() + 64U * 48U * 4U);
 };
 const auto clean = save(false, false, "café", 2);
 const auto truth = save(true, false, "café", 2);
 const auto prediction = save(false, true, "café", 2);
 const auto both = save(true, true, "café", 2);
 CHECK(both == prediction);  // Detection background and glyphs are the last opaque painter.
 CHECK(truth != prediction);
 CHECK(truth != clean);
 CHECK(prediction != clean);
 const auto origin = (4U * 64U + 2U) * 4U;
 CHECK(truth[origin] == 0U);
 CHECK(truth[origin + 1U] == 255U);
 CHECK(prediction[origin] == 255U);
 CHECK(prediction[origin + 1U] == 0U);
 std::size_t glyphs = 0;
 for (unsigned y = 4; y < 26; ++y)
  for (unsigned x = 2; x < 42; ++x) {
   const auto offset = (y * 64U + x) * 4U;
   if (prediction[offset] == 255U && prediction[offset + 1U] > 0U) ++glyphs;
  }
 CHECK(glyphs > 20U);
 CHECK(save(false, true, "0", 2) != prediction);
 CHECK(save(false, true, "cafe", 2) != prediction);  // The accented scalar contributes actual saved glyph pixels.
 const auto clipped = save(false, true, "café", 62);
 CHECK(clipped[(4U * 64U + 61U) * 4U] == 64U);
 CHECK(clipped[(4U * 64U + 62U) * 4U] == 255U);
 CHECK(clipped[(47U * 64U + 63U) * 4U] == 64U);  // Full native extent survives the 8×9 visual envelope.
}
namespace {
class PendingOutputRuntime final : public mmltk::controller::ValidationRuntime {
public:
 explicit PendingOutputRuntime(
  mmltk::frameworks::gpu::DeviceExecution execution, mmltk::controller::contracts::ComputeOperationOutcome outcome = mmltk::controller::contracts::ComputeOperationOutcome::Cancelled)
     : execution_(std::move(execution)), outcome_(outcome) {}
 mmltk::controller::ValidationRuntimeResult Run(mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token stop, const mmltk::controller::ComputeProgressSink&,
  const mmltk::backend::models::rfdetr::ValidationDelivery& delivery, std::uint64_t) override {
  namespace r = mmltk::backend::models::rfdetr;
  auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"café"});
  if (outcome_ == mmltk::controller::contracts::ComputeOperationOutcome::Succeeded) {
   delivery.samples_selected({}, catalog);
   return {.terminal = mmltk::controller::contracts::make_compute_terminal(outcome_)};
  }
  const std::array<std::uint32_t, 2> selected{7U, 9U};
  delivery.samples_selected(selected, catalog);
  auto source = PredictionSource::Device(execution_, {32U, 24U}, std::vector<float>(32U * 24U * 3U, 0.25F), {}, catalog);
  const r::PredictionRecord record{.dataset_index = 7};
  delivery.sample({record, {.chw = source.pixels(), .width = 32U, .height = 24U, .device = execution_.device, .custody = source.custody()}, source.annotations(), {}});
  if (outcome_ == mmltk::controller::contracts::ComputeOperationOutcome::Failed) throw std::runtime_error("selected runtime failed after delivery");
  mmltk::testsupport::StopGate stopped;
  static_cast<void>(stopped.Wait(stop));
  mmltk::controller::ValidationRuntimeResult result{.terminal = mmltk::controller::contracts::make_compute_terminal(mmltk::controller::contracts::ComputeOperationOutcome::Cancelled, 0U, 1U)};
  result.evaluation.emplace();
  result.evaluation->class_catalog = catalog;
  result.evaluation->summary.bbox.available = true;
  result.evaluation->summary.bbox.ap = 0.625;
  return result;
 }

private:
 mmltk::frameworks::gpu::DeviceExecution execution_;
 mmltk::controller::contracts::ComputeOperationOutcome outcome_;
};
}  // namespace
namespace {
mmltk::backend::imaging::raster::RenderedImageWriter::PngEncoder pending_png_encoder(mmltk::testsupport::TestGate& encoding) {
 return [gate = encoding.receipt()](const char* path, int width, int height, int channels, const void* pixels, int stride) {
  gate.ArriveAndWait();
  return stbi_write_png(path, width, height, channels, pixels, stride);
 };
}
class PendingValidationRun final {
 struct Release {
  mmltk::controller::ValidationSystem& validation;
  mmltk::testsupport::TestGate& encoding;
  void operator()() const {
   encoding.Release();
   validation.Shutdown();
  }
 };
 mmltk::testsupport::ScopedTestCleanup<Release> release_;

public:
 PendingValidationRun(mmltk::controller::ValidationSystem& validation, mmltk::testsupport::TestGate& encoding) : release_(Release{validation, encoding}) {
  static_cast<void>(validation.Start({}));
  REQUIRE(encoding.WaitEntered(std::chrono::seconds(5)));
 }
};
void check_validation_sample(const std::filesystem::path& path) {
 CHECK(path.filename() == "sample-7.png");
 CHECK_FALSE(std::filesystem::exists(path.string() + ".partial"));
 check_validation_png_pixels(path);
}
}  // namespace
TEST_CASE("validation public Stop and Shutdown settle an engaged PNG before terminal output facts", "[controller][validation][gpu][output]") {
 using namespace mmltk::controller;
 const bool shutdown = GENERATE(false, true);
 const auto root = mmltk::testsupport::make_temp_root("validation-stop-output");
 ApplicationDataFixture fixture{root};
 fixture.PrepareModel(contracts::FeatureId::Validate);
 auto [settings, dataset, model] = fixture.systems();
 const auto execution = mmltk::frameworks::gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 mmltk::testsupport::TestGate encoding("public validation PNG write");
 std::promise<ValidationSnapshot> terminal;
 ValidationSystem validation(
  settings, dataset, model, [&](DirectComputeConfiguration) { return std::make_unique<PendingOutputRuntime>(execution); },
  [&](auto event) {
   if (auto* changed = std::get_if<ValidationChanged>(&event);
    changed && !changed->snapshot.operation.active && changed->snapshot.operation.terminal.outcome == contracts::ComputeOperationOutcome::Cancelled) {
    try {
     terminal.set_value(changed->snapshot);
    } catch (const std::future_error&) {}
   }
  },
  [execution](int, int) { return DirectComputeConfiguration{execution}; }, {}, pending_png_encoder(encoding));
 PendingValidationRun run(validation, encoding);
 std::future<void> joining;
 std::promise<void> shutdown_entered;
 if (shutdown)
  joining = std::async(std::launch::async, [&] {
   shutdown_entered.set_value();
   validation.Shutdown();
  });
 else
  CHECK(validation.Stop().operation.active);
 mmltk::testsupport::ScopedTestCleanup unblock([&] { encoding.Release(); });
 if (shutdown) {
  mmltk::testsupport::await_test_promise(shutdown_entered, "validation shutdown entered");
  CHECK(joining.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
 }
 CHECK(validation.snapshot().operation.active);
 CHECK(validation.snapshot().operation.output.completed_samples == 0U);
 encoding.Release();
 const auto settled = mmltk::testsupport::await_test_promise(terminal, "public stopped validation output", std::chrono::seconds(10));
 if (joining.valid()) mmltk::testsupport::await_test_future(joining, "validation shutdown joined", std::chrono::seconds(10));
 CHECK(settled.operation.output.completed_samples == 1U);
 REQUIRE(settled.metrics);
 CHECK(settled.metrics->bbox.ap == 0.625);
 const auto path = std::filesystem::path(settled.operation.output.recent_sample);
 check_validation_sample(path);
}
TEST_CASE("validation runtime throw settles selected PNG before failure and restart", "[controller][validation][gpu][output]") {
 using namespace mmltk::controller;
 const auto root = mmltk::testsupport::make_temp_root("validation-throw-output");
 ApplicationDataFixture fixture{root};
 fixture.PrepareModel(contracts::FeatureId::Validate);
 auto [settings, dataset, model] = fixture.systems();
 const auto execution = mmltk::frameworks::gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 mmltk::testsupport::TestGate encoding("throwing validation PNG write");
 std::promise<ValidationSnapshot> failed, restarted;
 auto failure = failed.get_future();
 unsigned runs = 0;
 ValidationSystem validation(
  settings, dataset, model,
  [&](
   DirectComputeConfiguration) { return std::make_unique<PendingOutputRuntime>(execution, runs++ == 0 ? contracts::ComputeOperationOutcome::Failed : contracts::ComputeOperationOutcome::Succeeded); },
  [&](auto event) {
   if (auto* changed = std::get_if<ValidationChanged>(&event); changed && !changed->snapshot.operation.active) {
    try {
     if (changed->snapshot.operation.terminal.outcome == contracts::ComputeOperationOutcome::Failed)
      failed.set_value(changed->snapshot);
     else if (changed->snapshot.operation.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded)
      restarted.set_value(changed->snapshot);
    } catch (const std::future_error&) {}
   }
  },
  [execution](int, int) { return DirectComputeConfiguration{execution}; }, {}, pending_png_encoder(encoding));
 PendingValidationRun run(validation, encoding);
 CHECK(failure.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
 CHECK(validation.snapshot().operation.active);
 encoding.Release();
 const auto settled = mmltk::testsupport::await_test_future(failure, "throwing validation output", std::chrono::seconds(10));
 CHECK(settled.operation.terminal.detail.find("selected runtime failed after delivery") != std::string::npos);
 CHECK(settled.operation.output.completed_samples == 1U);
 const auto path = std::filesystem::path(settled.operation.output.recent_sample);
 check_validation_sample(path);
 static_cast<void>(validation.Start({}));
 const auto fresh = mmltk::testsupport::await_test_promise(restarted, "validation restart after throw", std::chrono::seconds(10));
 CHECK(fresh.operation.output.completed_samples == 0U);
 CHECK(fresh.operation.output.recent_sample.empty());
 CHECK(fresh.operation.output.directory != settled.operation.output.directory);
 CHECK(std::filesystem::is_regular_file(path));
 CHECK(runs == 2U);
}
TEST_CASE("validation caption anchors reject malformed coordinates and retain completed images", "[controller][validation][gpu][output]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace rfdetr = mmltk::backend::models::rfdetr;
 using namespace mmltk::controller;
 const bool ground_truth = GENERATE(false, true);
 const unsigned axis = GENERATE(0U, 1U);
 const auto coordinate = GENERATE(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -2.0F, 62.0F,
  static_cast<float>(std::numeric_limits<int>::min()));
 const bool valid = std::isfinite(coordinate) && static_cast<double>(coordinate) >= std::numeric_limits<int>::min() && static_cast<double>(coordinate) <= std::numeric_limits<int>::max();
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 const mmltk::testsupport::ScopedTempDir directory("validation-caption-coordinate");
 std::vector<std::filesystem::path> published;
 detail::ValidationSampleOutput output({execution}, {}, [&](const auto& path) { published.push_back(path); });
 contracts::ValidationRunPreview options;
 options.overlays.prediction_boxes = options.overlays.prediction_masks = false;
 options.overlays.ground_truth_boxes = options.overlays.ground_truth_masks = false;
 options.ground_truth_labels = ground_truth;
 options.prediction_labels = !ground_truth;
 const std::array<std::uint32_t, 2> selected{17U, 19U};
 output.Begin(directory.path(), options, selected);
 auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"café"});
 for (unsigned index = 0; index < 2; ++index) {
  rfdetr::Prediction detection{.class_reference = 0, .score = 0.8F, .bbox_xyxy = {2, 26, 64, 40}};
  if (index == 1) detection.bbox_xyxy[axis] = coordinate;
  auto source = PredictionSource::Device(execution, {64U, 48U}, std::vector<float>(64U * 48U * 3U, 0.25F), {detection}, catalog);
  const std::array truth{detection};
  const rfdetr::PredictionRecord record{.dataset_index = selected[index], .detections = {detection}};
  REQUIRE(output.Capture({record, {.chw = source.pixels(), .width = 64U, .height = 48U, .device = 0, .custody = source.custody()}, source.annotations(), truth}));
 }
 if (valid)
  output.Finish(true);
 else
  CHECK_THROWS_WITH(output.Finish(true), "validation caption coordinate is not finite and int-representable");
 REQUIRE(published.size() == (valid ? 2U : 1U));
 CHECK(published.front().filename() == "sample-17.png");
 for (const auto& path : published) {
  int width = 0, height = 0, channels = 0;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load(path.c_str(), &width, &height, &channels, 4), stbi_image_free);
  REQUIRE(pixels);
  CHECK(width == 64);
  CHECK(height == 48);
  CHECK(pixels.get()[(47U * 64U + 63U) * 4U] == 64U);
  CHECK_FALSE(std::filesystem::exists(path.string() + ".partial"));
 }
 if (!valid) CHECK_FALSE(std::filesystem::exists(directory.path() / "samples" / "sample-19.png"));
}
