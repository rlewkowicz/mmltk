#include "src/controller/subsystems/explore/explore_system.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "src/controller/presentation/detail/monotonic_identity.h"
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
        std::scoped_lock lock(mutex_);
        if (!wake_) return;
        try {
            wake_();
        } catch (...) {}
    }

    void Disconnect() noexcept {
        std::scoped_lock lock(mutex_);
        wake_ = {};
    }

   private:
    std::mutex mutex_;
    std::function<void()> wake_;
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
              std::move(factory), [this](const std::exception_ptr failure) { Failed(failure); },
              diagnostics_.valid()
                  ? detail::VisualRuntimeOwner::ActivityObservation{[diagnostics](const detail::VisualRuntimeOwner::ActivityStage stage,
                                                                                  const std::uint64_t value) noexcept {
                        diagnostics({.system = VisualSystemKind::Explore,
                                     .operation = VisualDiagnosticOperation::ExploreContinuationStarted,
                                     .value = value,
                                     .detail = 30U + static_cast<std::uint64_t>(stage)});
                    }}
                  : detail::VisualRuntimeOwner::ActivityObservation{}) {
        if (!settings_.valid() || nproc == 0U || nproc > kExploreMaximumParallelism)
            throw contracts::InvalidIntentError("Explore device settings or nproc are invalid");
        RegisterGalleryContinuation();
    }
    ~Impl() {
        gallery_wake_->Disconnect();
        worker_.StopAndWait();
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
                [this, request = std::move(request), generation, nproc, reconstruct](
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
                    bool candidate_render_started = false;
                    ExploreRenderPlan committed_plan;
                    {
                        std::scoped_lock state_lock(mutex_);
                        if (reconstruct) {
                            configured_algorithm_ = nullptr;
                            state_.ready = false;
                            active_gallery_generation_ = 0U;
                            gallery_readiness_ = {};
                            document_.reset();
                        }
                        runtime_settings_ = settings_candidate;
                        settings_.device = settings_candidate.device_id;
                        settings_.numa_node = settings_candidate.loading.numa_node;
                        had_committed_product = state_.ready && state_.frame.valid();
                        committed_plan = Plan(generation);
                    }
                    const auto cancel_open = [&]() {
                        return RollbackOpenCancellation(runtime, algorithm, committed_plan, had_committed_product, candidate_render_started,
                                                        generation);
                    };
                    try {
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
                        const ExploreRenderPlan plan{
                            .viewport = viewport,
                            .overlay = preferences.overlay,
                            .augmentation_config = settings_candidate.augmentation,
                            .augmentation = {.enabled = settings_candidate.augmentation_preview_enabled, .seed = 0U},
                            .detail = {.show_original_dimensions = settings_candidate.show_original_dimensions},
                            .dataset_identity = opened.dataset_identity,
                            .generation = generation,
                        };
                        candidate_render_started = true;
                        if (!Render(runtime, algorithm, plan, generation, &candidate, stop) || stop.stop_requested()) return cancel_open();
                        candidate.order = algorithm.Visible(viewport, &candidate);
                        opened.order = candidate.order;
                        auto frame = Frame(runtime, ProductExtent(algorithm, plan), generation);
                        std::scoped_lock open_finalization_lock(open_finalization_mutex_);
                        if (stop.stop_requested()) return cancel_open();
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
                        CompleteProduct(settled, frame);
                        auto notification = ChangedNotification(settled);
                        if (catalog_changed) {
                            settings_system_.persist_explore_class_catalog(settings_candidate, settled.dataset.class_catalog_identity,
                                                                           preferences);
                            settings_candidate = settings_system_.explore_settings_candidate();
                        } else if (settings_system_.explore_settings_candidate().version != settings_candidate.version) {
                            throw contracts::BusyError("Explore settings candidate is stale");
                        }
                        std::scoped_lock finalization_lock(mutex_);
                        algorithm.Commit(std::move(candidate));
                        committed_dataset_identity_ = opened.dataset_identity;
                        committed_source_ = request.compiled_source;
                        installed_settings_ = settings_candidate;
                        augmentation_config_ = plan.augmentation_config;
                        state_ = std::move(settled);
                        return notification;
                    } catch (...) {
                        if (!had_committed_product) {
                            algorithm.DiscardCandidate();
                            throw;
                        }
                        return RollbackOpenFailure(runtime, algorithm, committed_plan, generation, std::current_exception());
                    }
                },
                std::move(prior), reconstruct);
            if (reconstruct) runtime_initialized_ = false;
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
        return latest_generation_;
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
                desired.augmentation.seed = presentation::detail::advance_monotonic_identity(desired.augmentation.seed);
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
            if (desired_) {
                desired_.reset();
                desired_settings_.reset();
                desired_persist_ = false;
                desired_navigation_ = 0;
                latest_generation_ = ReserveGeneration();
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

    void Shutdown() noexcept { worker_.StopAndWait(); }
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
            if (!state_.ready || state_.mode != ExploreMode::Detail || state_.frame != frame || document_revision_ != frame.revision)
                return {};
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
        const auto generation = viewport_update && previous_viewport == desired_->viewport ? latest_generation_ : NextGeneration();
        auto requested = *desired_;
        ExploreRenderPlan plan{.viewport = requested.viewport,
                               .overlay = requested.overlay,
                               .mode = requested.mode,
                               .selected_image = requested.selected_image,
                               .focused_image = requested.focused_image,
                               .augmentation_config = desired_augmentation_config_,
                               .augmentation = requested.augmentation,
                               .detail = requested.detail,
                               .dataset_identity = committed_dataset_identity_,
                               .generation = generation};
        if (!worker_.SubmitLatest([this, plan, requested = std::move(requested), generation, offset, settings = desired_settings_,
                                   persist = desired_persist_](mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                                               std::stop_token stop) mutable -> detail::VisualRuntimeOwner::Notification {
                ExploreRenderPlan committed;
                {
                    std::scoped_lock lock(mutex_);
                    if (generation != latest_generation_) return {};
                    committed = Plan(generation);
                }
                auto& algorithm = explore_algorithm(runtime);
                try {
                    auto order = algorithm.Visible(plan.viewport);
                    if (plan.focused_image &&
                        std::ranges::find(order.visible_indices, *plan.focused_image) == order.visible_indices.end()) {
                        plan.focused_image.reset();
                        requested.focused_image.reset();
                        std::scoped_lock lock(mutex_);
                        if (generation == latest_generation_ && desired_) desired_->focused_image.reset();
                    }
                    if (offset != 0 && requested.selected_image) {
                        const auto selected = algorithm.Adjacent(*requested.selected_image, offset);
                        if (!selected) throw contracts::UnavailableError("Explore filtered order is empty");
                        std::scoped_lock lock(mutex_);
                        if (generation != latest_generation_) return {};
                        requested.selected_image = selected;
                        plan.selected_image = selected;
                        desired_->selected_image = selected;
                        desired_navigation_ = 0;
                    }
                    const bool rendered = Render(runtime, algorithm, plan, generation, nullptr, stop);
                    std::scoped_lock transaction(desired_admission_mutex_);
                    {
                        std::scoped_lock lock(mutex_);
                        if (generation != latest_generation_ || stop.stop_requested()) {
                            if (desired_ || state_.busy) return {};
                            committed.generation = latest_generation_;
                        } else {
                            if (desired_) requested.overlay.show_labels = desired_->overlay.show_labels;
                            if (settings && desired_settings_ && desired_augmentation_config_ == plan.augmentation_config) {
                                // View-only preferences can refresh the checked
                                // settings version while these pixels are pending.
                                settings = desired_settings_;
                            }
                        }
                    }
                    if (committed.generation != generation || !rendered)
                        return RestoreCancellation(runtime, algorithm, committed, committed.generation);
                    const auto frame = Frame(runtime, ProductExtent(algorithm, plan), generation);
                    if (settings && persist)
                        settings = settings_system_.persist_explore_product(
                            *settings, {.filter = requested.filter, .overlay = requested.overlay}, requested.augmentation.enabled);
                    else if (settings && settings_system_.explore_settings_candidate().version != settings->version)
                        throw contracts::BusyError("Explore settings candidate is stale");
                    std::scoped_lock lock(mutex_);
                    requested.order = std::move(order);
                    requested.detail = state_.detail;
                    requested.viewport_result = state_.viewport_result;
                    requested.revision = state_.revision;
                    const bool discrete_pending = state_.busy;
                    CompleteProduct(requested, frame);
                    requested.busy = discrete_pending;
                    state_ = std::move(requested);
                    augmentation_config_ = plan.augmentation_config;
                    if (settings) installed_settings_ = std::move(*settings);
                    desired_.reset();
                    desired_settings_.reset();
                    desired_persist_ = false;
                    return ChangedNotification(state_);
                } catch (...) {
                    const auto detail = visual_failure_detail(std::current_exception(), "Explore render mutation failed");
                    std::scoped_lock transaction(desired_admission_mutex_);
                    {
                        std::scoped_lock lock(mutex_);
                        if (generation != latest_generation_ && (desired_ || state_.busy)) return {};
                        committed.generation = latest_generation_;
                        desired_.reset();
                        desired_settings_.reset();
                        desired_persist_ = false;
                        desired_navigation_ = 0;
                    }
                    return RestorePersistenceFailure(runtime, algorithm, committed, committed.generation, detail, true);
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
                auto candidate = prepare(algorithm, execution.nproc, seed, stop);
                if (stop.stop_requested()) return RollbackCandidateCancellation(runtime, algorithm, execution.committed, generation);
                render_plan.viewport = ClampViewport(render_plan.viewport, candidate.order.matching_count);
                if (render_overlay) render_plan.overlay = *render_overlay;
                auto frame = RenderGallery(runtime, algorithm, render_plan, candidate, generation, stop);
                if (!frame || stop.stop_requested())
                    return RollbackCandidateCancellation(runtime, algorithm, execution.committed, generation);
                if (stop.stop_requested()) return RollbackCandidateCancellation(runtime, algorithm, execution.committed, generation);
                bool stale;
                {
                    std::scoped_lock decision_lock(mutex_);
                    stale = stop.stop_requested() || generation != latest_generation_;
                }
                if (stale) return RollbackCandidateCancellation(runtime, algorithm, execution.committed, generation);
                ExploreSnapshot settled;
                {
                    std::scoped_lock snapshot_lock(mutex_);
                    settled = state_;
                }
                install(settled, candidate.order);
                settled.viewport = render_plan.viewport;
                CompleteProduct(settled, *frame);
                auto notification = ChangedNotification(settled);
                auto refreshed_settings = persist(execution.settings);
                {
                    std::scoped_lock commit_lock(mutex_);
                    algorithm.Commit(std::move(candidate));
                    if (refreshed_settings) installed_settings_ = std::move(*refreshed_settings);
                    state_ = std::move(settled);
                }
                return notification;
            } catch (...) {
                return RollbackCandidateFailure(runtime, algorithm, execution.committed, generation, std::current_exception(),
                                                failure_fallback);
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

    [[nodiscard]] bool Render(mmltk::frameworks::gpu::SystemImageRuntime& runtime, ExploreAlgorithm& algorithm,
                              const ExploreRenderPlan& plan, const std::uint64_t generation,
                              const ExploreOrderCandidate* candidate = nullptr, const std::stop_token stop = {}) {
        if (stop.stop_possible() && stop.stop_requested()) return false;
        if (configured_algorithm_ != &algorithm) {
            algorithm.SetGalleryReadySink(ExploreAlgorithm::GalleryReadySink{[wake = std::weak_ptr{gallery_wake_}] {
                if (const auto gate = wake.lock()) gate->Invoke();
            }});
            configured_algorithm_ = &algorithm;
        }
        algorithm.PrepareOutputPublication();
        ExploreGalleryPublication publication;
        const auto product_extent = ProductExtent(algorithm, plan);
        runtime.Publish(product_extent.width, product_extent.height, [&](const auto clean, const auto semantic, const auto stream) {
            if (plan.mode == ExploreMode::Gallery)
                publication = algorithm.BeginGallery(plan, candidate, state_.nproc, clean, semantic, stream);
            else
                algorithm.RenderDetail(plan, state_.nproc, clean, semantic, stream);
        });
        {
            std::scoped_lock lock(mutex_);
            auto document = plan.mode == ExploreMode::Detail ? algorithm.Document() : nullptr;
            if (!document || document != document_) clean_revision_ = presentation::detail::advance_monotonic_identity(clean_revision_);
            document_ = std::move(document);
            content_ = plan.mode == ExploreMode::Detail ? algorithm.DetailContent(plan) : VisualRegion{};
            document_revision_ = runtime.output().revision();
        }
        if (plan.mode == ExploreMode::Gallery) {
            {
                std::scoped_lock lock(mutex_);
                if (generation != latest_generation_) return false;
                active_gallery_generation_ = generation;
                gallery_readiness_ = {.generation = publication.generation, .slots = publication.ready_slots};
                SubmitGalleryContinuation();
            }
            diagnostics_({.system = VisualSystemKind::Explore,
                          .operation = VisualDiagnosticOperation::PlaceholderPublished,
                          .device = settings_.device,
                          .generation = generation,
                          .value = plan.viewport.first_row,
                          .context = {.capacity_width = plan.viewport.extent.width,
                                      .capacity_height = plan.viewport.extent.height,
                                      .staging_bytes = publication.active_pinned_bytes}});
            if (publication.cumulative_tiles != 0U)
                diagnostics_({.system = VisualSystemKind::Explore,
                              .operation = VisualDiagnosticOperation::TileBatchPublished,
                              .device = settings_.device,
                              .generation = generation,
                              .value = publication.cumulative_tiles,
                              .detail = publication.cumulative_tiles,
                              .context = {.staging_bytes = publication.active_pinned_bytes}});
        } else {
            std::scoped_lock lock(mutex_);
            active_gallery_generation_ = 0U;
            gallery_readiness_ = {};
        }
        diagnostics_({.system = VisualSystemKind::Explore,
                      .operation = VisualDiagnosticOperation::RenderCompleted,
                      .device = settings_.device,
                      .generation = generation,
                      .value = state_.nproc,
                      .detail = (plan.augmentation.enabled ? 1U : 0U) | (plan.augmentation_config.enabled ? 2U : 0U)});
        return true;
    }
    void SubmitGalleryContinuation() noexcept { static_cast<void>(worker_.NotifyContinuation()); }
    void RegisterGalleryContinuation() {
        worker_.RegisterContinuation(
            [this](mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                   const std::stop_token stop) -> detail::VisualRuntimeOwner::Notification {
                diagnostics_({.system = VisualSystemKind::Explore,
                              .operation = VisualDiagnosticOperation::ExploreContinuationStarted,
                              .device = settings_.device});
                if (stop.stop_requested()) return {};
                auto& algorithm = explore_algorithm(runtime);
                std::uint64_t generation;
                VisualExtent extent;
                {
                    std::scoped_lock lock(mutex_);
                    generation = active_gallery_generation_;
                    if (generation == 0U || generation != latest_generation_ || state_.mode != ExploreMode::Gallery) return {};
                    extent = state_.viewport.extent;
                }
                const auto advanced = algorithm.AdvanceGallery();
                diagnostics_({.system = VisualSystemKind::Explore,
                              .operation = VisualDiagnosticOperation::ExploreContinuationStarted,
                              .device = settings_.device,
                              .generation = advanced.generation,
                              .value = advanced.cumulative_tiles,
                              .detail = 20U,
                              .context = {.capacity_width = static_cast<std::uint32_t>(advanced.remaining_tiles),
                                          .staging_bytes = advanced.active_pinned_bytes}});
                if (stop.stop_requested()) return {};
                if (advanced.stale_discarded != 0U)
                    diagnostics_({.system = VisualSystemKind::Explore,
                                  .operation = VisualDiagnosticOperation::StaleThumbnailDiscarded,
                                  .device = settings_.device,
                                  .generation = advanced.generation,
                                  .value = advanced.stale_discarded,
                                  .context = {.capacity_width = extent.width,
                                              .capacity_height = extent.height,
                                              .staging_bytes = advanced.active_pinned_bytes}});
                {
                    std::scoped_lock lock(mutex_);
                    if (generation != latest_generation_ || generation != active_gallery_generation_ || advanced.generation != generation ||
                        state_.mode != ExploreMode::Gallery)
                        return {};
                }
                if (stop.stop_requested()) return {};
                {
                    std::scoped_lock lock(mutex_);
                    if (advanced.ready_slots != gallery_readiness_.slots) {
                        gallery_readiness_ = {.generation = advanced.generation, .slots = advanced.ready_slots};
                        auto changed = state_;
                        changed.gallery = gallery_readiness_;
                        clean_revision_ = presentation::detail::advance_monotonic_identity(clean_revision_);
                        changed.frame = Frame(runtime, extent, generation);
                        changed.labels = algorithm.Labels();
                        changed.revision = presentation::detail::advance_monotonic_identity(changed.revision);
                        auto notification = ChangedNotification(changed);
                        state_ = std::move(changed);
                        diagnostics_({.system = VisualSystemKind::Explore,
                                      .operation = VisualDiagnosticOperation::TileBatchPublished,
                                      .device = settings_.device,
                                      .generation = generation,
                                      .value = advanced.cumulative_tiles,
                                      .detail = advanced.reused_tiles,
                                      .context = {.capacity_width = extent.width,
                                                  .capacity_height = extent.height,
                                                  .staging_bytes = advanced.active_pinned_bytes}});
                        // Completion freed physical lanes. Preserve this capacity
                        // event while the current notification leaves the worker.
                        SubmitGalleryContinuation();
                        return notification;
                    }
                }
                const bool ready_tiles = algorithm.HasGalleryTiles();
                diagnostics_({.system = VisualSystemKind::Explore,
                              .operation = VisualDiagnosticOperation::ExploreContinuationStarted,
                              .device = settings_.device,
                              .generation = generation,
                              .value = ready_tiles ? 1U : 0U,
                              .detail = 21U});
                if (!ready_tiles) return {};
                algorithm.PrepareOutputPublication();
                ExploreGalleryPublication published;
                diagnostics_(
                    {.system = VisualSystemKind::Explore,
                     .operation = VisualDiagnosticOperation::TileBatchPublishStarted,
                     .device = settings_.device,
                     .generation = generation,
                     .value = advanced.cumulative_tiles,
                     .context = {
                         .capacity_width = extent.width, .capacity_height = extent.height, .staging_bytes = advanced.active_pinned_bytes}});
                runtime.Publish(extent.width, extent.height, [&](const auto clean, const auto semantic, const auto stream) {
                    diagnostics_({.system = VisualSystemKind::Explore,
                                  .operation = VisualDiagnosticOperation::TileBatchComposeStarted,
                                  .device = settings_.device,
                                  .generation = generation,
                                  .value = advanced.cumulative_tiles,
                                  .context = {.capacity_width = extent.width,
                                              .capacity_height = extent.height,
                                              .staging_bytes = advanced.active_pinned_bytes}});
                    published = algorithm.PublishGalleryTiles(clean, semantic, stream);
                    diagnostics_({.system = VisualSystemKind::Explore,
                                  .operation = VisualDiagnosticOperation::TileBatchComposeCompleted,
                                  .device = settings_.device,
                                  .generation = generation,
                                  .value = published.cumulative_tiles,
                                  .context = {.capacity_width = extent.width,
                                              .capacity_height = extent.height,
                                              .staging_bytes = published.active_pinned_bytes}});
                });
                // The completion event publishes pixels, labels and exact
                // readiness together. Submission alone is not a ready tile.
                return {};
            },
            [this]() noexcept {
                diagnostics_({.system = VisualSystemKind::Explore,
                              .operation = VisualDiagnosticOperation::ExploreContinuationStarted,
                              .device = settings_.device,
                              .detail = 22U});
            });
    }
    [[nodiscard]] std::optional<VisualFrame> RenderGallery(mmltk::frameworks::gpu::SystemImageRuntime& runtime, ExploreAlgorithm& algorithm,
                                                           ExploreRenderPlan& plan, ExploreOrderCandidate& candidate,
                                                           const std::uint64_t generation, const std::stop_token stop) {
        plan.mode = ExploreMode::Gallery;
        plan.selected_image.reset();
        if (!Render(runtime, algorithm, plan, generation, &candidate, stop)) return std::nullopt;
        candidate.order = algorithm.Visible(plan.viewport, &candidate);
        return Frame(runtime, plan.viewport.extent, generation);
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
    [[nodiscard]] VisualFrame Frame(const mmltk::frameworks::gpu::SystemImageRuntime& runtime, const VisualExtent extent,
                                    const std::uint64_t generation) const {
        auto frame = visual_frame({PresentationSourceKind::Explore, 1U}, extent, runtime.output().revision());
        frame.content = content_;
        frame.clean_revision = clean_revision_;
        if (diagnostics_.valid())
            diagnostics_({.system = VisualSystemKind::Explore,
                          .operation = VisualDiagnosticOperation::ExploreFramePublished,
                          .device = settings_.device,
                          .generation = generation,
                          .value = frame.revision,
                          .context = {.capacity_width = frame.extent.width, .capacity_height = frame.extent.height}});
        return frame;
    }
    struct Execution final {
        ExploreRenderPlan requested;
        ExploreRenderPlan committed;
        ExploreSettingsCandidate settings;
        std::size_t nproc;
    };
    [[nodiscard]] Execution BeginExecution(const std::uint64_t generation) {
        std::scoped_lock lock(mutex_);
        latest_generation_ = generation;
        auto requested = Plan(generation);
        return {
            .requested = requested,
            .committed = std::move(requested),
            .settings = settings_system_.explore_settings_candidate(),
            .nproc = state_.nproc,
        };
    }
    [[nodiscard]] ExploreRenderPlan Plan(const std::uint64_t generation) const {
        return {.viewport = state_.viewport,
                .overlay = state_.overlay,
                .mode = state_.mode,
                .selected_image = state_.selected_image,
                .focused_image = state_.focused_image,
                .augmentation_config = augmentation_config_,
                .augmentation = state_.augmentation,
                .detail = state_.detail,
                .dataset_identity = committed_dataset_identity_,
                .generation = generation};
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
        return presentation::detail::take_monotonic_identity(next_generation_);
    }
    [[nodiscard]] std::uint64_t NextGeneration() {
        latest_generation_ = ReserveGeneration();
        return latest_generation_;
    }
    void ActivateGeneration(const std::uint64_t generation) {
        std::scoped_lock lock(mutex_);
        latest_generation_ = generation;
        runtime_initialized_ = true;
    }
    void AdvanceRevision() { state_.revision = presentation::detail::advance_monotonic_identity(state_.revision); }
    static void Complete(ExploreSnapshot& snapshot) {
        snapshot.busy = false;
        snapshot.cancellation_requested = false;
        snapshot.revision = presentation::detail::advance_monotonic_identity(snapshot.revision);
    }
    void CompleteProduct(ExploreSnapshot& snapshot, const VisualFrame frame) {
        snapshot.ready = true;
        snapshot.failure.clear();
        snapshot.failure_kind = ExploreFailureKind::None;
        snapshot.frame = frame;
        snapshot.gallery = gallery_readiness_;
        snapshot.scene = document_ ? document_->scene : contracts::AnnotationSceneContent{};
        snapshot.labels = configured_algorithm_ ? configured_algorithm_->Labels() : std::vector<ExploreLabel>{};
        Complete(snapshot);
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification FinalizeCancellation() {
        std::scoped_lock lock(mutex_);
        return FinalizeCancellationLocked();
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification FinalizeCancellationLocked() {
        latest_generation_ = active_gallery_generation_;
        Complete(state_);
        return ChangedNotification(state_);
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification FinalizeQueuedCancellation() {
        std::scoped_lock lock(mutex_);
        Complete(state_);
        return ChangedNotification(state_);
    }
    [[nodiscard]] ExploreSnapshot UnavailableSnapshotLocked() const {
        return {
            .revision = presentation::detail::advance_monotonic_identity(state_.revision),
            .nproc = state_.nproc,
            .maximum_atlas_extent = state_.maximum_atlas_extent,
        };
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification RollbackOpenCancellation(
        mmltk::frameworks::gpu::SystemImageRuntime& runtime, ExploreAlgorithm& algorithm, const ExploreRenderPlan& committed_plan,
        const bool had_committed_product, const bool candidate_render_started, const std::uint64_t generation) {
        algorithm.DiscardCandidate();
        if (had_committed_product && candidate_render_started) return RestoreCancellation(runtime, algorithm, committed_plan, generation);
        if (had_committed_product) return FinalizeCancellation();
        algorithm.Reset();
        std::scoped_lock lock(mutex_);
        return FinalizeUnavailableOpenLocked();
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification RollbackOpenFailure(mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                                                               ExploreAlgorithm& algorithm,
                                                                               const ExploreRenderPlan& committed_plan,
                                                                               const std::uint64_t generation,
                                                                               const std::exception_ptr failure) {
        algorithm.DiscardCandidate();
        return RestorePersistenceFailure(runtime, algorithm, committed_plan, generation,
                                         visual_failure_detail(failure, "Explore dataset candidate failed"));
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification RollbackCandidateCancellation(
        mmltk::frameworks::gpu::SystemImageRuntime& runtime, ExploreAlgorithm& algorithm, const ExploreRenderPlan& committed_plan,
        const std::uint64_t generation) {
        algorithm.DiscardCandidate();
        return RestoreCancellation(runtime, algorithm, committed_plan, generation);
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification RollbackCandidateFailure(
        mmltk::frameworks::gpu::SystemImageRuntime& runtime, ExploreAlgorithm& algorithm, const ExploreRenderPlan& committed_plan,
        const std::uint64_t generation, const std::exception_ptr failure, const std::string_view fallback) {
        algorithm.DiscardCandidate();
        return RestorePersistenceFailure(runtime, algorithm, committed_plan, generation, visual_failure_detail(failure, fallback));
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification FinalizeUnavailableOpenLocked() {
        auto unavailable = UnavailableSnapshotLocked();
        auto notification = ChangedNotification(unavailable);
        state_ = std::move(unavailable);
        active_gallery_generation_ = 0U;
        return notification;
    }
    [[nodiscard]] ExploreSnapshot RestoreCommittedProduct(mmltk::frameworks::gpu::SystemImageRuntime& runtime, ExploreAlgorithm& algorithm,
                                                          const ExploreRenderPlan& committed_plan, const std::uint64_t generation,
                                                          const bool preserve_busy = false) {
        algorithm.AbortRenderGeneration();
        if (!Render(runtime, algorithm, committed_plan, generation))
            throw std::logic_error("Explore committed product restoration was cancelled");
        const auto frame = Frame(runtime, ProductExtent(algorithm, committed_plan), generation);
        std::scoped_lock lock(mutex_);
        auto restored = state_;
        const bool was_busy = restored.busy;
        CompleteProduct(restored, frame);
        if (preserve_busy) restored.busy = was_busy;
        state_ = restored;
        return restored;
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification RestoreCancellation(mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                                                               ExploreAlgorithm& algorithm,
                                                                               const ExploreRenderPlan& committed_plan,
                                                                               const std::uint64_t generation) {
        return ChangedNotification(RestoreCommittedProduct(runtime, algorithm, committed_plan, generation));
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Notification RestorePersistenceFailure(mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                                                                     ExploreAlgorithm& algorithm,
                                                                                     const ExploreRenderPlan& committed_plan,
                                                                                     const std::uint64_t generation, std::string detail,
                                                                                     const bool preserve_busy = false) {
        auto restored = RestoreCommittedProduct(runtime, algorithm, committed_plan, generation, preserve_busy);
        {
            std::scoped_lock lock(mutex_);
            restored.failure = detail;
            restored.failure_kind = ExploreFailureKind::Operation;
            state_.failure = detail;
            state_.failure_kind = restored.failure_kind;
        }
        auto published = restored;
        detail::VisualRuntimeOwner::Notification notification = [this, published = std::move(published),
                                                                 detail = std::move(detail)]() mutable noexcept {
            Publish(ExploreFailed{std::move(published), std::move(detail)});
            // A settings change can invalidate the candidate while its
            // discrete work is busy. Recheck after rollback, just as on
            // successful completion, once scheduler admission is idle.
            ExecutionSettingsChanged();
        };
        return notification;
    }
    void Failed(const std::exception_ptr failure) noexcept {
        auto detail = visual_failure_detail(failure, "Explore GPU worker failed");
        ExploreFailureKind kind = ExploreFailureKind::Operation;
        try {
            if (failure) std::rethrow_exception(failure);
        } catch (const mmltk::frameworks::gpu::GdrTransportUnavailable&) {
            kind = ExploreFailureKind::SelectedTransportUnavailable;
        } catch (...) {}
        ExploreSnapshot failed;
        {
            std::scoped_lock lock(mutex_);
            // CLEANUP-IGNORE: Shared failure formatting and diagnostics are consolidated; Explore alone rebuilds its
            // unavailable snapshot before publishing its typed failure.
            failed = UnavailableSnapshotLocked();
            failed.failure = detail;
            failed.failure_kind =
                kind == ExploreFailureKind::Operation && !runtime_initialized_ ? ExploreFailureKind::RuntimeInitialization : kind;
            state_ = failed;
            desired_.reset();
            desired_settings_.reset();
            desired_persist_ = false;
            desired_navigation_ = 0;
            active_gallery_generation_ = 0U;
            configured_algorithm_ = nullptr;
            runtime_initialized_ = false;
        }
        report_visual_worker_failure(diagnostics_, VisualSystemKind::Explore, settings_.device, detail);
        Publish(ExploreFailed{std::move(failed), std::move(detail)});
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
    ExploreGalleryReadiness gallery_readiness_;
    std::optional<ExploreSnapshot> desired_;
    std::optional<ExploreSettingsCandidate> desired_settings_;
    bool desired_persist_ = false;
    std::mutex desired_admission_mutex_;
    std::int64_t desired_navigation_ = 0;
    std::shared_ptr<const VisualDocument> document_;
    std::uint64_t document_revision_ = 0U;
    std::uint64_t clean_revision_ = 0U;
    VisualRegion content_{};
    ExploreSettingsCandidate installed_settings_{};
    mmltk::backend::models::rfdetr::GpuAugmentationConfig augmentation_config_{};
    mmltk::backend::models::rfdetr::GpuAugmentationConfig desired_augmentation_config_{};
    std::uint64_t committed_dataset_identity_ = 0U;
    std::uint64_t next_generation_ = 1U;
    std::uint64_t latest_generation_ = 0U;
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
