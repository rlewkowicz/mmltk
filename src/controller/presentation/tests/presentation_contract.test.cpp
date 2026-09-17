#include "src/backend/imaging/upscale/upscale_execution.h"
#include "src/common/io/scoped_fd.h"
#include "src/common/types/generation.h"
#include "src/controller/presentation/abi/workspace_frame_signal.h"
#include "src/controller/presentation/detail/native_presentation_writer_test_access.h"
#include "src/controller/presentation/detail/workspace_surface_import_channel.h"
#include "src/controller/presentation/presentation_system.h"
#include "src/controller/presentation/tests/support/visual_runtime_fixture.h"
#include "src/controller/presentation/tests/support/visual_system_fixture.h"
#include "src/controller/presentation/tests/support/workspace_surface_socket_test_utils.hpp"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/controller/subsystems/live/tests/support/live_system_fixture.h"
#include "src/controller/subsystems/upscale/upscale_system.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cuda_runtime_api.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
namespace mmltk::controller {
namespace {
using namespace visual_test_support;
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
TEST_CASE("Graphics signal publishes exact bounded image metadata with its physical transfer", "[presentation][metadata]") {
    namespace graphics = presentation::detail;
    auto owned = presentation::WorkspaceSurfaceFrameSignal::create();
    auto* signal = owned.mapping();
    const std::array first{std::byte{0x83}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    graphics::publish_workspace_frame_signal(signal, 1U, 1U, presentation::WorkspacePresentationLayer::Primary, {1U, 7U}, 3U, 640U, 480U, 7U, first);
    CHECK(signal->metadata_bytes == first.size());
    CHECK(signal->transfer_sequence == 1U);
    CHECK(signal->physical_revision == 7U);
    CHECK((signal->sequence_lock & 1U) == 0U);
    const auto* payload = reinterpret_cast<const std::byte*>(signal) + sizeof(*signal);
    CHECK(std::equal(first.begin(), first.end(), payload));
    const auto sequence = signal->sequence_lock;
    const std::vector<std::byte> oversized(graphics::kWorkspaceMetadataByteCapacity + 1U);
    CHECK_THROWS_AS(
        graphics::publish_workspace_frame_signal(signal, 3U, 2U, presentation::WorkspacePresentationLayer::Primary, {1U, 8U}, 4U, 640U, 480U, 8U, oversized),
        std::length_error);
    CHECK(signal->sequence_lock == sequence);
    CHECK(signal->transfer_sequence == 1U);
    CHECK(std::equal(first.begin(), first.end(), payload));
    const std::array second{std::byte{0x81}, std::byte{0x09}};
    graphics::publish_workspace_frame_signal(signal, 3U, 2U, presentation::WorkspacePresentationLayer::Primary, {1U, 8U}, 4U, 640U, 480U, 8U, second);
    CHECK(signal->metadata_bytes == second.size());
    CHECK(signal->transfer_sequence == 2U);
    CHECK(std::equal(second.begin(), second.end(), payload));
}
TEST_CASE("Committed visual frames exactly authorize one- and two-plane products") {
    auto backend = std::make_shared<FakeImageBackend>();
    mmltk::frameworks::gpu::SystemImageRuntime clean{{.device = 0, .backend = backend, .output_layout = mmltk::frameworks::gpu::ImageProductLayout::Clean}};
    clean.Publish(8U, 6U, [](auto, auto, auto) {});
    auto clean_product = clean.Borrow();
    REQUIRE(clean_product.valid());
    const auto clean_frame = visual_frame({PresentationSourceKind::Explore, 1U}, {8U, 6U}, clean_product.plane(0U).revision());
    CHECK(visual_product_matches_frame(clean_frame, clean_product));
    CHECK_FALSE(visual_product_matches_frame({}, clean_product));
    CHECK_FALSE(visual_product_matches_frame(visual_frame(clean_frame.source, clean_frame.extent, clean_frame.revision + 1U), clean_product));
    CHECK_FALSE(visual_product_matches_frame(visual_frame(clean_frame.source, {clean_frame.extent.width + 1U, clean_frame.extent.height}, clean_frame.revision),
                                             clean_product));
    CHECK_FALSE(visual_product_matches_frame(clean_frame, {}));
    mmltk::frameworks::gpu::SystemImageRuntime layered{
        {.device = 0, .backend = backend, .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic}};
    layered.Publish(9U, 7U, [](auto, auto, auto) {});
    auto layered_product = layered.Borrow();
    REQUIRE(layered_product.valid());
    REQUIRE(layered_product.plane_count() == 2U);
    const auto layered_frame = visual_frame({PresentationSourceKind::Annotation, 1U}, {9U, 7U}, layered_product.plane(0U).revision());
    CHECK(visual_product_matches_frame(layered_frame, layered_product));
}
TEST_CASE("Workspace layout rejects delayed undersized capacity") {
    namespace abi = presentation::detail::workspace_surface_import;
    abi::Record layout{.opcode = abi::Opcode::ArenaReady,
                       .id_high = 1U,
                       .width = 100U,
                       .height = 100U,
                       .stride = 400U,
                       .size = 40'000U,
                       .device_incarnation = 2U,
                       .alignment = 16U,
                       .device_uuid = {1U},
                       .memory_type_bits = 1U};
    CHECK(abi::valid(layout));
    layout.width = 200U;
    layout.height = 200U;
    CHECK_FALSE(abi::valid(layout));
    layout.stride = 1024U;
    layout.size = 204'800U;
    CHECK(abi::valid(layout));
    layout.stride = 799U;
    CHECK_FALSE(abi::valid(layout));
}
class PresentationSourceFixture final {
   public:
    explicit PresentationSourceFixture(const std::size_t buffers = 1U)
        : backend_(std::make_shared<FakeImageBackend>()),
          revisions_(std::make_shared<mmltk::frameworks::gpu::ImageProductRevisionSequence>()),
          source_(std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
              .device = 0, .backend = backend_, .output_buffer_count = buffers, .product_revisions = revisions_})),
          sources_{VisualSourceReader{
              .source = identity_,
              .observe =
                  [this] {
                      return VisualSourceObservation{
                          .frame = visual_frame(identity_, {16U, 16U}, latest_revision_.load(std::memory_order_acquire)),
                          .snapshot_revision = snapshot_revision_.load(std::memory_order_acquire),
                      };
                  },
              .borrow = [this] { return source_->Borrow(); },
          }} {
        source_->Publish(16U, 16U, [](auto, auto, auto) {});
    }
    [[nodiscard]] std::shared_ptr<FakeImageBackend> backend() const noexcept { return backend_; }
    [[nodiscard]] std::span<const VisualSourceReader> sources() const noexcept { return sources_; }
    [[nodiscard]] PresentationSourceIdentity identity() const noexcept { return identity_; }
    void AdvanceObservation() { snapshot_revision_.fetch_add(1U, std::memory_order_acq_rel); }
    void PublishUnobserved() {
        source_->Publish(16U, 16U, [](auto, auto, auto) {});
    }
    void Advance() {
        PublishUnobserved();
        UpdateObservation();
    }
    [[nodiscard]] auto Completed() const { return source_->Completed(); }
    void SelectCompleted(const mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput& product) {
        source_->SelectOutput(product);
        UpdateObservation();
    }
    void Reconstruct() {
        source_ = std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(
            mmltk::frameworks::gpu::SystemImageRuntimeConfig{.device = 0, .backend = backend_, .product_revisions = revisions_});
        Advance();
    }

   private:
    void UpdateObservation() {
        const auto borrowed = source_->Borrow();
        REQUIRE(borrowed.valid());
        latest_revision_.store(borrowed.plane(0U).revision(), std::memory_order_release);
        snapshot_revision_.fetch_add(1U, std::memory_order_acq_rel);
    }
    std::shared_ptr<FakeImageBackend> backend_;
    std::shared_ptr<mmltk::frameworks::gpu::ImageProductRevisionSequence> revisions_;
    std::unique_ptr<mmltk::frameworks::gpu::SystemImageRuntime> source_;
    std::atomic<std::uint64_t> latest_revision_{1U};
    std::atomic<std::uint64_t> snapshot_revision_{1U};
    PresentationSourceIdentity identity_{PresentationSourceKind::Explore, 1U};
    std::array<VisualSourceReader, 1U> sources_;
};
class PresentationFailureProbe final {
   public:
    void Observe(PresentationSystem::event_type event) {
        if (std::holds_alternative<PresentationFailed>(event)) failures_.fetch_add(1U, std::memory_order_acq_rel);
        events_.Advance();
    }
    [[nodiscard]] EventGate& events() noexcept { return events_; }
    [[nodiscard]] std::size_t failures() const noexcept { return failures_.load(std::memory_order_acquire); }

