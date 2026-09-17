#pragma once
#include "src/common/io/event_fd.h"
#include <sys/types.h>
#include "src/common/types/generation.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <cuda_runtime_api.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/controller/presentation/visual_document.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/services/diagnostics_client.h"
#include "src/controller/services/runtime_diagnostics.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/common/io/scoped_fd.h"
#include "src/controller/presentation/presentation_system.h"
#include "src/controller/presentation/detail/native_presentation_writer_test_access.h"
namespace mmltk::controller::visual_test_support {
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
void Fill(const mmltk::frameworks::gpu::ImagePlaneView plane, const std::uint8_t value);
struct MutationCommitProbe final {
    std::promise<void> committed;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
};
struct TestPresentationWriterState final {
    std::atomic_bool advertise_waiting_candidate{false};
    TestPresentationWriterState() : readiness(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK)), pump_release(release_pump.get_future().share()) {}
    void SignalReadiness() const {
        const std::uint64_t value = 1U;
        REQUIRE(::write(readiness.get(), &value, sizeof(value)) == sizeof(value));
    }
    void AcknowledgeAllocationRetirement(const std::uint64_t generation) {
        retirement_acknowledgement.store(generation, std::memory_order_release);
        SignalReadiness();
    }
    void RecordPump() {
        {
            std::scoped_lock lock(pump_mutex);
            ++pump_count;
        }
        pump_changed.notify_all();
    }
    [[nodiscard]] std::uint64_t PumpCount() const {
        std::scoped_lock lock(pump_mutex);
        return pump_count;
    }
    [[nodiscard]] bool WaitForPumpAfter(const std::uint64_t prior) {
        std::unique_lock lock(pump_mutex);
        return pump_changed.wait_for(lock, 2s, [&] { return pump_count > prior; });
    }
    // CLEANUP-IGNORE: This descriptor begins presentation-writer readiness and lifecycle evidence, not fake GPU
    // synchronization and transfer counters.
    mmltk::common::io::ScopedFd readiness;
    // CLEANUP-OFF: These presentation lifecycle facts are independent test evidence with distinct assertions, not
    // a second physical-backend telemetry schema.
    std::atomic<std::uint64_t> timeline{0U};
    std::atomic<std::uint64_t> presentation_revision{0U};
    std::atomic<std::uint64_t> submissions{0U};
    std::atomic<std::uint64_t> browser_terminals{0U};
    std::atomic<std::uint64_t> retirements{0U};
    std::atomic<std::uint64_t> allocation_retirements{0U};
    std::atomic<std::uint64_t> retained_allocation_generation{0U};
    std::atomic<std::uint64_t> retirement_acknowledgement{0U};
    std::atomic<std::uint64_t> constructions{0U};
    std::atomic<std::uint64_t> context_bindings{0U};
    // CLEANUP-ON
    std::atomic_bool allow_publication{true};
    std::atomic_bool fail_pump{false};
    std::atomic_bool retire_binding{false};
    std::atomic_bool block_pump{false};
    std::atomic_bool pump_block_reported{false};
    std::atomic<PresentationNativeWriter::Retirement> terminal_retirement{PresentationNativeWriter::Retirement::Released};
    std::atomic_uint terminal_custody_notifications{0U};
    std::promise<void> first_submission;
    std::promise<void> second_submission;
    std::promise<void> third_submission;
    std::promise<void> allocation_retired;
    std::promise<void> pump_entered;
    std::promise<void> release_pump;
    std::shared_future<void> pump_release;
    mutable std::mutex pump_mutex;
    std::condition_variable pump_changed;
    std::uint64_t pump_count = 0U;
};
// Declared immediately after the system, before assertions. Event/callback
// storage precedes the system and therefore outlives terminal settlement.
class PresentationScenario final {
   public:
    PresentationScenario(PresentationSystem& system, std::shared_ptr<TestPresentationWriterState> state) : system_(system), state_(std::move(state)) {}
    ~PresentationScenario() {
        mmltk::testsupport::release_test_promise(state_->release_pump);
        if (system_.stopped()) return;
        system_.CloseAdmission();
        system_.BrowserPeerLost();
        static_cast<void>(system_.Shutdown());
    }
    PresentationScenario(const PresentationScenario&) = delete;
    PresentationScenario& operator=(const PresentationScenario&) = delete;

