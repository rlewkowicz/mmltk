#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/image_failure.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "src/common/types/generation.h"
#include "src/controller/services/settings_system.h"
#include "src/common/system/cpu_affinity.h"
#include "src/frameworks/gpu/gdr_mapped_buffer.h"

namespace mmltk::controller {
namespace {

static_assert(std::is_nothrow_move_assignable_v<ExploreSnapshot>);
static_assert(std::is_nothrow_move_constructible_v<ExploreOrderCandidate>);

[[nodiscard]] ExploreAlgorithm& explore_algorithm(mmltk::frameworks::gpu::SystemImageRuntime& runtime) {
    auto* const algorithm = dynamic_cast<ExploreAlgorithm*>(runtime.model());
    if (algorithm == nullptr) throw std::runtime_error("Explore compiled renderer is unavailable");
    return *algorithm;
}

[[nodiscard]] VisualRuntimeFactory bind_explore_demand(VisualRuntimeFactory factory, ExploreDemandCheck demand) {
    if (!factory) return {};
    return [factory = std::move(factory), demand = std::move(demand)](auto revisions) {
        auto runtime = factory(std::move(revisions));
        if (runtime) explore_algorithm(*runtime).SetCurrentDemand(demand);
        return runtime;
    };
}

[[nodiscard]] ExploreRenderPlan make_render_plan(const ExploreSnapshot& requested,
                                                 const mmltk::backend::models::rfdetr::GpuAugmentationConfig& augmentation_config,
                                                 const std::uint64_t dataset_identity, const std::uint64_t generation) {
    return {.viewport = requested.viewport,
            .overlay = requested.overlay,
            .mode = requested.mode,
            .selected_image = requested.selected_image,
            .focused_image = requested.focused_image,
            .augmentation_config = augmentation_config,
            .augmentation = requested.augmentation,
            .detail = requested.detail,
            .dataset_identity = dataset_identity,
            .generation = generation};
}

[[nodiscard]] bool selection_valid(const ExploreClassSelection& selection, const std::uint32_t class_count) {
    if (selection.mode == ExploreClassSelectionMode::All || selection.mode == ExploreClassSelectionMode::None)
        return selection.classes.empty();
    return selection.mode == ExploreClassSelectionMode::Subset && !selection.classes.empty() &&
           selection.classes.size() <= kExploreClassCapacity &&
           std::ranges::all_of(selection.classes, [class_count](const auto value) { return value < class_count; }) &&
           std::ranges::adjacent_find(selection.classes, std::greater_equal{}) == selection.classes.end();
}

[[nodiscard]] bool filter_valid(const ExploreFilter& filter, const std::uint32_t class_count) {
    if (filter.minimum_instances > filter.maximum_instances || filter.minimum_compiled_index > filter.maximum_compiled_index ||
        (filter.order != ExploreOrder::Sequential && filter.order != ExploreOrder::Shuffled))
        return false;
    return selection_valid(filter.class_selection, class_count);
}

[[nodiscard]] bool overlay_valid(const ExploreOverlay& overlay, const std::uint32_t class_count) {
    return selection_valid(overlay.class_selection, class_count);
}

[[nodiscard]] bool range_valid(const ExploreFilter& filter, const std::uint32_t image_count) {
    return image_count != 0U && filter.minimum_compiled_index < image_count &&
           (filter.maximum_compiled_index == std::numeric_limits<std::uint64_t>::max() || filter.maximum_compiled_index < image_count);
}

void require_policy_valid(const ExploreFilterUpdate& policy, const std::uint32_t class_count, const std::uint32_t image_count,
                          const std::string_view detail) {
    if (!filter_valid(policy.filter, class_count) || !overlay_valid(policy.overlay, class_count) ||
        !range_valid(policy.filter, image_count))
        throw contracts::InvalidIntentError(std::string{detail});
}

[[nodiscard]] ExploreFilterUpdate normalize_policy(ExploreFilterUpdate policy, const std::uint32_t class_count,
                                                   const std::uint32_t image_count) {
    const auto normalize_selection = [class_count](ExploreClassSelection& selection) {
        if (selection.mode != ExploreClassSelectionMode::Subset) {
            selection.classes.clear();
            return;
        }
        std::erase_if(selection.classes, [class_count](const auto value) { return value >= class_count; });
        if (selection.classes.empty()) selection.mode = ExploreClassSelectionMode::None;
    };
    normalize_selection(policy.filter.class_selection);
    normalize_selection(policy.overlay.class_selection);
    const auto last = static_cast<std::uint64_t>(image_count - 1U);
    policy.filter.minimum_compiled_index = std::min(policy.filter.minimum_compiled_index, last);
    if (policy.filter.maximum_compiled_index != std::numeric_limits<std::uint64_t>::max())
        policy.filter.maximum_compiled_index = std::min(policy.filter.maximum_compiled_index, last);
    if (policy.filter.maximum_compiled_index < policy.filter.minimum_compiled_index)
        policy.filter.minimum_compiled_index = policy.filter.maximum_compiled_index;
    return policy;
}

class GalleryWakeGate final {
   public:
    explicit GalleryWakeGate(std::function<void()> wake) : wake_(std::move(wake)) {}

    void Invoke() noexcept {
        auto users = users_.load(std::memory_order_acquire);
        do {
            if ((users & kDisconnected) != 0U) return;
        } while (!users_.compare_exchange_weak(users, users + 1U, std::memory_order_acq_rel, std::memory_order_acquire));
        try {
            wake_();
        } catch (...) {}
        users_.fetch_sub(1U, std::memory_order_acq_rel);
        users_.notify_all();
    }

    void Disconnect() noexcept {
        auto users = users_.fetch_or(kDisconnected, std::memory_order_acq_rel) | kDisconnected;
        while (users != kDisconnected) {
            users_.wait(users, std::memory_order_acquire);
            users = users_.load(std::memory_order_acquire);
        }
    }

   private:
    static constexpr std::uint64_t kDisconnected = std::uint64_t{1U} << 63U;
    std::atomic<std::uint64_t> users_{0U};
    const std::function<void()> wake_;
};

}  // namespace

std::size_t normalize_explore_parallelism(const std::size_t requested) {
    const auto allowed = mmltk::common::system::allowed_cpu_set().size();
    const auto available = std::clamp(allowed, std::size_t{1U}, kExploreMaximumParallelism);
    return requested == 0U ? available : std::clamp(requested, std::size_t{1U}, available);
}

std::size_t normalize_explore_parallelism(const std::size_t requested, const mmltk::common::system::ExecutionPlacement& placement) {
    if (placement.cpus.empty()) throw contracts::InvalidIntentError("Explore has no local CPU eligibility");
    return std::clamp(requested == 0U ? placement.cpus.size() : requested, std::size_t{1U}, kExploreMaximumParallelism);
}

class ExploreSystem::Impl final {
   public:
    Impl(SettingsSystem& settings_system, const VisualDeviceSettings settings, const std::size_t nproc, VisualRuntimeFactory factory,
         SystemEventSink<event_type> events, const VisualDiagnosticSink diagnostics)
        : settings_system_(settings_system),
          settings_(settings),
          events_(std::move(events)),
          diagnostics_(diagnostics),
          state_{.nproc = nproc, .maximum_atlas_extent = {settings.maximum_width, settings.maximum_height}},
          gallery_wake_(std::make_shared<GalleryWakeGate>([this] { SubmitGalleryContinuation(); })),
          worker_(
              bind_explore_demand(std::move(factory), ExploreDemandCheck{latest_generation_}),
              [this](const std::exception_ptr failure) { Failed(failure); },
              diagnostics_.valid()
                  ? detail::VisualRuntimeOwner::ActivityObservation{[diagnostics](const detail::VisualRuntimeOwner::ActivityStage stage,
                                                                                  const std::uint64_t value) noexcept {
                        diagnostics.Emit([&] {
                            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                                        .operation = VisualDiagnosticOperation::ExploreContinuationStarted,
                                                        .value = value,
                                                        .detail = 30U + static_cast<std::uint64_t>(stage)};
                        });
                    }}
                  : detail::VisualRuntimeOwner::ActivityObservation{}) {
        if (!settings_.valid() || nproc == 0U || nproc > kExploreMaximumParallelism)
            throw contracts::InvalidIntentError("Explore device settings or nproc are invalid");
        RegisterGalleryContinuation();
    }
    ~Impl() {
        gallery_wake_->Disconnect();
        Shutdown();
    }