   private:
    EventGate events_;
    std::atomic<std::size_t> failures_{0U};
};
[[nodiscard]] PresentationSystem make_failure_presentation(const PresentationSourceFixture& source,
                                                           const std::shared_ptr<TestPresentationWriterState>& writer_state,
                                                           PresentationFailureProbe& failures) {
    return PresentationSystem{kDevice, TestPresentationWriter::Factory(source.backend(), writer_state), source.sources(),
                              [&failures](PresentationSystem::event_type event) { failures.Observe(std::move(event)); }};
}
TEST_CASE("Presentation monotonic identities fail before wrap") {
    namespace identity = mmltk::common::types;
    CHECK(identity::advance_monotonic_identity(0U) == 1U);
    CHECK(identity::advance_monotonic_identity(4U, 2U) == 6U);
    CHECK_THROWS_AS(identity::advance_monotonic_identity(4U, 0U), std::overflow_error);
    CHECK_THROWS_AS(identity::advance_monotonic_identity(std::numeric_limits<std::uint64_t>::max()), std::overflow_error);
    std::uint64_t next = 0U;
    CHECK_THROWS_AS(identity::take_monotonic_identity(next), std::overflow_error);
    CHECK(next == 0U);
    next = 3U;
    CHECK(identity::take_monotonic_identity(next, 0U) == 3U);
    CHECK(next == 3U);
    CHECK(identity::take_monotonic_identity(next, 2U) == 3U);
    CHECK(next == 5U);
    std::uint64_t timeline = std::numeric_limits<std::uint64_t>::max() - 1U;
    CHECK_THROWS_AS(identity::take_monotonic_identity(timeline, 2U), std::overflow_error);
    CHECK(timeline == std::numeric_limits<std::uint64_t>::max() - 1U);
}
TEST_CASE("Workspace transfer sequences map to their odd timeline values") {
    namespace workspace = mmltk::controller::presentation::detail;
    CHECK(workspace::workspace_timeline_ready(1U) == 1U);
    CHECK(workspace::workspace_timeline_ready(2U) == 3U);
    CHECK(workspace::workspace_timeline_ready(3U) == 5U);
    CHECK_THROWS_AS(workspace::workspace_timeline_ready(0U), std::overflow_error);
    CHECK_THROWS_AS(workspace::workspace_timeline_ready(std::numeric_limits<std::uint64_t>::max()), std::overflow_error);
}
TEST_CASE("Native Basic settled allocation and launch failures permit neural work and lazy Basic retry", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    const bool allocation_failure = GENERATE(false, true);
    using namespace mmltk::frameworks::gpu;
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    const Stage failure_stage = allocation_failure ? Stage::BasicAllocationAdmitted : Stage::BasicLaunchAdmitted;
    std::atomic_uint32_t stage_occurrence = 0U;
    std::atomic_bool injected = false;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](const Stage stage) {
        if (stage == failure_stage && stage_occurrence.fetch_add(1U, std::memory_order_relaxed) == 1U) {
            injected.store(true, std::memory_order_relaxed);
            throw CudaError(allocation_failure ? cudaErrorMemoryAllocation : cudaErrorInvalidPitchValue, "injected settled Basic operation failure");
        }
    })(std::make_shared<ImageProductRevisionSequence>());
    runtime->BeginWork();
    auto* model = dynamic_cast<UpscaleAlgorithm*>(runtime->model());
    REQUIRE(model != nullptr);
    runtime->PublishInput(8U, 8U, [model](auto clean, auto semantic, auto stream) {
        model->Semantics({}, clean, stream);
        model->Semantics({}, semantic, stream);
    });
    {
        const auto input = runtime->BorrowInput();
        const auto publish = [&](const UpscaleKernel method) {
            runtime->Publish(32U, 32U, [&](auto output, auto semantic, auto stream) {
                auto source = input.plane(0U).plane();
                model->Run(method, source, output, stream);
                model->Semantics({}, semantic, stream);
            });
        };
        try {
            publish(UpscaleKernel::Default);
            FAIL("Basic accepted the failing allocation or launch");
        } catch (const CudaError& failure) {
            CHECK(failure.status() == (allocation_failure ? cudaErrorMemoryAllocation : cudaErrorInvalidPitchValue));
            CHECK_FALSE(failure.shared_failure());
        }
        CHECK(injected.load(std::memory_order_relaxed));
        CHECK_NOTHROW(publish(UpscaleKernel::ShiftLut));
        CHECK_NOTHROW(publish(UpscaleKernel::Default));
    }
    // The local error must not poison the aggregate's checked release.
    CHECK(runtime->Retire().safe_to_destroy);
}
TEST_CASE("Presentation serializes consecutive growth through exact retirement acknowledgements") {
    ProductPresentationSources products;
    const auto sources = products.sources();
    REQUIRE(sources.size() >= 5U);
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    auto third_submission = writer_state->third_submission.get_future();
    EventGate events;
    PresentationSystem presentation{kDevice,
                                    [backend = products.backend(), writer_state] { return std::make_unique<TestPresentationWriter>(0, backend, writer_state); },
                                    sources, [&events](PresentationSystem::event_type) { events.Advance(); }};
    PresentationScenario scenario{presentation, writer_state};
    const auto source_a = sources[0].source;
    const auto source_b = sources[2].source;
    const auto source_c = sources[4].source;
    static_cast<void>(presentation.Select(source_a));
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.source == source_a; }));
    const auto generation_a = presentation.snapshot().capability.generation;
    static_cast<void>(presentation.Select(source_b));
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.source == source_b; }));
    const auto generation_b = presentation.snapshot().capability.generation;
    REQUIRE(generation_b > generation_a);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == generation_a);
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 0U);
    static_cast<void>(presentation.Select(source_c));
    writer_state->SignalReadiness();
    REQUIRE(third_submission.wait_for(2s) == std::future_status::ready);
    third_submission.get();
    CHECK(presentation.snapshot().completed.source == source_b);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == generation_a);
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 0U);
    auto pump_count = writer_state->PumpCount();
    writer_state->AcknowledgeAllocationRetirement(generation_b);
    REQUIRE(writer_state->WaitForPumpAfter(pump_count));
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == generation_a);
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 0U);
    writer_state->AcknowledgeAllocationRetirement(generation_a);
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.source == source_c; }));
    CHECK(presentation.snapshot().completed.revision == sources[4].observe().frame.revision);
    CHECK(presentation.snapshot().capability.generation > generation_b);
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 1U);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == generation_b);
    pump_count = writer_state->PumpCount();
    writer_state->AcknowledgeAllocationRetirement(generation_a);
    REQUIRE(writer_state->WaitForPumpAfter(pump_count));
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 1U);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == generation_b);
    pump_count = writer_state->PumpCount();
    writer_state->AcknowledgeAllocationRetirement(generation_b);
    REQUIRE(writer_state->WaitForPumpAfter(pump_count));
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 2U);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == 0U);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
}
TEST_CASE("Source admission writes retain complete packet and frame provenance", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::receive_workspace_record;
    using mmltk::testsupport::send_workspace_record;
    const bool deferred = GENERATE(false, true);
    enum class Diagnostics { Absent, Disabled, Enabled };
    const auto diagnostics = GENERATE(Diagnostics::Absent, Diagnostics::Disabled, Diagnostics::Enabled);
    CAPTURE(deferred, static_cast<int>(diagnostics));
    mmltk::testsupport::ScopedTempDir root{"presentation-admission-diagnostics"};
    const auto path = root.path() / "import.sock";
    struct Capture final {
        std::array<VisualDiagnosticFact, 2> facts{};
        std::size_t count = 0U;
        bool enabled = false;
    } capture;
    capture.enabled = diagnostics == Diagnostics::Enabled;
    const VisualDiagnosticSink sink{.context = &capture,
                                    .write =
                                        [](void* context, const VisualDiagnosticFact fact) noexcept {
                                            auto& value = *static_cast<Capture*>(context);
                                            if (value.count < value.facts.size()) value.facts[value.count] = fact;
                                            ++value.count;
                                        },
                                    .enabled = [](void* context) noexcept { return static_cast<Capture*>(context)->enabled; }};
    presentation::WorkspaceSurfaceImportChannel channel{path, diagnostics == Diagnostics::Absent ? VisualDiagnosticSink{} : sink};
    auto peer = mmltk::testsupport::connect_workspace_surface_shell(path);
    channel.pump();
    REQUIRE(channel.connected());
    std::size_t queued_fillers = 0U;
    const abi::Record filler{.opcode = abi::Opcode::Drop, .id_high = 101U, .id_low = 102U};
    if (deferred) {
        const int send_bytes = 4096;
        REQUIRE(::setsockopt(channel.poll_fd(), SOL_SOCKET, SO_SNDBUF, &send_bytes, sizeof(send_bytes)) == 0);
        // Fill the real outbound socket before admitting the source. The bounded
        // loop stops on EAGAIN; no sleeps or assumed kernel queue length.
        while (queued_fillers < 256U && send_workspace_record(channel.poll_fd(), filler)) ++queued_fillers;
        REQUIRE(queued_fillers > 0U);
        REQUIRE(queued_fillers < 256U);
    }
    auto edge = mmltk::testsupport::workspace_surface_event_descriptor();
    auto signal = presentation::WorkspaceSurfaceFrameSignal::create();
    const presentation::WorkspaceSurfaceImportId id{11U, 12U};
    abi::Record record{.id_high = id.high,
                       .id_low = id.low,
                       .width = 64U,
                       .height = 32U,
                       .stride = 512U,
                       .size = 32768U,
                       .descriptors = abi::kAllocateDescriptorCount,
                       .arena_high = 1U,
                       .arena_low = 2U,
                       .allocation_identity = 3U,
                       .device_incarnation = 4U,
                       .offset = 256U,
                       .alignment = 256U,
                       .device_uuid = {1U},
                       .memory_type_bits = 1U};
    const auto admitted = record;
    REQUIRE(channel.admit_source(record, 7U, edge.get(), signal.descriptor(), edge.get(), 19U, 23U));
    CHECK(channel.claimable(id) == !deferred);
    CHECK(channel.wants_write() == deferred);
    REQUIRE(capture.count == (capture.enabled ? (deferred ? 1U : 2U) : 0U));
    record = {};
    if (deferred) {
        for (std::size_t index = 0U; index < queued_fillers; ++index) {
            abi::Record received{};
            CHECK(receive_workspace_record(peer.get(), received).descriptor_count == 0U);
            CHECK(received.opcode == filler.opcode);
            CHECK(received.id_high == filler.id_high);
            CHECK(received.id_low == filler.id_low);
        }
        channel.pump();
    }
    REQUIRE(channel.claimable(id));
    CHECK_FALSE(channel.wants_write());
    CHECK_FALSE(channel.terminal_error());
    abi::Record sent{};
    const auto descriptors = receive_workspace_record(peer.get(), sent);
    REQUIRE(descriptors.descriptor_count == abi::kAllocateDescriptorCount);
    for (std::size_t index = 0U; index < descriptors.descriptor_count; ++index) CHECK(::fcntl(descriptors.descriptors[index].get(), F_GETFD) >= 0);
    CHECK(abi::valid(sent));
    // Record is the canonical allocation-request wire layout.
    CHECK(std::memcmp(&sent, &admitted, sizeof(sent)) == 0);
    REQUIRE(capture.count == (capture.enabled ? 2U : 0U));
    if (!capture.enabled) return;
    CHECK(capture.facts[0].operation == VisualDiagnosticOperation::PresentationSourceAdmissionEnqueued);
    CHECK(capture.facts[1].operation == VisualDiagnosticOperation::PresentationSourceAdmissionWritten);
    for (const auto& fact : capture.facts) {
        CHECK(fact.context.surface_high == id.high);
        CHECK(fact.context.surface_low == id.low);
        CHECK(fact.generation == 7U);
        CHECK(fact.context.selection_generation == 19U);
        CHECK(fact.context.frame_revision == 23U);
        CHECK(fact.context.condition == static_cast<std::uint64_t>(PresentationCapabilityCondition::Admitted));
        CHECK(fact.context.outcome == 1U);
        CHECK(fact.context.allocation.allocation_generation == 7U);
        CHECK(fact.context.capacity_width == sent.width);
        CHECK(fact.context.capacity_height == sent.height);
        const auto& workspace = fact.context.workspace;
        CHECK(workspace.workspace_source_high == sent.id_high);
        CHECK(workspace.workspace_source_low == sent.id_low);
        CHECK(workspace.workspace_allocation == sent.allocation_identity);
        CHECK(workspace.workspace_arena_high == sent.arena_high);
        CHECK(workspace.workspace_arena_low == sent.arena_low);
        CHECK(workspace.workspace_bytes == sent.size);
        CHECK(workspace.workspace_pitch == sent.stride);
        CHECK(workspace.workspace_width == sent.width);
        CHECK(workspace.workspace_height == sent.height);
    }
}
class WorkspaceChannelFixture final {
    using Record = presentation::detail::workspace_surface_import::Record;
    using Opcode = presentation::detail::workspace_surface_import::Opcode;
    using Id = presentation::WorkspaceSurfaceImportId;
    mmltk::testsupport::ScopedTempDir root_{"workspace-channel"};

