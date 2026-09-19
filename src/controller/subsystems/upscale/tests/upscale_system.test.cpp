#include "src/controller/subsystems/validate/detail/validation_samples.h"
#include "src/frameworks/serialization/reflected_cbor.h"
#include <algorithm>
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/controller/subsystems/upscale/tests/support/upscale_system_fixture.h"
#include "src/controller/subsystems/explore/tests/support/explore_system_fixture.h"
#include "src/controller/presentation/tests/support/visual_system_fixture.h"
#include "src/controller/subsystems/upscale/upscale_system.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/backend/imaging/upscale/upscale_execution.h"
#include "src/common/system/runtime_paths.h"
#include "src/common/io/scoped_fd.h"
#include "src/controller/presentation/presentation_system.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/image_failure.h"
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <utility>
#include <variant>
#include <vector>
namespace mmltk::controller {
namespace {
using namespace visual_test_support;
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
class FailingUpscaleAlgorithm final : public UpscaleAlgorithm {
   public:
    void Semantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) override {}
    explicit FailingUpscaleAlgorithm(std::shared_ptr<std::atomic_uint32_t> runs, const bool physical = false) : runs_(std::move(runs)), physical_(physical) {}
    void Warm() override {}
    void Run(UpscaleKernel, mmltk::frameworks::gpu::ImagePlaneView, const mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t,
             const std::function<bool()>&) override {
        if (runs_->fetch_add(1U, std::memory_order_acq_rel) == 1U) {
            if (physical_)
                throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(
                    std::make_exception_ptr(mmltk::frameworks::gpu::CudaError(cudaErrorIllegalAddress, "injected shared execution failure")));
            throw std::runtime_error("deterministic Upscale failure");
        }
        Fill(target, 1U);
    }

   private:
    std::shared_ptr<std::atomic_uint32_t> runs_;
    bool physical_ = false;
};
struct UpscaleActivationProbe final {
    std::atomic<std::size_t> warms{0U};
    std::atomic<std::size_t> runs{0U};
    std::atomic<std::size_t> fallback_activations{0U};
    std::array<std::atomic_bool, 3U> ready{};
    std::atomic_bool fail_warm{false};
    std::atomic_bool hold_warm{false};
    std::promise<void> warm_entered;
    std::promise<void> warm_release;
    // CLEANUP-IGNORE: This shared future is activation-gate custody, not shell wiring state.
    std::shared_future<void> warm_released = warm_release.get_future().share();
    // CLEANUP-IGNORE: First-warm completion is a separate one-shot activation observation.
    std::promise<void> first_warm_completed;
};
class ActivationUpscaleAlgorithm final : public UpscaleAlgorithm {
   public:
    void Semantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) override {}
    explicit ActivationUpscaleAlgorithm(std::shared_ptr<UpscaleActivationProbe> probe) : probe_(std::move(probe)) {}
    void Warm() override {
        const auto call = probe_->warms.fetch_add(1U, std::memory_order_acq_rel);
        if (call == 0U && probe_->hold_warm.load(std::memory_order_acquire)) {
            probe_->warm_entered.set_value();
            probe_->warm_released.wait();
        }
        if (probe_->fail_warm.exchange(false, std::memory_order_acq_rel)) throw std::runtime_error("deterministic Upscale warm failure");
        Activate();
        if (call == 0U) probe_->first_warm_completed.set_value();
    }
    void Run(UpscaleKernel, const mmltk::frameworks::gpu::ImagePlaneView source, const mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t,
             const std::function<bool()>&) override {
        if (!probe_->ready[0U].load(std::memory_order_acquire)) {
            probe_->fallback_activations.fetch_add(1U, std::memory_order_acq_rel);
            Activate();
        }
        probe_->runs.fetch_add(1U, std::memory_order_acq_rel);
        Fill(target, *reinterpret_cast<const std::uint8_t*>(source.data));
    }