    void ExecutionSettingsChanged() noexcept {
        try {
            // Settings persistence can synchronously notify this system while
            // an admission transaction is active. Its completion rechecks facts.
            std::unique_lock admission(desired_admission_mutex_, std::try_to_lock);
            if (!admission.owns_lock()) return;
            const auto selected = settings_system_.explore_settings_candidate();
            ExploreOpen request;
            bool reconstruct = false;
            bool augmentation_changed = false;
            {
                std::scoped_lock lock(mutex_);
                if (committed_source_.empty() || !state_.ready || state_.busy || worker_.busy() || worker_.stopped() || !runtime_settings_)
                    return;
                reconstruct = runtime_settings_->device_id != selected.device_id || runtime_settings_->loading != selected.loading;
                augmentation_changed = state_.augmentation.enabled && augmentation_config_ != selected.augmentation;
                request = {.viewport = state_.viewport, .compiled_source = committed_source_};
            }
            admission.unlock();
            if (reconstruct)
                (void)Open(std::move(request));
            else if (augmentation_changed)
                (void)QueueDesired([](ExploreSnapshot&) {}, 0, false, true);
        } catch (const contracts::BusyError&) {
            // The next completion notification rechecks the saved execution facts.
        } catch (...) { Failed(std::current_exception()); }
    }
    [[nodiscard]] ExploreSnapshot Open(ExploreOpen request) {
        GuardIdle();
        ValidateViewport(request.viewport);
        if (request.compiled_source.empty()) throw contracts::InvalidIntentError("Explore compiled source is unavailable");
        settings_system_.require_loaded();
        ExploreSnapshot admitted;
        {
            std::scoped_lock admission_lock(mutex_);
            RequireIdle();
            auto prior = state_;
            state_.viewport_result.reset();
            Admit();
            const auto generation = ReserveGeneration();
            const auto nproc = state_.nproc;
            const auto selected = settings_system_.explore_settings_candidate();
            const bool reconstruct =
                runtime_settings_ && (runtime_settings_->device_id != selected.device_id || runtime_settings_->loading != selected.loading);
            admitted = QueueAdmitted(
                [this, request = std::move(request), generation, nproc](
                    mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                    const std::stop_token stop) mutable -> detail::VisualRuntimeOwner::Notification {
                    ActivateGeneration(generation);
                    auto& algorithm = explore_algorithm(runtime);
                    auto settings_candidate = settings_system_.explore_settings_candidate();
                    const auto* placement = runtime.execution();
                    if (!algorithm.UsesLoadingOptions(settings_candidate.loading) ||
                        (placement && (placement->device != settings_candidate.device_id ||
                                       (settings_candidate.loading.numa_node >= 0 &&
                                        placement->placement.numa_node != settings_candidate.loading.numa_node))))
                        throw contracts::BusyError("Explore execution settings changed during initialization; reopen Explore");
                    if (!mmltk::backend::models::rfdetr::gpu_augmentation_config_valid(settings_candidate.augmentation))
                        throw contracts::InvalidIntentError("persisted Explore augmentation is invalid");
                    bool had_committed_product;
                    {
                        std::scoped_lock state_lock(mutex_);
                        had_committed_product = state_.ready && state_.frame.valid();
                    }
                    const auto cancel_open = [&]() { return RollbackOpenCancellation(algorithm, had_committed_product); };
                    try {
                        if (had_committed_product) algorithm.PrepareOutputPublication(ExploreOutputChange::Initialize);
                        ExploreOpened opened = algorithm.Open(request.compiled_source, stop);
                        if (opened.dataset.image_count == 0U) return cancel_open();
                        opened.dataset.class_catalog_identity = explore_class_catalog_identity(opened.dataset.class_names);
                        auto preferences =
                            normalize_policy(settings_candidate.preferences.policy,
                                             static_cast<std::uint32_t>(opened.dataset.class_names.size()), opened.dataset.image_count);
                        const bool catalog_changed =
                            settings_candidate.preferences.class_catalog_identity != opened.dataset.class_catalog_identity;
                        if (catalog_changed) {
                            preferences.filter.class_selection = {};
                            preferences.overlay.class_selection = {};
                        }
                        require_policy_valid(preferences, static_cast<std::uint32_t>(opened.dataset.class_names.size()),
                                             opened.dataset.image_count, "persisted Explore filter is invalid");
                        std::uint64_t seed;
                        {
                            std::scoped_lock policy_lock(mutex_);
                            seed = ResolveSeed(preferences.filter, false);
                        }
                        auto candidate = algorithm.PrepareFilter(preferences.filter, seed, nproc, stop);
                        if (stop.stop_requested()) return cancel_open();
                        const auto viewport = ClampViewport(request.viewport, candidate.order.matching_count);
                        ExploreRenderPlan plan{
                            .viewport = viewport,
                            .overlay = preferences.overlay,
                            .augmentation_config = settings_candidate.augmentation,
                            .augmentation = {.enabled = settings_candidate.augmentation_preview_enabled, .seed = 0U},
                            .detail = {.show_original_dimensions = settings_candidate.show_original_dimensions},
                            .dataset_identity = opened.dataset_identity,
                            .generation = generation,
                        };
                        auto rendered = Render(runtime, algorithm, plan, generation, &candidate, stop, true);
                        if (!rendered || stop.stop_requested()) return cancel_open();
                        candidate.order = algorithm.Visible(viewport, &candidate);
                        opened.order = candidate.order;
                        ExploreSnapshot settled;
                        {
                            std::scoped_lock finalization_lock(mutex_);
                            settled = state_;
                        }
                        settled.dataset = std::move(opened.dataset);
                        settled.dataset.identity = opened.dataset_identity;
                        settled.order = std::move(opened.order);
                        settled.viewport = viewport;
                        settled.filter = preferences.filter;
                        settled.overlay = preferences.overlay;
                        settled.augmentation = plan.augmentation;
                        settled.detail = plan.detail;
                        settled.mode = ExploreMode::Gallery;
                        settled.selected_image.reset();
                        settled.focused_image.reset();
                        CompleteProduct(settled, *rendered);
                        ExploreSettingsCandidate runtime_settings;
                        const auto catalog_identity = settled.dataset.class_catalog_identity;
                        auto notification = CommitPreparedProduct(
                            runtime, algorithm, std::move(*rendered), std::move(settled),
                            [&] {
                                if (catalog_changed) {
                                    settings_system_.persist_explore_class_catalog(settings_candidate, catalog_identity, preferences);
                                    settings_candidate = settings_system_.explore_settings_candidate();
                                } else if (settings_system_.explore_settings_candidate().version != settings_candidate.version) {
                                    throw contracts::BusyError("Explore settings candidate is stale");
                                }
                                runtime_settings = settings_candidate;
                            },
                            [&] noexcept {
                                algorithm.Commit(std::move(candidate));
                                committed_source_ = std::move(request.compiled_source);
                                runtime_settings_ = std::move(runtime_settings);
                                settings_.device = settings_candidate.device_id;
                                settings_.numa_node = settings_candidate.loading.numa_node;
                                installed_settings_ = std::move(settings_candidate);
                                augmentation_config_ = plan.augmentation_config;
                            });
                        if (!notification) return cancel_open();
                        return notification;
                    } catch (...) {
                        if (mmltk::frameworks::gpu::is_image_execution_failure(std::current_exception())) throw;
                        if (!had_committed_product) {
                            RollbackOutput(algorithm);
                            algorithm.DiscardCandidate();
                            throw;
                        }
                        return RestoreCommittedProduct(
                            algorithm, visual_failure_detail(std::current_exception(), "Explore dataset candidate failed"), false, true);
                    }
                },
                std::move(prior), reconstruct);
        }
        return admitted;
    }

    void UpdateViewport(const ExploreViewportUpdate request) {
        if (!request.valid()) throw contracts::InvalidIntentError("Explore viewport requires exact square cells");
        const auto outcome =
            !request.viewport.valid() ? ExploreViewportOutcome::VisibleCapacityExceeded
            : request.viewport.extent.width > settings_.maximum_width || request.viewport.extent.height > settings_.maximum_height
                ? ExploreViewportOutcome::AtlasExtentExceeded
                : ExploreViewportOutcome::Ready;
        if (outcome != ExploreViewportOutcome::Ready) {
            ExploreSnapshot changed;
            {
                std::scoped_lock admission(desired_admission_mutex_);
                std::scoped_lock lock(mutex_);
                RequireReady();
                state_.viewport_result = ExploreViewportResult{request, outcome};
                AdvanceRevision();
                changed = state_;
            }
            Publish(ExploreChanged{std::move(changed)});
            return;
        }
        ValidateViewport(request.viewport);
        static_cast<void>(QueueDesired(
            [&](ExploreSnapshot& desired) {
                desired.viewport = ClampViewport(request.viewport, desired.order.matching_count);
                desired.focused_image = request.focused_compiled_index;
                desired.viewport_result = ExploreViewportResult{request, ExploreViewportOutcome::Ready};
            },
            0, false, false, true));
    }

    [[nodiscard]] std::uint64_t LastInteractionGeneration() const {
        std::scoped_lock lock(mutex_);
        return latest_generation_->load(std::memory_order_acquire);
    }