   private:
    PresentationSystem& system_;
    std::shared_ptr<TestPresentationWriterState> state_;
};
class TestPresentationWriter final : public PresentationNativeWriter {
   public:
    [[nodiscard]] static PresentationNativeWriterFactory Factory(std::shared_ptr<FakeImageBackend> backend,
                                                                 std::shared_ptr<TestPresentationWriterState> state) {
        return [backend = std::move(backend), state = std::move(state)] { return std::make_unique<TestPresentationWriter>(0, backend, state); };
    }
    TestPresentationWriter(const int device, std::shared_ptr<FakeImageBackend> backend, std::shared_ptr<TestPresentationWriterState> state)
        : context_(device, std::move(backend)),
          stream_(context_),
          sample_arena_(context_, mmltk::frameworks::gpu::ImageProductLayout::Clean),
          state_(std::move(state)) {
        state_->constructions.fetch_add(1U, std::memory_order_acq_rel);
    }
    ~TestPresentationWriter() override { state_->retirements.fetch_add(1U, std::memory_order_acq_rel); }
    void Submit(PresentationSubmittedSource submitted, const VisualSourceReader& reader) override {
        context_.Bind();
        state_->context_bindings.fetch_add(1U, std::memory_order_acq_rel);
        const auto submission = state_->submissions.fetch_add(1U, std::memory_order_acq_rel);
        if (submission == 0U) state_->first_submission.set_value();
        if (submission == 1U) state_->second_submission.set_value();
        if (submission == 2U) state_->third_submission.set_value();
        submitted_ = submitted;
        const auto& frame = submitted.observation.frame;
        const auto width = frame.extent.width;
        const auto height = frame.extent.height;
        if (!Contains(active_, width, height)) {
            if (candidate_ && !Contains(candidate_, width, height)) candidate_.reset();
            if (!candidate_) {
                candidate_ = Allocation{
                    .width = active_ ? std::max(active_->width, width) : width,
                    .height = active_ ? std::max(active_->height, height) : height,
                    .generation = next_generation_++,
                };
            }
        }
        const auto& target = Contains(active_, width, height) ? *active_ : *candidate_;
        reader_ = std::addressof(reader);
        pending_ = PresentationPublication{
            .capability =
                {
                    .surface_high = 1U,
                    .surface_low = target.generation,
                    .extent = {target.width, target.height},
                    .generation = target.generation,
                    .condition = PresentationCapabilityCondition::Ready,
                },
        };
    }
    PresentationNativeOutcome Pump(const std::uint64_t current_selection_generation) override {
        context_.Bind();
        state_->context_bindings.fetch_add(1U, std::memory_order_acq_rel);
        const auto consumed = mmltk::common::io::read_counter_fd(state_->readiness.get());
        if (consumed.bytes < 0 && consumed.error != EAGAIN) throw std::runtime_error("test presentation readiness read failed");
        if (state_->fail_pump.load(std::memory_order_acquire)) throw std::runtime_error("test presentation writer failure");
        if (state_->block_pump.load(std::memory_order_acquire)) {
            if (!state_->pump_block_reported.exchange(true, std::memory_order_acq_rel)) state_->pump_entered.set_value();
            state_->pump_release.wait();
        }
        const auto retirement_acknowledgement = state_->retirement_acknowledgement.exchange(0U, std::memory_order_acq_rel);
        if (retiring_ && retirement_acknowledgement == retiring_->generation) {
            retiring_.reset();
            state_->retained_allocation_generation.store(0U, std::memory_order_release);
            const auto prior = state_->allocation_retirements.fetch_add(1U, std::memory_order_acq_rel);
            if (prior == 0U) state_->allocation_retired.set_value();
        }
        mmltk::testsupport::ScopedTestCleanup pumped{[state = state_] { state->RecordPump(); }};
        if (state_->retire_binding.exchange(false, std::memory_order_acq_rel)) {
            pending_.reset();
            return {.progress = PresentationNativeProgress::BindingRetired};
        }
        if (submitted_.selection_generation != current_selection_generation) {
            pending_.reset();
            return {
                .progress = PresentationNativeProgress::Superseded,
                .submitted = submitted_,
            };
        }
        if (!pending_ || !state_->allow_publication.load(std::memory_order_acquire)) { return {.capability = capability()}; }
        const bool candidate_target = candidate_ && pending_->capability.generation == candidate_->generation;
        if (candidate_target && retiring_) return {.capability = capability()};
        auto source = reader_->borrow();
        if (!visual_product_matches_frame(submitted_.observation.frame, source)) {
            pending_.reset();
            reader_ = nullptr;
            return {
                .progress = PresentationNativeProgress::Superseded,
                .submitted = submitted_,
                .capability = capability(),
            };
        }
        // This injected writer includes the simulated browser receiver and its sample storage.
        static_cast<void>(sample_arena_.CopyFrom(stream_, std::move(source)));
        stream_.Synchronize();
        if (candidate_target) {
            if (active_) {
                retiring_ = std::exchange(active_, std::nullopt);
                state_->retained_allocation_generation.store(retiring_->generation, std::memory_order_release);
            }
            active_ = std::exchange(candidate_, std::nullopt);
        }
        pending_->timeline_ready = state_->timeline.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        pending_->transfer_sequence = pending_->timeline_ready;
        pending_->presentation_revision = state_->presentation_revision.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        auto outcome = PresentationNativeOutcome{
            .progress = PresentationNativeProgress::Published,
            .submitted = submitted_,
            .publication = std::exchange(pending_, std::nullopt).value(),
            .capability = capability(),
        };
        reader_ = nullptr;
        return outcome;
    }
    int poll_fd() const noexcept override { return state_->readiness.get(); }
    int completion_fd() const noexcept override { return -1; }
    bool wants_write() const noexcept override { return false; }
    void SetExpectedBrowserProcessGroup(pid_t) override {}
    Retirement BrowserPeerLost() noexcept override {
        try {
            context_.Bind();
            state_->context_bindings.fetch_add(1U, std::memory_order_acq_rel);
        } catch (...) { return Retirement::ReleasedWithFailure; }
        state_->browser_terminals.fetch_add(1U, std::memory_order_acq_rel);
        return state_->terminal_retirement.load(std::memory_order_acquire);
    }
    void TerminalCustodyInstalled() noexcept override { state_->terminal_custody_notifications.fetch_add(1U, std::memory_order_acq_rel); }