   private:
    void Activate() noexcept {
        for (auto& ready : probe_->ready) ready.store(true, std::memory_order_release);
    }
    std::shared_ptr<UpscaleActivationProbe> probe_;
};
struct UpscaleAdmissionRaceProbe final {
    std::atomic<std::uint8_t> received_value{0U};
    std::atomic<std::uint32_t> received_width{0U};
    std::atomic<std::uint32_t> received_height{0U};
};
class RacingUpscaleAlgorithm final : public UpscaleAlgorithm {
   public:
    void Semantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) override {}
    explicit RacingUpscaleAlgorithm(std::shared_ptr<UpscaleAdmissionRaceProbe> probe) : probe_(std::move(probe)) {}
    void Warm() override {}
    void Run(UpscaleKernel, const mmltk::frameworks::gpu::ImagePlaneView source, const mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t,
             const std::function<bool()>&) override {
        const auto value = *reinterpret_cast<const std::uint8_t*>(source.data);
        probe_->received_value.store(value, std::memory_order_release);
        probe_->received_width.store(source.descriptor.width, std::memory_order_release);
        probe_->received_height.store(source.descriptor.height, std::memory_order_release);
        Fill(target, value);
    }

   private:
    std::shared_ptr<UpscaleAdmissionRaceProbe> probe_;
};
struct UpscaleReleaseFailureProbe final {
    std::promise<void> warmed;
    std::atomic<std::size_t> releases{0U};
    std::atomic_bool destroyed{false};
};
class ReleaseFailingUpscaleAlgorithm final : public UpscaleAlgorithm {
   public:
    void Semantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) override {}
    explicit ReleaseFailingUpscaleAlgorithm(std::shared_ptr<UpscaleReleaseFailureProbe> probe)
        : probe_(std::move(probe)), failure_(std::make_exception_ptr(std::runtime_error("deterministic Upscale release failure"))) {}
    ~ReleaseFailingUpscaleAlgorithm() override { probe_->destroyed.store(true, std::memory_order_release); }
    void Warm() override { probe_->warmed.set_value(); }
    void Run(UpscaleKernel, mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t,
             const std::function<bool()>&) override {}
    [[nodiscard]] Release ReleaseResources() noexcept override {
        probe_->releases.fetch_add(1U, std::memory_order_acq_rel);
        return {.all_released = false, .failure = failure_};
    }

   private:
    std::shared_ptr<UpscaleReleaseFailureProbe> probe_;
    std::exception_ptr failure_;
};
[[nodiscard]] auto settle_upscale_on_exit(UpscaleSystem& upscale, std::promise<void>& release) {
    return mmltk::testsupport::ScopedTestCleanup{[&upscale, &release] {
        mmltk::testsupport::release_test_promise(release);
        upscale.Stop();
        upscale.Shutdown();
    }};
}
struct UpscaleSourceFixture final {
    MutableVisualSource source;
    EventGate events;
    UpscaleSystem upscale;
    UpscaleSourceFixture(const std::shared_ptr<FakeImageBackend>& backend, VisualExtent extent, std::uint8_t value, VisualRuntimeFactory runtime)
        : source(backend, extent, value),
          upscale{kDevice, std::move(runtime), [this](const VisualFrame& frame) { return source.BorrowExact(frame); },
                  [this](UpscaleSystem::event_type) { events.Advance(); }} {}
};
TEST_CASE("Upscale owns and publishes its receiver image") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore opened_explore{backend, {32U, 32U}};
    auto& explore = opened_explore.system();
    EventGate upscale_events;
    // CLEANUP-IGNORE: Upscale and Annotation are independently sealed receiver systems with different model factories,
    // intents, snapshots, and output invariants.
    auto selected_kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [selected_kernel] { return std::make_unique<TestUpscaleAlgorithm>(selected_kernel); }, 4U),
                          borrow_exactly_from(explore), [&upscale_events](UpscaleSystem::event_type) { upscale_events.Advance(); }};
    CHECK_FALSE(upscale.BorrowFrame().valid());
    static_cast<void>(upscale.Start(test_upscale_request({
        .source = explore.snapshot().frame,
    })));
    REQUIRE(upscale_events.Wait([&] { return upscale.snapshot().ready; }));
    REQUIRE(upscale.BorrowFrame().valid());
    CHECK(upscale.snapshot().frame.extent.width == 128U);
    CHECK(upscale.snapshot().frame.extent.height == 128U);
    CHECK(selected_kernel->load(std::memory_order_acquire) == UpscaleKernel::Default);
    static_cast<void>(upscale.Start(test_upscale_request({
        .source = explore.snapshot().frame,
        .kernel = UpscaleKernel::ShiftLut,
    })));
    REQUIRE(upscale_events.Wait([&] { return selected_kernel->load(std::memory_order_acquire) == UpscaleKernel::ShiftLut; }));
    static_cast<void>(upscale.Start(test_upscale_request({
        .source = explore.snapshot().frame,
        .kernel = UpscaleKernel::RealPlksr,
    })));
    REQUIRE(upscale_events.Wait([&] { return selected_kernel->load(std::memory_order_acquire) == UpscaleKernel::RealPlksr; }));
}
TEST_CASE("Upscale equal extent source changes replace actual receiver pixels") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<UpscaleExtentProbe>();
    UpscaleSourceFixture subject{
        backend,
        {16U, 8U},
        3U,
        RuntimeFactory(
            0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, [probe] { return std::make_unique<ExtentUpscaleAlgorithm>(probe); }, 4U)};
    auto& source = subject.source;
    auto& events = subject.events;
    auto& upscale = subject.upscale;
    for (const auto value : {3U, 19U, 3U}) {
        source.Publish({16U, 8U}, static_cast<std::uint8_t>(value));
        const auto input = source.frame();
        static_cast<void>(upscale.Start(test_upscale_request({.source = input})));
        REQUIRE(events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().input == input; }));
        const auto output = upscale.BorrowDocument(upscale.snapshot().frame);
        REQUIRE(output.valid());
        const auto plane = output.pixels.plane(0U).plane();
        const auto* pixels = reinterpret_cast<const std::uint8_t*>(plane.data);
        CHECK(pixels[0] == value);
        CHECK(pixels[(plane.descriptor.height - 1U) * plane.descriptor.pitch_bytes + plane.descriptor.row_bytes() - 1U] == value);
    }
}
TEST_CASE("Upscale exact repeats avoid copying and method switches preserve completed pixels") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}, 3U};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    auto semantics = std::make_shared<std::atomic_uint32_t>(0U);
    std::atomic_uint32_t copies{0U};
    EventGate events;
    const VisualDiagnosticSink diagnostics{.context = &copies, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
                                               if (fact.operation == VisualDiagnosticOperation::CopyCompleted)
                                                   static_cast<std::atomic_uint32_t*>(context)->fetch_add(1U);
                                           }};
    UpscaleSystem upscale{kDevice, TestUpscaleAlgorithm::CreateRuntime(backend, kernel, runs, semantics),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); }, [&events](UpscaleSystem::event_type) { events.Advance(); },
                          diagnostics};
    for (const auto selected : {UpscaleKernel::Default, UpscaleKernel::ShiftLut, UpscaleKernel::Default}) {
        const auto request = test_upscale_request({.source = source.frame(), .kernel = selected});
        static_cast<void>(upscale.Start(request));
        REQUIRE(events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().kernel == selected; }));
        const auto completed = upscale.snapshot().frame;
        CHECK(upscale.ObserveWorkspace().product_revision == completed.revision);
        const auto copied = copies.load();
        const auto inferred = runs->load();
        {
            const auto output = upscale.BorrowDocument(completed);
            REQUIRE(output.valid());
            CHECK(*reinterpret_cast<const std::uint8_t*>(output.pixels.plane(0U).plane().data) == static_cast<std::uint8_t>(selected) + 1U);
        }
        const auto semantic_runs = semantics->load();
        const auto allocation = upscale.BorrowDocument(completed).pixels.plane(0U).plane().allocation;
        for (const bool original : {false, true, false, true}) {
            auto repeated = request;
            repeated.original_content = original;
            const auto selected_result = upscale.Start(repeated);
            CHECK(selected_result.ready);
            CHECK(selected_result.frame == completed);
            CHECK(selected_result.methods[static_cast<std::size_t>(selected)].completed == repeated);
            CHECK(upscale.BorrowDocument(completed).pixels.plane(0U).plane().allocation == allocation);
            CHECK(copies.load() == copied);
            CHECK(runs->load() == inferred);
            CHECK(semantics->load() == semantic_runs);
        }
    }
    CHECK(runs->load() == 2U);
    const auto retained = upscale.snapshot().frame;
    upscale.Stop();
    CHECK_FALSE(upscale.snapshot().ready);
    CHECK(upscale.BorrowDocument(retained).valid());
    const auto previous = source.frame();
    source.Publish(previous.extent, 19U);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().input == source.frame(); }));
    CHECK(runs->load() == 3U);
    auto cropped = source.frame();
    cropped.content = {1U, 1U, 4U, 3U};
    cropped.source_extent = {4000U, 2000U};
    static_cast<void>(upscale.Start(test_upscale_request({.source = cropped})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().input == cropped; }));
    CHECK(runs->load() == 4U);
    CHECK(upscale.snapshot().frame.source_extent == cropped.source_extent);
}
TEST_CASE("Validation clean identity reuses native Upscale while replacing paired semantics", "[controller][gpu][validation][upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    namespace rfdetr = mmltk::backend::models::rfdetr;
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    EventGate source_events, output_events;
    detail::ValidationSamples samples({.device = 0, .maximum_width = 512U, .maximum_height = 576U}, [&] { source_events.Advance(); });
    const auto catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"truth"});
    auto pixels = std::make_shared<std::array<std::uint8_t, 8U * 8U * 3U>>();
    const mmltk::backend::ml::runtime::AnalysisAnnotationStorage annotations{.class_catalog = catalog};
    const std::array truth{rfdetr::Prediction{.class_reference = 0, .bbox_xyxy = {2, 2, 5, 5}}};
    const std::array<std::uint32_t, 1U> indices{3U};
    samples.Begin(1U, indices);
    const rfdetr::PredictionRecord record{.dataset_index = 3U};
    samples.Capture(1U, {record, {.width = 8U, .height = 8U, .device = 0, .rgb8 = pixels->data(), .custody = pixels}, annotations, truth});
    samples.Settle(1U, true);
    REQUIRE(source_events.Wait([&] { return samples.snapshot().sample_available[0]; }, 20s));
    samples.Select({1U, 3U});
    REQUIRE(source_events.Wait([&] { return samples.snapshot().detail; }, 20s));
    const auto initial = samples.snapshot();
    REQUIRE(initial.frame.clean_revision != 0U);
    REQUIRE(initial.frame.content == VisualRegion{0U, 0U, 8U, 8U});
    std::atomic_uint32_t runs = 0U;
    UpscaleSystem upscale{kDevice,
                          make_native_upscale_runtime_factory(kDevice,
                                                              [&](Stage stage) {
                                                                  if (stage == Stage::BasicLaunchAdmitted) ++runs;
                                                              }),
                          [&samples](const VisualFrame& frame) { return samples.BorrowDocument(frame); },
                          [&](UpscaleSystem::event_type) { output_events.Advance(); }};
    static_cast<void>(upscale.Start({.source = initial.frame, .document = initial.document}));
    REQUIRE(output_events.Wait([&] { return upscale.snapshot().ready; }, 120s));
    const auto clean_launches = runs.load();
    REQUIRE(clean_launches > 0U);
    const auto output = upscale.snapshot().frame;
    const auto original_metadata = upscale.ImageSourceMetadata(output);
    REQUIRE(original_metadata);
    const auto expected_initial = mmltk::frameworks::serialization::reflected_transport_value(*samples.ImageSnapshot(initial.frame));
    REQUIRE(expected_initial);
    CHECK(*original_metadata == *expected_initial);
    samples.SetOverlays({false, false, false, false});
    REQUIRE(source_events.Wait([&] { return samples.snapshot().frame.revision != initial.frame.revision; }, 20s));
    const auto changed = samples.snapshot();
    CHECK(visual_clean_content_identity(changed.frame) == visual_clean_content_identity(initial.frame));
    static_cast<void>(upscale.Start({.source = changed.frame, .document = changed.document}));
    REQUIRE(output_events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().input == changed.frame; }, 120s));
    CHECK(runs.load() == clean_launches);
    CHECK(upscale.snapshot().frame.clean_revision == output.clean_revision);
    const auto current_metadata = upscale.ImageSourceMetadata(upscale.snapshot().frame);
    REQUIRE(current_metadata);
    const auto expected_current = mmltk::frameworks::serialization::reflected_transport_value(*samples.ImageSnapshot(changed.frame));
    REQUIRE(expected_current);
    CHECK(*current_metadata == *expected_current);
    CHECK(*original_metadata == *expected_initial);
    auto result = upscale.BorrowDocument(upscale.snapshot().frame);
    REQUIRE(result.valid());
    // The processing receiver owns these planes after Validation releases its sources.
    samples.Shutdown();
    const auto& semantic = result.pixels.plane(1U);
    semantic.context().Bind();
    const auto plane = semantic.plane();
    std::vector<std::uint8_t> rgba(plane.descriptor.width * plane.descriptor.height * 4U);
    REQUIRE(cudaMemcpy2D(rgba.data(), plane.descriptor.row_bytes(), reinterpret_cast<const void*>(plane.data), plane.descriptor.pitch_bytes,
                         plane.descriptor.row_bytes(), plane.descriptor.height, cudaMemcpyDeviceToHost) == cudaSuccess);
    CHECK(std::ranges::all_of(rgba, [](auto byte) { return byte == 0U; }));
}
TEST_CASE("Upscale semantic revisions reuse clean pixels and retain exact input provenance") {
    const auto source_kind = GENERATE(PresentationSourceKind::Explore, PresentationSourceKind::Validation);
    const auto displayed = [source_kind](VisualFrame frame) {
        frame.source.kind = source_kind;
        return frame;
    };
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}, 3U};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    EventGate events;
    UpscaleSystem upscale{kDevice, TestUpscaleAlgorithm::CreateRuntime(backend, kernel, runs),
                          [&source, source_kind](const VisualFrame& frame) {
                              if (frame.source.kind != source_kind) return VisualDocumentRead{};
                              auto native = frame;
                              native.source.kind = PresentationSourceKind::Explore;
                              auto result = source.BorrowExact(native);
                              result.image_metadata = std::make_shared<const mmltk::frameworks::serialization::wire::Value>(frame.revision);
                              return result;
                          },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }};
    const auto initial = displayed(source.frame());
    static_cast<void>(upscale.Start(test_upscale_request({.source = initial})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    const auto completed_frame = upscale.snapshot().frame;
    const auto retained_metadata = upscale.ImageSourceMetadata(completed_frame);
    REQUIRE(retained_metadata);
    CHECK(*retained_metadata == mmltk::frameworks::serialization::wire::Value(initial.revision));
    const auto clean_revision = completed_frame.clean_revision;
    const auto baseline = upscale.BorrowDocument(upscale.snapshot().frame);
    REQUIRE(baseline.valid());
    backend->watched_copy_source.store(baseline.pixels.plane(1U).plane().data);
    CHECK(upscale.snapshot().input == initial);
    source.SetSemantics(55U);
    const auto current = displayed(source.frame());
    REQUIRE(current.clean_revision == initial.clean_revision);
    REQUIRE(current.revision != initial.revision);
    CHECK(upscale.ImageSourceMetadata(completed_frame) == retained_metadata);
    const auto copies = backend->same_copies.load(std::memory_order_acquire);
    static_cast<void>(upscale.Start(test_upscale_request({.source = current})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().input == current; }));
    CHECK(runs->load(std::memory_order_acquire) == 1U);
    const auto next_metadata = upscale.ImageSourceMetadata(upscale.snapshot().frame);
    REQUIRE(next_metadata);
    CHECK(*next_metadata == mmltk::frameworks::serialization::wire::Value(current.revision));
    CHECK(*retained_metadata == mmltk::frameworks::serialization::wire::Value(initial.revision));
    CHECK_FALSE(upscale.ImageSourceMetadata(completed_frame));
    CHECK(upscale.snapshot().frame.clean_revision == clean_revision);
    CHECK(backend->same_copies.load(std::memory_order_acquire) == copies + 2U);
    CHECK(backend->watched_source_copies.load() == 0U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(baseline.pixels.plane(1U).plane().data) == 23U);
    const auto output = upscale.BorrowDocument(upscale.snapshot().frame);
    REQUIRE(output.valid());
    REQUIRE(output.pixels.plane_count() == 2U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(output.pixels.plane(0U).plane().data) == 1U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(output.pixels.plane(1U).plane().data) == 55U);
    CHECK(output.document->scene.categories.size() == 1U);
    CHECK(output.document->facts().resource == test_document({}).document->facts().resource);
    CHECK(output.document->facts().meaning_identity != test_document({}).document->facts().meaning_identity);
}
TEST_CASE("Upscale cached selection cannot redirect an active candidate and all methods retain one input") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}, 3U};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    auto gate = std::make_shared<MutationCommitProbe>();
    auto entered = gate->committed.get_future();
    std::atomic_uint32_t borrows{0U};
    EventGate events;
    DiagnosticCapture diagnostics;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [kernel, gate, runs] { return std::make_unique<TestUpscaleAlgorithm>(kernel, gate, runs, 3U); }, 4U),
                          [&source, &borrows](const VisualFrame& frame) {
                              ++borrows;
                              return source.BorrowExact(frame);
                          },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }, diagnostics.sink()};
    auto settle_upscale = settle_upscale_on_exit(upscale, gate->release);
    std::array<VisualFrame, 2U> cached;
    for (const auto method : {UpscaleKernel::Default, UpscaleKernel::ShiftLut}) {
        static_cast<void>(upscale.Start(test_upscale_request({source.frame(), method})));
        REQUIRE(events.Wait([&] { return !upscale.snapshot().busy; }));
        cached[static_cast<std::size_t>(method)] = upscale.snapshot().frame;
    }
    const auto copies = backend->same_copies.load();
    static_cast<void>(upscale.Start(test_upscale_request({source.frame(), UpscaleKernel::RealPlksr})));
    const bool running = entered.wait_for(2s) == std::future_status::ready;
    if (!running) gate->release.set_value();
    REQUIRE(running);
    const auto selected = upscale.Start(test_upscale_request({source.frame(), UpscaleKernel::Default}));
    CHECK(selected.busy);
    CHECK(selected.pending->kernel == UpscaleKernel::RealPlksr);
    CHECK(selected.frame == cached[0U]);
    CHECK(selected.frame.revision < cached[1U].revision);
    CHECK(upscale.ObserveWorkspace().product_revision == selected.frame.revision);
    CHECK(diagnostics.reused_revision.load() == selected.frame.revision);
    CHECK(diagnostics.reused_meaning.load() == test_document({}).document->facts().meaning_identity);
    CHECK(diagnostics.reused_observation.load() == selected.revision);
    const auto read = upscale.BorrowDocument(selected.frame);
    REQUIRE(read.valid());
    CHECK(*reinterpret_cast<const std::uint8_t*>(read.pixels.plane(0U).plane().data) == 1U);
    gate->release.set_value();
    REQUIRE(events.Wait([&] { return !upscale.snapshot().busy; }));
    CHECK(upscale.snapshot().frame == cached[0U]);
    CHECK(upscale.ObserveWorkspace().product_revision == cached[0U].revision);
    for (const auto& method : upscale.snapshot().methods) CHECK(method.available);
    CHECK(upscale.Start(test_upscale_request({source.frame(), UpscaleKernel::RealPlksr})).kernel == UpscaleKernel::RealPlksr);
    CHECK(runs->load() == 3U);
    CHECK(borrows.load() == 1U);
    CHECK(backend->same_copies.load() == copies);
}
TEST_CASE("Held output readers prevent obsolete foreground and warm work from preparing a candidate") {
    const bool preempt_warm = GENERATE(false, true);
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    struct Admissions {
        std::atomic_uint32_t started{0U}, completed{0U}, prepared{0U};
        std::promise<void> blocked, returned;
    } admissions;
    auto blocked = admissions.blocked.get_future();
    auto returned = admissions.returned.get_future();
    const VisualDiagnosticSink diagnostics{.context = &admissions, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
                                               auto& probe = *static_cast<Admissions*>(context);
                                               if (fact.operation == VisualDiagnosticOperation::UpscaleOutputAdmissionStarted && ++probe.started == 5U)
                                                   probe.blocked.set_value();
                                               if (fact.operation == VisualDiagnosticOperation::UpscaleOutputAdmissionCompleted && ++probe.completed == 5U)
                                                   probe.returned.set_value();
                                               if (fact.operation == VisualDiagnosticOperation::UpscaleOutputAllocation) ++probe.prepared;
                                           }};
    EventGate events;
    std::atomic<mmltk::frameworks::gpu::SystemImageRuntime*> output_runtime{nullptr};
    const auto factory = TestUpscaleAlgorithm::CreateRuntime(backend, kernel, runs);
    UpscaleSystem upscale{kDevice,
                          [&, factory](auto revisions) {
                              auto runtime = factory(std::move(revisions));
                              output_runtime.store(runtime.get(), std::memory_order_release);
                              return runtime;
                          },
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); }, [&events](UpscaleSystem::event_type) { events.Advance(); },
                          diagnostics};
    std::vector<mmltk::frameworks::gpu::BorrowedImageProductReadView> readers;
    for (std::uint8_t value = 1U; value <= 4U; ++value) {
        source.Publish({16U, 8U}, value);
        static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
        REQUIRE(events.Wait([&] { return !upscale.snapshot().busy && upscale.snapshot().input == source.frame(); }));
        readers.push_back(upscale.BorrowFrame());
        REQUIRE(readers.back().valid());
    }
    auto* const runtime = output_runtime.load(std::memory_order_acquire);
    REQUIRE(runtime != nullptr);
    // Check custody before starting competing work: an active reservation
    // must not mask an incorrectly writable reader-owned slot.
    REQUIRE(readers.size() == 4U);
    mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput baseline;
    auto reservation = std::async(std::launch::async, [&] { return runtime->TryAcquireOutput(baseline).valid(); });
    REQUIRE_FALSE(mmltk::testsupport::await_test_future(reservation, "four held Upscale readers"));
    if (preempt_warm)
        upscale.Warm({16U, 8U});
    else {
        source.Publish({16U, 8U}, 5U);
        static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
    }
    REQUIRE(blocked.wait_for(2s) == std::future_status::ready);
    blocked.get();
    const auto copied = backend->same_copies.load();
    const auto allocated = backend->planes_allocated.load();
    const auto prepared = admissions.prepared.load();
    source.Publish({16U, 8U}, 6U);
    const auto latest = test_upscale_request({.source = source.frame(), .kernel = UpscaleKernel::ShiftLut});
    static_cast<void>(upscale.Start(latest));
    readers[0U] = {};
    REQUIRE(returned.wait_for(2s) == std::future_status::ready);
    returned.get();
    REQUIRE(events.Wait([&] { return !upscale.snapshot().busy && upscale.snapshot().kernel == latest.kernel && upscale.snapshot().input == latest.source; }));
    CHECK(runs->load() == 5U);
    CHECK(admissions.prepared.load() == prepared + 1U);
    CHECK(backend->same_copies.load() == copied + 2U);
    CHECK(backend->planes_allocated.load() == allocated);
    readers.clear();
    upscale.Stop();
}
TEST_CASE("Upscale publishes only the newest selected kernel after obsolete device work settles") {
    auto backend = std::make_shared<FakeImageBackend>();
    // CLEANUP-IGNORE: This source feeds a supersession/commit gate; the cached-selection scenario retains three
    // completed methods and validates a different ownership path.
    MutableVisualSource source{backend, {16U, 8U}};
    // CLEANUP-IGNORE: Supersession and cached selection retain separate commit probes and publication-count oracles.
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    auto gate = std::make_shared<MutationCommitProbe>();
    auto entered = gate->committed.get_future();
    std::atomic_uint32_t ready_publications{0U};
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [kernel, gate, runs] { return std::make_unique<TestUpscaleAlgorithm>(kernel, gate, runs); }, 4U),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&](UpscaleSystem::event_type event) {
                              if (const auto* changed = std::get_if<UpscaleChanged>(&event); changed && changed->snapshot.ready)
                                  ready_publications.fetch_add(1U, std::memory_order_acq_rel);
                              events.Advance();
                          }};
    auto settle_upscale = settle_upscale_on_exit(upscale, gate->release);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame(), .kernel = UpscaleKernel::ShiftLut})));
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame(), .kernel = UpscaleKernel::RealPlksr})));
    CHECK_FALSE(upscale.snapshot().ready);
    gate->release.set_value();
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && ready_publications.load(std::memory_order_acquire) == 1U; }));
    CHECK(upscale.snapshot().kernel == UpscaleKernel::RealPlksr);
    CHECK(upscale.snapshot().input == source.frame());
    CHECK(runs->load(std::memory_order_acquire) == 2U);
    CHECK(ready_publications.load(std::memory_order_acquire) == 1U);
}
TEST_CASE("Upscale Stop reaches active latest work after its receiver copy settles") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    struct CopyGate final {
        std::atomic_bool first{true};
        std::promise<void> copied;
        std::promise<void> release;
        std::shared_future<void> released = release.get_future().share();
    } gate;
    const VisualDiagnosticSink diagnostics{.context = &gate, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
                                               auto& current = *static_cast<CopyGate*>(context);
                                               if (fact.operation == VisualDiagnosticOperation::CopyCompleted && current.first.exchange(false)) {
                                                   current.copied.set_value();
                                                   current.released.wait();
                                               }
                                           }};
    EventGate events;
    UpscaleSystem upscale{kDevice, TestUpscaleAlgorithm::CreateRuntime(backend, kernel, runs),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); }, [&events](UpscaleSystem::event_type) { events.Advance(); },
                          diagnostics};
    auto settle_upscale = settle_upscale_on_exit(upscale, gate.release);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(gate.copied, "gate.copied", 2s));
    upscale.Stop();
    CHECK_FALSE(upscale.snapshot().busy);
    CHECK_FALSE(upscale.snapshot().ready);
    gate.release.set_value();
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    CHECK(runs->load(std::memory_order_acquire) == 1U);
    CHECK(upscale.snapshot().input == source.frame());
}
// CLEANUP-IGNORE: This cancellation-at-borrow test has a two-window source-custody gate, unlike held-output
// admission pressure even though both begin with an Upscale fixture.
TEST_CASE("Superseded Upscale demand releases source custody without copying at both borrow windows") {
    const bool after_borrow = GENERATE(false, true);
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    struct Gate final {
        bool after = false;
        std::atomic_bool first{true};
        std::atomic_uint32_t borrows{0U};
        std::promise<void> entered;
        std::promise<void> release;
        std::shared_future<void> released = release.get_future().share();
        void Await() {
            if (!first.exchange(false)) return;
            entered.set_value();
            released.wait();
        }
    } gate;
    gate.after = after_borrow;
    const VisualDiagnosticSink diagnostics{.context = &gate, .write = [](void* context, const VisualDiagnosticFact fact) noexcept {
                                               auto& observed_gate = *static_cast<Gate*>(context);
                                               if (!observed_gate.after && fact.operation == VisualDiagnosticOperation::UpscaleWorkerStarted)
                                                   observed_gate.Await();
                                           }};
    EventGate events;
    UpscaleSystem upscale{kDevice, TestUpscaleAlgorithm::CreateRuntime(backend, kernel, runs),
                          [&source, &gate](const VisualFrame& frame) {
                              ++gate.borrows;
                              auto borrowed = source.BorrowExact(frame);
                              if (gate.after) gate.Await();
                              return borrowed;
                          },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }, diagnostics};
    auto settle_upscale = settle_upscale_on_exit(upscale, gate.release);
    static_cast<void>(upscale.Start(test_upscale_request({source.frame(), UpscaleKernel::Default})));
    const auto entered = gate.entered.get_future().wait_for(2s);
    if (entered != std::future_status::ready) gate.release.set_value();
    REQUIRE(entered == std::future_status::ready);
    static_cast<void>(upscale.Start(test_upscale_request({source.frame(), UpscaleKernel::ShiftLut})));
    gate.release.set_value();
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    CHECK(upscale.snapshot().kernel == UpscaleKernel::ShiftLut);
    CHECK(runs->load() == 1U);
    CHECK(gate.borrows.load() == (after_borrow ? 2U : 1U));
    CHECK(backend->same_copies.load() == 2U);
}
TEST_CASE("Upscale completed and failed facts require exact immutable document meaning") {
    auto backend = std::make_shared<FakeImageBackend>();
    // CLEANUP-IGNORE: This source is mutated to an inconsistent document meaning; the semantic-revision scenario
    // preserves valid document provenance and tests receiver-copy reuse.
    MutableVisualSource source{backend, {16U, 8U}};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    EventGate events;
    UpscaleSystem upscale{kDevice, TestUpscaleAlgorithm::CreateRuntime(backend, kernel, runs),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }};
    const auto request = test_upscale_request({source.frame()});
    static_cast<void>(upscale.Start(request));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    auto mismatched = request;
    mismatched.document.meaning_identity += 1U;
    static_cast<void>(upscale.Start(mismatched));
    REQUIRE(events.Wait([&] { return !upscale.snapshot().busy; }));
    CHECK_FALSE(upscale.snapshot().methods[0U].available);
    CHECK(upscale.snapshot().methods[0U].failure == mismatched);
    CHECK(upscale.snapshot().methods[0U].completed == request);
    static_cast<void>(upscale.Start(request));
    CHECK(upscale.snapshot().methods[0U].available);
    CHECK_FALSE(upscale.snapshot().methods[0U].failed);
    CHECK(runs->load() == 1U);
}
// CLEANUP-IGNORE: Warm idempotence and injected release-failure custody use distinct algorithms and lifecycle
// assertions.
TEST_CASE("Upscale warm activates every mode once and repeated ready signals are idempotent") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto activation = std::make_shared<UpscaleActivationProbe>();
    auto completed = activation->first_warm_completed.get_future();
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [activation] { return std::make_unique<ActivationUpscaleAlgorithm>(activation); }, 4U),
                          [](const VisualFrame&) { return VisualDocumentRead{}; }, [&events](UpscaleSystem::event_type) { events.Advance(); }};
    upscale.Warm({32U, 32U});
    upscale.Warm({32U, 32U});
    REQUIRE(completed.wait_for(2s) == std::future_status::ready);
    completed.get();
    upscale.Warm({32U, 32U});
    REQUIRE(events.Wait([&] { return std::ranges::all_of(upscale.snapshot().methods, [](const auto& method) { return method.warm; }); }));
    CHECK(activation->warms.load(std::memory_order_acquire) == 1U);
    CHECK(activation->runs.load(std::memory_order_acquire) == 9U);
    for (const auto& ready : activation->ready) CHECK(ready.load(std::memory_order_acquire));
}
TEST_CASE("Upscale CUDA tile replay preserves reference pixels and pitched receiver storage", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using namespace mmltk::frameworks::gpu;
    struct ReferenceEnvironment final {
        std::optional<std::string> prior;
        ReferenceEnvironment() {
            if (const auto* value = std::getenv("MMLTK_UPSCALE_ONNX_REFERENCE")) prior = value;
        }
        ~ReferenceEnvironment() {
            if (prior)
                static_cast<void>(::setenv("MMLTK_UPSCALE_ONNX_REFERENCE", prior->c_str(), 1));
            else
                static_cast<void>(::unsetenv("MMLTK_UPSCALE_ONNX_REFERENCE"));
        }
    } environment;
    constexpr std::uint32_t width = 197U, height = 193U;
    SystemImageRuntime source{{.device = 0, .context_mode = DeviceContextMode::PrimaryInterop}};
    SystemImageRuntime readback{{.device = 0, .context_mode = DeviceContextMode::PrimaryInterop}};
    source.BeginWork();
    auto staging = PinnedHostBuffer::ForCurrentDevice();
    staging->ensure_bytes(width * height * 4U);
    auto* pixels = static_cast<std::uint8_t*>(staging->data());
    std::array<std::vector<std::uint8_t>, 6U> reference;
    for (const bool reference_run : {true, false}) {
        REQUIRE(::setenv("MMLTK_UPSCALE_ONNX_REFERENCE", reference_run ? "1" : "0", 1) == 0);
        using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
        std::size_t tensor_allocations = 0U;
        std::size_t model_constructions = 0U;
        auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage stage) {
            if (stage == Stage::BuffersAllocated) ++tensor_allocations;
            if (stage == Stage::ContextCreated) ++model_constructions;
        })(std::make_shared<mmltk::frameworks::gpu::ImageProductRevisionSequence>());
        runtime->BeginWork();
        auto* model = dynamic_cast<UpscaleAlgorithm*>(runtime->model());
        REQUIRE(model != nullptr);
        CUdeviceptr previous_input = 0U;
        std::array<CUdeviceptr, 2U> output_slots{};
        std::size_t warm_tensors = 0U, warm_models = 0U;
        for (std::size_t iteration = 0U; iteration < (reference_run ? 6U : 12U); ++iteration) {
            const auto pattern = iteration % 2U;
            const auto method = static_cast<UpscaleKernel>((iteration / 2U) % 3U);
            CAPTURE(reference_run, iteration, method);
            for (std::uint32_t y = 0U; y < height; ++y) {
                for (std::uint32_t x = 0U; x < width; ++x) {
                    const auto pixel = (static_cast<std::size_t>(y) * width + x) * 4U;
                    pixels[pixel] = static_cast<std::uint8_t>((x * 7U + y * 3U + pattern * 73U) % 256U);
                    pixels[pixel + 1U] = static_cast<std::uint8_t>((x ^ y) + pattern * 41U);
                    pixels[pixel + 2U] = static_cast<std::uint8_t>((x * y + pattern * 109U) % 256U);
                    pixels[pixel + 3U] = 255U;
                }
            }
            source.Publish(width, height, [&](const auto clean, const auto, const auto stream) {
                CUDA_MEMCPY2D upload{};
                upload.srcMemoryType = CU_MEMORYTYPE_HOST;
                upload.srcHost = pixels;
                upload.srcPitch = width * 4U;
                upload.dstMemoryType = CU_MEMORYTYPE_DEVICE;
                upload.dstDevice = clean.data;
                upload.dstPitch = clean.descriptor.pitch_bytes;
                upload.WidthInBytes = width * 4U;
                upload.Height = height;
                REQUIRE(cuMemcpy2DAsync(&upload, reinterpret_cast<CUstream>(stream)) == CUDA_SUCCESS);
            });
            const auto copied = runtime->CopyInputFrom(source.Borrow(), [model](const auto plane, const auto stream) { model->Semantics({}, plane, stream); });
            CHECK(copied[0U] == ImageCopyPath::SameDevice);
            const auto input = runtime->BorrowInput();
            REQUIRE(input.valid());
            const auto input_plane = input.plane(0U).plane();
            if (previous_input != 0U) CHECK(input_plane.data == previous_input);
            previous_input = input_plane.data;
            CHECK(input_plane.descriptor.pitch_bytes >= width * 4U);
            CHECK(input_plane.descriptor.pitch_bytes % 16U == 0U);
            runtime->Publish(width * 4U, height * 4U, [model, &input, method](const auto clean, const auto semantic, const auto stream) {
                model->Run(method, input.plane(0U).plane(), clean, stream);
                model->Semantics({}, semantic, stream);
            });
            // The receiver-copy boundary waits for producer completion before
            // this test-only readback. Product execution never reads through CPU memory.
            CHECK(readback.CopyFrom(runtime->Borrow())[0U] == ImageCopyPath::SameDevice);
            {
                const auto output = runtime->Borrow();
                const auto plane = output.plane(0U).plane();
                const auto slot = iteration % output_slots.size();
                if (output_slots[slot] != 0U) CHECK(plane.data == output_slots[slot]);
                output_slots[slot] = plane.data;
                if (output_slots[1U] != 0U) CHECK(output_slots[0U] != output_slots[1U]);
                CHECK(plane.descriptor.pitch_bytes % 16U == 0U);
            }
            const auto returned = readback.Borrow();
            const auto plane = returned.plane(0U).plane();
            std::vector<std::uint8_t> actual(static_cast<std::size_t>(width) * height * 64U);
            CUDA_MEMCPY2D download{};
            download.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            download.srcDevice = plane.data;
            download.srcPitch = plane.descriptor.pitch_bytes;
            download.dstMemoryType = CU_MEMORYTYPE_HOST;
            download.dstHost = actual.data();
            download.dstPitch = width * 16U;
            download.WidthInBytes = width * 16U;
            download.Height = height * 4U;
            REQUIRE(cuMemcpy2D(&download) == CUDA_SUCCESS);
            if (iteration == 5U) {
                warm_tensors = tensor_allocations;
                warm_models = model_constructions;
            } else if (iteration > 5U) {
                CHECK(tensor_allocations == warm_tensors);
                CHECK(model_constructions == warm_models);
            }
            if (reference_run)
                reference[iteration] = std::move(actual);
            else
                CHECK(actual == reference[iteration % reference.size()]);
        }
        REQUIRE(runtime->Retire().safe_to_destroy);
    }
}
TEST_CASE("Native Upscale warm aggregate retires before model and primary context destruction") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    auto runtime = make_native_upscale_runtime_factory(kDevice)(std::make_shared<mmltk::frameworks::gpu::ImageProductRevisionSequence>());
    runtime->BeginWork();
    auto* const model = dynamic_cast<UpscaleAlgorithm*>(runtime->model());
    REQUIRE(model != nullptr);
    model->Warm();
    const auto retirement = runtime->Retire();
    CHECK(retirement.safe_to_destroy);
    CHECK_FALSE(retirement.failure);
    CHECK_FALSE(retirement.custody.valid());
    CHECK_NOTHROW(runtime.reset());
}
void run_native_upscale(mmltk::frameworks::gpu::SystemImageRuntime& runtime, UpscaleKernel kernel, const std::function<bool()>& current = {}) {
    runtime.BeginWork();
    auto* model = dynamic_cast<UpscaleAlgorithm*>(runtime.model());
    REQUIRE(model != nullptr);
    if (!runtime.BorrowInput().valid()) {
        runtime.PublishInput(8U, 8U, [model](auto clean, auto semantic, auto stream) {
            model->Semantics({}, clean, stream);
            model->Semantics({}, semantic, stream);
        });
    }
    const auto input = runtime.BorrowInput();
    runtime.Publish(32U, 32U, [&](auto clean, auto semantic, auto stream) {
        model->Run(kernel, input.plane(0U).plane(), clean, stream, current);
        model->Semantics({}, semantic, stream);
    });
}
class NativeUpscaleSource final {
   public:
    NativeUpscaleSource()
        : runtime_({.device = 0,
                    .context_mode = mmltk::frameworks::gpu::DeviceContextMode::PrimaryInterop,
                    .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic}) {
        runtime_.BeginWork();
        runtime_.Publish(8U, 8U, [](auto clean, auto semantic, auto stream) {
            for (auto plane : {clean, semantic})
                REQUIRE(cuMemsetD2D8Async(plane.data, plane.descriptor.pitch_bytes, 0, 32U, 8U, reinterpret_cast<CUstream>(stream)) == CUDA_SUCCESS);
        });
        frame = visual_frame({PresentationSourceKind::Explore, 1U}, {8U, 8U}, runtime_.OutputFacts().revision);
        frame.clean_revision = frame.revision;
    }
    VisualDocumentRead Borrow(const VisualFrame& requested) const { return test_document(borrow_matching_visual_product(requested, runtime_.Borrow())); }
    VisualFrame frame{};

