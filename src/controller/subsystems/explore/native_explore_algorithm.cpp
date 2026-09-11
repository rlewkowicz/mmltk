#include "src/backend/data/compiled_dataset.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/controller/subsystems/explore/detail/gallery_stream.h"
#include "src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h"
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

#include "src/backend/data/compiled_format.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/execution_policy.h"
#include "src/common/io/event_fd.h"
#include "src/common/io/scoped_fd.h"
#include "src/backend/imaging/raster/detail/raster_color.h"

import mmltk.backend.imaging.explore.compiled_explore_store;
import mmltk.backend.imaging.explore.explore_render_core;
import mmltk.backend.models.rfdetr.augmentation.augmentation_metadata;

namespace mmltk::controller {

class ExploreAcceptanceGate::Impl final {
   public:
    void* product_context = nullptr;
    void (*product_observer)(void*, ProductObservation) noexcept = nullptr;
    void* submission_context = nullptr;
    void (*submission_observer)(void*, std::uintptr_t, SubmissionStage) = nullptr;
    void* read_context = nullptr;
    void (*read_observer)(void*, std::uint64_t, std::uint32_t) = nullptr;
    void* initial_wait_context = nullptr;
    void (*initial_wait_observer)(void*, std::uint64_t) = nullptr;
    mutable std::atomic<PublicationStage> publication_failure{PublicationStage::None};
    mutable std::atomic_bool probe_failure{false};
    explicit Impl(const int command_descriptor) : command_(command_descriptor), stop_(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK)) {
        if (command_.get() < 0 || stop_.get() < 0) throw std::invalid_argument("invalid Explore acceptance gate descriptor");
        reader_ = std::jthread([this] { ReadCommands(); });
    }
    ~Impl() { StopAndJoin(); }

    void AdvanceGeneration(const std::uint64_t generation) noexcept {
        std::scoped_lock lock(mutex_);
        current_generation_ = generation;
        changed_.notify_all();
    }

    [[nodiscard]] WaitResult AwaitInitialRelease(const std::uint64_t generation) {
        std::unique_lock lock(mutex_);
        if (terminal_ || generation != current_generation_) return WaitResult::Stale;
        if (!release_all_ && !release_one_ && !std::exchange(initial_wait_announced_, true)) {
            const std::uint8_t waiting = 0x80U;
            if (::send(command_.get(), &waiting, sizeof(waiting), MSG_NOSIGNAL | MSG_DONTWAIT) != sizeof(waiting)) terminal_ = true;
        }
        ++waiters_;
        changed_.wait(lock, [this, generation] { return terminal_ || generation != current_generation_ || release_all_ || release_one_; });
        --waiters_;
        if (terminal_ || generation != current_generation_) return WaitResult::Stale;
        if (!release_all_) {
            release_one_ = false;
            initial_released_generation_ = generation;
        }
        return WaitResult::Proceed;
    }

    [[nodiscard]] bool ClaimHeldCompletion() {
        std::scoped_lock lock(mutex_);
        if (terminal_ || held_claimed_) return false;
        held_claimed_ = true;
        held_pending_ = true;
        return true;
    }

    [[nodiscard]] bool ClaimTerminalReport() {
        std::scoped_lock lock(mutex_);
        return terminal_ && !std::exchange(terminal_reported_, true);
    }

    [[nodiscard]] WaitResult AwaitHeldCompletion(const std::uint64_t generation, const std::uint64_t slot,
                                                 const std::uint64_t compiled_index, const std::uint64_t staging_bytes) {
        ControlObservation observation{.event = ControlEvent::HeldWait,
                                       .generation = generation,
                                       .slot = slot,
                                       .compiled_index = compiled_index,
                                       .staging_bytes = staging_bytes};
        if (!SendControlObservation(observation)) {
            Terminal();
            return WaitResult::Stale;
        }
        const auto finish = [this, &observation](const WaitResult result) {
            observation.event = result == WaitResult::Proceed ? ControlEvent::HeldProceed : ControlEvent::HeldStale;
            static_cast<void>(SendControlObservation(observation));
            return result;
        };
        std::unique_lock lock(mutex_);
        ++waiters_;
        changed_.wait(lock, [this, generation] { return terminal_ || generation != current_generation_ || release_held_; });
        --waiters_;
        held_pending_ = false;
        return finish(terminal_ || generation != current_generation_ ? WaitResult::Stale : WaitResult::Proceed);
    }

    void SetFrontendCommand(FrontendCommand callback) {
        auto retained = callback ? std::make_shared<const FrontendCommand>(std::move(callback)) : nullptr;
        std::scoped_lock lock(mutex_);
        frontend_command_ = std::move(retained);
    }