   public:
    presentation::WorkspaceSurfaceImportChannel channel{root_.path() / "import.sock"};
    mmltk::common::io::ScopedFd peer = mmltk::testsupport::connect_workspace_surface_shell(root_.path() / "import.sock");
    WorkspaceChannelFixture() {
        channel.pump();
        REQUIRE(channel.connected());
    }
    [[nodiscard]] static Record Layout(const Id id) {
        return {.opcode = Opcode::ArenaReady,
                .id_high = id.high,
                .id_low = id.low,
                .width = 4U,
                .height = 3U,
                .stride = 32U,
                .size = 96U,
                .device_incarnation = 5U,
                .alignment = 32U,
                .device_uuid = {1U},
                .memory_type_bits = 1U};
    }
    [[nodiscard]] static Record Sample(const Id arena) {
        return {.opcode = Opcode::Presented, .id_high = arena.high, .id_low = arena.low, .stride = 7U, .size = 8U, .code = 1U, .presentation_revision = 9U};
    }
    [[nodiscard]] static Record Acquisition(const Id source) {
        auto acquired = Sample(source);
        acquired.opcode = Opcode::Acquired;
        acquired.code = 0U;
        acquired.offset = 1U;
        return acquired;
    }
    void ReleaseRead(Record acquired) {
        acquired.opcode = Opcode::ReleaseSubmitted;
        REQUIRE(mmltk::testsupport::send_workspace_record(peer.get(), acquired));
        channel.pump();
        REQUIRE(channel.take_source_transition());
        REQUIRE(channel.read_settled({acquired.id_high, acquired.id_low}, {7U, 8U}, 9U, 1U));
        Record settled{};
        static_cast<void>(mmltk::testsupport::receive_workspace_record(peer.get(), settled));
        CHECK(settled.opcode == Opcode::ReadSettled);
    }
    Record AdmitArena(const Id id, const std::uint64_t generation, const bool direct = false) {
        auto layout = Layout(id);
        layout.direct_sampling = direct ? 1U : 0U;
        REQUIRE(channel.admit_arena(id, generation, layout.width, layout.height));
        ExpectRecord(id, Opcode::Arena);
        REQUIRE(mmltk::testsupport::send_workspace_record(peer.get(), layout));
        channel.pump();
        const auto outcome = channel.take_outcome();
        REQUIRE(outcome.has_value());
        CHECK(outcome->id == id);
        REQUIRE(outcome->imported);
        return layout;
    }
    void AdmitSource(const Id id, const Id arena, const Record layout, const std::uint64_t generation) {
        auto memory = mmltk::testsupport::workspace_surface_event_descriptor();
        auto edge = mmltk::testsupport::workspace_surface_event_descriptor();
        auto signal = presentation::WorkspaceSurfaceFrameSignal::create();
        auto imported = layout;
        imported.id_high = id.high;
        imported.id_low = id.low;
        imported.arena_high = arena.high;
        imported.arena_low = arena.low;
        imported.allocation_identity = 6U;
        REQUIRE(channel.admit_source(imported, generation, edge.get(), signal.descriptor(), edge.get()));
        Record received{};
        CHECK(mmltk::testsupport::receive_workspace_record(peer.get(), received).descriptor_count ==
              presentation::detail::workspace_surface_import::kAllocateDescriptorCount);
        auto timeline = mmltk::testsupport::workspace_surface_event_descriptor();
        const std::array descriptors{memory.get(), timeline.get()};
        received.opcode = Opcode::Ready;
        received.descriptors = presentation::detail::workspace_surface_import::kReadyDescriptorCount;
        REQUIRE(mmltk::testsupport::send_workspace_record(peer.get(), received, descriptors));
        channel.pump();
        const auto outcome = channel.take_outcome();
        REQUIRE(outcome.has_value());
        REQUIRE(outcome->imported);
    }
    void Withdraw(const Id id) {
        REQUIRE(channel.withdraw(id).progress == presentation::WorkspaceSurfaceWithdrawalProgress::Submitted);
        ExpectRecord(id, Opcode::Drop);
    }
    void Retire(const Id id, const std::uint64_t generation) {
        REQUIRE(mmltk::testsupport::send_workspace_record(peer.get(), {.opcode = Opcode::Retired, .id_high = id.high, .id_low = id.low}));
        channel.pump();
        const auto retired = channel.take_retirement();
        REQUIRE(retired.has_value());
        CHECK(retired->id == id);
        CHECK(retired->generation == generation);
    }

