#include "src/controller/subsystems/system/tests/prediction_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/controller/subsystems/validate/detail/validation_samples.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include <catch2/catch_test_macros.hpp>
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
using namespace mmltk::controller::test_support;
namespace mmltk::controller {
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
            auto frame =
                pool.Capture(nullptr, {2U, 2U}, 0U, {}, source.annotations(), source.classes(), 0, source.rgb8(), source.custody(), nullptr, nullptr, {}, true);
            REQUIRE(frame);
            frames[index] = frame;
            regions[index] = {std::move(frame), {static_cast<std::uint32_t>(index % 3U) * 2U, static_cast<std::uint32_t>(index / 3U) * 2U, 2U, 2U}};
        }
        auto candidate = runtime.AcquireOutput();
        fault.enabled = outer;
        receiver_fault.enabled = !outer;
        auto draw = std::async(std::launch::async, [&] { Composition::Draw(runtime, candidate, {6U, 4U}, regions, {}); });
        auto& failure_gate = outer ? fault.settlement : receiver_fault.upload;
        mmltk::testsupport::ScopedTestCleanup release([&] { failure_gate.Release(); });
        REQUIRE(failure_gate.WaitEntered(std::chrono::seconds(20)));
        CHECK(fault.callback_returned == outer);  // Event recording follows the complete outer callback.
        CHECK(receiver_fault.uploads == (outer ? 6U : 3U));
        for (const auto& source : sources) CHECK_FALSE(source.expired());
        auto extra = PredictionSource::Decoded({2U, 2U}, pixels);
        auto concurrent =
            pool.Capture(nullptr, {2U, 2U}, 0U, {}, extra.annotations(), extra.classes(), 0, extra.rgb8(), extra.custody(), nullptr, nullptr, {}, true);
        REQUIRE(concurrent);  // Source delivery continues while the independent draw owns its set.
        CHECK_FALSE(
            pool.Capture(nullptr, {2U, 2U}, 0U, {}, extra.annotations(), extra.classes(), 0, extra.rgb8(), extra.custody(), nullptr, nullptr, {}, true));
        failure_gate.Release();
        CHECK_THROWS(draw.get());
        CHECK(pool.HasUnsafeCustody());
        CHECK(retirement->fact().occupancy == 1U);
        regions = {};
        for (const auto& frame : frames) CHECK_FALSE(frame.expired());
        for (const auto& source : sources) CHECK_FALSE(source.expired());
        for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
            CHECK_THROWS(
                pool.Capture(nullptr, {2U, 2U}, 0U, {}, extra.annotations(), extra.classes(), 0, extra.rgb8(), extra.custody(), nullptr, nullptr, {}, true));
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
                                [&] {
                                    ++constructions;
                                    return std::make_unique<FakeNonvisualComputeRuntime>(ComputeScenario{.gate = gate});
                                },
                                [&](ValidationSystem::event_type event) {
                                    if (std::holds_alternative<ValidationChanged>(event)) settled.set_value(std::move(event));
                                }};
    const auto admitted = validation.Start({});
    CHECK(admitted.operation.active);
    observation->inspect_started.get_future().wait();
    CHECK(constructions == 0U);
    CHECK_THROWS_AS(dataset.Compile({}), contracts::BusyError);
    static_cast<void>(validation.Stop());
    const auto terminal = settled.get_future().get();
    CHECK(std::get<ValidationChanged>(terminal).snapshot.operation.terminal.outcome == contracts::ComputeOperationOutcome::Cancelled);
    CHECK(constructions == 0U);
    CHECK(observation->compile_calls == 0);
}
TEST_CASE("validation retains the limited sample atlas and selects detail without a producer", "[controller][gpu][validation]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace rfdetr = mmltk::backend::models::rfdetr;
    const bool labelled = GENERATE(false, true);
    const auto sample_count = GENERATE(std::size_t{2U}, std::size_t{6U});
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
    for (const auto index : selected_indices) {
        const rfdetr::PredictionRecord record{.dataset_index = index, .detections = source.detections()};
        samples.Capture(7U, {record, {.chw = source.pixels(), .width = 2U, .height = 2U, .device = 0, .custody = source.custody()}, source.annotations(), source.detections()});
    }
    const auto await = [&](auto predicate) {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(20), predicate));
    };
    await([&] {
        const auto snapshot = samples.snapshot();
        return std::cmp_equal(std::count(snapshot.sample_available.begin(), snapshot.sample_available.end(), true), sample_count);
    });
    const auto atlas = samples.snapshot();
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
    await([&] { return samples.snapshot().detail; });
    const auto detail = samples.snapshot();
    CHECK(detail.content_identity != atlas.content_identity);
    CHECK(detail.frame.extent == VisualExtent{2U, 2U});
    auto document = samples.BorrowDocument(detail.frame);
    REQUIRE(document.valid());
    CHECK(document.document->facts() == detail.document);
    CHECK(document.document->scene.frame_index == 3U);
    CHECK(document.document->scene.categories[0].value == "original");
    REQUIRE(document.image_metadata);
    document.pixels = {};
    auto retained = samples.BorrowFrame();
    REQUIRE(retained.valid());
    samples.Settle(7U, true);
    samples.Begin(8U, selected_indices);  // Empty newer run cannot replace the retained detail's atlas.
    source = PredictionSource::Device(execution, {2U, 2U}, pixels, detections,
                                      std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"replacement"}));
    samples.CloseDetail();
    await([&] { return !samples.snapshot().detail; });
    CHECK(samples.snapshot().content_identity == atlas.content_identity);
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
    await([&] { return samples.snapshot().frame.revision > before.frame.revision; });
    CHECK(samples.snapshot().content_identity == atlas.content_identity);
    CHECK(samples.snapshot().overlays == ValidationOverlays{false, false, false, false});
    CHECK(samples.ImageSnapshot(samples.snapshot().frame)->overlays == samples.snapshot().overlays);
    atlas_reader = {};
    samples.Select({7U, 1U});
    await([&] { return samples.snapshot().detail; });
    samples.Begin(9U, selected_indices);
    const auto capture = [&](std::uint32_t index) {
        const rfdetr::PredictionRecord record{.dataset_index = index, .detections = source.detections()};
        samples.Capture(9U, {record, {.chw = source.pixels(), .width = 2U, .height = 2U, .device = 0, .custody = source.custody()}, source.annotations(), source.detections()});
    };
    capture(1U);  // A partial newer set stays pending while old detail is selected.
    samples.CloseDetail();
    await([&] { return !samples.snapshot().detail; });
    CHECK(samples.snapshot().content_identity == atlas.content_identity);
    for (const auto overlays : {ValidationOverlays{true, true, true, true, false, true}, ValidationOverlays{true, true, true, true, true, false},
                                ValidationOverlays{true, true, true, true, false, false}, ValidationOverlays{true, false, false, false}, ValidationOverlays{false, true, false, false},
                                ValidationOverlays{false, false, true, false}, ValidationOverlays{false, false, false, true}}) {
        const auto revision = samples.snapshot().frame.revision;
        samples.SetOverlays(overlays);
        await([&] { return samples.snapshot().frame.revision > revision; });
        CHECK(samples.snapshot().content_identity == atlas.content_identity);
        CHECK(samples.snapshot().overlays == overlays);
    }
    const auto preserved = samples.snapshot();
    std::size_t failure_notifications = 0U;
    {
        std::scoped_lock lock(mutex);
        failure_notifications = notifications + 2U;
    }
    fault.partial_draw = true;
    samples.SetOverlays({true, true, true, true});
    await([&] { return notifications >= failure_notifications; });
    CHECK(samples.snapshot().frame == preserved.frame);
    CHECK(samples.snapshot().overlays == preserved.overlays);
    CHECK(samples.snapshot().overlay_selection.value == preserved.overlays);
    CHECK(samples.snapshot().overlay_selection.revision > preserved.overlay_selection.revision);
    CHECK(samples.ImageSnapshot(preserved.frame)->overlays == preserved.overlays);
    fault.partial_draw = false;
    samples.SetOverlays({true, true, true, true});  // Repeating a refused request is an explicit retry.
    await([&] { return samples.snapshot().frame.revision > preserved.frame.revision; });
    CHECK(samples.snapshot().content_identity == atlas.content_identity);
    fault.draw_failures_remaining = 1U;
    capture(3U);  // Last capture's ordinary draw failure retries without another producer callback.
    await([&] { return samples.snapshot().sample_identities[0].generation == 9U; });
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
    const auto outcome = GENERATE(contracts::ComputeOperationOutcome::Succeeded, contracts::ComputeOperationOutcome::Failed,
                                  contracts::ComputeOperationOutcome::Cancelled);
    const bool refuse = GENERATE(false, true);
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    std::mutex mutex;
    std::condition_variable changed;
    detail::ValidationSamples samples({.device = 0, .maximum_width = 512U, .maximum_height = 576U}, [&] {
        std::scoped_lock lock(mutex);
        changed.notify_all();
    });
    const auto await = [&](auto predicate) {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(20), predicate));
    };
    const std::array<std::uint32_t, 2U> indices{3U, 7U};
    samples.Begin(1U, indices);
    await([&] { return samples.snapshot().frame.valid(); });
    const auto empty = samples.snapshot();
    CHECK(empty.frame.extent == VisualExtent{512U, 576U});
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
            REQUIRE(cudaMemcpy2D(pixels.data(), 512U * 4U, reinterpret_cast<const void*>(plane.data), plane.descriptor.pitch_bytes,
                                 512U * 4U, 576U, cudaMemcpyDeviceToHost) == cudaSuccess);
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
    samples.Settle(1U, true);
    await([&] { return samples.snapshot().sample_available[0]; });
    if (detail_open) {
        samples.Select({1U, 3U});
        await([&] { return samples.snapshot().detail; });
    }
    const auto incumbent = samples.snapshot();
    const auto incumbent_metadata = samples.ImageSnapshot(incumbent.frame);
    REQUIRE(incumbent_metadata);
    samples.Begin(2U, indices);
    const std::array<float, 12U> green{0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    source = PredictionSource::Device(execution, {2U, 2U}, green, {},
        std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"replacement"}));
    samples.Begin(1U, indices);  // A stale selection callback cannot restart an older generation.
    capture(1U, 7U);  // A stale producer cannot fill the new set's matching slot.
    capture(2U, 7U);
    if (!detail_open) await([&] { return samples.snapshot().sample_identities[1].generation == 2U; });
    const auto preview = samples.snapshot();
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
        await([&] { return samples.snapshot().content_identity == incumbent.content_identity
            && (!republished || samples.snapshot().frame.revision > preview.frame.revision); });
        const auto after = samples.snapshot();
        CHECK(after.selected == incumbent.selected);
        CHECK(after.document == incumbent.document);
        CHECK(after.sample_identities == incumbent.sample_identities);
        CHECK(after.sample_available == incumbent.sample_available);
        const auto metadata = samples.ImageSnapshot(after.frame);
        REQUIRE(metadata);
        CHECK(metadata->samples[0].original_extent == incumbent_metadata->samples[0].original_extent);
        if (detail_open) {
            auto document = samples.BorrowDocument(after.frame);
            REQUIRE(document.valid());
            CHECK(document.document->facts() == incumbent.document);
            CHECK(document.document->scene.categories[0].value == "retained");
        }
    } else {
        await([&] { return samples.snapshot().frame.revision > preview.frame.revision; });
        CHECK(samples.snapshot().sample_identities[1].generation == 2U);
        CHECK_FALSE(samples.snapshot().sample_available[0]);
        CHECK(samples.snapshot().sample_available[1]);
    }
    auto image = samples.BorrowFrame();
    REQUIRE(image.valid());
    const auto metadata = samples.ImageSnapshot(samples.snapshot().frame);
    REQUIRE(metadata);
    const auto& sample = metadata->samples[metadata->samples[0].available ? 0U : 1U];
    const auto& read = image.plane(0U);
    read.context().Bind();
    std::array<std::uint8_t, 4U> pixel{};
    const auto plane = read.plane();
    REQUIRE(cudaMemcpy(pixel.data(), reinterpret_cast<const void*>(plane.data + sample.crop.y * plane.descriptor.pitch_bytes + sample.crop.x * 4U),
                       4U, cudaMemcpyDeviceToHost) == cudaSuccess);
    CHECK(pixel == (restored ? std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U} : std::array<std::uint8_t, 4U>{0U, 255U, 0U, 255U}));
    image = {};
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
    const auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"prediction", "truth"});
    const std::array<float, 12U * 12U * 3U> pixels{};
    std::array<std::uint8_t, 12U * 12U> mask{};
    mask[3U * 12U + 3U] = 1U;
    const rfdetr::Prediction prediction{.class_reference = 0,
                                        .class_domain = mmltk::backend::data::catalog::ClassReferenceDomain::Foreground,
                                        .score = 0.75F,
                                        .bbox_xyxy = {2, 2, 4, 4},
                                        .has_mask = true};
    auto truth = prediction;
    truth.class_reference = same_class ? 0 : 1;
    truth.bbox_xyxy = {8, 8, 10, 10};
    truth.mask = {.height = 12U, .width = 12U, .area = 2U, .runs = {{3U * 12U + 3U, 1U}, {8U * 12U + 8U, 1U}}};
    truth.has_mask = true;
    auto source = PredictionSource::Device(execution, {12U, 12U}, pixels, {prediction}, catalog, mask);
    gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
    detail::PredictionPreviewPool pool(execution, context);
    const std::array ground_truth{truth};
    auto frame = pool.Capture(source.pixels(), source.extent(), 0U, source.detections(), source.annotations(), catalog, 2, nullptr, source.custody(), nullptr,
                              nullptr, ground_truth, true);
    REQUIRE(frame);
    const std::array regions{Composition::Region{frame, {0U, 0U, 12U, 12U}}};
    gpu::SystemImageRuntime runtime(
        {.device = 0, .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .output_buffer_count = 2U, .adopted_context = context});
    for (unsigned flags = 0U; flags < 16U; ++flags) {
        const Composition::Options options{bool(flags & 1U), bool(flags & 2U), bool(flags & 4U), bool(flags & 8U), complementary};
        auto candidate = runtime.AcquireOutput();
        Composition::Draw(runtime, candidate, {12U, 12U}, regions, options);
        auto completed = runtime.CommitOutput(std::move(candidate));
        auto image = completed.Borrow();
        REQUIRE(image.valid());
        std::array<std::uint8_t, 12U * 12U * 4U> rgba{};
        context.Bind();
        const auto semantic = image.plane(1U).plane();
        REQUIRE(cudaMemcpy2D(rgba.data(), 12U * 4U, reinterpret_cast<const void*>(semantic.data), semantic.descriptor.pitch_bytes, 12U * 4U, 12U,
                             cudaMemcpyDeviceToHost) == cudaSuccess);
        const auto pixel = [&](std::size_t x, std::size_t y) {
            const auto offset = (y * 12U + x) * 4U;
            return std::array{rgba[offset], rgba[offset + 1U], rgba[offset + 2U], rgba[offset + 3U]};
        };
        // Disjoint known probes: red prediction, cyan truth, and untouched background.
        // These expected values do not call the production palette or rasterizer.
        const std::array<std::uint8_t, 4U> empty{};
        CHECK(pixel(1U, 3U) == (options.prediction_boxes ? std::array<std::uint8_t, 4U>{255, 0, 0, 255} : empty));
        const bool cyan = same_class == complementary;
        const std::array<std::uint8_t, 4U> truth_mask{static_cast<std::uint8_t>(cyan ? 0 : 255), static_cast<std::uint8_t>(cyan ? 255 : 0),
                                                   static_cast<std::uint8_t>(cyan ? 255 : 0), 96};
        auto truth_box = truth_mask;
        truth_box[3] = 255;
        auto overlap = options.prediction_masks ? std::array<std::uint8_t, 4U>{255, 0, 0, 96} : empty;
        if (options.ground_truth_masks) {
            overlap = truth_mask;
            if (complementary && options.prediction_masks) overlap[0] = 255;
        }
        CHECK(pixel(3U, 3U) == overlap);
        CHECK(pixel(7U, 8U) == (options.ground_truth_boxes ? truth_box : empty));
        CHECK(pixel(8U, 8U) == (options.ground_truth_masks ? truth_mask : empty));
        CHECK(pixel(11U, 0U) == empty);
        const auto clean = image.plane(0U).plane();
        REQUIRE(cudaMemcpy2D(rgba.data(), 12U * 4U, reinterpret_cast<const void*>(clean.data), clean.descriptor.pitch_bytes, 12U * 4U, 12U,
                             cudaMemcpyDeviceToHost) == cudaSuccess);
        CHECK(pixel(3U, 3U) == std::array<std::uint8_t, 4U>{0, 0, 0, 255});
        CHECK(frame->classes()[0] == "prediction");
        CHECK(frame->classes()[1] == "truth");
    }
}
namespace {
class RefusedValidationPreview final : public ValidationRuntime {
   public:
    explicit RefusedValidationPreview(std::atomic_size_t& runs) : runs_(runs) {}
    ValidationRuntimeResult Run(mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token, const ComputeProgressSink& progress,
                                const mmltk::backend::models::rfdetr::ValidationDelivery& delivery) override {
        namespace rfdetr = mmltk::backend::models::rfdetr;
        ++runs_;
        const std::array<std::uint32_t, 1U> selected{3U};
        const auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"measured"});
        delivery.samples_selected(selected, catalog);
        const rfdetr::PredictionRecord prediction{.dataset_index = 3U};
        const mmltk::backend::ml::runtime::AnalysisAnnotationStorage annotations{.class_catalog = catalog};
        delivery.sample({prediction, {.preview_failure = "preview storage refused"}, annotations, {}});
        progress({1U, 1U, 1U, "Validating"});
        ValidationRuntimeResult result{.terminal = contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded, 0U, 1U)};
        result.evaluation.emplace();
        result.evaluation->summary.bbox.available = true;
        result.evaluation->summary.bbox.ap = 0.625;
        result.evaluation->class_catalog = catalog;
        return result;
    }

   private:
    std::atomic_size_t& runs_;
};
}  // namespace
TEST_CASE("validation semantic metrics survive optional preview refusal without reinference", "[controller][systems][gpu][validation]") {
    const auto root = mmltk::testsupport::make_temp_root("validation-preview-refusal");
    ApplicationDataFixture fixture{root};
    fixture.PrepareModel(contracts::FeatureId::Validate);
    auto [settings, dataset, model] = fixture.systems();
    std::atomic_size_t runs = 0U;
    std::promise<ValidationSnapshot> finished;
    ValidationSystem validation(
        settings, dataset, model, [&] { return std::make_unique<RefusedValidationPreview>(runs); },
        [&](auto event) {
            if (auto* changed = std::get_if<ValidationChanged>(&event); changed && !changed->snapshot.operation.active
                && changed->snapshot.operation.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded) {
                try { finished.set_value(changed->snapshot); } catch (const std::future_error&) {}
            }
        },
        {}, {.device = 0, .maximum_width = 768U, .maximum_height = 512U});
    static_cast<void>(validation.Start({}));
    const auto result = finished.get_future().get();
    REQUIRE(result.metrics);
    CHECK(result.metrics->bbox.ap == 0.625);
    CHECK(result.operation.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
    CHECK(result.operation.terminal.completed == 1U);
    CHECK(std::ranges::none_of(result.sample_available, [](bool available) { return available; }));
    CHECK_THROWS_AS(validation.SelectSample({result.operation.generation_frontier, 3U}), contracts::InvalidIntentError);
    static_cast<void>(validation.CloseDetail());
    validation.Shutdown();
    CHECK(runs == 1U);
}
}  // namespace mmltk::controller