    void SetRedrawCommand(std::function<bool()> callback) {
        auto retained = callback ? std::make_shared<const std::function<bool()>>(std::move(callback)) : nullptr;
        std::scoped_lock lock(mutex_);
        redraw_command_ = std::move(retained);
    }

    [[nodiscard]] bool ObserveFrontend(const contracts::IntegrationControlReceipt receipt) noexcept {
        std::scoped_lock lock(mutex_);
        using Kind = contracts::IntegrationControlKind;
        if (terminal_ || !frontend_command_ || receipt.sequence != frontend_sequence_ || frontend_settled_ || receipt.kind == Kind::Advance)
            return false;
        if ((receipt.kind == Kind::Failed) != (receipt.failureline != 0U)) return false;
        if (receipt.kind == Kind::Progress) {
            if (receipt.progress <= frontend_progress_ || (receipt.progress & 3U) == 0U) return false;
            frontend_progress_ = receipt.progress;
        }
        if (receipt.kind == Kind::PressureEntered && std::exchange(frontend_pressure_, true)) return false;
        if (receipt.kind == Kind::Settled) frontend_settled_ = true;
        const bool sent = SendControlObservation({.event = ControlEvent::Frontend,
                                                  .generation = receipt.sequence,
                                                  .slot = static_cast<std::uint64_t>(receipt.kind),
                                                  .compiled_index = receipt.progress,
                                                  .staging_bytes = receipt.failureline});
        if (!sent || receipt.kind == Kind::Failed) {
            terminal_ = true;
            changed_.notify_all();
        }
        return sent;
    }

    void Stop() noexcept {
        const bool wake_requested = mmltk::common::io::signal_event_fd(stop_.get());
        static_cast<void>(wake_requested);
        Terminal();
    }

    void StopAndJoin() noexcept {
        Stop();
        std::scoped_lock lock(join_mutex_);
        if (reader_.joinable()) reader_.join();
    }

   private:
    [[nodiscard]] bool SendControlObservation(const ControlObservation& observation) const noexcept {
        ssize_t written = -1;
        do {
            written = ::send(command_.get(), &observation, sizeof(observation), MSG_NOSIGNAL | MSG_DONTWAIT);
        } while (written < 0 && errno == EINTR);
        return written == static_cast<ssize_t>(sizeof(observation));
    }

    [[nodiscard]] std::uint8_t ReadCommand() noexcept {
        std::array<pollfd, 2U> descriptors{{
            {.fd = command_.get(), .events = POLLIN | POLLHUP | POLLERR, .revents = 0},
            {.fd = stop_.get(), .events = POLLIN | POLLHUP | POLLERR, .revents = 0},
        }};
        int ready = -1;
        do {
            ready = ::poll(descriptors.data(), descriptors.size(), -1);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || descriptors[1].revents != 0) return {};
        if ((descriptors[0].revents & POLLIN) == 0) return {};
        std::uint8_t command = 0U;
        ssize_t consumed = -1;
        do {
            consumed = ::recv(command_.get(), &command, sizeof(command), MSG_TRUNC);
        } while (consumed < 0 && errno == EINTR);
        return consumed == sizeof(command) ? command : static_cast<std::uint8_t>(0U);
    }

    void ReadCommands() noexcept {
        for (;;) {
            const auto command = ReadCommand();
            std::shared_ptr<const FrontendCommand> callback;
            std::shared_ptr<const std::function<bool()>> redraw;
            std::uint64_t sequence = 0U;
            {
                std::scoped_lock lock(mutex_);
                if (terminal_) return;
                if (command == 1U) {
                    if (current_generation_ == 0U || initial_released_generation_ != current_generation_) release_one_ = true;
                } else if (command == 2U) {
                    release_all_ = true;
                } else if (command == 4U) {
                    release_held_ = true;
                } else if (command == 16U && !redraw_claimed_ && !release_all_ && !release_one_ &&
                           initial_released_generation_ == 0U && redraw_command_) {
                    redraw_claimed_ = true;
                    redraw = redraw_command_;
                } else if (command == 8U && frontend_settled_ && waiters_ == 0U && !held_pending_ && frontend_command_ &&
                           frontend_sequence_ != std::numeric_limits<std::uint64_t>::max()) {
                    release_all_ = false;
                    release_one_ = false;
                    release_held_ = false;
                    held_claimed_ = false;
                    initial_wait_announced_ = false;
                    initial_released_generation_ = 0U;
                    frontend_settled_ = false;
                    frontend_progress_ = 0U;
                    frontend_pressure_ = false;
                    sequence = ++frontend_sequence_;
                    callback = frontend_command_;
                } else {
                    terminal_ = true;
                }
                changed_.notify_all();
                if (terminal_) return;
            }
            if (redraw || callback) {
                try {
                    const bool accepted =
                        redraw ? (*redraw)() : (*callback)({.kind = contracts::IntegrationControlKind::Advance, .sequence = sequence});
                    if (accepted) continue;
                } catch (...) {}
                Terminal();
                return;
            }
        }
    }