   private:
    mmltk::frameworks::gpu::SystemImageRuntime runtime_;
};
class NativeUpscaleFailureScenario final {
   public:
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using Injection = std::function<void(Stage, std::atomic_bool&)>;
    explicit NativeUpscaleFailureScenario(Injection injection)
        : upscale{kDevice,
                  make_native_upscale_runtime_factory(kDevice, [this, injection = std::move(injection)](const Stage reached) { injection(reached, armed); }),
                  [this](const VisualFrame& frame) { return source.Borrow(frame); },
                  [this](UpscaleSystem::event_type event) {
                      if (auto* failure = std::get_if<UpscaleFailed>(&event)) failed.set_value(*failure);
                      events.Advance();
                  }} {}
    NativeUpscaleSource source;
    std::atomic_bool armed{true};
    std::promise<UpscaleFailed> failed;
    EventGate events;
    UpscaleSystem upscale;
};
TEST_CASE("Native Upscale cancellation settles admitted work without recording initialization failure", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    const auto stage =
        GENERATE(Stage::InitializationAdmitted, Stage::ChecksumAdmitted, Stage::BasicAllocationAdmitted, Stage::BasicLaunchAdmitted, Stage::PreprocessAdmitted,
                 Stage::TargetAdmitted, Stage::TilePrepared, Stage::ContextCreated, Stage::WarmInputSubmitted, Stage::WarmSettled, Stage::CaptureBegan,
                 Stage::CaptureEnded, Stage::GraphInstantiated, Stage::ReplaySettled, Stage::CacheLockAdmitted);
    const bool stop = GENERATE(false, true);
    const bool basic = stage == Stage::BasicAllocationAdmitted || stage == Stage::BasicLaunchAdmitted;
    const bool neural_warm = (stage >= Stage::WarmInputSubmitted && stage <= Stage::ReplaySettled) || stage == Stage::CacheLockAdmitted;
    const auto method = basic ? UpscaleKernel::Default : neural_warm ? UpscaleKernel::RealPlksr : UpscaleKernel::ShiftLut;
    CAPTURE(static_cast<std::uint32_t>(stage), stop, static_cast<std::uint32_t>(method));
    NativeUpscaleSource source;
    std::promise<void> admitted, release;
    auto released = release.get_future().share();
    std::atomic_bool armed{true};
    std::array<std::atomic_uint32_t, static_cast<std::size_t>(Stage::Count)> stages{};
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          make_native_upscale_runtime_factory(kDevice,
                                                              [&](Stage reached) {
                                                                  ++stages[static_cast<std::size_t>(reached)];
                                                                  if (reached == stage && armed.exchange(false)) {
                                                                      admitted.set_value();
                                                                      released.wait();
                                                                  }
                                                              }),
                          [&source](const VisualFrame& frame) { return source.Borrow(frame); }, [&events](UpscaleSystem::event_type) { events.Advance(); }};
    auto settle_native = settle_upscale_on_exit(upscale, release);
    static_cast<void>(upscale.Start(test_upscale_request({source.frame, method})));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(admitted, "native Upscale admitted boundary", 120s));
    if (stop)
        static_cast<void>(upscale.Stop());
    else
        static_cast<void>(
            upscale.Start(test_upscale_request({source.frame, method == UpscaleKernel::Default ? UpscaleKernel::ShiftLut : UpscaleKernel::Default})));
    release.set_value();
    if (stop) {
        // A subsequent request is queued behind settlement, proving Stop did
        // not leave an activated owner or an incomplete capture behind.
        static_cast<void>(upscale.Start(test_upscale_request({source.frame, UpscaleKernel::Default})));
    }
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && !upscale.snapshot().busy; }, 120s));
    CHECK_FALSE(upscale.snapshot().methods[static_cast<std::size_t>(method)].initialization_failed);
    if (stage == Stage::ReplaySettled) CHECK(stages[static_cast<std::size_t>(Stage::ContextCreated)].load() == 1U);
}
TEST_CASE("Native CUDA failure scope reaches Upscale quarantine or isolated retry", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const auto status =
        GENERATE(cudaErrorDeviceUninitialized, cudaErrorContained, cudaErrorTensorMemoryLeak, cudaErrorSystemNotReady, cudaErrorMpsClientTerminated,
                 cudaErrorExternalDevice, cudaErrorMemoryAllocation, cudaErrorInvalidValue, cudaErrorLaunchOutOfResources);
    NativeUpscaleFailureScenario scenario{[status](const Stage reached, std::atomic_bool& armed) {
        if (reached == Stage::ContextCreated && armed.exchange(false)) throw CudaError(status, "injected native CUDA scope");
    }};
    static_cast<void>(scenario.upscale.Start(test_upscale_request({scenario.source.frame})));
    REQUIRE(scenario.events.Wait([&] { return scenario.upscale.snapshot().ready; }));
    static_cast<void>(scenario.upscale.Start(test_upscale_request({scenario.source.frame, UpscaleKernel::ShiftLut})));
    auto failure = scenario.failed.get_future();
    REQUIRE(failure.wait_for(120s) == std::future_status::ready);
    const auto result = failure.get();
    CHECK((result.kind == UpscaleFailureKind::Physical) == cuda_shared_failure(status));
    if (cuda_shared_failure(status)) {
        CHECK_FALSE(result.snapshot.ready);
        CHECK_FALSE(scenario.upscale.BorrowFrame().valid());
        for (const auto& method : result.snapshot.methods) CHECK_FALSE(method.available);
    } else {
        CHECK(result.snapshot.ready);
        CHECK(result.snapshot.methods[0U].available);
        static_cast<void>(scenario.upscale.Start(test_upscale_request({scenario.source.frame, UpscaleKernel::ShiftLut})));
        REQUIRE(scenario.events.Wait([&] { return scenario.upscale.snapshot().kernel == UpscaleKernel::ShiftLut && !scenario.upscale.snapshot().busy; }, 120s));
        CHECK_FALSE(scenario.upscale.snapshot().methods[1U].initialization_failed);
    }
}
TEST_CASE("Completed native activation survives same-method withdrawal without repeating initialization", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const auto boundary =
        GENERATE(Stage::ReplaySettled, Stage::PreprocessAdmitted, Stage::TargetAdmitted, Stage::TilePrepared, Stage::RuntimeEnqueued, Stage::BindingsReady);
    const auto method = boundary == Stage::BindingsReady ? UpscaleKernel::ShiftLut : UpscaleKernel::RealPlksr;
    std::array<unsigned, static_cast<std::size_t>(Stage::Count)> counts{};
    bool current = true;
    bool armed = true;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage reached) {
        const auto count = ++counts[static_cast<std::size_t>(reached)];
        // Both TRT lanes must finish; withdrawing after lane one remains a
        // partial activation and intentionally does not establish residency.
        if (armed && reached == boundary && (boundary != Stage::ReplaySettled || count == 2U)) {
            armed = false;
            current = false;
        }
    })(std::make_shared<ImageProductRevisionSequence>());
    CHECK_NOTHROW(run_native_upscale(*runtime, method, [&] { return current; }));
    REQUIRE_FALSE(armed);
    REQUIRE_FALSE(current);
    const auto completed = counts;
    current = true;
    CHECK_NOTHROW(run_native_upscale(*runtime, method, [&] { return current; }));
    const auto check_unchanged = [&](const std::initializer_list<Stage> stages) {
        for (const auto stage : stages) {
            CAPTURE(boundary, stage);
            CHECK(counts[static_cast<std::size_t>(stage)] == completed[static_cast<std::size_t>(stage)]);
        }
    };
    check_unchanged({Stage::InitializationAdmitted, Stage::ChecksumAdmitted, Stage::CacheLockAdmitted, Stage::BuildAdmitted, Stage::ContextCreated,
                     Stage::BuffersAllocated, Stage::StreamCreated, Stage::EventCreated, Stage::BindingsReady});
    if (method == UpscaleKernel::RealPlksr) {
        check_unchanged({Stage::WarmInputSubmitted, Stage::WarmSubmitted, Stage::WarmSettled, Stage::CaptureBegan, Stage::CaptureSubmitted, Stage::CaptureEnded,
                         Stage::GraphInstantiated, Stage::ReplaySubmitted, Stage::ReplaySettled});
        CHECK(counts[static_cast<std::size_t>(Stage::ContextCreated)] == 2U);
        CHECK(counts[static_cast<std::size_t>(Stage::ReplaySettled)] == 2U);
    }
    CHECK(runtime->Retire().safe_to_destroy);
}
TEST_CASE("Native method failures are classified at the actual activation completion boundary", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const auto boundary = GENERATE(Stage::ContextCreated, Stage::PreprocessAdmitted, Stage::TargetAdmitted, Stage::WarmSubmitted, Stage::RuntimeEnqueued);
    NativeUpscaleFailureScenario scenario{[boundary](const Stage reached, std::atomic_bool& armed) {
        if (reached == boundary && armed.exchange(false)) throw CudaError(cudaErrorMemoryAllocation, "settled activation-boundary allocation failure");
    }};
    const auto request = test_upscale_request({scenario.source.frame, UpscaleKernel::ShiftLut});
    static_cast<void>(scenario.upscale.Start(request));
    auto failure = scenario.failed.get_future();
    REQUIRE(failure.wait_for(120s) == std::future_status::ready);
    const auto result = failure.get();
    CHECK(result.kind == UpscaleFailureKind::Failed);
    CHECK(result.request == request);
    CHECK(result.snapshot.methods[1U].failed);
    CHECK(result.snapshot.methods[1U].initialization_failed == (boundary == Stage::ContextCreated));
    static_cast<void>(scenario.upscale.Start(request));
    REQUIRE(scenario.events.Wait([&] { return scenario.upscale.snapshot().ready && !scenario.upscale.snapshot().busy; }, 120s));
    CHECK_FALSE(scenario.upscale.snapshot().methods[1U].initialization_failed);
}
TEST_CASE("Native withdrawal does not erase a concurrently reported method failure", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const bool physical = GENERATE(false, true);
    bool current = true;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage stage) {
        if (stage == Stage::WarmSubmitted && std::exchange(current, false))
            throw CudaError(physical ? cudaErrorContained : cudaErrorMemoryAllocation, "method failure concurrent with withdrawal");
    })(std::make_shared<ImageProductRevisionSequence>());
    std::exception_ptr failure;
    try {
        run_native_upscale(*runtime, UpscaleKernel::ShiftLut, [&] { return current; });
    } catch (...) { failure = std::current_exception(); }
    REQUIRE(failure);
    CHECK_FALSE(current);
    CHECK(is_image_execution_failure(failure) == physical);
    CHECK(static_cast<bool>(find_image_failure<CudaError>(failure)));
    CHECK(runtime->Retire().safe_to_destroy);
}
TEST_CASE("Native cache lock admission remains cancellable while another process owns the cache", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    {
        auto seed = make_native_upscale_runtime_factory(kDevice)(std::make_shared<ImageProductRevisionSequence>());
        run_native_upscale(*seed, UpscaleKernel::RealPlksr);
        REQUIRE(seed->Retire().safe_to_destroy);
    }
    const auto cache = mmltk::common::system::runtime_paths::repository_root() / ".cache" / "mmltk" / "upscalers";
    std::vector<mmltk::common::io::ScopedFd> locks;
    for (const auto& entry : std::filesystem::directory_iterator(cache)) {
        if (entry.path().extension() != ".lock") continue;
        mmltk::common::io::ScopedFd descriptor{::open(entry.path().c_str(), O_RDWR | O_CLOEXEC)};
        REQUIRE(descriptor.get() >= 0);
        REQUIRE(::flock(descriptor.get(), LOCK_EX | LOCK_NB) == 0);
        locks.push_back(std::move(descriptor));
    }
    REQUIRE_FALSE(locks.empty());
    const bool stop = GENERATE(false, true);
    std::atomic_bool observed{false};
    std::promise<void> waiting;
    NativeUpscaleSource source;
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          make_native_upscale_runtime_factory(kDevice,
                                                              [&](Stage stage) {
                                                                  if (stage == Stage::CacheLockWaiting && !observed.exchange(true)) waiting.set_value();
                                                              }),
                          [&source](const VisualFrame& frame) { return source.Borrow(frame); }, [&events](UpscaleSystem::event_type) { events.Advance(); }};
    static_cast<void>(upscale.Start(test_upscale_request({source.frame, UpscaleKernel::RealPlksr})));
    const auto reached = waiting.get_future().wait_for(120s);
    if (reached != std::future_status::ready) static_cast<void>(upscale.Stop());
    REQUIRE(reached == std::future_status::ready);
    if (stop) static_cast<void>(upscale.Stop());
    static_cast<void>(upscale.Start(test_upscale_request({source.frame, UpscaleKernel::Default})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && !upscale.snapshot().busy; }));
    CHECK_FALSE(upscale.snapshot().methods[2U].initialization_failed);
    // Cache locks are still owned: completion therefore proves withdrawal,
    // rather than eventual admission after a lock happened to become free.
}
TEST_CASE("TensorRT build progress accepts cancellation without an initialization exception", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using namespace mmltk::backend::ml::runtime;
    const bool during_build = GENERATE(false, true);
    std::atomic_bool building{false};
    std::atomic_uint32_t observations{0U};
    TensorRtEngineOptions options{};
    options.device = 0;
    options.optimization_profiles = {{.input_name = "input", .minimum = {1, 3, 256, 256}, .optimum = {1, 3, 256, 256}, .maximum = {1, 3, 256, 256}}};
    options.log = [&](std::string_view message) {
        if (message == "[trt:build] building serialized engine") building.store(true);
    };
    options.continue_build = [&] {
        if (!during_build) return false;
        if (!building.load()) return true;
        // The first check is the pre-build admission; the second comes from
        // the vendor progress monitor while buildSerializedNetwork is active.
        return ++observations < 2U;
    };
    std::optional<TensorRtEngine> engine;
    try {
        engine.emplace(mmltk::common::system::runtime_paths::repository_root() / "src/backend/imaging/upscale/assets/RealPLKSR_fp16.onnx", std::move(options));
    } catch (const TensorRtOperationError& failure) { FAIL("TensorRT cancellation reported operation error code " << failure.code()); }
    REQUIRE(engine);
    CHECK(engine->cancelled());
    CHECK(engine->native_engine_handle() == 0U);
    if (during_build) CHECK(observations.load() >= 2U);
}
TEST_CASE("Installed neural activation owns every submitted stage through settled failure and retry", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const auto stage = GENERATE(Stage::StreamCreated, Stage::EventCreated, Stage::ContextCreated, Stage::BuffersAllocated, Stage::WarmInputSubmitted,
                                Stage::WarmSubmitted, Stage::WarmSettled, Stage::CaptureBegan, Stage::CaptureSubmitted, Stage::CaptureEnded,
                                Stage::GraphInstantiated, Stage::ReplaySubmitted, Stage::ReplaySettled);
    const auto method = GENERATE(UpscaleKernel::ShiftLut, UpscaleKernel::RealPlksr);
    const auto occurrence = GENERATE(1U, 2U, 3U, 4U);
    CAPTURE(static_cast<std::uint32_t>(stage), static_cast<std::uint32_t>(method), occurrence);
    if (method == UpscaleKernel::ShiftLut && (stage == Stage::WarmInputSubmitted || stage > Stage::WarmSubmitted)) return;
    const auto maximum = method == UpscaleKernel::ShiftLut ? ((stage == Stage::EventCreated || stage == Stage::WarmSubmitted)       ? 3U
                                                              : (stage == Stage::BuffersAllocated || stage == Stage::StreamCreated) ? 2U
                                                                                                                                    : 1U)
                                                           : ((stage == Stage::BuffersAllocated || stage == Stage::EventCreated) ? 4U
                                                              : stage == Stage::StreamCreated                                    ? 3U
                                                                                                                                 : 2U);
    if (occurrence > maximum) return;
    bool armed = true;
    unsigned reached_count = 0U;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage reached) {
        if (reached == stage && ++reached_count == occurrence && std::exchange(armed, false))
            throw CudaError(cudaErrorMemoryAllocation, "injected settled neural activation allocation failure");
    })(std::make_shared<ImageProductRevisionSequence>());
    std::exception_ptr failure;
    try {
        for (unsigned replay = 0U; replay < 3U && armed; ++replay) run_native_upscale(*runtime, method);
    } catch (...) { failure = std::current_exception(); }
    REQUIRE_FALSE(armed);
    REQUIRE(failure);
    CHECK_FALSE(is_image_execution_failure(failure));
    CHECK(static_cast<bool>(find_image_failure<CudaError>(failure)));
    CHECK_NOTHROW(run_native_upscale(*runtime, method == UpscaleKernel::ShiftLut ? UpscaleKernel::RealPlksr : UpscaleKernel::ShiftLut));
    CHECK_NOTHROW(run_native_upscale(*runtime, method));
    CHECK(runtime->Retire().safe_to_destroy);
}
TEST_CASE("Neural shared activation and release failures retain typed physical custody", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const bool release_failure = GENERATE(false, true);
    bool armed = true;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage reached) {
        const auto selected = release_failure ? Stage::ContextReleased : Stage::WarmSubmitted;
        if (reached == selected && std::exchange(armed, false)) throw CudaError(cudaErrorIllegalAddress, "injected shared neural physical failure");
    })(std::make_shared<ImageProductRevisionSequence>());
    if (release_failure) {
        run_native_upscale(*runtime, UpscaleKernel::ShiftLut);
        auto retirement = runtime->Retire();
        CHECK_FALSE(armed);
        CHECK_FALSE(retirement.safe_to_destroy);
        CHECK(retirement.custody.valid());
        CHECK(is_image_execution_failure(retirement.failure));
        CHECK(static_cast<bool>(find_image_failure<CudaError>(retirement.failure)));
    } else {
        std::exception_ptr failure;
        try {
            run_native_upscale(*runtime, UpscaleKernel::ShiftLut);
        } catch (...) { failure = std::current_exception(); }
        CHECK_FALSE(armed);
        CHECK(is_image_execution_failure(failure));
        CHECK(static_cast<bool>(find_image_failure<CudaError>(failure)));
        CHECK(runtime->Retire().safe_to_destroy);
    }
}
TEST_CASE("Neural cleanup attempts all settled releases after each destruction boundary fails", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const auto stage = GENERATE(Stage::GraphExecutableDestroyed, Stage::GraphDestroyed, Stage::EventDestroyed, Stage::StreamDestroyed, Stage::BufferReleased,
                                Stage::ContextReleased);
    std::array<unsigned, static_cast<std::size_t>(Stage::Count)> released{};
    bool injected = false;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage reached) {
        ++released[static_cast<std::size_t>(reached)];
        if (reached == stage && !std::exchange(injected, true)) throw CudaError(cudaErrorUnknown, "injected neural release failure");
    })(std::make_shared<ImageProductRevisionSequence>());
    run_native_upscale(*runtime, UpscaleKernel::RealPlksr);
    auto retirement = runtime->Retire();
    CHECK(injected);
    CHECK_FALSE(retirement.safe_to_destroy);
    CHECK(retirement.custody.valid());
    CHECK(is_image_execution_failure(retirement.failure));
    CHECK(released[static_cast<std::size_t>(Stage::GraphExecutableDestroyed)] == 2U);
    CHECK(released[static_cast<std::size_t>(Stage::GraphDestroyed)] == 2U);
    CHECK(released[static_cast<std::size_t>(Stage::EventDestroyed)] == 4U);
    CHECK(released[static_cast<std::size_t>(Stage::StreamDestroyed)] == 3U);
    CHECK(released[static_cast<std::size_t>(Stage::BufferReleased)] == 4U);
    CHECK(released[static_cast<std::size_t>(Stage::ContextReleased)] == 2U);
}
TEST_CASE("Valid TensorRT cache survives activation allocation and shared-context failures", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using namespace mmltk::frameworks::gpu;
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    const bool physical = GENERATE(false, true);
    {
        auto seed = make_native_upscale_runtime_factory(kDevice)(std::make_shared<ImageProductRevisionSequence>());
        run_native_upscale(*seed, UpscaleKernel::RealPlksr);
        REQUIRE(seed->Retire().safe_to_destroy);
    }
    std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> cache;
    const auto directory = mmltk::common::system::runtime_paths::repository_root() / ".cache" / "mmltk" / "upscalers";
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == ".engine") cache.emplace_back(entry.path(), entry.last_write_time());
    REQUIRE_FALSE(cache.empty());
    bool injected = false;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage stage) {
        if (stage == Stage::ContextCreated && !std::exchange(injected, true))
            throw CudaError(physical ? cudaErrorIllegalAddress : cudaErrorMemoryAllocation, "valid cache activation failure");
    })(std::make_shared<ImageProductRevisionSequence>());
    std::exception_ptr failure;
    try {
        run_native_upscale(*runtime, UpscaleKernel::RealPlksr);
    } catch (...) { failure = std::current_exception(); }
    REQUIRE(injected);
    REQUIRE(failure);
    CHECK(is_image_execution_failure(failure) == physical);
    CHECK(static_cast<bool>(find_image_failure<CudaError>(failure)));
    for (const auto& [path, modified] : cache) {
        REQUIRE(std::filesystem::is_regular_file(path));
        CHECK(std::filesystem::last_write_time(path) == modified);
    }
    CHECK(runtime->Retire().safe_to_destroy);
}
TEST_CASE("Warmed Upscale retains physical custody when model release fails") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto release = std::make_shared<UpscaleReleaseFailureProbe>();
    auto warmed = release->warmed.get_future();
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [release] { return std::make_unique<ReleaseFailingUpscaleAlgorithm>(release); }, 4U),
                          [](const VisualFrame&) { return VisualDocumentRead{}; }};
    upscale.Warm({32U, 32U});
    REQUIRE(warmed.wait_for(2s) == std::future_status::ready);
    warmed.get();
    upscale.Shutdown();
    CHECK(upscale.stopped());
    CHECK(release->releases.load(std::memory_order_acquire) == 1U);
    CHECK_FALSE(release->destroyed.load(std::memory_order_acquire));
    CHECK(backend->contexts_destroyed.load(std::memory_order_acquire) == 0U);
    CHECK(backend->streams_destroyed.load(std::memory_order_acquire) == 0U);
}
TEST_CASE("Upscale requests behind warmup retain identities without queued source leases") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {20U, 12U}, 7U};
    auto activation = std::make_shared<UpscaleActivationProbe>();
    activation->hold_warm.store(true, std::memory_order_release);
    auto warm_entered = activation->warm_entered.get_future();
    std::atomic_uint32_t borrows{0U};
    std::atomic_uint32_t failures{0U};
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [activation] { return std::make_unique<ActivationUpscaleAlgorithm>(activation); }, 4U),
                          [&source, &borrows](const VisualFrame& frame) {
                              borrows.fetch_add(1U);
                              return source.BorrowExact(frame);
                          },
                          [&events, &failures](UpscaleSystem::event_type event) {
                              if (std::holds_alternative<UpscaleFailed>(event)) failures.fetch_add(1U);
                              events.Advance();
                          }};
    auto settle_upscale = settle_upscale_on_exit(upscale, activation->warm_release);
    upscale.Warm(source.frame().extent);
    const bool warming = warm_entered.wait_for(2s) == std::future_status::ready;
    if (!warming) activation->warm_release.set_value();
    REQUIRE(warming);
    const auto admitted = upscale.Start(test_upscale_request({.source = source.frame()}));
    CHECK(admitted.busy);
    CHECK(borrows.load() == 0U);
    CHECK(activation->runs.load(std::memory_order_acquire) == 0U);
    source.Publish({20U, 12U}, 19U);
    activation->warm_release.set_value();
    REQUIRE(events.Wait([&] { return failures.load() == 1U; }));
    CHECK_FALSE(upscale.snapshot().ready);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    CHECK(upscale.snapshot().input == source.frame());
    CHECK(borrows.load() == 2U);
    CHECK(activation->fallback_activations.load(std::memory_order_acquire) == 0U);
}
TEST_CASE("Upscale warm failure is isolated and Start retains first-use activation fallback") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {18U, 10U}, 5U};
    auto activation = std::make_shared<UpscaleActivationProbe>();
    activation->fail_warm.store(true, std::memory_order_release);
    std::promise<void> warm_failed;
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [activation] { return std::make_unique<ActivationUpscaleAlgorithm>(activation); }, 4U),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&warm_failed, &events](UpscaleSystem::event_type event) {
                              if (std::holds_alternative<UpscaleFailed>(event)) warm_failed.set_value();
                              events.Advance();
                          }};
    upscale.Warm({32U, 32U});
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(warm_failed, "warm_failed", 2s));
    upscale.Warm({32U, 32U});
    CHECK(activation->warms.load(std::memory_order_acquire) == 1U);
    CHECK_FALSE(upscale.snapshot().busy);
    CHECK_FALSE(upscale.snapshot().ready);
    REQUIRE(events.Wait([&] { return upscale.snapshot().methods[1U].warm && upscale.snapshot().methods[2U].warm; }));
    CHECK(activation->runs.load(std::memory_order_acquire) == 6U);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame(), .kernel = UpscaleKernel::RealPlksr})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    REQUIRE(events.Wait([&] { return activation->runs.load(std::memory_order_acquire) == 7U; }));
    CHECK(activation->fallback_activations.load(std::memory_order_acquire) == 1U);
    for (const auto& ready : activation->ready) CHECK(ready.load(std::memory_order_acquire));
    CHECK(upscale.snapshot().methods[0U].initialization_failed);
    CHECK_FALSE(upscale.snapshot().methods[2U].initialization_failed);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame(), .kernel = UpscaleKernel::Default})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().kernel == UpscaleKernel::Default; }));
    CHECK_FALSE(upscale.snapshot().methods[0U].initialization_failed);
}
TEST_CASE("A preempted warm failure remains method-local through another method completion") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {18U, 10U}, 5U};
    auto activation = std::make_shared<UpscaleActivationProbe>();
    activation->fail_warm = true;
    activation->hold_warm = true;
    auto entered = activation->warm_entered.get_future();
    std::promise<UpscaleSnapshot> completed;
    auto result = completed.get_future();
    std::atomic_bool observed{false};
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [activation] { return std::make_unique<ActivationUpscaleAlgorithm>(activation); }, 4U),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&](UpscaleSystem::event_type event) {
                              if (const auto* changed = std::get_if<UpscaleChanged>(&event);
                                  changed && changed->snapshot.ready && changed->snapshot.kernel == UpscaleKernel::RealPlksr && !observed.exchange(true))
                                  completed.set_value(changed->snapshot);
                          }};
    auto settle_upscale = settle_upscale_on_exit(upscale, activation->warm_release);
    upscale.Warm({32U, 32U});
    const auto waiting = entered.wait_for(2s);
    if (waiting != std::future_status::ready) activation->warm_release.set_value();
    REQUIRE(waiting == std::future_status::ready);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame(), .kernel = UpscaleKernel::RealPlksr})));
    activation->warm_release.set_value();
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    const auto snapshot = result.get();
    CHECK(snapshot.methods[0U].initialization_failed);
    CHECK_FALSE(snapshot.methods[0U].warm);
    CHECK(snapshot.methods[2U].warm);
    CHECK_FALSE(snapshot.methods[2U].initialization_failed);
}
TEST_CASE("Upscale exact-frame admission separates invalid kernels from unavailable sources") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {24U, 15U}};
    auto selected_kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    std::atomic<std::size_t> borrows{0U};
    std::atomic<UpscaleFailureKind> failure_kind{UpscaleFailureKind::Failed};
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [selected_kernel] { return std::make_unique<TestUpscaleAlgorithm>(selected_kernel); }, 4U),
                          [&source, &borrows](const VisualFrame& frame) {
                              borrows.fetch_add(1U, std::memory_order_acq_rel);
                              return source.BorrowExact(frame);
                          },
                          [&events, &failure_kind](UpscaleSystem::event_type event) {
                              if (const auto* failure = std::get_if<UpscaleFailed>(&event)) failure_kind.store(failure->kind);
                              events.Advance();
                          }};
    const auto current = source.frame();
    CHECK_THROWS_AS(upscale.Start(test_upscale_request({.source = current, .kernel = static_cast<UpscaleKernel>(255U)})), contracts::InvalidIntentError);
    CHECK(borrows.load(std::memory_order_acquire) == 0U);
    for (auto invalid : {VisualDocumentFacts{}, test_document({}).document->facts()}) {
        invalid.meaning_identity = 0U;
        CHECK_THROWS_AS(upscale.Start({.source = current, .document = invalid}), contracts::InvalidIntentError);
    }
    auto invalid_resource = test_document({}).document->facts();
    invalid_resource.resource = {};
    CHECK_THROWS_AS(upscale.Start({.source = current, .document = invalid_resource}), contracts::InvalidIntentError);
    auto invalid_revision = test_document({}).document->facts();
    invalid_revision.resource.revision = 0U;
    CHECK_THROWS_AS(upscale.Start({.source = current, .document = invalid_revision}), contracts::InvalidIntentError);
    CHECK_THROWS_AS(upscale.Start(test_upscale_request({.source = {}})), contracts::InvalidIntentError);
    CHECK(borrows.load(std::memory_order_acquire) == 0U);
    for (const auto& expired : {visual_frame(current.source, current.extent, current.revision + 1U),
                                visual_frame(current.source, {current.extent.width + 1U, current.extent.height}, current.revision),
                                visual_frame({PresentationSourceKind::Explore, 2U}, current.extent, current.revision)}) {
        static_cast<void>(upscale.Start(test_upscale_request({.source = expired})));
        REQUIRE(events.Wait([&] { return !upscale.snapshot().busy && failure_kind.load() == UpscaleFailureKind::Unavailable; }));
        CHECK_FALSE(upscale.snapshot().ready);
        CHECK(upscale.snapshot().methods[0U].failed);
        CHECK(failure_kind.load() == UpscaleFailureKind::Unavailable);
    }
    CHECK(borrows.load(std::memory_order_acquire) == 3U);
}
TEST_CASE("Upscale copies the admitted exact revision before a source update can replace it") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto copy_gate = backend->HoldSameDeviceCopies("Presentation source copy");
    auto race = std::make_shared<UpscaleAdmissionRaceProbe>();
    UpscaleSourceFixture subject{
        backend,
        {24U, 15U},
        11U,
        RuntimeFactory(
            0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, [race] { return std::make_unique<RacingUpscaleAlgorithm>(race); }, 4U)};
    auto& source = subject.source;
    auto& events = subject.events;
    auto& upscale = subject.upscale;
    const auto admitted_frame = source.frame();
    mmltk::testsupport::ScopedTestCleanup settle_copy{[&] {
        copy_gate->Release();
        upscale.Stop();
        upscale.Shutdown();
    }};
    static_cast<void>(upscale.Start(test_upscale_request({.source = admitted_frame})));
    const bool copy_entered = copy_gate->WaitEntered(2s);
    if (!copy_entered) copy_gate->Release();
    REQUIRE(copy_entered);
    REQUIRE_FALSE(source.TryReserveOutput().valid());
    auto replacement = std::async(std::launch::async, [&source] { source.Publish({24U, 15U}, 29U); });
    mmltk::testsupport::ScopedTestCleanup release_replacement{[&] { copy_gate->Release(); }};
    copy_gate->Release();
    REQUIRE(replacement.wait_for(2s) == std::future_status::ready);
    replacement.get();
    CHECK(source.frame().revision > admitted_frame.revision);
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    CHECK(backend->same_copies.load(std::memory_order_acquire) == 2U);
    CHECK(race->received_value.load(std::memory_order_acquire) == 11U);
    CHECK(race->received_width.load(std::memory_order_acquire) == admitted_frame.extent.width);
    CHECK(race->received_height.load(std::memory_order_acquire) == admitted_frame.extent.height);
    auto product = upscale.BorrowFrame();
    REQUIRE(product.valid());
    CHECK(*reinterpret_cast<const std::uint8_t*>(product.plane(0U).plane().data) == 11U);
}
TEST_CASE("Upscale derives a checked fixed output envelope") {
    CHECK(kUpscaleOutputScale == 4U);
    CHECK((checked_upscale_output_extent({1U, 1U}) == VisualExtent{4U, 4U}));
    CHECK((checked_upscale_output_extent({320U, 180U}) == VisualExtent{1280U, 720U}));
    CHECK_THROWS_AS(checked_upscale_output_extent({}), contracts::InvalidIntentError);
    CHECK_THROWS_AS(checked_upscale_output_extent({std::numeric_limits<std::uint32_t>::max() / kUpscaleOutputScale + 1U, 1U}), contracts::InvalidIntentError);
    CHECK_THROWS_AS(checked_upscale_output_extent({1U, std::numeric_limits<std::uint32_t>::max() / kUpscaleOutputScale + 1U}), contracts::InvalidIntentError);
}
TEST_CASE("Upscale accepts output beyond the source systems base envelope") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore opened_explore{backend, {64U, 64U}};
    auto& explore = opened_explore.system();
    auto selected_kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    EventGate events;
    const VisualDeviceSettings base_envelope{
        .device = kDevice.device,
        .maximum_width = 64U,
        .maximum_height = 64U,
    };
    const VisualDeviceSettings output_envelope{
        .device = kDevice.device,
        .maximum_width = 256U,
        .maximum_height = 256U,
    };
    CHECK(explore.snapshot().frame.extent.width <= base_envelope.maximum_width);
    // CLEANUP-IGNORE: The base-envelope assertion and later high-water scenario setup are unrelated test operations.
    CHECK(explore.snapshot().frame.extent.height <= base_envelope.maximum_height);
    UpscaleSystem upscale{output_envelope,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              // CLEANUP-IGNORE: Upscale Start and Annotation Open are distinct typed runtime paths.
                              [selected_kernel] { return std::make_unique<TestUpscaleAlgorithm>(selected_kernel); }, 4U),
                          borrow_exactly_from(explore), [&events](UpscaleSystem::event_type) { events.Advance(); }};
    static_cast<void>(upscale.Start(test_upscale_request({.source = explore.snapshot().frame})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    CHECK((upscale.snapshot().frame.extent == VisualExtent{256U, 256U}));
    REQUIRE(upscale.BorrowFrame().valid());
    CHECK(upscale.BorrowFrame().plane(0U).plane().descriptor.width == 256U);
    CHECK(upscale.BorrowFrame().plane(0U).plane().descriptor.height == 256U);
    const std::array sources{read_from(explore), read_from(upscale)};
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    auto allocation_retired = writer_state->allocation_retired.get_future();
    EventGate presentation_events;
    PresentationSystem presentation{output_envelope, [backend, writer_state] { return std::make_unique<TestPresentationWriter>(0, backend, writer_state); },
                                    std::span{sources}, [&presentation_events](PresentationSystem::event_type) { presentation_events.Advance(); }};
    PresentationScenario scenario{presentation, writer_state};
    static_cast<void>(presentation.Select(sources[0].source));
    writer_state->SignalReadiness();
    REQUIRE(presentation_events.Wait([&] { return presentation.snapshot().completed.source == sources[0].source; }));
    CHECK((presentation.snapshot().capability.extent == VisualExtent{64U, 64U}));
    const auto initial_generation = presentation.snapshot().capability.generation;
    static_cast<void>(presentation.Select(sources[1].source));
    writer_state->SignalReadiness();
    REQUIRE(presentation_events.Wait([&] { return presentation.snapshot().completed.source == sources[1].source; }));
    CHECK((presentation.snapshot().capability.extent == VisualExtent{256U, 256U}));
    CHECK(presentation.snapshot().capability.generation > initial_generation);
    CHECK(presentation.snapshot().completed.revision == upscale.snapshot().frame.revision);
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 0U);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == initial_generation);
    writer_state->AcknowledgeAllocationRetirement(initial_generation);
    REQUIRE(allocation_retired.wait_for(2s) == std::future_status::ready);
    allocation_retired.get();
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 1U);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == 0U);
    const auto pump_count = writer_state->PumpCount();
    writer_state->AcknowledgeAllocationRetirement(initial_generation);
    REQUIRE(writer_state->WaitForPumpAfter(pump_count));
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 1U);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    upscale.Shutdown();
    CHECK(upscale.stopped());
}
TEST_CASE("Upscale invalid exact-source admission preserves the active operation snapshot") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore opened_explore{backend, {32U, 32U}};
    auto& explore = opened_explore.system();
    auto selected_kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto gate = std::make_shared<MutationCommitProbe>();
    auto entered = gate->committed.get_future();
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [selected_kernel, gate] { return std::make_unique<TestUpscaleAlgorithm>(selected_kernel, gate); }, 4U),
                          borrow_exactly_from(explore), [&events](UpscaleSystem::event_type) { events.Advance(); }};
    auto settle_upscale = settle_upscale_on_exit(upscale, gate->release);
    const auto admitted = upscale.Start(test_upscale_request({
        .source = explore.snapshot().frame,
        .kernel = UpscaleKernel::ShiftLut,
    }));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();
    CHECK_FALSE(upscale.BorrowFrame().valid());
    CHECK_THROWS_AS(upscale.Start(test_upscale_request({
                        .source = {},
                        .kernel = UpscaleKernel::RealPlksr,
                    })),
                    contracts::InvalidIntentError);
    const auto after_duplicate = upscale.snapshot();
    CHECK(after_duplicate.revision == admitted.revision);
    CHECK(after_duplicate.busy);
    CHECK(after_duplicate.pending->kernel == UpscaleKernel::ShiftLut);
    gate->release.set_value();
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
}
TEST_CASE("Upscale method failure preserves healthy resident products and permits isolated retry") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore opened_explore{backend, {32U, 32U}};
    auto& explore = opened_explore.system();
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    std::promise<UpscaleFailed> failure;
    EventGate events;
    UpscaleSystem upscale{
        kDevice,
        RuntimeFactory(
            0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, [runs] { return std::make_unique<FailingUpscaleAlgorithm>(runs); }, 4U),
        borrow_exactly_from(explore), [&failure, &events](UpscaleSystem::event_type event) {
            events.Advance();
            if (auto* failed = std::get_if<UpscaleFailed>(&event)) failure.set_value(std::move(*failed));
        }};
    static_cast<void>(upscale.Start(test_upscale_request({.source = explore.snapshot().frame})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    REQUIRE(upscale.BorrowFrame().valid());
    static_cast<void>(upscale.Start(test_upscale_request({.source = explore.snapshot().frame, .kernel = UpscaleKernel::ShiftLut})));
    auto failed_event = failure.get_future();
    REQUIRE(failed_event.wait_for(2s) == std::future_status::ready);
    const auto failed = failed_event.get();
    CHECK(failed.snapshot.ready);
    CHECK(failed.snapshot.frame.valid());
    CHECK(upscale.BorrowFrame().valid());
    CHECK(failed.snapshot.methods[0U].available);
    CHECK(failed.snapshot.methods[1U].failed);
    static_cast<void>(upscale.Start(test_upscale_request({.source = explore.snapshot().frame, .kernel = UpscaleKernel::ShiftLut})));
    REQUIRE(events.Wait([&] { return !upscale.snapshot().busy && upscale.snapshot().kernel == UpscaleKernel::ShiftLut; }));
    CHECK(runs->load(std::memory_order_acquire) == 3U);
    const auto recovered = upscale.BorrowDocument(upscale.snapshot().frame);
    REQUIRE(recovered.valid());
    CHECK(upscale.snapshot().input == explore.snapshot().frame);
    CHECK(*reinterpret_cast<const std::uint8_t*>(recovered.pixels.plane(0U).plane().data) == 1U);
}
TEST_CASE("Shared Upscale execution failure invalidates every resident product and preserves typed failure authority") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}};
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    std::promise<UpscaleFailed> failed;
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [runs] { return std::make_unique<FailingUpscaleAlgorithm>(runs, true); }, 4U),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&events, &failed](UpscaleSystem::event_type event) {
                              if (auto* failure = std::get_if<UpscaleFailed>(&event)) failed.set_value(*failure);
                              events.Advance();
                          }};
    static_cast<void>(upscale.Start(test_upscale_request({source.frame()})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    const auto request = test_upscale_request({source.frame(), UpscaleKernel::ShiftLut});
    static_cast<void>(upscale.Start(request));
    auto failure = failed.get_future();
    REQUIRE(failure.wait_for(2s) == std::future_status::ready);
    const auto result = failure.get();
    CHECK(result.kind == UpscaleFailureKind::Physical);
    CHECK(result.request == request);
    CHECK_FALSE(result.snapshot.ready);
    CHECK_FALSE(upscale.BorrowFrame().valid());
    for (const auto& method : result.snapshot.methods) CHECK_FALSE(method.available);
    CHECK(mmltk::frameworks::gpu::CudaError(cudaErrorIllegalAddress, "shared").shared_failure());
    CHECK_FALSE(mmltk::frameworks::gpu::CudaError(cudaErrorMemoryAllocation, "local").shared_failure());
}
}  // namespace
}  // namespace mmltk::controller