    [[nodiscard]] ExploreSnapshot UpdateFilter(const ExploreFilterUpdate request) {
        if (request.filter == snapshot().filter) return UpdateOverlay(request.overlay);
        GuardIdle();
        std::uint64_t seed;
        {
            std::scoped_lock lock(mutex_);
            RequireReady();
            require_policy_valid(request, static_cast<std::uint32_t>(state_.dataset.class_names.size()), state_.dataset.image_count,
                                 "Explore filter is invalid");
            seed = ResolveSeed(request.filter, false);
        }
        settings_system_.require_loaded();
        return SubmitCandidateMutation(
            [filter = request.filter](ExploreAlgorithm& algorithm, const std::size_t nproc, const std::uint64_t resolved_seed,
                                      const std::stop_token stop) { return algorithm.PrepareFilter(filter, resolved_seed, nproc, stop); },
            [request](ExploreSnapshot& state, ExploreOrderFacts order) {
                state.filter = request.filter;
                state.overlay = request.overlay;
                state.order = std::move(order);
                state.mode = ExploreMode::Gallery;
                state.selected_image.reset();
                state.focused_image.reset();
            },
            [this, request](const ExploreSettingsCandidate& installed) -> std::optional<ExploreSettingsCandidate> {
                return settings_system_.persist_explore_filter(installed, request);
            },
            seed, request.overlay, "Explore filter candidate failed");
    }

    [[nodiscard]] ExploreSnapshot Reroll() {
        GuardIdle();
        ExploreFilter filter;
        std::uint64_t shuffle_seed;
        {
            std::scoped_lock lock(mutex_);
            RequireReady();
            if (state_.filter.order != ExploreOrder::Shuffled) throw contracts::UnavailableError("Explore reroll requires shuffled order");
            filter = state_.filter;
            shuffle_seed = NextShuffleSeed();
        }
        return SubmitCandidateMutation(
            [filter](ExploreAlgorithm& algorithm, const std::size_t nproc, const std::uint64_t seed, const std::stop_token stop) {
                return algorithm.PrepareFilter(filter, seed, nproc, stop);
            },
            [](auto& state, ExploreOrderFacts order) {
                state.order = std::move(order);
                state.mode = ExploreMode::Gallery;
                state.selected_image.reset();
                state.focused_image.reset();
            },
            [](const ExploreSettingsCandidate&) { return std::optional<ExploreSettingsCandidate>{}; }, shuffle_seed, std::nullopt,
            // CLEANUP-IGNORE: This candidate-failure label closes Reroll; detail update starts an independent endpoint.
            "Explore order candidate failed");
    }

    [[nodiscard]] ExploreSnapshot UpdateAugmentation(const ExploreAugmentationUpdate request) {
        return UpdateRenderSetting([request](ExploreSnapshot& state) { state.augmentation.enabled = request.enabled; },
                                   [this, request](const ExploreSettingsCandidate& candidate) {
                                       return settings_system_.persist_explore_augmentation(candidate, request.enabled);
                                   });
    }
    [[nodiscard]] ExploreSnapshot UpdateOverlay(const ExploreOverlay request) {
        std::unique_lock transaction(desired_admission_mutex_);
        std::optional<ExploreFilterUpdate> label_only;
        bool augmentation = false;
        {
            std::scoped_lock lock(mutex_);
            RequireReady();
            if (!overlay_valid(request, static_cast<std::uint32_t>(state_.dataset.class_names.size())))
                throw contracts::InvalidIntentError("Explore overlay is invalid");
            const auto& target = desired_ ? *desired_ : state_;
            auto semantics = request;
            semantics.show_labels = target.overlay.show_labels;
            if (semantics == target.overlay) {
                label_only = ExploreFilterUpdate{.filter = target.filter, .overlay = request};
                augmentation = target.augmentation.enabled;
            }
        }
        if (!label_only) {
            transaction.unlock();
            return QueueDesired([&](ExploreSnapshot& desired) { desired.overlay = request; }, 0, true);
        }
        auto refreshed = settings_system_.persist_explore_product(settings_system_.explore_settings_candidate(), *label_only, augmentation);
        ExploreSnapshot changed;
        {
            std::scoped_lock lock(mutex_);
            installed_settings_ = std::move(refreshed);
            if (desired_settings_) desired_settings_ = installed_settings_;
            state_.overlay.show_labels = request.show_labels;
            if (desired_) desired_->overlay.show_labels = request.show_labels;
            AdvanceRevision();
            changed = state_;
        }
        transaction.unlock();
        Publish(ExploreChanged{changed});
        ExecutionSettingsChanged();
        return changed;
    }

    [[nodiscard]] ExploreSnapshot RerollAugmentation() {
        return QueueDesired(
            [](ExploreSnapshot& desired) {
                if (!desired.augmentation.enabled) throw contracts::UnavailableError("Explore augmentation reroll requires preview");
                desired.augmentation.seed = mmltk::common::types::advance_monotonic_identity(desired.augmentation.seed);
            },
            0, false, true);
    }

    [[nodiscard]] ExploreSnapshot UpdateDetail(const ExploreDetailUpdate request) {
        std::unique_lock transaction(desired_admission_mutex_);
        auto refreshed =
            settings_system_.persist_explore_detail(settings_system_.explore_settings_candidate(), request.show_original_dimensions);
        ExploreSnapshot changed;
        {
            std::scoped_lock lock(mutex_);
            installed_settings_ = std::move(refreshed);
            if (desired_settings_) desired_settings_ = installed_settings_;
            state_.detail.show_original_dimensions = request.show_original_dimensions;
            if (desired_) desired_->detail = state_.detail;
            AdvanceRevision();
            changed = state_;
        }
        transaction.unlock();
        Publish(ExploreChanged{changed});
        ExecutionSettingsChanged();
        return changed;
    }

    [[nodiscard]] ExploreSnapshot Select(const ExploreSelect request) {
        return QueueDesired([&](ExploreSnapshot& desired) {
            if (std::ranges::find(desired.order.visible_indices, request.compiled_index) == desired.order.visible_indices.end())
                throw contracts::InvalidIntentError("Explore selection is outside the visible filtered order");
            desired_navigation_ = 0;
            desired.selected_image = request.compiled_index;
            desired.mode = ExploreMode::Detail;
        });
    }

    [[nodiscard]] ExploreSnapshot Navigate(const ExploreNavigate request) {
        if (request.direction != ExploreNavigation::Previous && request.direction != ExploreNavigation::Next)
            throw contracts::InvalidIntentError("Explore navigation direction is invalid");
        return QueueDesired(
            [](ExploreSnapshot& desired) {
                if (!desired.selected_image) throw contracts::UnavailableError("Explore detail selection is unavailable");
                desired.mode = ExploreMode::Detail;
            },
            request.direction == ExploreNavigation::Next ? 1 : -1);
    }

    [[nodiscard]] ExploreSnapshot CloseDetail() {
        return QueueDesired([](ExploreSnapshot& desired) {
            desired.selected_image.reset();
            desired.mode = ExploreMode::Gallery;
        });
    }

    [[nodiscard]] ExploreSnapshot Stop() noexcept {
        std::scoped_lock transaction(desired_admission_mutex_);
        std::scoped_lock open_finalization_lock(open_finalization_mutex_);
        ExploreSnapshot result;
        {
            std::scoped_lock lock(mutex_);
            // Stop abandons further thumbnails; the selected product retains
            // its readiness facts independently of this stream-demand decision.
            active_gallery_generation_ = 0U;
            latest_generation_->store(ReserveGeneration(), std::memory_order_release);
            if (desired_) {
                desired_.reset();
                desired_settings_.reset();
                desired_persist_ = false;
                desired_navigation_ = 0;
                AdvanceRevision();
            }
            if (state_.busy && !state_.cancellation_requested) {
                state_.cancellation_requested = true;
                AdvanceRevision();
            }
            result = state_;
        }
        if (worker_.RequestActiveStop()) {
            std::scoped_lock lock(mutex_);
            result = state_;
        }
        // CLEANUP-IGNORE: Explore owns different stop admission and typed snapshot state from Annotation; their common
        // worker and checked-borrow mechanics are already consolidated.
        return result;
    }