    void Terminal() noexcept {
        std::scoped_lock lock(mutex_);
        terminal_ = true;
        changed_.notify_all();
    }

    mmltk::common::io::ScopedFd command_;
    mmltk::common::io::ScopedFd stop_;
    std::mutex mutex_;
    std::mutex join_mutex_;
    std::condition_variable changed_;
    std::size_t waiters_ = 0U;
    bool initial_wait_announced_ = false;
    bool terminal_ = false;
    bool release_one_ = false;
    bool release_all_ = false;
    bool release_held_ = false;
    bool held_claimed_ = false;
    bool held_pending_ = false;
    bool terminal_reported_ = false;
    std::uint64_t current_generation_ = 0U;
    std::uint64_t initial_released_generation_ = 0U;
    std::shared_ptr<const FrontendCommand> frontend_command_;
    std::shared_ptr<const std::function<bool()>> redraw_command_;
    bool redraw_claimed_ = false;
    std::uint64_t frontend_sequence_ = 1U;
    bool frontend_settled_ = false;
    bool frontend_pressure_ = false;
    std::uint64_t frontend_progress_ = 0U;
    std::jthread reader_;
};

ExploreAcceptanceGate::ExploreAcceptanceGate(const int command_descriptor) : impl_(std::make_unique<Impl>(command_descriptor)) {}
ExploreAcceptanceGate::~ExploreAcceptanceGate() = default;
void ExploreAcceptanceGate::AdvanceGeneration(const std::uint64_t generation) noexcept { impl_->AdvanceGeneration(generation); }
auto ExploreAcceptanceGate::AwaitInitialRelease(const std::uint64_t generation) -> WaitResult {
    if (impl_->initial_wait_observer) impl_->initial_wait_observer(impl_->initial_wait_context, generation);
    return impl_->AwaitInitialRelease(generation);
}
void ExploreAcceptanceGate::SetInitialWaitObserver(void* context, void (*observer)(void*, std::uint64_t)) noexcept {
    impl_->initial_wait_context = context;
    impl_->initial_wait_observer = observer;
}
auto ExploreAcceptanceGate::AwaitHeldCompletion(const std::uint64_t generation, const std::uint64_t slot,
                                                const std::uint64_t compiled_index, const std::uint64_t staging_bytes) -> WaitResult {
    return impl_->AwaitHeldCompletion(generation, slot, compiled_index, staging_bytes);
}
bool ExploreAcceptanceGate::ClaimHeldCompletion() { return impl_->ClaimHeldCompletion(); }
bool ExploreAcceptanceGate::ClaimTerminalReport() { return impl_->ClaimTerminalReport(); }
void ExploreAcceptanceGate::Stop() noexcept { impl_->Stop(); }
void ExploreAcceptanceGate::StopAndJoin() noexcept { impl_->StopAndJoin(); }
void ExploreAcceptanceGate::SetFrontendCommand(FrontendCommand callback) { impl_->SetFrontendCommand(std::move(callback)); }
void ExploreAcceptanceGate::SetRedrawCommand(std::function<bool()> callback) { impl_->SetRedrawCommand(std::move(callback)); }
bool ExploreAcceptanceGate::ObserveFrontend(const contracts::IntegrationControlReceipt receipt) noexcept {
    return impl_->ObserveFrontend(receipt);
}
void ExploreAcceptanceGate::SetProductObserver(void* context, void (*observer)(void*, ProductObservation) noexcept) noexcept {
    impl_->product_context = context;
    impl_->product_observer = observer;
}
void ExploreAcceptanceGate::ObserveProduct(ProductObservation observation) const noexcept {
    if (impl_->product_observer) impl_->product_observer(impl_->product_context, std::move(observation));
}
void ExploreAcceptanceGate::SetSubmissionObserver(void* context, void (*observer)(void*, std::uintptr_t, SubmissionStage)) noexcept {
    impl_->submission_context = context;
    impl_->submission_observer = observer;
}
void ExploreAcceptanceGate::ObserveSubmission(const std::uintptr_t stream, const SubmissionStage stage) const {
    if (impl_->submission_observer) impl_->submission_observer(impl_->submission_context, stream, stage);
}
void ExploreAcceptanceGate::SetReadObserver(void* context, void (*observer)(void*, std::uint64_t, std::uint32_t)) noexcept {
    impl_->read_context = context;
    impl_->read_observer = observer;
}
void ExploreAcceptanceGate::ObserveRead(const std::uint64_t generation, const std::uint32_t index) const {
    if (impl_->read_observer) impl_->read_observer(impl_->read_context, generation, index);
}
void ExploreAcceptanceGate::FailNextPublicationAt(const PublicationStage stage) noexcept {
    impl_->publication_failure.store(stage, std::memory_order_release);
}
void ExploreAcceptanceGate::FailNextProbe() noexcept { impl_->probe_failure.store(true); }
void ExploreAcceptanceGate::CheckProbe() const {
    if (impl_->probe_failure.exchange(false)) throw std::runtime_error("Explore acceptance probe preparation failure");
}
void ExploreAcceptanceGate::CheckPublication(const PublicationStage stage) const {
    auto expected = stage;
    if (impl_->publication_failure.compare_exchange_strong(expected, PublicationStage::None, std::memory_order_acq_rel))
        throw std::runtime_error("deterministic native Explore publication failure");
}