   private:
    void ExpectRecord(const Id id, const Opcode opcode) {
        Record received{};
        CHECK(mmltk::testsupport::receive_workspace_record(peer.get(), received).descriptor_count == 0U);
        CHECK(received.opcode == opcode);
        CHECK(received.id_high == id.high);
        CHECK(received.id_low == id.low);
    }
};
TEST_CASE("Retired source admission does not retire its occupied sample arena", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::receive_workspace_record;
    using mmltk::testsupport::send_workspace_record;
    WorkspaceChannelFixture fixture;
    auto& channel = fixture.channel;
    auto& peer = fixture.peer;
    const presentation::WorkspaceSurfaceImportId arena{1U, 2U};
    const presentation::WorkspaceSurfaceImportId source{3U, 4U};
    abi::Record received{};
    const auto layout = fixture.AdmitArena(arena, 1U, GENERATE(false, true));
    fixture.AdmitSource(source, arena, layout, 2U);
    // A capacity retry may repeat content and presentation while representing
    // a new physical source transfer. The wire must preserve that distinction.
    for (const auto transfer : {1U, 2U}) {
        REQUIRE(send_workspace_record(peer.get(), {.opcode = abi::Opcode::Acquired,
                                                   .id_high = source.high,
                                                   .id_low = source.low,
                                                   .stride = 7U,
                                                   .size = 8U,
                                                   .presentation_revision = 9U,
                                                   .offset = transfer}));
        channel.pump();
        const auto acquisition = channel.take_source_transition();
        REQUIRE(acquisition.has_value());
        CHECK(acquisition->offset == transfer);
        CHECK(acquisition->opcode == abi::Opcode::Acquired);
        CHECK_FALSE(channel.take_source_transition().has_value());
        CHECK_FALSE(channel.read_settled(source, {7U, 8U}, 9U, transfer));
        auto release = *acquisition;
        release.opcode = abi::Opcode::ReleaseSubmitted;
        REQUIRE(send_workspace_record(peer.get(), release));
        channel.pump();
        CHECK_FALSE(channel.read_settled(source, {7U, 8U}, 9U, transfer));
        const auto submitted = channel.take_source_transition();
        REQUIRE(submitted.has_value());
        CHECK(submitted->opcode == abi::Opcode::ReleaseSubmitted);
        CHECK(submitted->offset == transfer);
        REQUIRE(channel.read_settled(source, {7U, 8U}, 9U, transfer));
        static_cast<void>(receive_workspace_record(peer.get(), received));
        CHECK(received.opcode == abi::Opcode::ReadSettled);
        CHECK(received.offset == transfer);
        CHECK(received.stride == 7U);
        CHECK(received.size == 8U);
        CHECK(received.presentation_revision == 9U);
        CHECK(abi::valid(received));
        auto malformed = received;
        malformed.offset = 0U;
        CHECK_FALSE(abi::valid(malformed));
        malformed = received;
        malformed.opcode = abi::Opcode::Presented;
        malformed.code = 1U;
        CHECK_FALSE(abi::valid(malformed));
    }
    CHECK_FALSE(channel.read_settled(source, {7U, 8U}, 9U, 0U));
    auto sample = WorkspaceChannelFixture::Sample(arena);
    REQUIRE(send_workspace_record(peer.get(), sample));
    channel.pump();
    fixture.Withdraw(source);
    fixture.Retire(source, 2U);
    CHECK(channel.claimable(arena));
    CHECK_FALSE(channel.terminal_error());
    fixture.Withdraw(arena);
    sample.opcode = abi::Opcode::Completed;
    REQUIRE(send_workspace_record(peer.get(), sample));
    fixture.Retire(arena, 1U);
    CHECK_FALSE(channel.terminal_error());
}
// CLEANUP-IGNORE: Binding replacement and mailbox-independent completion use the same fixture for different channel identities and
// transition sequences.
TEST_CASE("Scoped binding retirement preserves a replacement while an old physical read settles", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::receive_workspace_record;
    using mmltk::testsupport::send_workspace_record;
    WorkspaceChannelFixture fixture;
    const presentation::WorkspaceSurfaceImportId old_arena{11U, 12U}, new_arena{21U, 22U}, source{31U, 32U};
    const auto layout = fixture.AdmitArena(old_arena, 1U, GENERATE(false, true));
    fixture.AdmitSource(source, old_arena, layout, 2U);
    auto acquired = WorkspaceChannelFixture::Acquisition(source);
    REQUIRE(send_workspace_record(fixture.peer.get(), acquired));
    fixture.channel.pump();
    REQUIRE(fixture.channel.take_source_transition());
    REQUIRE(send_workspace_record(fixture.peer.get(), {.opcode = abi::Opcode::BindingRetired, .id_high = old_arena.high, .id_low = old_arena.low}));
    fixture.channel.pump();
    const auto binding = fixture.channel.take_source_transition();
    REQUIRE(binding);
    CHECK(binding->opcode == abi::Opcode::BindingRetired);
    fixture.AdmitArena(new_arena, 3U);
    CHECK(fixture.channel.claimable(new_arena));
    fixture.Withdraw(old_arena);
    CHECK_FALSE(fixture.channel.read_settled(source, {7U, 8U}, 9U, 1U));
    fixture.ReleaseRead(acquired);
    fixture.Withdraw(source);
    fixture.Retire(source, 2U);
    fixture.Retire(old_arena, 1U);
    CHECK(fixture.channel.claimable(new_arena));
    CHECK_FALSE(fixture.channel.terminal_error());
}
TEST_CASE("A completed source can acquire and settle without any presented mailbox", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::receive_workspace_record;
    using mmltk::testsupport::send_workspace_record;
    WorkspaceChannelFixture fixture;
    const presentation::WorkspaceSurfaceImportId arena{41U, 42U}, older{51U, 52U}, newer{61U, 62U};
    const auto layout = fixture.AdmitArena(arena, 1U, GENERATE(false, true));
    fixture.AdmitSource(older, arena, layout, 2U);
    fixture.AdmitSource(newer, arena, layout, 3U);
    for (const auto id : {newer, older}) {
        auto acquired = WorkspaceChannelFixture::Acquisition(id);
        REQUIRE(send_workspace_record(fixture.peer.get(), acquired));
        fixture.channel.pump();
        REQUIRE(fixture.channel.take_source_transition());
        CHECK_FALSE(fixture.channel.read_settled(id, {7U, 8U}, 9U, 1U));
        fixture.ReleaseRead(acquired);
    }
    REQUIRE(send_workspace_record(fixture.peer.get(), WorkspaceChannelFixture::Sample(arena)));
    fixture.channel.pump();
    fixture.Withdraw(older);
    fixture.Retire(older, 2U);
    fixture.Withdraw(newer);
    fixture.Retire(newer, 3U);
    CHECK(fixture.channel.claimable(arena));
    CHECK_FALSE(fixture.channel.terminal_error());
}
TEST_CASE("A blocked acquisition notification retains exact settlement after a racing Drop", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::receive_workspace_record;
    using mmltk::testsupport::send_workspace_record;
    const bool direct = GENERATE(false, true);
    const unsigned terminal = GENERATE(0U, 1U, 2U);
    CAPTURE(direct, terminal);
    WorkspaceChannelFixture fixture;
    auto& channel = fixture.channel;
    auto& peer = fixture.peer;
    const presentation::WorkspaceSurfaceImportId arena{1U, 2U};
    const presentation::WorkspaceSurfaceImportId source{3U, 4U};
    const auto layout = fixture.AdmitArena(arena, 1U, direct);
    fixture.AdmitSource(source, arena, layout, 2U);
    const int send_bytes = 4096;
    REQUIRE(::setsockopt(peer.get(), SOL_SOCKET, SO_SNDBUF, &send_bytes, sizeof(send_bytes)) == 0);
    const abi::Record filler{.opcode = abi::Opcode::Drop, .id_high = 101U, .id_low = 102U};
    std::size_t fillers = 0U;
    while (fillers < 256U && send_workspace_record(peer.get(), filler)) ++fillers;
    REQUIRE(fillers > 0U);
    REQUIRE(fillers < 256U);
    const auto acquired = WorkspaceChannelFixture::Acquisition(source);
    auto submitted_release = acquired;
    submitted_release.opcode = abi::Opcode::ReleaseSubmitted;
    std::barrier blocked{2};
    std::barrier drop_seen{2};
    std::barrier writable{2};
    bool backpressured = false;
    bool release_backpressured = false;
    bool dropped = false;
    bool notified = false;
    std::exception_ptr failure;
    std::jthread browser([&] {
        try {
            backpressured = !send_workspace_record(peer.get(), acquired);
            release_backpressured = !send_workspace_record(peer.get(), submitted_release);
        } catch (...) { failure = std::current_exception(); }
        blocked.arrive_and_wait();
        try {
            abi::Record received{};
            static_cast<void>(receive_workspace_record(peer.get(), received));
            dropped = received.opcode == abi::Opcode::Drop && received.id_high == source.high && received.id_low == source.low;
        } catch (...) { failure = std::current_exception(); }
        drop_seen.arrive_and_wait();
        writable.arrive_and_wait();
        try {
            if (terminal == 1U)
                peer.reset();
            else {
                notified = send_workspace_record(peer.get(), acquired);
                if (terminal == 2U) {
                    peer.reset();
                } else {
                    notified = notified && send_workspace_record(peer.get(), submitted_release);
                }
            }
        } catch (...) { failure = std::current_exception(); }
    });
    blocked.arrive_and_wait();
    const auto withdrawal = channel.withdraw(source);
    CHECK(withdrawal.progress == presentation::WorkspaceSurfaceWithdrawalProgress::Submitted);
    CHECK_FALSE(channel.claimable(source));
    drop_seen.arrive_and_wait();
    CHECK(backpressured);
    CHECK(release_backpressured);
    CHECK(dropped);
    // Remove transport fillers directly; they are deliberately not protocol
    // messages delivered to the native channel. Drop already crossed in the
    // opposite direction while the real Acquired datagram could not be sent.
    for (std::size_t index = 0U; index < fillers; ++index) {
        abi::Record received{};
        static_cast<void>(receive_workspace_record(channel.poll_fd(), received));
        CHECK(received.opcode == filler.opcode);
    }
    writable.arrive_and_wait();
    browser.join();
    if (failure) std::rethrow_exception(failure);
    channel.pump();
    if (terminal) {
        CHECK_FALSE(channel.connected());
        if (const auto observed = channel.take_source_transition()) {
            CHECK(terminal == 2U);
            CHECK(observed->opcode == abi::Opcode::Acquired);
        }
        CHECK_FALSE(channel.take_source_transition());
        CHECK_FALSE(channel.read_settled(source, {7U, 8U}, 9U, 1U));
        CHECK_FALSE(channel.take_retirement());
        return;
    }
    REQUIRE(notified);
    const auto acquisition = channel.take_source_transition();
    REQUIRE(acquisition.has_value());
    CHECK(acquisition->opcode == abi::Opcode::Acquired);
    CHECK_FALSE(channel.read_settled(source, {7U, 8U}, 9U, 1U));
    const auto release = channel.take_source_transition();
    REQUIRE(release.has_value());
    CHECK(release->opcode == abi::Opcode::ReleaseSubmitted);
    CHECK(release->offset == acquired.offset);
    CHECK_FALSE(channel.take_source_transition());
    CHECK_FALSE(channel.claimable(source));
    CHECK_FALSE(channel.take_retirement());
    CHECK_FALSE(channel.read_settled(source, {7U, 99U}, 9U, 1U));
    CHECK_FALSE(channel.read_settled(source, {7U, 8U}, 10U, 1U));
    CHECK_FALSE(channel.read_settled(source, {7U, 8U}, 9U, 2U));
    REQUIRE(channel.read_settled(source, {7U, 8U}, 9U, 1U));
    CHECK_FALSE(channel.read_settled(source, {7U, 8U}, 9U, 1U));
    abi::Record settled{};
    static_cast<void>(receive_workspace_record(peer.get(), settled));
    CHECK(settled.opcode == abi::Opcode::ReadSettled);
    CHECK(settled.stride == acquired.stride);
    CHECK(settled.size == acquired.size);
    CHECK(settled.presentation_revision == acquired.presentation_revision);
    CHECK(settled.offset == acquired.offset);
    fixture.Retire(source, 2U);
    CHECK_FALSE(channel.read_settled(source, {7U, 8U}, 9U, 1U));
    CHECK_FALSE(channel.take_retirement());
    CHECK_FALSE(channel.terminal_error());
}
TEST_CASE("Source admission rejects duplicate acquisition and retirement before exact settlement", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::send_workspace_record;
    const bool premature_retirement = GENERATE(false, true);
    WorkspaceChannelFixture fixture;
    const presentation::WorkspaceSurfaceImportId arena{1U, 2U};
    const presentation::WorkspaceSurfaceImportId source{3U, 4U};
    fixture.AdmitSource(source, arena, fixture.AdmitArena(arena, 1U), 2U);
    const auto acquired = WorkspaceChannelFixture::Acquisition(source);
    REQUIRE(send_workspace_record(fixture.peer.get(), acquired));
    fixture.channel.pump();
    REQUIRE(fixture.channel.take_source_transition().has_value());
    if (premature_retirement) {
        fixture.Withdraw(source);
        REQUIRE(send_workspace_record(fixture.peer.get(), {.opcode = abi::Opcode::Retired, .id_high = source.high, .id_low = source.low}));
    } else {
        REQUIRE(send_workspace_record(fixture.peer.get(), acquired));
    }
    fixture.channel.pump();
    CHECK(fixture.channel.terminal_error().has_value());
    CHECK_FALSE(fixture.channel.take_retirement());
}
TEST_CASE("Source release submission requires one exact acquired transfer", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::send_workspace_record;
    const unsigned invalid = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U);
    CAPTURE(invalid);
    WorkspaceChannelFixture fixture;
    const presentation::WorkspaceSurfaceImportId arena{1U, 2U};
    const presentation::WorkspaceSurfaceImportId source{3U, 4U};
    fixture.AdmitSource(source, arena, fixture.AdmitArena(arena, 1U), 2U);
    const auto acquired = WorkspaceChannelFixture::Acquisition(source);
    auto release = acquired;
    release.opcode = abi::Opcode::ReleaseSubmitted;
    if (invalid != 0U) {
        REQUIRE(send_workspace_record(fixture.peer.get(), acquired));
        fixture.channel.pump();
        REQUIRE(fixture.channel.take_source_transition().has_value());
    }
    switch (invalid) {
        case 0U: break;  // Release before any acquisition.
        case 1U:
            REQUIRE(send_workspace_record(fixture.peer.get(), release));
            fixture.channel.pump();
            REQUIRE(fixture.channel.take_source_transition().has_value());
            break;  // Duplicate release.
        case 2U: ++release.offset; break;
        case 3U: ++release.presentation_revision; break;
        case 4U: ++release.stride; break;
        case 5U: ++release.size; break;
        case 6U: ++release.id_low; break;
        default: FAIL("unknown invalid source release case");
    }
    REQUIRE(send_workspace_record(fixture.peer.get(), release));
    fixture.channel.pump();
    CHECK(fixture.channel.terminal_error().has_value());
    CHECK_FALSE(fixture.channel.take_source_transition());
    CHECK_FALSE(fixture.channel.read_settled(source, {7U, 8U}, 9U, 1U));
    CHECK_FALSE(fixture.channel.take_retirement());
}
TEST_CASE("A release receipt from an earlier transfer cannot settle the next acquisition", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::send_workspace_record;
    WorkspaceChannelFixture fixture;
    const presentation::WorkspaceSurfaceImportId arena{1U, 2U};
    const presentation::WorkspaceSurfaceImportId source{3U, 4U};
    fixture.AdmitSource(source, arena, fixture.AdmitArena(arena, 1U), 2U);
    auto acquired = WorkspaceChannelFixture::Acquisition(source);
    auto release = acquired;
    release.opcode = abi::Opcode::ReleaseSubmitted;
    REQUIRE(send_workspace_record(fixture.peer.get(), acquired));
    REQUIRE(send_workspace_record(fixture.peer.get(), release));
    fixture.channel.pump();
    const auto first = fixture.channel.take_source_transition();
    const auto second = fixture.channel.take_source_transition();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->opcode == abi::Opcode::Acquired);
    CHECK(second->opcode == abi::Opcode::ReleaseSubmitted);
    REQUIRE(fixture.channel.read_settled(source, {7U, 8U}, 9U, 1U));
    abi::Record settled{};
    static_cast<void>(mmltk::testsupport::receive_workspace_record(fixture.peer.get(), settled));
    ++acquired.offset;
    REQUIRE(send_workspace_record(fixture.peer.get(), acquired));
    fixture.channel.pump();
    REQUIRE(fixture.channel.take_source_transition().has_value());
    REQUIRE(send_workspace_record(fixture.peer.get(), release));
    fixture.channel.pump();
    CHECK(fixture.channel.terminal_error().has_value());
    CHECK_FALSE(fixture.channel.read_settled(source, {7U, 8U}, 9U, 2U));
}
TEST_CASE("Arena capacity tickets remain exact until consumed or withdrawn", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::send_workspace_record;
    WorkspaceChannelFixture fixture;
    auto& channel = fixture.channel;
    auto& peer = fixture.peer;
    const presentation::WorkspaceSurfaceImportId arena{1U, 2U};
    fixture.AdmitArena(arena, arena.high);
    auto sample = WorkspaceChannelFixture::Sample(arena);
    REQUIRE(send_workspace_record(peer.get(), sample));
    sample.opcode = abi::Opcode::Completed;
    REQUIRE(send_workspace_record(peer.get(), sample));
    auto available = sample;
    available.opcode = abi::Opcode::Available;
    REQUIRE(send_workspace_record(peer.get(), available));
    channel.pump();
    SECTION("one original availability survives unrelated outcomes and retirement") {
        // Model an ineligible consumer by leaving the ticket untouched. This
        // proves channel retention, not the native writer's GPU interleaving.
        for (std::uint64_t iteration = 2U; iteration <= 301U; ++iteration) {
            const presentation::WorkspaceSurfaceImportId unrelated{iteration, 3U};
            fixture.AdmitArena(unrelated, unrelated.high);
            fixture.Withdraw(unrelated);
            fixture.Retire(unrelated, unrelated.high);
            channel.pump();
            REQUIRE_FALSE(channel.terminal_error());
        }
        const auto ticket = channel.take_capacity_wake();
        REQUIRE(ticket.has_value());
        CHECK(*ticket == arena);
    }
    SECTION("repeated availability coalesces into one exact ticket") {
        for (std::uint64_t iteration = 0U; iteration < 300U; ++iteration) {
            REQUIRE(send_workspace_record(peer.get(), available));
            channel.pump();
            REQUIRE_FALSE(channel.terminal_error());
        }
        const auto ticket = channel.take_capacity_wake();
        REQUIRE(ticket.has_value());
        CHECK(*ticket == arena);
    }
    SECTION("withdrawal clears its own ticket and ignores queued stale availability") {
        fixture.Withdraw(arena);
        REQUIRE(send_workspace_record(peer.get(), available));
        fixture.Retire(arena, arena.high);
    }
    SECTION("a replacement outcome cannot relabel a stale arena ticket") {
        const presentation::WorkspaceSurfaceImportId replacement{2U, 3U};
        fixture.AdmitArena(replacement, replacement.high);
        const auto ticket = channel.take_capacity_wake();
        REQUIRE(ticket.has_value());
        CHECK(*ticket == arena);
        CHECK(*ticket != replacement);
    }
    SECTION("old arena withdrawal cannot erase the replacement ticket") {
        const presentation::WorkspaceSurfaceImportId replacement{2U, 3U};
        fixture.AdmitArena(replacement, replacement.high);
        auto replacement_available = available;
        replacement_available.id_high = replacement.high;
        replacement_available.id_low = replacement.low;
        REQUIRE(send_workspace_record(peer.get(), replacement_available));
        channel.pump();
        fixture.Withdraw(arena);
        REQUIRE(send_workspace_record(peer.get(), available));
        fixture.Retire(arena, arena.high);
        const auto ticket = channel.take_capacity_wake();
        REQUIRE(ticket.has_value());
        CHECK(*ticket == replacement);
    }
    CHECK_FALSE(channel.take_capacity_wake().has_value());
    CHECK_FALSE(channel.take_outcome().has_value());
    CHECK_FALSE(channel.take_retirement().has_value());
    CHECK_FALSE(channel.terminal_error());
    CHECK(channel.connected());
}
TEST_CASE("Held acquired-read completion retains page capacity in either arrival order", "[workspace][protocol]") {
    for (const bool capacity_first : {false, true}) {
        PresentationAcceptanceGate gate;
        std::vector<PresentationAcceptanceGate::Receipt> observed;
        unsigned wakes = 0U;
        gate.SetObserver([&](auto receipt) { observed.push_back(receipt); });
        gate.SetWake([&] { ++wakes; });
        REQUIRE(gate.Arm());
        if (capacity_first) gate.ObserveCapacity();
        REQUIRE(gate.Hold({.source_high = 1U, .source_low = 2U, .transfer = 3U, .publication = 4U}));
        if (!capacity_first) gate.ObserveCapacity();
        gate.ObserveCapacity();
        REQUIRE(observed.size() == 2U);
        CHECK(observed[0].boundary == PresentationAcceptanceGate::Boundary::Completion);
        CHECK(observed[1].boundary == PresentationAcceptanceGate::Boundary::Capacity);
        CHECK(observed[0].source_high == observed[1].source_high);
        CHECK(observed[0].source_low == observed[1].source_low);
        CHECK(observed[0].transfer == observed[1].transfer);
        CHECK(observed[0].publication == observed[1].publication);
        REQUIRE(gate.Release());
        CHECK(wakes == 1U);
        CHECK_FALSE(gate.Hold({.source_high = 8U}));
        CHECK_FALSE(gate.Release());
    }
}
TEST_CASE("Workspace capability ledgers survive sequential retirement beyond concurrent capacity", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::receive_workspace_record;
    using mmltk::testsupport::send_workspace_record;
    WorkspaceChannelFixture fixture;
    auto& channel = fixture.channel;
    auto& peer = fixture.peer;
    auto edge = mmltk::testsupport::workspace_surface_event_descriptor();
    auto signal = presentation::WorkspaceSurfaceFrameSignal::create();
    auto timeline = mmltk::testsupport::workspace_surface_event_descriptor();
    auto memory = mmltk::testsupport::workspace_surface_event_descriptor();
    const std::array descriptors{memory.get(), timeline.get()};
    for (std::uint64_t iteration = 1U; iteration <= 300U; ++iteration) {
        for (const bool source : {false, true}) {
            for (const bool ready : {false, true}) {
                const presentation::WorkspaceSurfaceImportId id{iteration, 1U + 2U * source + ready};
                auto layout = WorkspaceChannelFixture::Layout(id);
                if (source) {
                    layout.arena_high = iteration;
                    layout.arena_low = 9U;
                    layout.allocation_identity = iteration;
                    REQUIRE(channel.admit_source(layout, iteration, edge.get(), signal.descriptor(), edge.get()));
                } else {
                    REQUIRE(channel.admit_arena(id, iteration, 4U, 3U));
                }
                abi::Record received{};
                static_cast<void>(receive_workspace_record(peer.get(), received));
                if (ready) {
                    if (source) {
                        received.opcode = abi::Opcode::Ready;
                        received.descriptors = abi::kReadyDescriptorCount;
                        REQUIRE(send_workspace_record(peer.get(), received, descriptors));
                    } else {
                        REQUIRE(send_workspace_record(peer.get(), layout));
                    }
                    channel.pump();
                    REQUIRE(channel.take_outcome().has_value());
                }
                fixture.Withdraw(id);
                REQUIRE(send_workspace_record(peer.get(), {.opcode = ready ? abi::Opcode::Retired : abi::Opcode::Failed,
                                                           .id_high = id.high,
                                                           .id_low = id.low,
                                                           .code = ready ? 0U : static_cast<std::uint32_t>(abi::FailureCode::NotAdmitted)}));
                channel.pump();
                REQUIRE(channel.take_retirement().has_value());
                CHECK_FALSE(channel.claimable(id));
                CHECK(channel.withdraw(id).progress == presentation::WorkspaceSurfaceWithdrawalProgress::Invalid);
                CHECK_FALSE(channel.take_outcome().has_value());
                CHECK_FALSE(channel.terminal_error());
                CHECK(channel.connected());
            }
        }
    }
    // A still-live admission remains unique even after all the preceding churn.
    const presentation::WorkspaceSurfaceImportId active{301U, 1U};
    REQUIRE(channel.admit_arena(active, 301U, 4U, 3U));
    CHECK_FALSE(channel.admit_arena(active, 301U, 4U, 3U));
    CHECK(channel.terminal_error().has_value());
}
void publish_first_presentation(PresentationSystem& presentation, const PresentationSourceFixture& source, const TestPresentationWriterState& writer,
                                EventGate& events) {
    static_cast<void>(presentation.Select(source.identity()));
    writer.SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 1U; }));
    CHECK(presentation.snapshot().completed_source_revision == 1U);
}
TEST_CASE("Published frames retain their physical allocation while a waiting candidate is advertised") {
    PresentationSourceFixture source;
    EventGate events;
    auto writer = std::make_shared<TestPresentationWriterState>();
    writer->advertise_waiting_candidate.store(true);
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer), source.sources(),
                                    [&events](PresentationSystem::event_type) { events.Advance(); }};
    PresentationScenario scenario{presentation, writer};
    publish_first_presentation(presentation, source, *writer, events);
    const auto completed = presentation.snapshot();
    CHECK(completed.capability.surface_low == 1U);
    CHECK(completed.capability.generation == 1U);
    CHECK(completed.capability.condition == PresentationCapabilityCondition::Ready);
    writer->allow_publication.store(false);
    source.Advance();
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(events.Wait([&] { return presentation.snapshot().capability.surface_low == 101U; }));
    CHECK(presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted);
    CHECK(presentation.snapshot().completed == completed.completed);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    REQUIRE(presentation.Shutdown() == PresentationShutdownResult::Stopped);
}
TEST_CASE("Presentation diagnostics join admitted and completed snapshots to one surface identity") {
    PresentationSourceFixture source;
    EventGate events;
    struct Capture final {
        std::mutex mutex;
        std::vector<VisualDiagnosticFact> facts;
    } capture;
    capture.facts.reserve(16U);
    const VisualDiagnosticSink diagnostics{
        .context = &capture, .write = [](void* context, const VisualDiagnosticFact fact) noexcept {
            if (fact.operation != VisualDiagnosticOperation::PresentationCapabilityPublished && fact.operation != VisualDiagnosticOperation::TimelineReady)
                return;
            auto& result = *static_cast<Capture*>(context);
            std::scoped_lock lock(result.mutex);
            result.facts.push_back(fact);
        }};
    auto writer = std::make_shared<TestPresentationWriterState>();
    writer->allow_publication.store(false, std::memory_order_release);
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer), source.sources(),
                                    [&events](PresentationSystem::event_type) { events.Advance(); }, diagnostics};
    PresentationScenario scenario{presentation, writer};
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(events.Wait([&] { return presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted; }));
    const auto admitted = presentation.snapshot().capability;
    writer->allow_publication.store(true, std::memory_order_release);
    writer->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 1U; }));
    const auto completed = presentation.snapshot();
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    REQUIRE(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    REQUIRE_FALSE(capture.facts.empty());
    for (const auto& fact : capture.facts) {
        CHECK(fact.context.surface_high == admitted.surface_high);
        CHECK(fact.context.surface_low == admitted.surface_low);
        CHECK(fact.generation == admitted.generation);
        CHECK(fact.context.selection_generation != 0U);
        CHECK(fact.context.frame_revision == completed.completed.revision);
    }
}
TEST_CASE("Presentation directly refreshes a selected private product") {
    PresentationSourceFixture source;
    EventGate events;
    std::atomic<std::uint64_t> completed_events{0U};
    std::atomic<std::uint64_t> last_completed_revision{0U};
    std::atomic<std::uint64_t> last_completed_product{0U};
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer_state), source.sources(),
                                    [&events, &completed_events, &last_completed_revision, &last_completed_product](PresentationSystem::event_type event) {
                                        if (const auto* completed = std::get_if<PresentationCompleted>(&event)) {
                                            last_completed_revision.store(completed->snapshot.revision, std::memory_order_release);
                                            completed_events.fetch_add(1U, std::memory_order_acq_rel);
                                            last_completed_product.store(completed->snapshot.completed.revision, std::memory_order_release);
                                        }
                                        events.Advance();
                                    }};
    PresentationScenario scenario{presentation, writer_state};
    publish_first_presentation(presentation, source, *writer_state, events);
    REQUIRE(events.Wait([&] { return last_completed_product.load(std::memory_order_acquire) == 1U; }));
    source.Advance();
    presentation.SourceChanged(source.identity());
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 2U; }));
    CHECK(presentation.snapshot().completed_source_revision == 2U);
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 2U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 2U);
    REQUIRE(events.Wait([&] { return last_completed_product.load(std::memory_order_acquire) == 2U; }));
    writer_state->allow_publication.store(false, std::memory_order_release);
    source.Advance();
    presentation.SourceChanged(source.identity());
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted; }));
    // A pending refresh can outlive its wake while the submitted frame waits.
    // The next producer notification must wake that submitted work as well.
    const auto before_coalesced_refresh = writer_state->PumpCount();
    presentation.SourceChanged(source.identity());
    REQUIRE(writer_state->WaitForPumpAfter(before_coalesced_refresh));
    source.Advance();
    writer_state->allow_publication.store(true, std::memory_order_release);
    presentation.SourceChanged(source.identity());
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 4U; }));
    CHECK(presentation.snapshot().completed_source_revision == 4U);
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 4U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 3U);
    presentation.SetApplicationPeerConnected(false);
    source.Advance();
    presentation.SourceChanged(source.identity());
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 5U; }));
    presentation.SetApplicationPeerConnected(true);
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 5U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 4U);
    writer_state->allow_publication.store(false, std::memory_order_release);
    source.Advance();
    presentation.SourceChanged(source.identity());
    REQUIRE(events.Wait([&] { return presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted; }));
    presentation.SetApplicationPeerConnected(false);
    writer_state->allow_publication.store(true, std::memory_order_release);
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 6U; }));
    presentation.SetApplicationPeerConnected(true);
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 6U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 5U);
    // A producer can retire its old storage before newer metadata arrives.
    // The stale advertised frame must not resubmit itself on every pump.
    source.PublishUnobserved();
    const auto before_rejected_borrow = writer_state->PumpCount();
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(writer_state->WaitForPumpAfter(before_rejected_borrow));
    const auto rejected_borrow = writer_state->PumpCount();
    writer_state->SignalReadiness();
    REQUIRE(writer_state->WaitForPumpAfter(rejected_borrow));
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 7U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 5U);
    CHECK(presentation.snapshot().completed.revision == 6U);
    source.Advance();
    presentation.SourceChanged(source.identity());
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 8U; }));
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 8U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 6U);
    // If the newer observation is already available at rejection, catch up
    // immediately even before its separate source notification is delivered.
    writer_state->allow_publication.store(false, std::memory_order_release);
    source.Advance();
    presentation.SourceChanged(source.identity());
    REQUIRE(events.Wait([&] { return presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted; }));
    source.Advance();
    writer_state->allow_publication.store(true, std::memory_order_release);
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 10U; }));
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 10U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 7U);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
}
TEST_CASE("Presentation republishes an unchanged completed product after scoped binding retirement") {
    PresentationSourceFixture source;
    EventGate events;
    // CLEANUP-IGNORE: This publication case starts admitted; the metadata case deliberately blocks publication before selecting its source.
    auto writer = std::make_shared<TestPresentationWriterState>();
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer), source.sources(),
                                    [&events](PresentationSystem::event_type) { events.Advance(); }};
    PresentationScenario scenario{presentation, writer};
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 1U; }));
    const auto prior = presentation.snapshot();
    writer->retire_binding.store(true, std::memory_order_release);
    writer->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().presentation_revision > prior.presentation_revision; }));
    CHECK(presentation.snapshot().completed == prior.completed);
    CHECK(presentation.snapshot().selected == prior.selected);
}
TEST_CASE("Presentation carries its submitted observation through metadata changes and reselection") {
    PresentationSourceFixture source;
    EventGate events;
    auto writer = std::make_shared<TestPresentationWriterState>();
    writer->allow_publication.store(false);
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer), source.sources(),
                                    [&events](PresentationSystem::event_type) { events.Advance(); }};
    PresentationScenario scenario{presentation, writer};
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(events.Wait([&] { return presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted; }));
    source.AdvanceObservation();
    writer->allow_publication.store(true);
    writer->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 1U; }));
    const auto completed = presentation.snapshot();
    CHECK(completed.completed_source_revision == 1U);
    CHECK(source.sources().front().observe().snapshot_revision == 2U);
    static_cast<void>(presentation.Select(source.identity()));
    writer->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().presentation_revision > completed.presentation_revision; }));
    CHECK(presentation.snapshot().completed == completed.completed);
    CHECK(presentation.snapshot().completed_source_revision == 2U);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
}
TEST_CASE("Presentation diagnostics retain exact observations across supersession cache reuse reconnect and reconstruction") {
    PresentationSourceFixture source{2U};
    EventGate events;
    struct Capture final {
        std::array<VisualDiagnosticFact, 6U> completed{};
        std::size_t count = 0U;
    } capture;
    const VisualDiagnosticSink diagnostics{.context = &capture, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
                                               auto& output = *static_cast<Capture*>(context);
                                               if (fact.operation == VisualDiagnosticOperation::TimelineReady && output.count < output.completed.size())
                                                   output.completed[output.count++] = fact;
                                           }};
    auto writer = std::make_shared<TestPresentationWriterState>();
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer), source.sources(),
                                    [&events](PresentationSystem::event_type) { events.Advance(); }, diagnostics};
    PresentationScenario scenario{presentation, writer};
    const auto await_publication = [&](std::uint64_t revision) {
        writer->SignalReadiness();
        REQUIRE(events.Wait([&] { return presentation.snapshot().presentation_revision == revision; }));
    };
    auto first = source.Completed();
    static_cast<void>(presentation.Select(source.identity()));
    await_publication(1U);
    writer->allow_publication.store(false);
    source.Advance();
    auto second = source.Completed();
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(writer->second_submission, "writer->second_submission", 2s));
    source.SelectCompleted(first);
    static_cast<void>(presentation.Select(source.identity()));
    writer->allow_publication.store(true);
    await_publication(2U);
    source.SelectCompleted(second);
    static_cast<void>(presentation.Select(source.identity()));
    await_publication(3U);
    source.SelectCompleted(first);
    static_cast<void>(presentation.Select(source.identity()));
    await_publication(4U);
    static_cast<void>(presentation.Select(source.identity()));  // Reconnected renderer requests a new physical receipt.
    await_publication(5U);
    first = {};
    second = {};
    source.Reconstruct();
    static_cast<void>(presentation.Select(source.identity()));
    await_publication(6U);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    REQUIRE(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    REQUIRE(capture.count == 6U);
    constexpr std::array products{1U, 1U, 2U, 1U, 1U, 3U};
    constexpr std::array observations{1U, 3U, 4U, 5U, 5U, 6U};
    for (std::size_t index = 0U; index < capture.count; ++index) {
        const auto& fact = capture.completed[index];
        CHECK(fact.context.frame_revision == products[index]);
        CHECK(fact.context.source.source_revision == products[index]);
        CHECK(fact.context.source.source_observation_revision == observations[index]);
        CHECK(fact.context.source.clean_revision == products[index]);
        CHECK(fact.context.publication.presentation_revision == index + 1U);
        CHECK(fact.context.transfer.transfer_sequence == index + 1U);
        CHECK(fact.context.transfer.timeline_ready == fact.value);
        CHECK(fact.context.allocation.allocation_generation == 1U);
        CHECK(fact.context.surface_high == capture.completed[0U].context.surface_high);
        CHECK(fact.context.surface_low == capture.completed[0U].context.surface_low);
    }
    CHECK(capture.completed[4U].context.selection_generation > capture.completed[3U].context.selection_generation);
}
TEST_CASE("Presentation operation projection never combines a new borrow with an incumbent publication") {
    const PresentationCapability allocation{
        .surface_high = 10U, .surface_low = 11U, .extent = {32U, 32U}, .generation = 7U, .condition = PresentationCapabilityCondition::Ready};
    const PresentationSubmittedSource first{
        {.frame = {.source = {PresentationSourceKind::Explore, 1U}, .extent = {16U, 16U}, .revision = 9U, .clean_revision = 5U}, .snapshot_revision = 20U},
        30U};
    const PresentationSubmittedSource second{
        {.frame = {.source = {PresentationSourceKind::Explore, 1U}, .extent = {16U, 16U}, .revision = 12U, .clean_revision = 8U}, .snapshot_revision = 21U},
        31U};
    const PresentationDiagnosticRecord incumbent{first,
                                                 {.capability = allocation, .timeline_ready = 17U, .presentation_revision = 40U, .transfer_sequence = 9U}};
    const auto released = presentation_diagnostic_fact(VisualDiagnosticOperation::PresentationReleaseWaitCompleted, incumbent, 0, 1U);
    CHECK(released.context.selection_generation == first.selection_generation);
    CHECK(released.context.frame_revision == released.context.source.source_revision);
    CHECK(released.context.source.source_revision == 9U);
    CHECK(released.value == released.context.publication.presentation_revision);
    CHECK(released.value == 40U);
    CHECK(released.context.transfer.transfer_sequence == 9U);
    const auto borrowed = presentation_diagnostic_fact(VisualDiagnosticOperation::PresentationSourceBorrowStarted, {second, {.capability = allocation}}, 0);
    CHECK(borrowed.context.selection_generation == second.selection_generation);
    CHECK(borrowed.context.frame_revision == borrowed.context.source.source_revision);
    CHECK(borrowed.context.source.source_revision == 12U);
    CHECK(borrowed.value == 0U);
    CHECK(borrowed.context.publication.presentation_revision == 0U);
    CHECK(borrowed.context.transfer.transfer_sequence == 0U);
    CHECK(borrowed.context.transfer.timeline_ready == 0U);
}
TEST_CASE("Native retirement and real detach requests retain one durable cleanup responsibility", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace fixture = gpu::test_support;
    // 0: parked behind raw access; 1: copying before Detach; 2: detached, with
    // the executing request still strong at its completed-service boundary.
    const unsigned ordering = GENERATE(0U, 1U, 2U);
    const bool fail_release = GENERATE(false, true);
    fixture::ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    auto workspace = mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::CreateAdmitted(
        backend, mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::Layout(0));
    auto observation = workspace->ObserveRetirement();
    std::exception_ptr producer_failure;
    unsigned failure_reports = 0U;
    detail::VisualRuntimeOwner owner{
        [backend](auto revisions) {
            return std::make_unique<gpu::SystemImageRuntime>(mmltk::frameworks::gpu::test_support::WorkspaceRuntimeConfig(backend, std::move(revisions)));
        },
        [&](std::exception_ptr failure) {
            producer_failure = failure;
            ++failure_reports;
        }};
    std::promise<void> initialized;
    bool prepared = false;
    REQUIRE(owner.SubmitOrdered([&](auto& runtime, std::stop_token) {
        runtime.Publish(4U, 3U, [](auto, auto, auto) {});
        prepared = runtime.PrepareDisplay(runtime.Completed().revision(), workspace);
        runtime.Publish(4U, 3U, [](auto, auto, auto) {});
        return detail::VisualRuntimeOwner::Notification{[&] { initialized.set_value(); }};
    }));
    mmltk::testsupport::await_test_promise(initialized, "direct workspace initialized");
    REQUIRE(prepared);
    auto copy_gate = backend->HoldSameDeviceCopies("active detach copy");
    mmltk::testsupport::TestGate completed_service{"detached request retains its strong attempt"};
    auto raw = ordering == 0U ? owner.Borrow() : gpu::BorrowedImageProductReadView{};
    mmltk::testsupport::ScopedTestCleanup settle_owner{[&] {
        raw = {};
        copy_gate->Release();
        completed_service.Release();
        owner.StopAndWait();
    }};
    auto request_diagnostics = std::make_shared<VisualWorkspaceDiagnostics>();
    if (ordering == 2U)
        request_diagnostics->sink = {
            .context = &completed_service, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
                if (fact.operation == VisualDiagnosticOperation::PresentationWorkspaceService && fact.failure_detail == "prepare_completed")
                    static_cast<mmltk::testsupport::TestGate*>(context)->receipt().ArriveAndWait();
            }};
    std::atomic<unsigned> responses{0U};
    std::promise<void> ready;
    owner.RequestWorkspace({.detach_only = true,
                            .destination = workspace,
                            .ready =
                                [&] {
                                    ++responses;
                                    ready.set_value();
                                },
                            .diagnostics = request_diagnostics});
    if (ordering == 0U) {
        std::promise<void> parked;
        REQUIRE(owner.SubmitOrdered([&](auto&, std::stop_token) { return detail::VisualRuntimeOwner::Notification{[&] { parked.set_value(); }}; }));
        mmltk::testsupport::await_test_promise(parked, "detach parked behind raw read");
    } else {
        REQUIRE(copy_gate->WaitEntered(2s));
        if (ordering == 2U) {
            copy_gate->Release();
            REQUIRE(completed_service.WaitEntered(2s));
            CHECK(workspace->product_owner() == 0U);
        }
    }
    CHECK(responses.load() == 0U);
    std::promise<void> native_releasing;
    struct NativeFacts {
        std::promise<void>* releasing;
        std::uint64_t outcome = 9U;
    } facts{&native_releasing};
    VisualDiagnosticSink diagnostics{.context = &facts, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
                                         auto& observed = *static_cast<NativeFacts*>(context);
                                         if (fact.operation == VisualDiagnosticOperation::PresentationSourceWorkspaceReleaseStarted)
                                             observed.releasing->set_value();
                                         if (fact.operation == VisualDiagnosticOperation::PresentationSourceWorkspaceReleaseCompleted)
                                             observed.outcome = fact.context.outcome;
                                     }};
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-native-detach-handoff"};
    auto writer = test_support::NativePresentationWriterTestAccess::Create(gpu::DeviceContext(0, backend), {.import_socket = temporary.path() / "import.sock"},
                                                                           std::move(workspace), diagnostics);
    auto terminal = std::async(std::launch::async, [&] { return writer->BrowserPeerLost(); });
    mmltk::testsupport::ScopedTestCleanup settle_terminal{[&] {
        raw = {};
        copy_gate->Release();
        completed_service.Release();
    }};
    mmltk::testsupport::await_test_promise(native_releasing, "native source releases its reference");
    if (ordering != 2U) {
        REQUIRE(terminal.wait_for(2s) == std::future_status::ready);
        CHECK(terminal.get() == PresentationNativeWriter::Retirement::Released);
        CHECK(facts.outcome == 2U);
    } else {
        CHECK_FALSE(observation.TakeResult().complete);
        CHECK_FALSE(observation.TransferToProducer());
    }
    // Only physical workspace cleanup fails. Producer stream and private raw
    // storage remain healthy after detachment.
    if (fail_release) fixture::ImportedImageBufferTestAccess::unmap_result = CUDA_ERROR_UNKNOWN;
    raw = {};
    copy_gate->Release();
    completed_service.Release();
    mmltk::testsupport::await_test_promise(ready, "detach request ready once");
    if (ordering == 2U) {
        REQUIRE(terminal.wait_for(2s) == std::future_status::ready);
        CHECK(terminal.get() == (fail_release ? PresentationNativeWriter::Retirement::ReleasedWithFailure : PresentationNativeWriter::Retirement::Released));
        CHECK(facts.outcome == (fail_release ? 0U : 1U));
    }
    owner.StopAndWait();
    CHECK(responses.load() == 1U);
    const auto released = observation.TakeResult();
    REQUIRE(released.complete);
    CHECK_FALSE(released.claimed);
    CHECK(bool(released.settlement.failure) == fail_release);
    CHECK(failure_reports == (fail_release && ordering != 2U ? 1U : 0U));
    if (fail_release && ordering != 2U) CHECK(fixture::ContainsImageFailure(producer_failure, released.settlement.failure));
}
TEST_CASE("Unretired adopted native sources release before their stream is destroyed", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace fixture = gpu::test_support;
    const bool completed_failure = GENERATE(false, true);
    const bool producer_attached = GENERATE(false, true);
    fixture::ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    auto workspace = mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::CreateAdmitted(
        backend, mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::Layout(1));
    auto observation = workspace->ObserveRetirement();
    std::unique_ptr<gpu::SystemImageRuntime> producer;
    if (producer_attached) {
        producer = std::make_unique<gpu::SystemImageRuntime>(
            gpu::SystemImageRuntimeConfig{.device = 0, .backend = backend, .workspace_finalize = [](auto clean, auto, auto destination, auto, auto) {
                                              fixture::CopyImagePlane(destination, clean);
                                          }});
        producer->Publish(4U, 3U, [](auto, auto, auto) {});
        REQUIRE(producer->PrepareDisplay(producer->Completed().revision(), workspace));
    }
    struct RollbackFacts {
        FakeImageBackend* backend;
        std::exception_ptr failure;
        unsigned releases = 0U;
        std::uint64_t outcome = 9U;
        bool released_before_stream = false;
        std::uintptr_t source_context = 0U;
        bool source_context_restored = false;
    } facts{backend.get(), completed_failure ? std::make_exception_ptr(std::runtime_error("completed source rollback failure")) : std::exception_ptr{}};
    VisualDiagnosticSink diagnostics{.context = &facts, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
                                         auto& observed = *static_cast<RollbackFacts*>(context);
                                         if (fact.operation == VisualDiagnosticOperation::PresentationSourceStreamSettlementStarted) {
                                             observed.source_context = observed.backend->last_bound_context.load();
                                             if (observed.failure)
                                                 observed.backend->FailAfter(FakeImageBackend::FailurePoint::SynchronizeStream, 0U, observed.failure);
                                         }
                                         if (fact.operation == VisualDiagnosticOperation::PresentationSourceWorkspaceReleaseCompleted) {
                                             ++observed.releases;
                                             observed.outcome = fact.context.outcome;
                                         }
                                         if (fact.operation == VisualDiagnosticOperation::PresentationSourceStreamDestructionStarted)
                                             observed.released_before_stream = observed.releases == 1U;
                                         if (fact.operation == VisualDiagnosticOperation::PresentationSourceProbeDeviceReleaseStarted)
                                             observed.source_context_restored = observed.backend->last_bound_context.load() == observed.source_context;
                                     }};
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-native-admission-rollback"};
    auto writer = test_support::NativePresentationWriterTestAccess::Create(gpu::DeviceContext(0, backend), {.import_socket = temporary.path() / "import.sock"},
                                                                           std::move(workspace), diagnostics);
    // Neither ordinary source removal nor terminal browser retirement ran.
    writer.reset();
    CHECK(facts.releases == 1U);
    CHECK(facts.released_before_stream);
    CHECK(facts.source_context_restored);
    CHECK(facts.outcome == (completed_failure ? 0U : producer_attached ? 2U : 1U));
    CHECK(observation.TakeResult().complete == !producer_attached);
    if (producer) {
        const auto retired = producer->Retire();
        CHECK(retired.safe_to_destroy);
        CHECK_FALSE(retired.failure);
        producer.reset();
    }
    const auto released = observation.TakeResult();
    CHECK(released.complete);
    CHECK_FALSE(released.claimed);
    CHECK(released.settlement.completion_reached);
    CHECK_FALSE(released.settlement.failure);
    CHECK(fixture::ImportedImageBufferTestAccess::unmaps == 1U);
    CHECK(fixture::ImportedImageBufferTestAccess::allocation_releases == 1U);
    CHECK(backend->planes_allocated == backend->planes_freed);
    CHECK(backend->events_created == backend->events_destroyed);
    CHECK(backend->contexts_created == backend->contexts_destroyed);
}
TEST_CASE("Native display-last retirement consumes physical cleanup and its diagnostic outcome", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace fixture = gpu::test_support;
    using Access = test_support::NativePresentationWriterTestAccess;
    const bool terminal = GENERATE(false, true);
    const bool fail_release = GENERATE(false, true);
    mmltk::frameworks::gpu::test_support::WorkspaceTestFixture resources{true};
    REQUIRE(resources.prepared);
    auto& backend = resources.backend;
    auto& producer = *resources.runtime;
    auto& workspace = resources.workspace;
    REQUIRE(producer.DetachDisplay(workspace));
    REQUIRE(producer.Retire().safe_to_destroy);
    auto observation = workspace->ObserveRetirement();
    struct Outcomes {
        std::uint64_t workspace = 9U;
        std::uint64_t source = 9U;
    } outcomes;
    VisualDiagnosticSink diagnostics{.context = &outcomes, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
                                         auto& result = *static_cast<Outcomes*>(context);
                                         if (fact.operation == VisualDiagnosticOperation::PresentationSourceWorkspaceReleaseCompleted)
                                             result.workspace = fact.context.outcome;
                                         if (fact.operation == VisualDiagnosticOperation::PresentationSourceRetirement) result.source = fact.context.outcome;
                                     }};
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-native-display-retirement"};
    auto writer = Access::Create(gpu::DeviceContext(0, backend), {.import_socket = temporary.path() / "import.sock"}, std::move(workspace), diagnostics);
    const auto cleanup = std::make_exception_ptr(std::runtime_error("display-last binding failure"));
    if (fail_release) backend->FailDeviceBinding(1, cleanup);
    if (terminal) {
        CHECK(writer->BrowserPeerLost() ==
              (fail_release ? PresentationNativeWriter::Retirement::ReleasedWithFailure : PresentationNativeWriter::Retirement::Released));
    } else {
        std::exception_ptr failure;
        try {
            Access::RetireSource(*writer);
        } catch (...) { failure = std::current_exception(); }
        CHECK(fixture::ContainsImageFailure(failure, cleanup) == fail_release);
        CHECK(writer->BrowserPeerLost() == PresentationNativeWriter::Retirement::Released);
    }
    const auto result = observation.TakeResult();
    REQUIRE(result.complete);
    CHECK_FALSE(result.claimed);
    CHECK(fixture::ContainsImageFailure(result.settlement.failure, cleanup) == fail_release);
    CHECK(outcomes.workspace == (fail_release ? 0U : 1U));
    CHECK(outcomes.source == (fail_release ? 0U : 1U));
    if (!fail_release) {
        CHECK(fixture::ImportedImageBufferTestAccess::unmaps == 1U);
        CHECK(fixture::ImportedImageBufferTestAccess::allocation_releases == 1U);
    }
}
TEST_CASE("Stopped visual owners finish released products without waiting for external readers", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    auto backend = std::make_shared<FakeImageBackend>();
    std::atomic<unsigned> failures{0U};
    detail::VisualRuntimeOwner owner{RuntimeFactory(0, backend), [&](std::exception_ptr) { ++failures; }};
    gpu::SystemImageRuntime::CompletedOutput retained;
    std::promise<void> published;
    REQUIRE(owner.SubmitOrdered([&](auto& runtime, std::stop_token) {
        runtime.Publish(4U, 3U, [](auto, auto, auto) {});
        retained = runtime.Completed();
        return detail::VisualRuntimeOwner::Notification{[&] { published.set_value(); }};
    }));
    mmltk::testsupport::await_test_promise(published, "retained product publication");
    auto reader = owner.Borrow();
    REQUIRE(reader.valid());
    owner.StopAndWait();
    owner.FinishStoppedRetirement();
    CHECK(backend->contexts_destroyed == 0U);
    retained = {};
    owner.FinishStoppedRetirement();
    CHECK(reader.valid());
    CHECK(backend->contexts_destroyed == 0U);
    reader = {};
    owner.FinishStoppedRetirement();
    CHECK(backend->planes_allocated == backend->planes_freed);
    CHECK(backend->contexts_created == backend->contexts_destroyed);
    CHECK(backend->events_created == backend->events_destroyed);
    CHECK(failures == 0U);
}
TEST_CASE("Native display release hands retained receiver cleanup to producer custody", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace fixture = gpu::test_support;
    const bool fail_release = GENERATE(false, true);
    mmltk::frameworks::gpu::test_support::WorkspaceTestFixture resources{true};
    REQUIRE(resources.prepared);
    auto& backend = resources.backend;
    auto& producer = *resources.runtime;
    auto& workspace = resources.workspace;
    auto completion = std::move(producer.BorrowWorkspace()).TakeCompletion();
    auto observation = workspace->ObserveRetirement();
    auto retirement = producer.Retire();
    REQUIRE(retirement.custody.deferred());
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-native-receiver-retirement"};
    auto writer = test_support::NativePresentationWriterTestAccess::Create(gpu::DeviceContext(0, backend), {.import_socket = temporary.path() / "import.sock"},
                                                                           std::move(workspace));
    CHECK(writer->BrowserPeerLost() == PresentationNativeWriter::Retirement::Released);
    CHECK_FALSE(observation.TakeResult().complete);
    CHECK(retirement.custody.deferred());
    std::atomic<unsigned> notifications{0U};
    retirement.custody.SetRetirementSink(std::make_shared<const std::function<void()>>([&] { ++notifications; }));
    const auto cleanup = std::make_exception_ptr(std::runtime_error("receiver-last binding failure"));
    if (fail_release) backend->FailDeviceBinding(1, cleanup);
    completion->Complete();
    CHECK_FALSE(observation.TakeResult().complete);
    completion.reset();
    CHECK(notifications.load() != 0U);
    const auto settled = retirement.custody.FinishRetirement();
    CHECK(settled.completion_reached == !fail_release);
    CHECK(fixture::ContainsImageFailure(settled.failure, cleanup) == fail_release);
    const auto result = observation.TakeResult();
    CHECK(result.complete);
    CHECK_FALSE(result.claimed);
    retirement.custody.SetRetirementSink({});
}
TEST_CASE("Native pending allocation release wakes retirement without treating a wake as completion", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace fixture = gpu::test_support;
    mmltk::frameworks::gpu::test_support::WorkspaceTestFixture resources;
    auto& backend = resources.backend;
    auto& attempt = resources.workspace;
    auto observation = attempt->ObserveRetirement();
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-native-pending-retirement"};
    auto writer =
        test_support::NativePresentationWriterTestAccess::Create(gpu::DeviceContext(0, backend), {.import_socket = temporary.path() / "import.sock"}, attempt);
    test_support::NativePresentationWriterTestAccess::RetireSource(*writer);
    CHECK_FALSE(observation.TakeResult().complete);
    // An unrelated writer wake cannot discharge the executing attempt's custody.
    test_support::NativePresentationWriterTestAccess::RetireSource(*writer);
    CHECK_FALSE(observation.TakeResult().complete);
    const auto cleanup = std::make_exception_ptr(std::runtime_error("pending allocation cleanup"));
    backend->FailDeviceBinding(1, cleanup);
    attempt.reset();
    std::exception_ptr failure;
    try {
        test_support::NativePresentationWriterTestAccess::RetireSource(*writer);
    } catch (...) { failure = std::current_exception(); }
    CHECK(fixture::ContainsImageFailure(failure, cleanup));
    CHECK(observation.TakeResult().complete);
    CHECK(writer->BrowserPeerLost() == PresentationNativeWriter::Retirement::Released);
}
TEST_CASE("Native terminal retirement observes independently completing detached allocation cleanup", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace fixture = gpu::test_support;
    mmltk::frameworks::gpu::test_support::WorkspaceTestFixture resources;
    auto& backend = resources.backend;
    auto& attempt = resources.workspace;
    auto observation = attempt->ObserveRetirement();
    std::promise<void> releasing;
    VisualDiagnosticSink diagnostics{.context = &releasing, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
                                         if (fact.operation == VisualDiagnosticOperation::PresentationSourceWorkspaceReleaseStarted)
                                             static_cast<std::promise<void>*>(context)->set_value();
                                     }};
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-native-terminal-pending"};
    auto writer = test_support::NativePresentationWriterTestAccess::Create(gpu::DeviceContext(0, backend), {.import_socket = temporary.path() / "import.sock"},
                                                                           attempt, diagnostics);
    auto terminal = std::async(std::launch::async, [&] { return writer->BrowserPeerLost(); });
    mmltk::testsupport::await_test_promise(releasing, "source release started");
    attempt.reset();
    REQUIRE(terminal.wait_for(2s) == std::future_status::ready);
    CHECK(terminal.get() == PresentationNativeWriter::Retirement::Released);
    CHECK(observation.TakeResult().complete);
}
TEST_CASE("Queued workspace requests release expired and displaced destinations without parking allocation custody", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace fixture = gpu::test_support;
    fixture::ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::promise<void> entered;
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<unsigned> failures{0U};
    detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures), [&](std::exception_ptr) { ++failures; }};
    auto settle = settle_visual_on_exit(owner, release);
    REQUIRE(owner.SubmitOrdered([&](auto&, std::stop_token) {
        entered.set_value();
        gate.wait();
        return detail::VisualRuntimeOwner::Notification{};
    }));
    mmltk::testsupport::await_test_promise(entered, "queued workspace gate");
    auto workspace = fixture::ImageWorkspaceTestAccess::Create(backend, fixture::ImageWorkspaceTestAccess::Layout());
    auto observation = workspace->ObserveRetirement();
    std::atomic<unsigned> displaced{0U};
    std::promise<void> expired;
    owner.RequestWorkspace({.detach_only = true, .destination = workspace, .ready = [&] { ++displaced; }});
    owner.RequestWorkspace({.detach_only = true, .destination = workspace, .ready = [&] { expired.set_value(); }});
    CHECK(displaced.load() == 1U);
    workspace.reset();
    CHECK(observation.TakeResult().complete);
    release.set_value();
    mmltk::testsupport::await_test_promise(expired, "expired workspace completion");
    owner.StopAndWait();
    std::atomic<unsigned> rejected{0U};
    auto rejected_workspace = fixture::ImageWorkspaceTestAccess::Create(backend, fixture::ImageWorkspaceTestAccess::Layout());
    owner.RequestWorkspace({.detach_only = true, .destination = rejected_workspace, .ready = [&] { ++rejected; }});
    CHECK(rejected.load() == 1U);
    CHECK(failures.load() == 0U);
}
TEST_CASE("Workspace retries cancellation and worker failure settle their ready response", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace fixture = gpu::test_support;
    const unsigned outcome = GENERATE(0U, 1U, 2U);
    fixture::ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    const auto injected = std::make_exception_ptr(std::runtime_error("workspace finalizer failure"));
    std::promise<void> failed;
    std::exception_ptr failure;
    detail::VisualRuntimeOwner owner{
        [=](auto revisions) {
            return std::make_unique<gpu::SystemImageRuntime>(gpu::SystemImageRuntimeConfig{.device = 0,
                                                                                           .backend = backend,
                                                                                           .workspace_finalize =
                                                                                               [=](auto clean, auto, auto destination, auto, auto) {
                                                                                                   if (outcome == 2U) std::rethrow_exception(injected);
                                                                                                   fixture::CopyImagePlane(destination, clean);
                                                                                               },
                                                                                           .product_revisions = std::move(revisions)});
        },
        [&](std::exception_ptr value) {
            failure = value;
            failed.set_value();
        }};
    std::promise<void> published;
    REQUIRE(owner.SubmitOrdered([&](auto& runtime, std::stop_token) {
        runtime.Publish(4U, 3U, [](auto, auto, auto) {});
        return detail::VisualRuntimeOwner::Notification{[&] { published.set_value(); }};
    }));
    mmltk::testsupport::await_test_promise(published, "workspace source publication");
    auto workspace = mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::CreateAdmitted(
        backend, mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::Layout(0));
    auto observation = workspace->ObserveRetirement();
    auto raw = outcome == 2U ? gpu::BorrowedImageProductReadView{} : owner.Borrow();
    const auto source = owner.ObserveWorkspace();
    std::atomic<unsigned> responses{0U};
    std::promise<void> ready;
    owner.RequestWorkspace({.product_owner = source.product_owner, .product_revision = source.product_revision, .destination = workspace, .ready = [&] {
                                ++responses;
                                ready.set_value();
                            }});
    if (outcome != 2U) {
        std::promise<void> attempted;
        std::promise<void> resume;
        const auto resumed = resume.get_future().share();
        auto cleanup = mmltk::testsupport::ScopedTestCleanup{[&] { mmltk::testsupport::release_test_promise(resume); }};
        REQUIRE(owner.SubmitOrdered([&](auto&, std::stop_token) {
            attempted.set_value();
            resumed.wait();
            return detail::VisualRuntimeOwner::Notification{};
        }));
        mmltk::testsupport::await_test_promise(attempted, "workspace retry parked");
        CHECK(responses.load() == 0U);
        workspace.reset();
        CHECK(observation.TakeResult().complete);
        resume.set_value();
        if (outcome == 1U) owner.StopAndWait();
        raw = {};
    }
    mmltk::testsupport::await_test_promise(ready, "workspace request settled");
    if (outcome == 2U) {
        mmltk::testsupport::await_test_promise(failed, "workspace worker failure");
        CHECK(fixture::ContainsImageFailure(failure, injected));
    }
    owner.StopAndWait();
    CHECK(responses.load() == 1U);
}
TEST_CASE("Presentation control remains available during a native wait") {
    PresentationSourceFixture source;
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    writer_state->block_pump.store(true, std::memory_order_release);
    auto pump_entered = writer_state->pump_entered.get_future();
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer_state), source.sources()};
    PresentationScenario scenario{presentation, writer_state};
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(pump_entered.wait_for(2s) == std::future_status::ready);
    pump_entered.get();
    auto admission_closed = std::async(std::launch::async, [&presentation] { presentation.CloseAdmission(); });
    mmltk::testsupport::ScopedTestCleanup release_wait{[&] {
        try {
            writer_state->release_pump.set_value();
        } catch (...) {}
    }};
    CHECK(admission_closed.wait_for(2s) == std::future_status::ready);
    writer_state->release_pump.set_value();
    mmltk::testsupport::await_test_future(admission_closed, "released Presentation admission close");
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
}
TEST_CASE("Presentation release failure publishes once and still stops") {
    PresentationSourceFixture source;
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    writer_state->terminal_retirement.store(PresentationNativeWriter::Retirement::ReleasedWithFailure, std::memory_order_release);
    PresentationFailureProbe failures;
    auto presentation = make_failure_presentation(source, writer_state, failures);
    PresentationScenario scenario{presentation, writer_state};
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(failures.events().Wait([&] { return presentation.snapshot().completed.valid(); }));
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    CHECK(presentation.stopped());
    CHECK(failures.failures() == 1U);
    CHECK(writer_state->browser_terminals.load(std::memory_order_acquire) == 1U);
    CHECK(writer_state->retirements.load(std::memory_order_acquire) == 1U);
}
TEST_CASE("Presentation retains an unprovable native source read through terminal shutdown") {
    PresentationSourceFixture source;
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    using Retirement = PresentationNativeWriter::Retirement;
    const auto outcome = GENERATE(Retirement::RetainedBrowserRead, Retirement::UnsafeFailure);
    writer_state->terminal_retirement.store(outcome, std::memory_order_release);
    PresentationFailureProbe failures;
    {
        auto presentation = make_failure_presentation(source, writer_state, failures);
        PresentationScenario scenario{presentation, writer_state};
        presentation.CloseAdmission();
        presentation.BrowserPeerLost();
        CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
        CHECK(failures.failures() == (outcome == Retirement::UnsafeFailure ? 1U : 0U));
        CHECK(writer_state->terminal_custody_notifications.load(std::memory_order_acquire) == (outcome == Retirement::RetainedBrowserRead ? 1U : 0U));
        CHECK(writer_state->browser_terminals.load(std::memory_order_acquire) == 1U);
        CHECK(writer_state->retirements.load(std::memory_order_acquire) == 0U);
    }
    CHECK(writer_state->retirements.load(std::memory_order_acquire) == 0U);
}
TEST_CASE("Presentation worker failure rejects later direct selection") {
    PresentationSourceFixture source;
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    writer_state->fail_pump.store(true, std::memory_order_release);
    PresentationFailureProbe failures;
    auto presentation = make_failure_presentation(source, writer_state, failures);
    PresentationScenario scenario{presentation, writer_state};
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(failures.events().Wait([&] { return failures.failures() == 1U; }));
    CHECK(writer_state->browser_terminals.load(std::memory_order_acquire) == 0U);
    CHECK(writer_state->retirements.load(std::memory_order_acquire) == 0U);
    CHECK_THROWS_AS(presentation.Select(source.identity()), contracts::FailedError);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    CHECK(writer_state->browser_terminals.load(std::memory_order_acquire) == 1U);
    CHECK(writer_state->retirements.load(std::memory_order_acquire) == 1U);
}
}  // namespace
}  // namespace mmltk::controller
namespace {
TEST_CASE("test_direct_presentation_descriptor", "[browser_runtime][direct][presentation]") {
    const mmltk::controller::PresentationPublication publication{
        .capability =
            {
                .surface_high = 4U,
                .surface_low = 5U,
                .extent = {1'200U, 800U},
                .generation = 2U,
                .condition = mmltk::controller::PresentationCapabilityCondition::Ready,
            },
        .timeline_ready = 3U,
        .presentation_revision = 4U,
    };
    CHECK(publication.valid());
}
}  // namespace