   private:
    struct Allocation final {
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        std::uint64_t generation = 0U;
    };
    [[nodiscard]] static bool Contains(const std::optional<Allocation>& allocation, const std::uint32_t width, const std::uint32_t height) noexcept {
        return allocation && width <= allocation->width && height <= allocation->height;
    }
    [[nodiscard]] PresentationCapability capability() const noexcept {
        const auto* allocation = candidate_ ? std::addressof(*candidate_) : active_ ? std::addressof(*active_) : nullptr;
        if (allocation == nullptr) return {};
        return {
            .surface_high = 1U,
            .surface_low = allocation->generation + (state_->advertise_waiting_candidate.load() ? 100U : 0U),
            .extent = {allocation->width, allocation->height},
            .generation = allocation->generation + (state_->advertise_waiting_candidate.load() ? 100U : 0U),
            .condition =
                state_->allow_publication.load(std::memory_order_acquire) ? PresentationCapabilityCondition::Ready : PresentationCapabilityCondition::Admitted,
        };
    }
    mmltk::frameworks::gpu::DeviceContext context_;
    mmltk::frameworks::gpu::ImageStream stream_;
    mmltk::frameworks::gpu::ImageProductBuffer sample_arena_;
    std::shared_ptr<TestPresentationWriterState> state_;
    PresentationSubmittedSource submitted_{};
    const VisualSourceReader* reader_ = nullptr;
    std::optional<PresentationPublication> pending_;
    std::optional<Allocation> active_;
    std::optional<Allocation> candidate_;
    std::optional<Allocation> retiring_;
    std::uint64_t next_generation_ = 1U;
};
class EventGate final {
   public:
    void Advance() {
        {
            std::scoped_lock lock(mutex_);
            ++revision_;
        }
        changed_.notify_all();
    }
    template <class Predicate>
    bool Wait(Predicate predicate, std::chrono::seconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, std::move(predicate));
    }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

   private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t revision_ = 0U;
};
class DiagnosticCapture final {
   public:
    [[nodiscard]] VisualDiagnosticSink sink() noexcept {
        return {
            .context = this,
            .write =
                [](void* context, const VisualDiagnosticFact fact) noexcept {
                    auto& capture = *static_cast<DiagnosticCapture*>(context);
                    capture.last_system.store(fact.system, std::memory_order_release);
                    if (fact.operation == VisualDiagnosticOperation::StaleThumbnailDiscarded)
                        capture.stale_discarded.fetch_add(fact.value, std::memory_order_acq_rel);
                    if (fact.operation == VisualDiagnosticOperation::UpscaleResultReused) {
                        capture.reused_revision.store(fact.value, std::memory_order_release);
                        capture.reused_meaning.store(fact.context.document_meaning_identity, std::memory_order_release);
                        capture.reused_observation.store(fact.context.observation_revision, std::memory_order_release);
                    }
                    capture.count.fetch_add(1U, std::memory_order_acq_rel);
                },
        };
    }
    // CLEANUP-OFF: These named trace assertions are independent domain evidence, not physical fake-backend
    // allocation and transfer counters.
    std::atomic<std::uint64_t> count{0U};
    std::atomic<std::uint64_t> stale_discarded{0U};
    std::atomic<std::uint64_t> reused_revision{0U};
    std::atomic<std::uint64_t> reused_meaning{0U};
    std::atomic<std::uint64_t> reused_observation{0U};
    std::atomic<contracts::DiagnosticOwner> last_system{contracts::DiagnosticOwner::Explore};
    // CLEANUP-ON
};
constexpr VisualDeviceSettings kDevice{
    .device = 0,
    .maximum_width = 1024U,
    .maximum_height = 1024U,
};
[[nodiscard]] bool has_cuda_device() noexcept;
class LoadedSettings final {
   public:
    explicit LoadedSettings(SystemEventSink<SettingsSystem::event_type> events = {}) : system_(std::move(events)) {
        REQUIRE(system_.Load(location()).applied());
    }
    [[nodiscard]] SettingsSystem& system() noexcept { return system_; }
    void BreakPersistence() {
        std::filesystem::remove_all(directory_.path());
        mmltk::testsupport::write_text_file(directory_.path(), "blocked");
    }
    void RestorePersistence() {
        std::filesystem::remove_all(directory_.path());
        std::filesystem::create_directories(directory_.path());
    }
    void MakeUnavailable() {
        std::filesystem::remove_all(directory_.path());
        std::filesystem::create_directories(directory_.path() / "gui.json");
        REQUIRE_FALSE(system_.Load(location()).applied());
    }
    [[nodiscard]] services::SettingsLocation location() const { return services::SettingsLocation{(directory_.path() / "gui.json").string()}; }