namespace explore_detail {

namespace data = mmltk::backend::data;
namespace explore = mmltk::backend::imaging::explore;
namespace rfdetr = mmltk::backend::models::rfdetr;

[[nodiscard]] std::uint64_t explore_dataset_identity(const std::string_view source, const data::FileHeader& header,
                                                     const std::span<const explore::ExploreImageSummary> summaries) noexcept {
    std::uint64_t value = 1469598103934665603ULL;
    const auto mix = [&value](const auto& item) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&item);
        for (std::size_t index = 0U; index != sizeof(item); ++index) {
            value ^= bytes[index];
            value *= 1099511628211ULL;
        }
    };
    for (const char character : source) {
        value ^= static_cast<std::uint8_t>(character);
        value *= 1099511628211ULL;
    }
    mix(header.image_width);
    mix(header.image_height);
    mix(header.num_images);
    mix(header.num_classes);
    for (const auto& summary : summaries) {
        mix(summary.classes);
        mix(summary.original_width);
        mix(summary.original_height);
        mix(summary.instance_count);
        mix(summary.has_masks);
    }
    return rfdetr::augmentation_mix64(value);
}

class NativeExploreAlgorithm final : public ExploreAlgorithm {
    struct DatasetState final {
        mmltk::backend::data::CompiledDataset store;
        std::vector<explore::ExploreImageSummary> summaries;
        std::vector<explore::ExploreRenderClassDescriptor> classes;
        std::vector<std::uint32_t> annotated_indices;
        ExploreFilter filter{};
        std::vector<std::uint32_t> order;
        std::vector<std::uint32_t> inverse{};
        std::vector<std::uint32_t> scratch;
        std::uint64_t shuffle_seed = 0U;
        std::uint64_t identity = 0U;
    };

   public:
    NativeExploreAlgorithm(ExploreNativeConfiguration configuration, const std::size_t nproc,
                           const mmltk::frameworks::gpu::DeviceExecution& execution, const std::uint32_t maximum_height)
        : configuration_(configuration), nproc_(nproc), gallery_(nproc, execution, configuration, maximum_height) {}
    [[nodiscard]] bool UsesLoadingOptions(const data::DataLoadingOptions& options) const override {
        return configuration_.loading == options;
    }
    ~NativeExploreAlgorithm() override = default;
    void StopIngress() noexcept override { gallery_.StopIngress(); }
    [[nodiscard]] mmltk::frameworks::gpu::SystemImageModel::Release ReleaseResources() noexcept override {
        return gallery_.ReleaseAfterRuntimeSettlement();
    }