    void Shutdown() noexcept {
        std::uint64_t generation;
        {
            std::scoped_lock lock(mutex_);
            generation = ReserveGeneration();
            latest_generation_->store(generation, std::memory_order_release);
        }
        worker_.StopAndWait();
        // A cancelled replacement may restore the incumbent while joining.
        // Reapply the same issued invalidation before releasing the issuer.
        std::scoped_lock lock(mutex_);
        latest_generation_->store(generation, std::memory_order_release);
    }
    [[nodiscard]] bool stopped() const noexcept { return worker_.stopped(); }
    [[nodiscard]] ExploreSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        return state_;
    }
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const {
        VisualFrame published;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.ready) return {};
            published = state_.frame;
        }

        return borrow_matching_visual_product(published, worker_);
    }
    [[nodiscard]] VisualDocumentRead BorrowDocument(const VisualFrame& frame) const {
        std::shared_ptr<const VisualDocument> document;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.ready || state_.mode != ExploreMode::Detail || state_.frame != frame) return {};
            document = document_;
        }
        return {borrow_matching_visual_product(frame, worker_), std::move(document)};
    }

   private:
    template <class Install, class Persist>
    [[nodiscard]] ExploreSnapshot UpdateRenderSetting(Install install, Persist persist) {
        bool ready;
        {
            std::scoped_lock lock(mutex_);
            RequireIdle();
            ready = state_.ready;
        }
        if (ready) return QueueDesired(std::move(install), 0, true, true);
        auto refreshed = persist(settings_system_.explore_settings_candidate());
        std::scoped_lock lock(mutex_);
        installed_settings_ = std::move(refreshed);
        install(state_);
        AdvanceRevision();
        return state_;
    }

    template <class Install>
    [[nodiscard]] ExploreSnapshot QueueDesired(Install install, const std::int64_t navigation = 0, const bool persist = false,
                                               const bool check_settings = false, const bool viewport_update = false) {
        std::scoped_lock desired_admission(desired_admission_mutex_);
        std::scoped_lock admission_lock(mutex_);
        RequireReady();
        if (!desired_) {
            desired_ = state_;
            desired_augmentation_config_ = augmentation_config_;
        }
        const auto previous_viewport = desired_->viewport;
        const auto previous_selection = desired_->selected_image;
        install(*desired_);
        if (persist || check_settings) {
            desired_settings_ = settings_system_.explore_settings_candidate();
            if (check_settings) desired_augmentation_config_ = desired_settings_->augmentation;
        }
        desired_persist_ = desired_persist_ || persist;
        if (desired_->selected_image != previous_selection) desired_navigation_ = 0;
        const auto count = static_cast<std::int64_t>(std::max(1U, desired_->order.matching_count));
        desired_navigation_ = (desired_navigation_ + navigation) % count;
        const auto offset = desired_navigation_;
        const auto generation = viewport_update && previous_viewport == desired_->viewport && active_gallery_generation_ != 0U &&
                                        latest_generation_->load(std::memory_order_acquire) != 0U
                                    ? latest_generation_->load(std::memory_order_acquire)
                                    : NextGeneration();
        auto requested = *desired_;
        auto plan = make_render_plan(requested, desired_augmentation_config_, state_.dataset.identity, generation);
        if (!worker_.SubmitLatest([this, plan, requested = std::move(requested), generation, offset, settings = desired_settings_,
                                   persist = desired_persist_](mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                                               std::stop_token stop) mutable -> detail::VisualRuntimeOwner::Notification {
                std::uint64_t committed_generation = generation;
                {
                    std::scoped_lock lock(mutex_);
                    if (generation != latest_generation_->load(std::memory_order_acquire)) return {};
                }
                auto& algorithm = explore_algorithm(runtime);
                try {
                    auto order = algorithm.Visible(plan.viewport);
                    if (plan.focused_image &&
                        std::ranges::find(order.visible_indices, *plan.focused_image) == order.visible_indices.end()) {
                        plan.focused_image.reset();
                        requested.focused_image.reset();
                        std::scoped_lock lock(mutex_);
                        if (generation == latest_generation_->load(std::memory_order_acquire) && desired_) desired_->focused_image.reset();
                    }
                    if (offset != 0 && requested.selected_image) {
                        const auto selected = algorithm.Adjacent(*requested.selected_image, offset);
                        if (!selected) throw contracts::UnavailableError("Explore filtered order is empty");
                        std::scoped_lock lock(mutex_);
                        if (generation != latest_generation_->load(std::memory_order_acquire)) return {};
                        requested.selected_image = selected;
                        plan.selected_image = selected;
                        desired_->selected_image = selected;
                        desired_navigation_ = 0;
                    }
                    auto rendered = Render(runtime, algorithm, plan, generation, nullptr, stop);
                    std::unique_lock transaction(desired_admission_mutex_);
                    bool superseded = false;
                    {
                        std::scoped_lock lock(mutex_);
                        if (generation != latest_generation_->load(std::memory_order_acquire) || stop.stop_requested()) {
                            if (desired_ || state_.busy)
                                superseded = true;
                            else
                                committed_generation = latest_generation_->load(std::memory_order_acquire);
                        } else {
                            if (desired_) requested.overlay.show_labels = desired_->overlay.show_labels;
                            if (settings && desired_settings_ && desired_augmentation_config_ == plan.augmentation_config) {
                                // View-only preferences can refresh the checked
                                // settings version while these pixels are pending.
                                settings = desired_settings_;
                            }
                        }
                    }
                    if (superseded) {
                        transaction.unlock();
                        RollbackOutput(algorithm);
                        return {};
                    }
                    if (committed_generation != generation || !rendered) {
                        transaction.unlock();
                        return RestoreCommittedProduct(algorithm);
                    }
                    {
                        std::scoped_lock lock(mutex_);
                        requested.order = std::move(order);
                        requested.detail = state_.detail;
                        requested.viewport_result = state_.viewport_result;
                        requested.revision = state_.revision;
                        const bool discrete_pending = state_.busy;
                        CompleteProduct(requested, *rendered);
                        requested.busy = discrete_pending;
                    }
                    const ExploreFilterUpdate policy{.filter = requested.filter, .overlay = requested.overlay};
                    const auto augmentation = requested.augmentation.enabled;
                    auto notification = CommitPreparedProduct(
                        runtime, algorithm, std::move(*rendered), std::move(requested),
                        [&] {
                            if (settings && persist)
                                settings = settings_system_.persist_explore_product(*settings, policy, augmentation);
                            else if (settings && settings_system_.explore_settings_candidate().version != settings->version)
                                throw contracts::BusyError("Explore settings candidate is stale");
                        },
                        [&] noexcept {
                            augmentation_config_ = plan.augmentation_config;
                            if (settings) installed_settings_ = std::move(*settings);
                            desired_.reset();
                            desired_settings_.reset();
                            desired_persist_ = false;
                        });
                    if (!notification) {
                        transaction.unlock();
                        return RestoreCommittedProduct(algorithm);
                    }
                    return notification;
                } catch (...) {
                    if (mmltk::frameworks::gpu::is_image_execution_failure(std::current_exception())) throw;
                    const auto detail = visual_failure_detail(std::current_exception(), "Explore render mutation failed");
                    std::unique_lock transaction(desired_admission_mutex_);
                    bool superseded = false;
                    {
                        std::scoped_lock lock(mutex_);
                        superseded = generation != latest_generation_->load(std::memory_order_acquire) && (desired_ || state_.busy);
                        if (!superseded) {
                            desired_.reset();
                            desired_settings_.reset();
                            desired_persist_ = false;
                            desired_navigation_ = 0;
                        }
                    }
                    if (superseded) {
                        transaction.unlock();
                        RollbackOutput(algorithm);
                        return {};
                    }
                    transaction.unlock();
                    return RestoreCommittedProduct(algorithm, detail, true);
                }
            })) {
            desired_.reset();
            desired_settings_.reset();
            desired_persist_ = false;
            desired_navigation_ = 0;
            throw contracts::UnavailableError("Explore desired-product ingress is stopped");
        }
        if (viewport_update) {
            state_.viewport_result = desired_->viewport_result;
            AdvanceRevision();
        }
        return state_;
    }

    template <class Prepare, class Install, class Persist>
    [[nodiscard]] ExploreSnapshot SubmitCandidateMutation(Prepare prepare, Install install, Persist persist, const std::uint64_t seed,
                                                          const std::optional<ExploreOverlay> render_overlay,
                                                          const std::string_view failure_fallback) {
        return QueueReadyMutation([this, prepare = std::move(prepare), install = std::move(install), persist = std::move(persist), seed,
                                   render_overlay,
                                   failure_fallback](const std::uint64_t generation, mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                                     const std::stop_token stop) mutable -> detail::VisualRuntimeOwner::Notification {
            auto execution = BeginExecution(generation);
            auto& render_plan = execution.requested;
            auto& algorithm = explore_algorithm(runtime);
            try {
                algorithm.PrepareOutputPublication(ExploreOutputChange::Initialize);
                auto candidate = prepare(algorithm, execution.nproc, seed, stop);
                if (stop.stop_requested()) return RestoreCommittedProduct(algorithm, {}, false, true);
                render_plan.viewport = ClampViewport(render_plan.viewport, candidate.order.matching_count);
                if (render_overlay) render_plan.overlay = *render_overlay;
                auto rendered = RenderGallery(runtime, algorithm, render_plan, candidate, generation, stop);
                if (!rendered || stop.stop_requested()) return RestoreCommittedProduct(algorithm, {}, false, true);
                bool stale;
                {
                    std::scoped_lock decision_lock(mutex_);
                    stale = stop.stop_requested() || generation != latest_generation_->load(std::memory_order_acquire);
                }
                if (stale) return RestoreCommittedProduct(algorithm, {}, false, true);
                ExploreSnapshot settled;
                {
                    std::scoped_lock snapshot_lock(mutex_);
                    settled = state_;
                }
                install(settled, candidate.order);
                settled.viewport = render_plan.viewport;
                CompleteProduct(settled, *rendered);
                std::optional<ExploreSettingsCandidate> refreshed_settings;
                auto notification = CommitPreparedProduct(
                    runtime, algorithm, std::move(*rendered), std::move(settled), [&] { refreshed_settings = persist(execution.settings); },
                    [&] noexcept {
                        algorithm.Commit(std::move(candidate));
                        if (refreshed_settings) installed_settings_ = std::move(*refreshed_settings);
                    });
                if (!notification) return RestoreCommittedProduct(algorithm, {}, false, true);
                return notification;
            } catch (...) {
                if (mmltk::frameworks::gpu::is_image_execution_failure(std::current_exception())) throw;
                return RestoreCommittedProduct(algorithm, visual_failure_detail(std::current_exception(), failure_fallback), false, true);
            }
        });
    }

    template <class Work>
    [[nodiscard]] ExploreSnapshot QueueReadyMutation(Work work) {
        std::scoped_lock lock(mutex_);
        RequireReady();
        auto prior = state_;
        Admit();
        const auto generation = ReserveGeneration();
        return QueueAdmitted(
            [work = std::move(work), generation](mmltk::frameworks::gpu::SystemImageRuntime& runtime, const std::stop_token stop) mutable {
                return work(generation, runtime, stop);
            },
            std::move(prior));
    }

    struct PreparedProduct final {
        mmltk::frameworks::gpu::SystemImageRuntime::OutputCandidate output;
        mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput retained;
        VisualFrame frame{};
        ExploreGalleryReadiness gallery{};
        std::shared_ptr<const VisualDocument> document;
        std::vector<ExploreLabel> labels;
    };

    template <class Persist, class Install>
    [[nodiscard]] detail::VisualRuntimeOwner::Notification CommitPreparedProduct(mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                                                                 ExploreAlgorithm& algorithm, PreparedProduct&& product,
                                                                                 ExploreSnapshot settled, Persist persist,
                                                                                 Install install) {
        static_assert(std::is_nothrow_invocable_v<Install>);
        // Every allocating projection is prepared while the incumbent is still
        // selected. Stop and finalization share this boundary for every route.
        auto notification = ChangedNotification(settled);
        std::scoped_lock finalization(open_finalization_mutex_);
        if (!worker_.TryCompleteActiveWork()) return {};
        persist();
        const bool continue_gallery = product.gallery.generation != 0U;
        {
            std::scoped_lock lock(mutex_);
            if (product.output.valid())
                runtime.CommitOutput(std::move(product.output));
            else
                runtime.SelectOutput(product.retained);
            algorithm.CommitOutputPublication();
            install();
            document_ = std::move(product.document);
            active_gallery_generation_ = product.gallery.generation;
            state_ = std::move(settled);
        }
        if (continue_gallery) SubmitGalleryContinuation();
        return notification;
    }

    [[nodiscard]] std::optional<PreparedProduct> Render(mmltk::frameworks::gpu::SystemImageRuntime& runtime, ExploreAlgorithm& algorithm,
                                                        ExploreRenderPlan& plan, const std::uint64_t generation,
                                                        const ExploreOrderCandidate* candidate = nullptr, const std::stop_token stop = {},
                                                        const bool new_artifact = false) {
        if (stop.stop_possible() && stop.stop_requested()) return std::nullopt;
        // This worker is the sole issuer. Demand and presentation changes do
        // not invalidate native semantics, and failed candidates cannot recycle
        // an identity previously attached to retained cache pixels.
        if (semantic_identity_ == 0U || semantic_overlay_.show_boxes != plan.overlay.show_boxes ||
            semantic_overlay_.show_masks != plan.overlay.show_masks || semantic_overlay_.class_selection != plan.overlay.class_selection) {
            const auto identity = mmltk::common::types::take_monotonic_identity(next_semantic_identity_);
            semantic_overlay_ = plan.overlay;
            semantic_identity_ = identity;
        }
        plan.semantic_identity = semantic_identity_;
        if (configured_algorithm_ != &algorithm) {
            algorithm.SetGalleryReadySink(ExploreAlgorithm::GalleryReadySink{[wake = std::weak_ptr{gallery_wake_}] {
                if (const auto gate = wake.lock()) gate->Invoke();
            }});
            runtime.SetOutputAvailableSink([wake = std::weak_ptr{gallery_wake_}] {
                if (const auto gate = wake.lock()) gate->Invoke();
            });
            configured_algorithm_ = &algorithm;
        }
        const auto change = algorithm.OutputChange(plan, candidate);
        algorithm.PrepareOutputPublication(change);
        ExploreGalleryPublication publication;
        const auto product_extent = ProductExtent(algorithm, plan);
        std::size_t nproc = 0U;
        std::uint64_t previous_clean_revision = 0U;
        std::shared_ptr<const VisualDocument> previous_document;
        bool preserve_gallery_clean = false;
        const bool has_committed_physical_product = runtime.Completed().valid();
        const auto candidate_visible = plan.mode == ExploreMode::Gallery && candidate != nullptr
                                           ? algorithm.Visible(plan.viewport, candidate).visible_indices
                                           : std::vector<std::uint32_t>{};
        {
            std::scoped_lock lock(mutex_);
            nproc = state_.nproc;
            previous_clean_revision = state_.frame.clean_revision;
            previous_document = document_;
            const bool same_gallery_clean = state_.ready && state_.mode == ExploreMode::Gallery && plan.mode == ExploreMode::Gallery &&
                                            plan.dataset_identity == state_.dataset.identity && plan.viewport == state_.viewport &&
                                            plan.augmentation_config == augmentation_config_ && plan.augmentation == state_.augmentation;
            preserve_gallery_clean = !new_artifact && has_committed_physical_product && same_gallery_clean &&
                                     (candidate == nullptr || candidate_visible == state_.order.visible_indices);
        }
        mmltk::frameworks::gpu::SystemImageRuntime::OutputCandidate output;
        mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput retained;
        if (change == ExploreOutputChange::Unchanged) {
            retained = runtime.Completed();
            if (plan.mode == ExploreMode::Gallery)
                publication = algorithm.BeginGallery(plan, candidate, nproc, {}, {}, 0U);
            else
                algorithm.RenderDetail(plan, nproc, {}, {}, 0U);
        } else {
            output = runtime.AcquireOutput(stop, change == ExploreOutputChange::Semantic
                                                     ? runtime.Completed()
                                                     : mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput{});
            if (!output.valid()) return std::nullopt;
            runtime.Publish(output, product_extent.width, product_extent.height,
                            [&](const auto clean, const auto semantic, const auto stream) {
                                if (plan.mode == ExploreMode::Gallery)
                                    publication = algorithm.BeginGallery(plan, candidate, nproc, clean, semantic, stream);
                                else
                                    algorithm.RenderDetail(plan, nproc, clean, semantic, stream);
                            });
        }
        DiagnoseStorage(runtime, algorithm, generation);
        auto document = plan.mode == ExploreMode::Detail ? algorithm.Document() : nullptr;
        const bool clean_changed = plan.mode == ExploreMode::Gallery ? !preserve_gallery_clean : !document || document != previous_document;
        // A changed clean product takes its identity from the producer's
        // lifetime sequence; semantic-only output retains the selected identity.
        const auto revision = retained.valid() ? retained.revision() : output.revision();
        const auto clean_revision = clean_changed ? revision : previous_clean_revision;
        auto frame = visual_frame({PresentationSourceKind::Explore, 1U}, product_extent, revision);
        frame.content = plan.mode == ExploreMode::Detail ? algorithm.DetailContent(plan) : VisualRegion{};
        frame.clean_revision = clean_revision;
        PreparedProduct rendered{
            .output = std::move(output),
            .retained = std::move(retained),
            .frame = frame,
            .gallery = plan.mode == ExploreMode::Gallery
                           ? ExploreGalleryReadiness{.generation = publication.generation, .slots = publication.ready_slots}
                           : ExploreGalleryReadiness{},
            .document = std::move(document),
            .labels = change == ExploreOutputChange::Unchanged ? std::vector<ExploreLabel>{} : algorithm.Labels(),
        };
        if (plan.mode == ExploreMode::Gallery) {
            bool stale = false;
            {
                std::scoped_lock lock(mutex_);
                stale = generation != latest_generation_->load(std::memory_order_acquire);
            }
            if (stale) { return std::nullopt; }
            if (change != ExploreOutputChange::Unchanged)
                diagnostics_.Emit([&] {
                    return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                                .operation = VisualDiagnosticOperation::PlaceholderPublished,
                                                .device = settings_.device,
                                                .generation = generation,
                                                .value = plan.viewport.first_row,
                                                .context = {.capacity_width = plan.viewport.extent.width,
                                                            .capacity_height = plan.viewport.extent.height,
                                                            .staging_bytes = publication.active_pinned_bytes}};
                });
            if (change != ExploreOutputChange::Unchanged && publication.cumulative_tiles != 0U)
                diagnostics_.Emit([&] {
                    return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                                .operation = VisualDiagnosticOperation::TileBatchPublished,
                                                .device = settings_.device,
                                                .generation = generation,
                                                .value = publication.cumulative_tiles,
                                                .detail = publication.reused_tiles,
                                                .context = {.staging_bytes = publication.active_pinned_bytes}};
                });
        } else {
            rendered.gallery = {};
        }
        if (change != ExploreOutputChange::Unchanged) DiagnoseFrame(rendered.frame, generation);
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                        .operation = VisualDiagnosticOperation::RenderCompleted,
                                        .device = settings_.device,
                                        .generation = generation,
                                        .value = nproc,
                                        .detail = (plan.augmentation.enabled ? 1U : 0U) | (plan.augmentation_config.enabled ? 2U : 0U)};
        });
        return rendered;
    }
    void SubmitGalleryContinuation() noexcept { static_cast<void>(worker_.NotifyContinuation()); }
    void RegisterGalleryContinuation() {
        worker_.RegisterContinuation(
            [this](mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                   const std::stop_token stop) -> detail::VisualRuntimeOwner::Notification {
                diagnostics_.Emit([&] {
                    return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                                .operation = VisualDiagnosticOperation::ExploreContinuationStarted,
                                                .device = settings_.device};
                });
                if (stop.stop_requested()) return {};
                auto& algorithm = explore_algorithm(runtime);
                std::uint64_t generation;
                VisualExtent extent;
                {
                    std::scoped_lock lock(mutex_);
                    generation = active_gallery_generation_;
                    if (generation == 0U || generation != latest_generation_->load(std::memory_order_acquire) ||
                        state_.mode != ExploreMode::Gallery)
                        return {};
                    extent = state_.viewport.extent;
                }
                const auto advanced = algorithm.AdvanceGallery();
                DiagnoseStorage(runtime, algorithm, generation);
                diagnostics_.Emit([&] {
                    return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                                .operation = VisualDiagnosticOperation::ExploreContinuationStarted,
                                                .device = settings_.device,
                                                .generation = advanced.generation,
                                                .value = advanced.cumulative_tiles,
                                                .detail = 20U,
                                                .context = {.capacity_width = static_cast<std::uint32_t>(advanced.remaining_tiles),
                                                            .staging_bytes = advanced.active_pinned_bytes}};
                });
                if (stop.stop_requested()) return {};
                if (advanced.stale_discarded != 0U)
                    diagnostics_.Emit([&] {
                        return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                                    .operation = VisualDiagnosticOperation::StaleThumbnailDiscarded,
                                                    .device = settings_.device,
                                                    .generation = advanced.generation,
                                                    .value = advanced.stale_discarded,
                                                    .context = {.capacity_width = extent.width,
                                                                .capacity_height = extent.height,
                                                                .staging_bytes = advanced.active_pinned_bytes}};
                    });
                {
                    std::scoped_lock lock(mutex_);
                    if (generation != latest_generation_->load(std::memory_order_acquire) || generation != active_gallery_generation_ ||
                        advanced.generation != generation || state_.mode != ExploreMode::Gallery)
                        return {};
                }
                if (stop.stop_requested()) return {};
                {
                    std::scoped_lock lock(mutex_);
                    if (advanced.ready_slots != state_.gallery.slots) {
                        auto changed = state_;
                        changed.gallery = {.generation = advanced.generation, .slots = advanced.ready_slots};
                        changed.labels = algorithm.Labels();
                        changed.revision = mmltk::common::types::advance_monotonic_identity(changed.revision);
                        auto notification = ChangedNotification(changed);
                        state_ = std::move(changed);
                        diagnostics_.Emit([&] {
                            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                                        .operation = VisualDiagnosticOperation::TileBatchPublished,
                                                        .device = settings_.device,
                                                        .generation = generation,
                                                        .value = advanced.cumulative_tiles,
                                                        .detail = advanced.reused_tiles,
                                                        .context = {.capacity_width = extent.width,
                                                                    .capacity_height = extent.height,
                                                                    .staging_bytes = advanced.active_pinned_bytes}};
                        });
                        // Completion freed physical lanes. Preserve this capacity
                        // event while the current notification leaves the worker.
                        SubmitGalleryContinuation();
                        return notification;
                    }
                }
                const bool ready_tiles = algorithm.HasGalleryTiles();
                diagnostics_.Emit([&] {
                    return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                                .operation = VisualDiagnosticOperation::ExploreContinuationStarted,
                                                .device = settings_.device,
                                                .generation = generation,
                                                .value = ready_tiles ? 1U : 0U,
                                                .detail = 21U};
                });
                if (!ready_tiles) return {};
                algorithm.PrepareOutputPublication(ExploreOutputChange::Semantic);
                ExploreGalleryPublication published;
                diagnostics_.Emit([&] {
                    return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                                .operation = VisualDiagnosticOperation::TileBatchPublishStarted,
                                                .device = settings_.device,
                                                .generation = generation,
                                                .value = advanced.cumulative_tiles,
                                                .context = {.capacity_width = extent.width,
                                                            .capacity_height = extent.height,
                                                            .staging_bytes = advanced.active_pinned_bytes}};
                });
                auto output = runtime.AcquireOutput(stop, runtime.Completed());
                if (!output.valid()) {
                    RollbackOutput(algorithm);
                    return {};
                }
                try {
                    runtime.Publish(output, extent.width, extent.height, [&](const auto clean, const auto semantic, const auto stream) {
                        services::RuntimeDiagnosticSpan compose_span{
                            diagnostics_, [&] {
                                return visual_diagnostic_boundary({.system = contracts::DiagnosticOwner::Explore,
                                                                   .operation = VisualDiagnosticOperation::TileBatchComposeStarted,
                                                                   .device = settings_.device,
                                                                   .generation = generation,
                                                                   .value = advanced.cumulative_tiles,
                                                                   .context = {.capacity_width = extent.width,
                                                                               .capacity_height = extent.height,
                                                                               .staging_bytes = advanced.active_pinned_bytes,
                                                                               .demand = {.demand_generation = generation}}},
                                                                  VisualDiagnosticOperation::TileBatchComposeCompleted);
                            }};
                        published = algorithm.PublishGalleryTiles(clean, semantic, stream);
                        compose_span.FinishWith([&](auto& fact) {
                            fact.value = published.cumulative_tiles;
                            fact.context.staging_bytes = published.active_pinned_bytes;
                        });
                    });
                    DiagnoseStorage(runtime, algorithm, generation);
                    auto frame = visual_frame({PresentationSourceKind::Explore, 1U}, extent, output.revision());
                    PreparedProduct product{
                        .output = std::move(output),
                        .retained = {},
                        .frame = frame,
                        .gallery = {.generation = published.generation, .slots = published.ready_slots},
                        .document = {},
                        .labels = algorithm.Labels(),
                    };
                    ExploreSnapshot changed;
                    std::unique_lock transaction(desired_admission_mutex_);
                    bool stale = false;
                    {
                        std::scoped_lock lock(mutex_);
                        stale = stop.stop_requested() || generation != latest_generation_->load(std::memory_order_acquire) ||
                                generation != active_gallery_generation_ || state_.mode != ExploreMode::Gallery;
                        if (!stale) {
                            frame.clean_revision = frame.revision;
                            product.frame = frame;
                            changed = state_;
                            ProjectProduct(changed, product);
                            changed.revision = mmltk::common::types::advance_monotonic_identity(changed.revision);
                        }
                    }
                    if (stale) {
                        transaction.unlock();
                        RollbackOutput(algorithm);
                        return {};
                    }
                    auto notification =
                        CommitPreparedProduct(runtime, algorithm, std::move(product), std::move(changed), [] {}, [] noexcept {});
                    if (!notification) {
                        transaction.unlock();
                        return RestoreCommittedProduct(algorithm);
                    }
                    DiagnoseFrame(frame, generation);
                    diagnostics_.Emit([&] {
                        return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                                    .operation = VisualDiagnosticOperation::TileBatchPublished,
                                                    .device = settings_.device,
                                                    .generation = published.generation,
                                                    .value = published.cumulative_tiles,
                                                    .detail = published.reused_tiles,
                                                    .context = {.capacity_width = extent.width,
                                                                .capacity_height = extent.height,
                                                                .staging_bytes = published.active_pinned_bytes}};
                    });
                    return notification;
                } catch (...) {
                    if (mmltk::frameworks::gpu::is_image_execution_failure(std::current_exception())) throw;
                    return RestoreCommittedProduct(
                        algorithm, visual_failure_detail(std::current_exception(), "Explore progressive publication failed"));
                }
            },
            [this]() noexcept {
                diagnostics_.Emit([&] {
                    return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                                .operation = VisualDiagnosticOperation::ExploreContinuationStarted,
                                                .device = settings_.device,
                                                .detail = 22U};
                });
            });
    }
    void DiagnoseStorage(const mmltk::frameworks::gpu::SystemImageRuntime& runtime, const ExploreAlgorithm& algorithm,
                         const std::uint64_t generation) const noexcept {
        diagnostics_.Emit([&] {
            const auto renderer = algorithm.StorageFootprint();
            const auto outputs = runtime.OutputStorageFootprint();
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                        .operation = VisualDiagnosticOperation::ExploreCacheStorage,
                                        .device = settings_.device,
                                        .generation = generation,
                                        .value = renderer.host_bytes,
                                        .detail = renderer.device_bytes + outputs.device_bytes,
                                        .context = {.capacity_width = renderer.cache_cards,
                                                    .staging_bytes = renderer.pinned_bytes + outputs.pinned_bytes,
                                                    .cache_bytes = renderer.host_bytes + renderer.cache_device_bytes,
                                                    .descriptor_bytes = renderer.descriptor_bytes,
                                                    .gpu_bytes = renderer.device_bytes + outputs.device_bytes,
                                                    .augmentation_device_bytes = renderer.augmentation_device_bytes,
                                                    .augmentation_pinned_bytes = renderer.augmentation_pinned_bytes}};
        });
    }
    void DiagnoseFrame(const VisualFrame& frame, const std::uint64_t generation) const noexcept {
        if (!diagnostics_.valid()) return;
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                        .operation = VisualDiagnosticOperation::ExploreFramePublished,
                                        .device = settings_.device,
                                        .generation = generation,
                                        .value = frame.revision,
                                        .context = {.capacity_width = frame.extent.width,
                                                    .capacity_height = frame.extent.height,
                                                    .source = visual_diagnostic_source({.frame = frame}),
                                                    .demand = {.demand_generation = generation}}};
        });
    }
    [[nodiscard]] std::optional<PreparedProduct> RenderGallery(mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                                               ExploreAlgorithm& algorithm, ExploreRenderPlan& plan,
                                                               ExploreOrderCandidate& candidate, const std::uint64_t generation,
                                                               const std::stop_token stop) {
        plan.mode = ExploreMode::Gallery;
        plan.selected_image.reset();
        auto rendered = Render(runtime, algorithm, plan, generation, &candidate, stop);
        if (!rendered) return std::nullopt;
        candidate.order = algorithm.Visible(plan.viewport, &candidate);
        return rendered;
    }
    [[nodiscard]] ExploreSnapshot QueueAdmitted(detail::VisualRuntimeOwner::Work work, ExploreSnapshot prior, bool reconstruct = false) {
        if (!worker_.SubmitDiscrete(
                std::move(work),
                [this] {
                    auto publication = FinalizeQueuedCancellation();
                    if (publication) publication();
                },
                reconstruct)) {
            state_ = std::move(prior);
            throw contracts::BusyError("Explore is busy");
        }
        return state_;
    }
    struct Execution final {
        ExploreRenderPlan requested;
        ExploreSettingsCandidate settings;
        std::size_t nproc;
    };
    [[nodiscard]] Execution BeginExecution(const std::uint64_t generation) {
        std::scoped_lock lock(mutex_);
        latest_generation_->store(generation, std::memory_order_release);
        auto requested = Plan(generation);
        return {
            .requested = std::move(requested),
            .settings = settings_system_.explore_settings_candidate(),
            .nproc = state_.nproc,
        };
    }
    [[nodiscard]] ExploreRenderPlan Plan(const std::uint64_t generation) const {
        return make_render_plan(state_, augmentation_config_, state_.dataset.identity, generation);
    }
    [[nodiscard]] static VisualExtent ProductExtent(ExploreAlgorithm& algorithm, const ExploreRenderPlan& plan) {
        return plan.mode == ExploreMode::Gallery ? plan.viewport.extent : algorithm.DetailExtent(plan);
    }
    [[nodiscard]] ExploreViewport ClampViewport(ExploreViewport viewport, const std::uint32_t matching_count) const {
        const auto card_extent = explore_atlas_card_extent(viewport);
        const auto rows =
            matching_count == 0U
                ? 1U
                : static_cast<std::uint32_t>((static_cast<std::uint64_t>(matching_count) + viewport.columns - 1U) / viewport.columns);
        viewport.first_row = std::min(viewport.first_row, rows - 1U);
        viewport.row_count = std::min(viewport.row_count, rows - viewport.first_row);
        viewport.extent.height = card_extent * viewport.row_count;
        ValidateViewport(viewport);
        return viewport;
    }
    [[nodiscard]] std::uint64_t NextShuffleSeed() {
        auto seed = next_shuffle_seed_++;
        if (seed == 0U) seed = next_shuffle_seed_++;
        return seed;
    }
    [[nodiscard]] std::uint64_t ResolveSeed(const ExploreFilter& filter, const bool reroll) {
        if (filter.order == ExploreOrder::Sequential) return 0U;
        return !reroll && filter.shuffle_seed != 0U ? filter.shuffle_seed : NextShuffleSeed();
    }
    void ValidateViewport(const ExploreViewport& viewport) const {
        if (!viewport.valid() || viewport.extent.width > settings_.maximum_width || viewport.extent.height > settings_.maximum_height)
            throw contracts::InvalidIntentError("Explore viewport is outside the configured device bounds");
    }
    void RequireIdle() const {
        if (state_.busy) throw contracts::BusyError("Explore is busy");
    }
    void GuardIdle() const {
        std::scoped_lock lock(mutex_);
        RequireIdle();
    }
    void RequireReady() const {
        RequireIdle();
        if (!state_.ready || !state_.frame.valid()) throw contracts::UnavailableError("Explore is not open");
    }
    void Admit() {
        state_.busy = true;
        state_.cancellation_requested = false;
        state_.failure.clear();
        state_.failure_kind = ExploreFailureKind::None;
        AdvanceRevision();
    }
    [[nodiscard]] std::uint64_t ReserveGeneration() {
        // CLEANUP-IGNORE: Explore owns its render-generation frontier separately from its observable snapshot revision.
        return mmltk::common::types::take_monotonic_identity(next_generation_);
    }
    [[nodiscard]] std::uint64_t NextGeneration() {
        latest_generation_->store(ReserveGeneration(), std::memory_order_release);
        return latest_generation_->load(std::memory_order_acquire);
    }
    void ActivateGeneration(const std::uint64_t generation) {
        std::scoped_lock lock(mutex_);
        latest_generation_->store(generation, std::memory_order_release);
        runtime_initialized_ = true;
    }
    void AdvanceRevision() { state_.revision = mmltk::common::types::advance_monotonic_identity(state_.revision); }
    static void Complete(ExploreSnapshot& snapshot) {
        snapshot.busy = false;
        snapshot.cancellation_requested = false;
        snapshot.revision = mmltk::common::types::advance_monotonic_identity(snapshot.revision);
    }
    void CompleteProduct(ExploreSnapshot& snapshot, const PreparedProduct& product) {
        snapshot.ready = true;
        snapshot.failure.clear();
        snapshot.failure_kind = ExploreFailureKind::None;
        ProjectProduct(snapshot, product);
        Complete(snapshot);
    }
    static void ProjectProduct(ExploreSnapshot& snapshot, const PreparedProduct& product) {
        snapshot.frame = product.frame;
        if (product.retained.valid()) {
            snapshot.gallery.generation = product.gallery.generation;
            return;
        }
        snapshot.gallery = product.gallery;
        snapshot.document = product.document ? product.document->facts() : VisualDocumentFacts{};
        snapshot.scene = product.document ? product.document->scene : contracts::AnnotationSceneContent{};
        snapshot.labels = product.labels;
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification FinalizeQueuedCancellation() {
        std::scoped_lock lock(mutex_);
        Complete(state_);
        return ChangedNotification(state_);
    }
    [[nodiscard]] ExploreSnapshot UnavailableSnapshotLocked() const {
        return {
            .revision = mmltk::common::types::advance_monotonic_identity(state_.revision),
            .nproc = state_.nproc,
            .maximum_atlas_extent = state_.maximum_atlas_extent,
        };
    }
    static void RollbackOutput(ExploreAlgorithm& algorithm) {
        if (!algorithm.RollbackOutputPublication()) {
            const auto initiating_failure = std::current_exception();
            auto rollback_failure =
                std::make_exception_ptr(std::runtime_error("Explore candidate rollback could not establish physical completion"));
            throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(initiating_failure ? initiating_failure : rollback_failure,
                                                                      initiating_failure ? rollback_failure : std::exception_ptr{});
        }
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification RollbackOpenCancellation(ExploreAlgorithm& algorithm,
                                                                                    const bool had_committed_product) {
        if (had_committed_product) { return RestoreCommittedProduct(algorithm, {}, false, true); }
        RollbackOutput(algorithm);
        algorithm.DiscardCandidate();
        algorithm.Reset();
        std::scoped_lock lock(mutex_);
        return FinalizeUnavailableOpenLocked();
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification FinalizeUnavailableOpenLocked() {
        auto unavailable = UnavailableSnapshotLocked();
        auto notification = ChangedNotification(unavailable);
        state_ = std::move(unavailable);
        active_gallery_generation_ = 0U;
        return notification;
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification RestoreCommittedProduct(ExploreAlgorithm& algorithm, std::string failure = {},
                                                                                   const bool preserve_busy = false,
                                                                                   const bool discard_order = false) {
        RollbackOutput(algorithm);
        if (discard_order) algorithm.DiscardCandidate();
        std::scoped_lock lock(mutex_);
        configured_algorithm_ = nullptr;
        if (!desired_) latest_generation_->store(active_gallery_generation_, std::memory_order_release);
        auto restored = state_;
        const bool was_busy = restored.busy;
        Complete(restored);
        if (preserve_busy) restored.busy = was_busy;
        detail::VisualRuntimeOwner::Notification notification;
        if (failure.empty()) {
            notification = ChangedNotification(restored);
        } else {
            restored.failure = failure;
            restored.failure_kind = ExploreFailureKind::Operation;
            notification = [this, published = restored, failure = std::move(failure)]() mutable noexcept {
                Publish(ExploreFailed{std::move(published), std::move(failure)});
                ExecutionSettingsChanged();
            };
        }
        state_ = std::move(restored);
        if (active_gallery_generation_ != 0U) SubmitGalleryContinuation();
        return notification;
    }
    void Failed(const std::exception_ptr failure) noexcept {
        auto detail = visual_failure_detail(failure, "Explore GPU worker failed");
        ExploreFailureKind kind = ExploreFailureKind::Operation;
        if (mmltk::frameworks::gpu::find_image_failure<mmltk::frameworks::gpu::GdrTransportUnavailable>(failure))
            kind = ExploreFailureKind::SelectedTransportUnavailable;
        ExploreSnapshot retained;
        {
            std::scoped_lock lock(mutex_);
            retained = state_;
        }
        const bool retain_product =
            retained.ready && retained.frame.valid() && visual_product_matches_frame(retained.frame, worker_.Borrow());
        ExploreSnapshot failed;
        bool resume_gallery = false;
        {
            std::scoped_lock lock(mutex_);
            if (retain_product) {
                failed = state_;
                Complete(failed);
            } else {
                // CLEANUP-IGNORE: Shared failure formatting and diagnostics are consolidated; Explore alone rebuilds its
                // unavailable snapshot before publishing its typed failure.
                failed = UnavailableSnapshotLocked();
            }
            failed.failure = detail;
            failed.failure_kind =
                kind == ExploreFailureKind::Operation && !runtime_initialized_ ? ExploreFailureKind::RuntimeInitialization : kind;
            state_ = failed;
            desired_.reset();
            desired_settings_.reset();
            desired_persist_ = false;
            desired_navigation_ = 0;
            configured_algorithm_ = nullptr;
            if (!retain_product) {
                active_gallery_generation_ = 0U;
                runtime_initialized_ = false;
            }
            resume_gallery = retain_product && active_gallery_generation_ != 0U;
        }
        report_visual_worker_failure(diagnostics_, contracts::DiagnosticOwner::Explore, settings_.device, detail);
        Publish(ExploreFailed{std::move(failed), std::move(detail)});
        if (resume_gallery) SubmitGalleryContinuation();
    }
    template <class Event>
    void Publish(Event event) noexcept {
        publish_visual_event_noexcept(events_, event_type{std::move(event)});
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification ChangedNotification(ExploreSnapshot snapshot) {
        return [this, snapshot = std::move(snapshot)]() mutable noexcept {
            Publish(ExploreChanged{std::move(snapshot)});
            ExecutionSettingsChanged();
        };
    }

    bool runtime_initialized_ = false;
    std::string committed_source_;
    std::optional<ExploreSettingsCandidate> runtime_settings_;
    SettingsSystem& settings_system_;
    VisualDeviceSettings settings_;
    SystemEventSink<event_type> events_;
    VisualDiagnosticSink diagnostics_{};
    std::mutex open_finalization_mutex_;
    mutable std::mutex mutex_;
    ExploreSnapshot state_;
    std::optional<ExploreSnapshot> desired_;
    std::optional<ExploreSettingsCandidate> desired_settings_;
    bool desired_persist_ = false;
    std::mutex desired_admission_mutex_;
    std::int64_t desired_navigation_ = 0;
    std::shared_ptr<const VisualDocument> document_;
    ExploreSettingsCandidate installed_settings_{};
    mmltk::backend::models::rfdetr::GpuAugmentationConfig augmentation_config_{};
    mmltk::backend::models::rfdetr::GpuAugmentationConfig desired_augmentation_config_{};
    std::uint64_t next_generation_ = 1U;
    ExploreOverlay semantic_overlay_{};
    std::uint64_t semantic_identity_ = 0U;
    std::uint64_t next_semantic_identity_ = 1U;
    const std::shared_ptr<std::atomic<std::uint64_t>> latest_generation_ = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::uint64_t active_gallery_generation_ = 0U;
    ExploreAlgorithm* configured_algorithm_ = nullptr;
    std::uint64_t next_shuffle_seed_ = 1U;
    std::shared_ptr<GalleryWakeGate> gallery_wake_;
    detail::VisualRuntimeOwner worker_;
};

ExploreSystem::ExploreSystem(SettingsSystem& settings_system, const VisualDeviceSettings settings, const std::size_t nproc,
                             VisualRuntimeFactory factory, SystemEventSink<event_type> events, const VisualDiagnosticSink diagnostics)
    : impl_(std::make_unique<Impl>(settings_system, settings, nproc, std::move(factory), std::move(events), diagnostics)) {}
ExploreSystem::~ExploreSystem() = default;
ExploreSnapshot ExploreSystem::Open(ExploreOpen request) { return impl_->Open(std::move(request)); }
void ExploreSystem::UpdateViewport(const ExploreViewportUpdate request) { impl_->UpdateViewport(request); }
std::uint64_t ExploreSystem::LastInteractionGeneration() const { return impl_->LastInteractionGeneration(); }
ExploreSnapshot ExploreSystem::UpdateFilter(const ExploreFilterUpdate request) { return impl_->UpdateFilter(request); }
ExploreSnapshot ExploreSystem::UpdateOverlay(const ExploreOverlay request) { return impl_->UpdateOverlay(request); }
ExploreSnapshot ExploreSystem::Reroll() { return impl_->Reroll(); }
ExploreSnapshot ExploreSystem::UpdateAugmentation(const ExploreAugmentationUpdate request) { return impl_->UpdateAugmentation(request); }
ExploreSnapshot ExploreSystem::RerollAugmentation() { return impl_->RerollAugmentation(); }
ExploreSnapshot ExploreSystem::UpdateDetail(const ExploreDetailUpdate request) { return impl_->UpdateDetail(request); }
ExploreSnapshot ExploreSystem::Select(const ExploreSelect request) { return impl_->Select(request); }
ExploreSnapshot ExploreSystem::Navigate(const ExploreNavigate request) { return impl_->Navigate(request); }
ExploreSnapshot ExploreSystem::CloseDetail() {
    // CLEANUP-IGNORE: Explore facade forwarding preserves its domain-specific CloseDetail endpoint.
    return impl_->CloseDetail();
}
ExploreSnapshot ExploreSystem::Stop() noexcept {
    // CLEANUP-IGNORE: Explore facade forwarding preserves its typed cancellation snapshot.
    return impl_->Stop();
}
void ExploreSystem::ExecutionSettingsChanged() noexcept { impl_->ExecutionSettingsChanged(); }
void ExploreSystem::Shutdown() noexcept { impl_->Shutdown(); }
bool ExploreSystem::stopped() const noexcept { return impl_->stopped(); }
ExploreSnapshot ExploreSystem::snapshot() const { return impl_->snapshot(); }
mmltk::frameworks::gpu::BorrowedImageProductReadView ExploreSystem::BorrowFrame() const { return impl_->BorrowFrame(); }
VisualDocumentRead ExploreSystem::BorrowDocument(const VisualFrame& frame) const { return impl_->BorrowDocument(frame); }

}  // namespace mmltk::controller