   private:
    mmltk::testsupport::ScopedTempDir directory_{"mmltk-explore-settings"};
    SettingsSystem system_;
};
[[nodiscard]] VisualDocumentRead test_document(mmltk::frameworks::gpu::BorrowedImageProductReadView pixels);
template <class System>
[[nodiscard]] VisualSourceReader read_from(System& system) {
    return {
        .source = system.snapshot().frame.source,
        .observe =
            [&system] {
                const auto snapshot = system.snapshot();
                return VisualSourceObservation{.frame = snapshot.frame, .snapshot_revision = snapshot.revision};
            },
        .borrow = [&system] { return system.BorrowFrame(); },
    };
}
class MutableVisualSource final {
   public:
    MutableVisualSource(std::shared_ptr<FakeImageBackend> backend, const VisualExtent extent, const std::uint8_t value = 1U)
        : runtime_(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
              .device = 0, .backend = std::move(backend), .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic}) {
        Publish(extent, value);
    }
    void Publish(const VisualExtent extent, const std::uint8_t value) {
        runtime_.Publish(extent.width, extent.height, [value](const auto clean, const auto semantic, const auto) {
            Fill(clean, value);
            Fill(semantic, static_cast<std::uint8_t>(value + 20U));
        });
        const auto borrowed = runtime_.Borrow();
        REQUIRE(borrowed.valid());
        std::scoped_lock lock(mutex_);
        frame_ = visual_frame(identity_, extent, borrowed.plane(0U).revision());
        frame_.clean_revision = frame_.revision;
        snapshot_revision_ = mmltk::common::types::advance_monotonic_identity(snapshot_revision_);
    }
    void SetSemantics(const std::uint8_t value) {
        const auto current = frame();
        auto candidate = runtime_.AcquireOutput({}, runtime_.Completed());
        runtime_.Publish(candidate, current.extent.width, current.extent.height,
                         [value](const auto, const auto semantic, const auto) { Fill(semantic, value); });
        static_cast<void>(runtime_.CommitOutput(std::move(candidate)));
        std::scoped_lock lock(mutex_);
        frame_.revision = runtime_.OutputFacts().revision;
        snapshot_revision_ = mmltk::common::types::advance_monotonic_identity(snapshot_revision_);
    }
    [[nodiscard]] mmltk::frameworks::gpu::SystemImageRuntime::OutputCandidate TryReserveOutput() {
        mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput baseline;
        return runtime_.TryAcquireOutput(baseline);
    }
    [[nodiscard]] VisualFrame frame() const {
        std::scoped_lock lock(mutex_);
        return frame_;
    }
    [[nodiscard]] VisualDocumentRead BorrowExact(const VisualFrame& frame) const {
        if (frame.source != identity_) return {};
        return test_document(borrow_matching_visual_product(frame, runtime_.Borrow()));
    }
    [[nodiscard]] ExactVisualDocumentBorrower WithDocument(std::shared_ptr<const VisualDocument> document) {
        return [this, document = std::move(document)](const VisualFrame& frame) {
            auto read = BorrowExact(frame);
            read.document = document;
            return read;
        };
    }
    [[nodiscard]] VisualSourceReader reader() {
        return {
            .source = identity_,
            .observe =
                [this] {
                    std::scoped_lock lock(mutex_);
                    return VisualSourceObservation{.frame = frame_, .snapshot_revision = snapshot_revision_};
                },
            .borrow = [this] { return runtime_.Borrow(); },
        };
    }

   private:
    mutable std::mutex mutex_;
    mmltk::frameworks::gpu::SystemImageRuntime runtime_;
    PresentationSourceIdentity identity_{PresentationSourceKind::Explore, 1U};
    VisualFrame frame_{};
    std::uint64_t snapshot_revision_ = 0U;
};
class ProductPresentationSources final {
   public:
    ProductPresentationSources() : backend_(std::make_shared<FakeImageBackend>()) {
        constexpr std::array kKinds{
            PresentationSourceKind::Explore, PresentationSourceKind::Annotation, PresentationSourceKind::Upscale,
            PresentationSourceKind::Live,    PresentationSourceKind::Predict,
        };
        runtimes_.reserve(kKinds.size());
        sources_.reserve(kKinds.size());
        for (std::size_t index = 0U; index != kKinds.size(); ++index) {
            auto runtime = std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(
                mmltk::frameworks::gpu::SystemImageRuntimeConfig{.device = 0, .backend = backend_});
            const auto extent = VisualExtent{static_cast<std::uint32_t>(16U + index), 16U};
            runtime->Publish(extent.width, extent.height, [](auto, auto, auto) {});
            auto* const private_runtime = runtime.get();
            const PresentationSourceIdentity identity{kKinds[index], 1U};
            runtimes_.push_back(std::move(runtime));
            sources_.push_back({
                .source = identity,
                .observe =
                    [identity, extent] {
                        return VisualSourceObservation{
                            .frame = visual_frame(identity, extent, 1U),
                            .snapshot_revision = 1U,
                        };
                    },
                .borrow = [private_runtime] { return private_runtime->Borrow(); },
            });
        }
    }
    [[nodiscard]] std::shared_ptr<FakeImageBackend> backend() const noexcept { return backend_; }
    [[nodiscard]] std::span<const VisualSourceReader> sources() const noexcept { return sources_; }

   private:
    std::shared_ptr<FakeImageBackend> backend_;
    std::vector<std::unique_ptr<mmltk::frameworks::gpu::SystemImageRuntime>> runtimes_;
    std::vector<VisualSourceReader> sources_;
};
}  // namespace mmltk::controller::visual_test_support