    [[nodiscard]] ExploreOpened Open(const std::string_view source, const std::stop_token stop) override {
        gallery_.Suspend();
        open_candidate_.reset();
        auto opened = mmltk::backend::data::CompiledDataset::open_source(std::filesystem::path{source}, configuration_.image_limit);
        if (!opened) throw std::system_error(opened.error(), "Explore compiled source could not be opened");
        if (opened->image_entries().empty()) throw contracts::InvalidIntentError("Explore compiled source contains no images");
        if (opened->image_entries().size() > std::numeric_limits<std::uint32_t>::max())
            throw contracts::InvalidIntentError("Explore image catalog exceeds the application boundary");
        if (opened->class_names().size() > kExploreClassCapacity)
            throw contracts::InvalidIntentError("Explore class catalog exceeds the application boundary");
        if (std::ranges::any_of(opened->class_names(),
                                [](const auto& name) { return name.size() > contracts::kArtifactClassNameCapacity; }))
            throw contracts::InvalidIntentError("Explore class name exceeds the application boundary");
        auto store = std::move(*opened);
        std::atomic_bool cancelled = false;
        std::stop_callback cancellation{stop, [&cancelled] { cancelled.store(true, std::memory_order_release); }};
        std::vector<explore::ExploreImageSummary> summaries;
        try {
            summaries = explore::build_explore_summaries(store, &cancelled, &gallery_.workers());
        } catch (...) {
            if (stop.stop_requested()) return {};
            throw;
        }
        std::vector<std::uint32_t> order;
        std::vector<std::uint32_t> scratch;
        if (!explore::rebuild_explore_order(summaries, {}, false, 0U, order, scratch, &cancelled, 0U, nullptr, &gallery_.workers()) ||
            stop.stop_requested())
            return {};
        std::vector<explore::ExploreRenderClassDescriptor> classes(std::max<std::size_t>(store.header().num_classes, 1U));
        for (std::size_t index = 0U; index != store.header().num_classes; ++index) {
            const auto name = store.class_names()[index];
            auto& target = classes[index];
            target.length = static_cast<std::uint8_t>(std::min<std::size_t>(name.size(), 31U));
            std::memcpy(target.name, name.data(), target.length);
            target.visible = 1U;
            mmltk::backend::imaging::raster::detail::color::class_color(static_cast<int>(index), static_cast<int>(classes.size()),
                                                                        target.color[0], target.color[1], target.color[2]);
        }
        ExploreDatasetFacts dataset{
            .image_count = static_cast<std::uint32_t>(store.image_entries().size()),
            .image_width = store.header().image_width,
            .image_height = store.header().image_height,
        };
        dataset.class_names.reserve(store.class_names().size());
        dataset.palette = contracts::annotation_class_palette(store.class_names().size());
        for (const auto& name : store.class_names())
            dataset.class_names.push_back({.value = name});
        if (stop.stop_requested()) return {};
        DatasetState candidate{
            .store = std::move(store),
            .summaries = std::move(summaries),
            .classes = std::move(classes),
            .annotated_indices = {},
            .order = std::move(order),
            .scratch = std::move(scratch),
        };
        candidate.annotated_indices.reserve(candidate.summaries.size());
        for (std::size_t index = 0U; index != candidate.summaries.size(); ++index)
            if (candidate.summaries[index].instance_count != 0U) candidate.annotated_indices.push_back(static_cast<std::uint32_t>(index));
        candidate.identity = explore_dataset_identity(source, candidate.store.header(), candidate.summaries);
        ExploreOrderFacts order_facts{
            .matching_count = static_cast<std::uint32_t>(candidate.order.size()),
        };
        order_facts.visible_indices.assign(
            candidate.order.begin(),
            candidate.order.begin() + static_cast<std::ptrdiff_t>(std::min(candidate.order.size(), kExploreVisibleItemCapacity)));
        open_candidate_ = std::make_shared<DatasetState>(std::move(candidate));
        return {.dataset = std::move(dataset), .order = std::move(order_facts), .dataset_identity = open_candidate_->identity};
    }

