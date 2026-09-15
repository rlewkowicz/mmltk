#include "validation_samples.h"
#include "src/controller/subsystems/system/detail/prediction_preview.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/frameworks/gpu/image_failure.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <stdexcept>
#include <limits>
#include <utility>
#include <cuda_runtime_api.h>
namespace mmltk::controller::detail {
namespace gpu = mmltk::frameworks::gpu;
namespace rfdetr = mmltk::backend::models::rfdetr;
class ValidationSamples::Impl final {
 public:
    struct Sample final {
        ValidationSampleMetadata metadata;
        std::uint64_t content_identity = 0U;
        std::shared_ptr<const PredictionPreviewFrame> raw;
    };
    struct Set final {
        std::array<Sample, rfdetr::kValidationSampleCapacity> samples;
        std::uint64_t generation = 0;
        std::uint64_t atlas_identity = 0;
    };
    struct Composition final {
        std::shared_ptr<Set> atlas;
        std::optional<ValidationSampleIdentity> detail;
        ValidationOverlays overlays;
    };
    Impl(VisualDeviceSettings visual, std::function<void()> changed, PredictionPreviewPool::TransferOperations transfers)
        : visual_(visual), changed_(std::move(changed)), transfers_(transfers), worker_([this](auto revisions) {
            EnsurePool();
            gpu::SystemImageRuntimeConfig config{.device = visual_.device,
                .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .output_buffer_count = 2U,
                .numa_node = visual_.numa_node, .execution = execution_, .product_revisions = std::move(revisions), .adopted_context = context_};
            configure_visual_workspace_finalization(config);
            return std::make_unique<gpu::SystemImageRuntime>(std::move(config));
        }, [this](std::exception_ptr) { Notify(); }) {
        if (visual_.valid() && (visual_.maximum_width < 3U || visual_.maximum_height < 2U))
            throw contracts::InvalidIntentError("validation requires at least a 3 by 2 image envelope");
        worker_.RegisterContinuation([this](auto& runtime, auto stop) { return Render(runtime, stop); }, {}, true);
    }
    ~Impl() { Shutdown(); }
    void EnsurePool() {
        std::scoped_lock lock(pool_mutex_);
        if (!retirement_->admission_open()) throw contracts::UnavailableError("validation sample custody is unobservable");
        if (pool_) return;
        if (!visual_.valid()) throw contracts::UnavailableError("validation visual device is unavailable");
        execution_ = resolve_visual_device_execution(visual_);
        context_ = CreatePredictionPreviewContext(*execution_, retirement_);
        pool_ = std::make_unique<PredictionPreviewPool>(*execution_, *context_,
            transfers_, retirement_, kRawCapacity);
    }
    void Notify() noexcept { if (changed_) { try { changed_(); } catch (...) {} } }
    void Begin(std::uint64_t generation, std::span<const std::uint32_t> indices) {
        if (indices.size() > rfdetr::kValidationSampleCapacity) throw contracts::InvalidIntentError("validation sample count exceeds six");
        auto next = std::make_shared<Set>();
        next->generation = generation;
        for (std::size_t index = 0; index < indices.size(); ++index) next->samples[index].metadata.identity = {generation, indices[index]};
        {
            std::scoped_lock lock(mutex_);
            if (content_frontier_ > std::numeric_limits<std::uint64_t>::max() - (rfdetr::kValidationSampleCapacity + 1U)) throw contracts::FailedError("validation content identities exhausted");
            next->atlas_identity = ++content_frontier_;
            for (auto& sample : next->samples) sample.content_identity = ++content_frontier_;
            current_ = std::move(next);
            // A new producer does not select a new displayed atlas or cancel
            // an in-flight interaction with the retained one.
        }
    }
    void Capture(rfdetr::ValidationSampleView sample) {
        if (!sample.pixels.preview_failure.empty()) return;
        if (!sample.pixels.chw && !sample.pixels.rgb8) return;
        EnsurePool();
        if (sample.pixels.width > visual_.maximum_width || sample.pixels.height > visual_.maximum_height) return;
        std::shared_ptr<const PredictionPreviewFrame> raw;
        {
            std::scoped_lock lock(pool_mutex_);
            raw = pool_->Capture(sample.pixels.chw, {sample.pixels.width, sample.pixels.height}, sample.pixels.stream,
                sample.prediction.detections, sample.annotations, sample.annotations.class_catalog,
                static_cast<int>(sample.annotations.class_catalog->size()), sample.pixels.rgb8, std::move(sample.pixels.custody),
                sample.pixels.stop_source, sample.pixels.source_control, sample.ground_truth, true);
        }
        if (!raw) return;
        ValidationSampleMetadata metadata;
        metadata.available = true;
        metadata.original_extent = {sample.pixels.width, sample.pixels.height};
        const auto palette = contracts::annotation_class_palette(raw->classes().size());
        const auto labels = [&](std::span<const rfdetr::Prediction> predictions, bool ground_truth) {
            for (const auto& prediction : predictions) {
                const auto category = static_cast<std::size_t>(prediction.class_reference);
                if (category >= raw->classes().size()) throw std::invalid_argument("validation sample category is absent");
                metadata.labels.push_back({{{prediction.bbox_xyxy[0], prediction.bbox_xyxy[1]}, {prediction.bbox_xyxy[2], prediction.bbox_xyxy[3]}},
                    palette[category], static_cast<std::uint32_t>(category), ground_truth, prediction.score, raw->classes()[category]});
            }
        };
        labels(raw->predictions(), false);
        labels(raw->ground_truth(), true);
        {
            std::scoped_lock lock(mutex_);
            if (!current_) return;
            auto found = std::ranges::find_if(current_->samples, [&](const auto& slot) {
                return slot.metadata.identity.generation != 0U && slot.metadata.identity.dataset_index == sample.prediction.dataset_index;
            });
            if (found == current_->samples.end()) throw std::logic_error("validation captured an unselected sample");
            metadata.identity = found->metadata.identity;
            found->metadata = std::move(metadata);
            found->raw = std::move(raw);
            if (!requested_.detail) {
                requested_.atlas = current_;
                RequestRender();
            }
        }
        static_cast<void>(worker_.NotifyContinuation());
    }
    void RequestRender() {
        if (request_revision_ == std::numeric_limits<std::uint64_t>::max()) throw contracts::FailedError("validation composition revision exhausted");
        ++request_revision_;
        dirty_ = render_requested_ = true;
        retry_remaining_ = 1U;
    }
    void Select(ValidationSampleIdentity identity) {
        {
            std::scoped_lock lock(mutex_);
            if (!displayed_) throw contracts::InvalidIntentError("validation sample is not displayed");
            const auto found = std::ranges::find_if(displayed_->samples, [&](const auto& sample) { return sample.raw && sample.metadata.identity == identity; });
            if (found == displayed_->samples.end()) throw contracts::InvalidIntentError("validation sample identity is stale");
            requested_.atlas = displayed_;
            requested_.detail = identity;
            RequestRender();
        }
        static_cast<void>(worker_.NotifyContinuation());
    }
    void CloseDetail() {
        {
            std::scoped_lock lock(mutex_);
            // Keep the atlas retained by a pending/committed detail selection.
            // Begin and partial captures never substitute their current set.
            if (!requested_.detail) requested_.atlas = displayed_;
            requested_.detail.reset();
            if (!requested_.atlas) return;
            RequestRender();
        }
        static_cast<void>(worker_.NotifyContinuation());
    }
    void SetOverlays(ValidationOverlays overlays) {
        {
            std::scoped_lock lock(mutex_);
            if (!dirty_ && image_.overlays == overlays) return;
            requested_.overlays = overlays;
            if (displayed_) {
                requested_.atlas = displayed_;
                requested_.detail.reset();
                if (image_.detail) {
                    for (const auto& sample : image_.samples)
                        if (sample.available) { requested_.detail = sample.identity; break; }
                }
            }
            if (requested_.atlas) RequestRender();
        }
        static_cast<void>(worker_.NotifyContinuation());
    }
    VisualRuntimeOwner::Notification Render(gpu::SystemImageRuntime& runtime, std::stop_token stop) {
        if (stop.stop_requested()) return {};
        std::uint64_t attempt = 0U;
        try {
            std::shared_ptr<Set> set;
            std::optional<ValidationSampleIdentity> selected;
            ValidationOverlays overlays;
            {
                std::scoped_lock lock(mutex_);
                if (!dirty_ || !render_requested_) return {};
            }
            gpu::SystemImageRuntime::CompletedOutput baseline;
            auto candidate = worker_.TryAcquireOutput(runtime, baseline);
            if (!candidate.valid()) return {};
            // Copy only six immutable sample handles/metadata under the owner lock.
            auto drawing = std::make_shared<Set>();
            {
                std::scoped_lock lock(mutex_);
                set = requested_.atlas;
                if (!set || !render_requested_) return {};
                selected = requested_.detail; overlays = requested_.overlays;
                *drawing = *set;
                attempt = request_revision_;
                render_requested_ = false;
            }
            const std::uint32_t cell_width = std::min<std::uint32_t>(256U, visual_.maximum_width / 3U);
            const std::uint32_t cell_height = std::min<std::uint32_t>(256U, visual_.maximum_height / 2U);
            VisualExtent extent{cell_width * 3U, cell_height * 2U};
            ValidationImageMetadata image;
            image.content_identity = drawing->atlas_identity;
            image.detail = selected.has_value();
            image.overlays = overlays;
            if (selected) {
                const auto found = std::ranges::find_if(drawing->samples, [&](const auto& slot) { return slot.raw && slot.metadata.identity == *selected; });
                if (found == drawing->samples.end()) return {};
                extent = found->metadata.original_extent;
                image.content_identity = found->content_identity;
            }
            std::array<PredictionPreviewComposition::Region, rfdetr::kValidationSampleCapacity> regions;
            std::size_t region_count = 0U;
            for (std::size_t index = 0; index < drawing->samples.size(); ++index) {
                const auto& sample = drawing->samples[index];
                if (!sample.raw || (selected && sample.metadata.identity != *selected)) continue;
                auto metadata = sample.metadata;
                metadata.crop = selected ? VisualRegion{0U, 0U, extent.width, extent.height} :
                    VisualRegion{static_cast<std::uint32_t>(index % 3U) * cell_width, static_cast<std::uint32_t>(index / 3U) * cell_height, cell_width, cell_height};
                regions[region_count++] = {sample.raw, metadata.crop};
                image.samples[index] = std::move(metadata);
            }
            PredictionPreviewComposition::Draw(runtime, candidate, extent, std::span(regions).first(region_count),
                {overlays.prediction_boxes, overlays.prediction_masks, overlays.ground_truth_boxes, overlays.ground_truth_masks});
            image.frame = visual_frame({PresentationSourceKind::Validation, 1U}, extent, candidate.revision());
            static_cast<void>(runtime.CommitOutput(std::move(candidate)));
            return [this, attempt, drawing = std::move(drawing), image = std::move(image)]() mutable {
                {
                    std::scoped_lock lock(mutex_);
                    displayed_ = std::move(drawing); image_ = std::move(image);
                    if (attempt == request_revision_) dirty_ = false;
                }
                Notify();
            };
        } catch (...) {
            const auto failure = std::current_exception();
            const auto context_failure = gpu::find_image_failure<gpu::CudaContextFailure>(failure);
            bool terminal_context = false;
            if (context_failure) {
                try { std::rethrow_exception(context_failure); }
                catch (const gpu::CudaContextFailure& error) { terminal_context = error.terminal(); }
            }
            if (terminal_context || !retirement_->admission_open() || gpu::is_image_execution_failure(failure)) {
                static_cast<void>(runtime.Retire(failure));
                throw;
            }
            // Proved ordinary draw failure leaves candidate rollback and the last
            // selected image with its original metadata in the healthy runtime.
            bool retry = false;
            {
                std::scoped_lock lock(mutex_);
                if ((attempt == 0U || attempt == request_revision_) && dirty_) {
                    retry = retry_remaining_ != 0U;
                    retry_remaining_ = 0U;
                    render_requested_ = retry;
                }
            }
            // One automatic retry per request; a persistent refusal retains the
            // exact pending request for an explicit repeated action/supersession.
            return [this, retry] {
                if (retry) static_cast<void>(worker_.NotifyContinuation());
                Notify();
            };
        }
    }
    void Shutdown() noexcept {
        worker_.StopAndWait();
        {
            std::scoped_lock lock(mutex_);
            current_.reset(); displayed_.reset(); requested_.atlas.reset();
        }
        {
            std::scoped_lock lock(pool_mutex_);
            pool_.reset(); context_.reset();
        }
        worker_.FinishStoppedRetirement();
    }
    mutable std::mutex mutex_, pool_mutex_;
    VisualDeviceSettings visual_;
    std::function<void()> changed_;
    PredictionPreviewPool::TransferOperations transfers_;
    std::optional<gpu::DeviceExecution> execution_;
    std::optional<gpu::DeviceContext> context_;
    // Current capture set, displayed immutable set and one pending/in-flight set.
    // Each raw slot reserves its own lease; composition and context construction
    // each reserve one additional exact aggregate before doing device work.
    static_assert(rfdetr::kValidationSampleCapacity == 3U * 2U);
    static constexpr std::size_t kRawCapacity = 3U * rfdetr::kValidationSampleCapacity;
    static_assert(kRawCapacity + 2U <= gpu::TerminalCudaRetirementOwner::kMaximumCapacity);
    std::shared_ptr<gpu::TerminalCudaRetirementOwner> retirement_ = std::make_shared<gpu::TerminalCudaRetirementOwner>(kRawCapacity + 2U);
    std::unique_ptr<PredictionPreviewPool> pool_;
    std::shared_ptr<Set> current_, displayed_;
    Composition requested_;
    ValidationImageMetadata image_;
    std::uint64_t content_frontier_ = 0U;
    std::uint64_t request_revision_ = 0U;
    unsigned retry_remaining_ = 0U;
    bool dirty_ = false, render_requested_ = false;
    VisualRuntimeOwner worker_;
};
ValidationSamples::ValidationSamples(VisualDeviceSettings visual, std::function<void()> changed, PredictionPreviewPool::TransferOperations transfers)
    : impl_(std::make_unique<Impl>(visual, std::move(changed), transfers)) {}
ValidationSamples::~ValidationSamples() = default;
void ValidationSamples::Begin(std::uint64_t generation, std::span<const std::uint32_t> indices) { impl_->Begin(generation, indices); }
void ValidationSamples::Capture(rfdetr::ValidationSampleView sample) { impl_->Capture(std::move(sample)); }
void ValidationSamples::Select(ValidationSampleIdentity identity) { impl_->Select(identity); }
void ValidationSamples::CloseDetail() { impl_->CloseDetail(); }
void ValidationSamples::SetOverlays(ValidationOverlays overlays) { impl_->SetOverlays(overlays); }
ValidationSnapshot ValidationSamples::snapshot() const {
    std::scoped_lock lock(impl_->mutex_);
    ValidationSnapshot result;
    result.frame = impl_->image_.frame;
    result.content_identity = impl_->image_.content_identity;
    result.detail = impl_->image_.detail;
    result.overlays = impl_->image_.overlays;
    for (std::size_t index = 0; index < impl_->image_.samples.size(); ++index) {
        result.sample_identities[index] = impl_->image_.samples[index].identity;
        result.sample_available[index] = impl_->image_.samples[index].available;
    }
    return result;
}
std::optional<ValidationImageMetadata> ValidationSamples::ImageSnapshot(const VisualFrame& frame) const {
    std::scoped_lock lock(impl_->mutex_);
    return impl_->image_.frame == frame ? std::optional{impl_->image_} : std::nullopt;
}
gpu::BorrowedImageProductReadView ValidationSamples::BorrowFrame() const { const auto frame = snapshot().frame; return borrow_matching_visual_product(frame, impl_->worker_); }
gpu::BorrowedImageWorkspace ValidationSamples::BorrowWorkspace() const { return impl_->worker_.BorrowWorkspace(); }
gpu::ImageWorkspaceObservation ValidationSamples::ObserveWorkspace() const { return impl_->worker_.ObserveWorkspace(); }
void ValidationSamples::RequestWorkspace(VisualWorkspaceRequest request) { impl_->worker_.RequestWorkspace(std::move(request)); }
void ValidationSamples::Shutdown() noexcept { impl_->Shutdown(); }
}