    [[nodiscard]] ExploreOrderCandidate PrepareFilter(const ExploreFilter& filter, const std::uint64_t shuffle_seed,
                                                      const std::size_t nproc, const std::stop_token stop) override {
        // Filtering builds a separate order over immutable summaries. Existing
        // physical inputs keep their dataset leases until the new demand can
        // adopt them by compiled identity or their callbacks settle.
        auto& dataset = CandidateDataset();
        if (nproc != nproc_) throw std::logic_error("Explore nproc changed after runtime construction");
        const auto generation = ++candidate_generation_;
        std::atomic_bool cancelled = false;
        std::stop_callback cancellation{stop, [&cancelled] { cancelled.store(true, std::memory_order_release); }};
        if (!RebuildOrder(dataset, filter, shuffle_seed != 0U, shuffle_seed, candidate_order_, candidate_scratch_, &cancelled)) return {};
        candidate_inverse_.assign(dataset.summaries.size(), std::numeric_limits<std::uint32_t>::max());
        for (std::size_t position = 0; position < candidate_order_.size(); ++position)
            candidate_inverse_[candidate_order_[position]] = static_cast<std::uint32_t>(position);
        prepared_generation_ = generation;
        ExploreOrderCandidate candidate{
            .filter = filter,
            .order = {.shuffle_seed = shuffle_seed},
            .generation = generation,
        };
        candidate.order = Visible({}, &candidate);
        return candidate;
    }
    void Commit(ExploreOrderCandidate candidate) noexcept override {
        if (candidate.generation == 0U || candidate.generation != prepared_generation_) std::terminate();
        if (open_candidate_) {
            open_candidate_->filter = std::move(candidate.filter);
            open_candidate_->order.swap(candidate_order_);
            open_candidate_->shuffle_seed = candidate.order.shuffle_seed;
            committed_ = std::move(open_candidate_);
            open_candidate_.reset();
        } else {
            committed_->filter = std::move(candidate.filter);
            committed_->order.swap(candidate_order_);
            committed_->shuffle_seed = candidate.order.shuffle_seed;
        }
        committed_->inverse.swap(candidate_inverse_);
        candidate_inverse_.clear();
        candidate_order_.clear();
        candidate_scratch_.clear();
        prepared_generation_ = 0U;
    }
    void AbortRenderGeneration() override {
        gallery_.Quiesce();
        gallery_.ClearLogicalState();
        render_classes_.clear();
    }
    void DiscardCandidate() override {
        open_candidate_.reset();
        candidate_inverse_.clear();
        candidate_order_.clear();
        candidate_scratch_.clear();
        prepared_generation_ = 0U;
    }
    void Reset() override {
        AbortRenderGeneration();
        committed_.reset();
        open_candidate_.reset();
        candidate_inverse_.clear();
        candidate_order_.clear();
        candidate_scratch_.clear();
        candidate_generation_ = 0U;
        prepared_generation_ = 0U;
    }
    [[nodiscard]] ExploreOrderFacts Visible(const ExploreViewport viewport,
                                            const ExploreOrderCandidate* candidate = nullptr) const override {
        const auto& order = CandidateOrder(candidate);
        ExploreOrderFacts facts{
            .matching_count = static_cast<std::uint32_t>(order.size()),
            .shuffle_seed = candidate == nullptr ? committed_->shuffle_seed : candidate->order.shuffle_seed,
        };
        const auto visible = VisibleRange(viewport, candidate);
        facts.visible_indices.assign(visible.begin(), visible.end());
        return facts;
    }
    [[nodiscard]] std::span<const std::uint32_t> VisibleRange(const ExploreViewport viewport,
                                                              const ExploreOrderCandidate* candidate) const {
        const auto& order = CandidateOrder(candidate);
        const std::size_t first =
            viewport.valid() ? std::min<std::size_t>(static_cast<std::size_t>(viewport.first_row) * viewport.columns, order.size()) : 0U;
        const std::size_t requested =
            viewport.valid() ? static_cast<std::size_t>(viewport.row_count) * viewport.columns : kExploreVisibleItemCapacity;
        const std::size_t count = std::min({requested, order.size() - first, kExploreVisibleItemCapacity});
        return std::span{order}.subspan(first, count);
    }
    [[nodiscard]] bool Contains(const std::uint32_t index) const override {
        RequireOpen();
        return index < committed_->inverse.size() && committed_->inverse[index] != std::numeric_limits<std::uint32_t>::max();
    }
    [[nodiscard]] VisualExtent DetailExtent(const ExploreRenderPlan& plan) const override {
        RequireOpen();
        if (!plan.selected_image || !Contains(*plan.selected_image))
            throw contracts::InvalidIntentError("Explore detail selection is unavailable");
        return {.width = committed_->store.header().image_width, .height = committed_->store.header().image_height};
    }
    [[nodiscard]] std::optional<std::uint32_t> Adjacent(const std::uint32_t selected, const std::int64_t offset) const override {
        RequireOpen();
        const auto& order = committed_->order;
        if (!Contains(selected)) return {};
        const auto count = static_cast<std::int64_t>(order.size());
        const auto position = static_cast<std::int64_t>(committed_->inverse[selected]);
        return order[static_cast<std::size_t>((position + offset % count + count) % count)];
    }
    [[nodiscard]] VisualRegion DetailContent(const ExploreRenderPlan& plan) const override {
        if (!plan.selected_image) return {};
        const auto crop = committed_->store.letterbox(*plan.selected_image);
        return {crop.offset_x, crop.offset_y, crop.resized_width, crop.resized_height};
    }
    [[nodiscard]] std::shared_ptr<const VisualDocument> Document() const override { return gallery_.Document(); }
    [[nodiscard]] std::vector<ExploreLabel> Labels() const override { return gallery_.Labels(); }

    void SetGalleryReadySink(GalleryReadySink sink) override { gallery_.SetReadySink(std::move(sink)); }
    void SetCurrentDemand(ExploreDemandCheck check) override { gallery_.SetCurrentDemand(std::move(check)); }
    [[nodiscard]] ExploreOutputChange OutputChange(const ExploreRenderPlan& plan, const ExploreOrderCandidate* candidate) const override {
        const auto* dataset = candidate != nullptr && open_candidate_ ? open_candidate_.get() : committed_.get();
        const auto& order = CandidateOrder(candidate);
        const auto first = GalleryThumbnailCache::WindowFirst(order.size(), plan.viewport);
        const auto count = GalleryThumbnailCache::WindowCount(order.size(), plan.viewport);
        return gallery_.OutputChange(
            plan, plan.mode == ExploreMode::Gallery ? VisibleRange(plan.viewport, candidate) : std::span<const std::uint32_t>{},
            &dataset->store, std::span{order}.subspan(first, count));
    }

    void PrepareOutputPublication(ExploreOutputChange change) override { gallery_.PrepareOutputPublication(change); }
    void CommitOutputPublication() noexcept override { gallery_.CommitOutputPublication(); }
    [[nodiscard]] bool RollbackOutputPublication() noexcept override { return gallery_.RollbackOutputPublication(); }

    [[nodiscard]] ExploreGalleryPublication BeginGallery(const ExploreRenderPlan& plan, const ExploreOrderCandidate* candidate,
                                                         const std::size_t nproc, const mmltk::frameworks::gpu::ImagePlaneView clean,
                                                         const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                                         const std::uintptr_t stream) override {
        auto& dataset = DatasetFor(candidate);
        if (nproc != nproc_ || !plan.viewport.valid() || plan.mode != ExploreMode::Gallery)
            throw contracts::InvalidIntentError("Explore gallery plan is invalid");
        const auto visible = VisibleRange(plan.viewport, candidate);
        const auto& order = CandidateOrder(candidate);
        const auto window_first = explore_detail::GalleryThumbnailCache::WindowFirst(order.size(), plan.viewport);
        const auto count = explore_detail::GalleryThumbnailCache::WindowCount(order.size(), plan.viewport);
        const auto window = std::span{order}.subspan(window_first, count);
        if (gallery_.OutputChange(plan, visible, &dataset.store, window) != ExploreOutputChange::Unchanged)
            ConfigureClasses(dataset.classes, plan.overlay, render_classes_);
        auto store = std::shared_ptr<const data::CompiledDataset>{candidate != nullptr && open_candidate_ ? open_candidate_ : committed_,
                                                                  &dataset.store};
        return gallery_.Begin(plan, visible, window, window_first, std::move(store), dataset.annotated_indices, render_classes_, clean,
                              semantic, stream);
    }

    [[nodiscard]] ExploreGalleryPublication AdvanceGallery() override { return gallery_.Advance(); }
    [[nodiscard]] ExploreStorageFootprint StorageFootprint() const override {
        auto footprint = gallery_.StorageFootprint();
        footprint.host_bytes += render_classes_.capacity() * sizeof(decltype(render_classes_)::value_type);
        return footprint;
    }

    [[nodiscard]] bool HasGalleryTiles() const override { return gallery_.HasReadyTiles(); }

    [[nodiscard]] ExploreGalleryPublication PublishGalleryTiles(const mmltk::frameworks::gpu::ImagePlaneView clean,
                                                                const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                                                const std::uintptr_t stream) override {
        return gallery_.PublishTiles(clean, semantic, stream);
    }

    void RenderDetail(const ExploreRenderPlan& plan, const std::size_t nproc, const mmltk::frameworks::gpu::ImagePlaneView clean,
                      const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream) override {
        RequireOpen();
        if (nproc != nproc_ || !plan.selected_image || !Contains(*plan.selected_image))
            throw contracts::InvalidIntentError("Explore detail selection is unavailable");
        if (gallery_.OutputChange(plan, {}, &committed_->store, {}) != ExploreOutputChange::Unchanged)
            ConfigureClasses(committed_->classes, plan.overlay, render_classes_);
        gallery_.RenderDetail(plan, std::shared_ptr<const data::CompiledDataset>{committed_, &committed_->store},
                              committed_->annotated_indices, render_classes_, clean, semantic, stream);
    }

   private:
    [[nodiscard]] const std::vector<std::uint32_t>& CandidateOrder(const ExploreOrderCandidate* candidate) const {
        if (candidate == nullptr) return committed_->order;
        if (candidate->generation == 0U || candidate->generation != prepared_generation_)
            throw std::logic_error("Explore render candidate is stale");
        return candidate_order_;
    }
    [[nodiscard]] DatasetState& CandidateDataset() {
        if (open_candidate_) return *open_candidate_;
        RequireOpen();
        return *committed_;
    }
    [[nodiscard]] DatasetState& DatasetFor(const ExploreOrderCandidate* candidate) {
        if (candidate != nullptr && open_candidate_) return *open_candidate_;
        RequireOpen();
        return *committed_;
    }
    [[nodiscard]] bool RebuildOrder(DatasetState& dataset, const ExploreFilter& requested, const bool shuffled, const std::uint64_t seed,
                                    std::vector<std::uint32_t>& order, std::vector<std::uint32_t>& scratch,
                                    const std::atomic_bool* cancelled) {
        std::array<bool, data::MAX_CLASSES> enabled{};
        for (const auto index : requested.class_selection.classes)
            if (index < dataset.store.header().num_classes) enabled[index] = true;
        explore::ExploreSampleFilter filter{
            .classes = explore::explore_class_mask(enabled),
            .min_instances = requested.minimum_instances,
            .max_instances = requested.maximum_instances,
            .min_compiled_index = requested.minimum_compiled_index,
            .max_compiled_index = requested.maximum_compiled_index,
            .require_boxes = requested.require_boxes,
            .require_masks = requested.require_masks,
            .restrict_classes = requested.class_selection.mode != ExploreClassSelectionMode::All,
        };
        return explore::rebuild_explore_order(dataset.summaries, filter, shuffled, seed, order, scratch, cancelled, 0U, nullptr,
                                              &gallery_.workers());
    }
    static void ConfigureClasses(const std::span<const explore::ExploreRenderClassDescriptor> source, const ExploreOverlay& overlay,
                                 std::vector<explore::ExploreRenderClassDescriptor>& classes) {
        classes.assign(source.begin(), source.end());
        const bool unrestricted = overlay.class_selection.mode == ExploreClassSelectionMode::All;
        std::size_t visible = 0U;
        for (std::size_t class_id = 0U; class_id != classes.size(); ++class_id) {
            const bool enabled =
                unrestricted || (overlay.class_selection.mode == ExploreClassSelectionMode::Subset &&
                                 std::ranges::binary_search(overlay.class_selection.classes, static_cast<std::uint32_t>(class_id)));
            classes[class_id].visible = static_cast<std::uint8_t>(enabled);
            visible += enabled ? 1U : 0U;
        }
        if (overlay.class_selection.mode == ExploreClassSelectionMode::Subset && visible != overlay.class_selection.classes.size())
            throw contracts::InvalidIntentError("Explore overlay class is outside the dataset");
    }
    void RequireOpen() const {
        if (!committed_) throw contracts::UnavailableError("Explore compiled source is not open");
    }

    ExploreNativeConfiguration configuration_;
    // The gallery retains aliasing owners of this exact store and its immutable
    // annotation catalog. Order promotion never invalidates either product.
    std::shared_ptr<DatasetState> committed_;
    std::shared_ptr<DatasetState> open_candidate_;
    std::vector<std::uint32_t> candidate_order_;
    std::vector<std::uint32_t> candidate_inverse_;
    std::vector<std::uint32_t> candidate_scratch_;
    std::uint64_t candidate_generation_ = 0U;
    std::uint64_t prepared_generation_ = 0U;
    std::size_t nproc_ = 1U;
    std::vector<explore::ExploreRenderClassDescriptor> render_classes_;
    // Join physical readers before releasing their borrowed dataset and catalog.
    GalleryStream gallery_;
};

}  // namespace explore_detail

VisualRuntimeFactory make_native_explore_runtime_factory(const VisualDeviceSettings settings, const std::size_t nproc,
                                                         ExploreNativeConfiguration configuration,
                                                         std::optional<mmltk::frameworks::gpu::DeviceExecution> resolved_execution) {
    if (!settings.valid() || configuration.image_limit == 0U || nproc == 0U || nproc > kExploreMaximumParallelism)
        throw contracts::InvalidIntentError("Explore native configuration is invalid");
    if (resolved_execution && (resolved_execution->device != settings.device || resolved_execution->placement.cpus.empty() ||
                               (settings.numa_node >= 0 && resolved_execution->placement.numa_node != settings.numa_node)))
        throw contracts::InvalidIntentError("Explore resolved placement contradicts selected device settings");
    return [settings, nproc, execution = resolved_execution ? std::move(*resolved_execution) : resolve_visual_device_execution(settings),
            configuration = std::move(configuration)](auto revisions) {
        mmltk::common::system::ScopedExecutionPolicy construction(
            {execution.placement.cpus, {}, 0, execution.placement.numa_node, -10, false});
        return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
            .device = settings.device,
            .model = std::make_unique<explore_detail::NativeExploreAlgorithm>(configuration, nproc, execution, settings.maximum_height),
            .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
            .output_buffer_count = 2U,
            .numa_node = settings.numa_node,
            .execution = execution,
            .product_revisions = std::move(revisions),
        });
    };
}

}  // namespace mmltk::controller
