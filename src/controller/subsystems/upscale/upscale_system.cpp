#include "src/controller/subsystems/upscale/upscale_system.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/frameworks/gpu/system_image_worker.h"

#include <cuda.h>

#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "src/common/types/generation.h"

import mmltk.backend.imaging.upscale.image_upscaler;
import mmltk.backend.imaging.raster;

namespace mmltk::controller {

VisualExtent checked_upscale_output_extent(const VisualExtent source) {
    if (!source.valid() || source.width > std::numeric_limits<std::uint32_t>::max() / kUpscaleOutputScale ||
        source.height > std::numeric_limits<std::uint32_t>::max() / kUpscaleOutputScale)
        throw contracts::InvalidIntentError("Upscale four-times extent is invalid or overflows");
    return {
        .width = source.width * kUpscaleOutputScale,
        .height = source.height * kUpscaleOutputScale,
    };
}

namespace {

namespace native_upscale = mmltk::backend::imaging::upscale;

void ensure_cuda_driver_ok(const CUresult status, const char* const operation) {
    if (status == CUDA_SUCCESS) return;
    const char* detail = nullptr;
    static_cast<void>(cuGetErrorString(status, &detail));
    throw std::runtime_error(std::string{operation} + ": " + (detail == nullptr ? "unknown CUDA error" : detail));
}

[[nodiscard]] CUcontext stream_context(const std::uintptr_t stream, const char* const operation) {
    CUcontext context = nullptr;
    ensure_cuda_driver_ok(cuStreamGetCtx(reinterpret_cast<CUstream>(stream), &context), operation);
    if (context == nullptr) throw std::runtime_error(std::string{operation} + ": stream has no CUDA context");
    return context;
}

class UpscaleStreamBridge final {
   public:
    explicit UpscaleStreamBridge(CUcontext context) : context_(context) {
        ensure_cuda_driver_ok(cuCtxSetCurrent(context_), "bind Upscale bridge context");
        ensure_cuda_driver_ok(cuEventCreate(&source_ready_, CU_EVENT_DISABLE_TIMING), "create Upscale source-ready event");
        try {
            ensure_cuda_driver_ok(cuEventCreate(&model_done_, CU_EVENT_DISABLE_TIMING), "create Upscale model-done event");
        } catch (...) {
            if (cuEventDestroy(source_ready_) != CUDA_SUCCESS) std::terminate();
            source_ready_ = nullptr;
            throw;
        }
    }
    ~UpscaleStreamBridge() {
        if (cuCtxSetCurrent(context_) != CUDA_SUCCESS) std::terminate();
        const auto model_status = cuEventDestroy(model_done_);
        const auto source_status = cuEventDestroy(source_ready_);
        if (model_status != CUDA_SUCCESS || source_status != CUDA_SUCCESS) std::terminate();
    }
    UpscaleStreamBridge(const UpscaleStreamBridge&) = delete;
    UpscaleStreamBridge& operator=(const UpscaleStreamBridge&) = delete;

    void AwaitSource(const std::uintptr_t source_stream, const std::uintptr_t model_stream) const {
        if (source_stream == model_stream) return;
        ensure_cuda_driver_ok(cuCtxSetCurrent(context_), "bind Upscale bridge context");
        ensure_cuda_driver_ok(cuEventRecord(source_ready_, reinterpret_cast<CUstream>(source_stream)), "record Upscale source readiness");
        ensure_cuda_driver_ok(cuStreamWaitEvent(reinterpret_cast<CUstream>(model_stream), source_ready_, 0U),
                              "wait for Upscale source readiness");
    }

    void JoinModel(const std::uintptr_t source_stream, const std::uintptr_t model_stream) const {
        if (source_stream == model_stream) return;
        ensure_cuda_driver_ok(cuCtxSetCurrent(context_), "bind Upscale bridge context");
        ensure_cuda_driver_ok(cuEventRecord(model_done_, reinterpret_cast<CUstream>(model_stream)), "record Upscale model completion");
        ensure_cuda_driver_ok(cuStreamWaitEvent(reinterpret_cast<CUstream>(source_stream), model_done_, 0U),
                              "join Upscale model completion");
    }

   private:
    CUcontext context_ = nullptr;
    CUevent source_ready_ = nullptr;
    CUevent model_done_ = nullptr;
};

class NativeUpscaleModel final : public UpscaleAlgorithm {
   public:
    void Semantics(const mmltk::frameworks::gpu::ImagePlaneView source, const mmltk::frameworks::gpu::ImagePlaneView target,
                   const std::uintptr_t stream) override {
        if (!source.valid()) {
            ensure_cuda_driver_ok(cuMemsetD2D8Async(target.data, target.descriptor.pitch_bytes, 0, target.descriptor.row_bytes(),
                                                    target.descriptor.height, reinterpret_cast<CUstream>(stream)),
                                  "clear Upscale absent semantics");
            return;
        }
        if (mmltk::backend::imaging::raster::scale_rgba_nearest(
                {reinterpret_cast<const std::uint8_t*>(source.data), source.descriptor.pitch_bytes,
                 static_cast<int>(source.descriptor.width), static_cast<int>(source.descriptor.height)},
                {reinterpret_cast<std::uint8_t*>(target.data), target.descriptor.pitch_bytes, static_cast<int>(target.descriptor.width),
                 static_cast<int>(target.descriptor.height)},
                stream) != 0)
            throw std::runtime_error("Upscale semantic scaling failed");
    }
    explicit NativeUpscaleModel(const int device)
        : device_(device),
          release_failure_(std::make_exception_ptr(std::runtime_error("failed to release Upscale model resources"))),
          models_{{
              {1U, 1U, 1U, device},
              {2U, 1U, 1U, device},
              {3U, 1U, 1U, device},
          }} {}
    ~NativeUpscaleModel() override {
        if (owner_ || stream_bridge_) std::terminate();
    }
    Release ReleaseResources() noexcept override {
        if (!owner_) return {};
        if (owner_->Stop() != native_upscale::kImageUpscalerSuccess) {
            return {
                .all_released = false,
                .failure = release_failure_,
            };
        }
        stream_bridge_.reset();
        owner_.reset();
        return {};
    }
    void Warm() override {
        CUcontext context = nullptr;
        ensure_cuda_driver_ok(cuCtxGetCurrent(&context), "resolve Upscale warm context");
        if (context == nullptr) throw std::runtime_error("Upscale warm context is unavailable");
        Activate(context);
    }
    void Run(const UpscaleKernel kernel, const mmltk::frameworks::gpu::ImagePlaneView source,
             const mmltk::frameworks::gpu::ImagePlaneView target, const std::uintptr_t stream) override {
        const auto source_context = stream_context(stream, "resolve Upscale image-runtime context");
        Activate(source_context);
        const auto index = static_cast<std::size_t>(kernel);
        const auto mode = static_cast<native_upscale::ImageUpscalerMode>(index);
        auto operation = owner_->client().ClaimOperation();
        const auto model_stream = operation ? operation.operation_stream(mode, device_) : 0U;
        if (!operation || model_stream == 0U) throw std::runtime_error("Upscale model execution failed");
        if (stream_context(model_stream, "resolve Upscale model context") != source_context)
            throw std::runtime_error("Upscale model and image runtime use different CUDA contexts");
        if (!stream_bridge_) stream_bridge_.emplace(source_context);
        stream_bridge_->AwaitSource(stream, model_stream);
        if (!operation.run_rgba8(models_[index], mode, reinterpret_cast<const std::uint8_t*>(source.data), source.descriptor.pitch_bytes,
                                 source.descriptor.width, source.descriptor.height, reinterpret_cast<std::uint8_t*>(target.data),
                                 target.descriptor.pitch_bytes, model_stream))
            throw std::runtime_error("Upscale model execution failed");
        stream_bridge_->JoinModel(stream, model_stream);
    }

   private:
    void Activate(CUcontext context) {
        if (owner_) return;
        ensure_cuda_driver_ok(cuCtxSetCurrent(context), "bind Upscale image-runtime context");
        auto created = native_upscale::ImageUpscaler::Create(device_, 1U, {.models = models_});
        if (!created) throw std::runtime_error("failed to construct the Upscale model aggregate");
        if (!(*created)->Activate()) throw std::runtime_error("failed to activate the Upscale model aggregate");
        owner_ = std::move(*created);
    }

    int device_ = -1;
    std::exception_ptr release_failure_;
    std::optional<UpscaleStreamBridge> stream_bridge_;
    std::unique_ptr<native_upscale::ImageUpscaler> owner_;
    std::array<native_upscale::ImageUpscalerModelHandle, 3U> models_{};
};

}  // namespace

class UpscaleSystem::Impl final {
   public:
    Impl(const VisualDeviceSettings settings, VisualRuntimeFactory factory, ExactVisualDocumentBorrower borrow_source,
         SystemEventSink<event_type> events, VisualDiagnosticSink diagnostics)
        : settings_(settings),
          borrow_source_(std::move(borrow_source)),
          events_(std::move(events)),
          diagnostics_(diagnostics),
          worker_(std::move(factory), [this](const std::exception_ptr failure) {
              auto detail = visual_failure_detail(failure, "Upscale GPU worker failed");
              {
                  std::scoped_lock lock(mutex_);
                  state_.ready = false;
                  state_.busy = false;
                  state_.frame = {};
                  warm_admitted_ = false;
                  warm_attempted_ = false;
                  processed_.reset();
                  processed_clean_revision_ = 0U;
                  AdvanceRevision();
              }
              report_visual_worker_failure(diagnostics_, VisualSystemKind::Upscale, settings_.device, detail);
              Publish(event_type{UpscaleFailed{snapshot(), std::move(detail)}});
          }) {
        if (!settings_.valid() || !borrow_source_) throw contracts::InvalidIntentError("Upscale device settings are invalid");
    }
    void Warm() noexcept {
        std::scoped_lock admission_lock(mutex_);
        if (warm_admitted_ || warm_attempted_) return;
        warm_admitted_ = true;
        diagnostics_({
            .system = VisualSystemKind::Upscale,
            .operation = VisualDiagnosticOperation::UpscaleWarmAdmissionStarted,
            .device = settings_.device,
        });
        bool admitted = false;
        try {
            admitted = worker_.SubmitOrdered(
                [this](mmltk::frameworks::gpu::SystemImageRuntime& runtime, std::stop_token) -> detail::VisualRuntimeOwner::Notification {
                    std::string failure;
                    try {
                        auto* const model = dynamic_cast<UpscaleAlgorithm*>(runtime.model());
                        if (model == nullptr) throw std::runtime_error("Upscale runtime model is unavailable during warm-up");
                        diagnostics_({
                            .system = VisualSystemKind::Upscale,
                            .operation = VisualDiagnosticOperation::UpscaleWarmRuntimeStarted,
                            .device = settings_.device,
                        });
                        model->Warm();
                        diagnostics_({
                            .system = VisualSystemKind::Upscale,
                            .operation = VisualDiagnosticOperation::UpscaleWarmRuntimeCompleted,
                            .device = settings_.device,
                        });
                    } catch (...) { failure = visual_failure_detail(std::current_exception(), "Upscale model warm-up failed"); }
                    {
                        std::scoped_lock lock(mutex_);
                        warm_admitted_ = false;
                        warm_attempted_ = true;
                    }
                    if (failure.empty()) return {};
                    return [this, failure = std::move(failure)]() mutable noexcept {
                        report_visual_worker_failure(diagnostics_, VisualSystemKind::Upscale, settings_.device, failure);
                        Publish(event_type{UpscaleFailed{snapshot(), std::move(failure)}});
                    };
                });
        } catch (...) {}
        if (!admitted) warm_admitted_ = false;
    }
    UpscaleSnapshot Start(const UpscaleRequest request) {
        if (request.kernel > UpscaleKernel::RealPlksr) throw contracts::InvalidIntentError("Upscale kernel is invalid");
        auto source = borrow_source_(request.source);
        if (!source.valid())
            throw contracts::UnavailableError("Upscale source frame is missing or no longer matches its identity, revision, and extent");
        const auto source_descriptor = source.pixels.plane(0U).plane().descriptor;
        const auto target = checked_upscale_output_extent({.width = source_descriptor.width, .height = source_descriptor.height});
        if (target.width > settings_.maximum_width || target.height > settings_.maximum_height)
            throw contracts::InvalidIntentError("Upscale four-times extent exceeds device bounds");
        {
            std::scoped_lock admission_lock(mutex_);
            const auto prior = state_;
            const auto demand = ++demand_;
            diagnostics_({.system = VisualSystemKind::Upscale,
                          .operation = VisualDiagnosticOperation::UpscaleRequestAdmitted,
                          .device = settings_.device,
                          .generation = demand,
                          .value = request.source.revision,
                          .detail = static_cast<std::uint64_t>(request.kernel)});
            state_.kernel = request.kernel;
            state_.input = request.source;
            state_.busy = true;
            state_.ready = false;
            AdvanceRevision();
            if (!worker_.SubmitLatest([this, request, target, demand, source_descriptor, source = std::move(source)](
                                          mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                          std::stop_token stop) mutable -> detail::VisualRuntimeOwner::Notification {
                    {
                        std::scoped_lock lock(mutex_);
                        if (demand != demand_) return {};
                    }
                    diagnostics_({.system = VisualSystemKind::Upscale,
                                  .operation = VisualDiagnosticOperation::UpscaleWorkerStarted,
                                  .device = settings_.device,
                                  .generation = demand,
                                  .value = request.source.revision,
                                  .detail = static_cast<std::uint64_t>(request.kernel)});
                    if (processed_ && processed_->source == request.source && processed_->kernel == request.kernel) {
                        std::scoped_lock lock(mutex_);
                        if (demand != demand_ || stop.stop_requested()) return {};
                        // Obsolete work may have replaced the private output without
                        // committing its document/frame. Only reselect a committed pair.
                        if (state_.input == request.source && state_.frame.revision == runtime.output().revision()) {
                            state_.busy = false;
                            state_.ready = true;
                            AdvanceRevision();
                            diagnostics_({.system = VisualSystemKind::Upscale,
                                          .operation = VisualDiagnosticOperation::UpscaleResultReused,
                                          .device = settings_.device,
                                          .generation = demand,
                                          .value = state_.frame.revision});
                            return [this] { PublishChanged(); };
                        }
                    }
                    auto document = scale_visual_document(source.document, kUpscaleOutputScale);
                    auto* const model = dynamic_cast<UpscaleAlgorithm*>(runtime.model());
                    if (model == nullptr) throw std::runtime_error("Upscale runtime model is unavailable");
                    diagnostics_({.system = VisualSystemKind::Upscale,
                                  .operation = VisualDiagnosticOperation::UpscaleCopyStarted,
                                  .device = settings_.device,
                                  .generation = demand,
                                  .value = request.source.revision});
                    const auto copied_planes = diagnostics_.valid() ? source.pixels.plane_count() : 0U;
                    const auto paths = runtime.CopyInputFrom(
                        std::move(source.pixels), [model](const auto plane, const auto stream) { model->Semantics({}, plane, stream); });
                    diagnostics_({
                        .system = VisualSystemKind::Upscale,
                        .operation = VisualDiagnosticOperation::CopyCompleted,
                        .device = settings_.device,
                        .generation = demand,
                        .value = request.source.revision,
                        .copy_path = paths[0U],
                    });
                    if (stop.stop_requested()) return {};
                    const auto input = runtime.BorrowInput();
                    if (!input.valid()) throw std::runtime_error("Upscale runtime model is unavailable");
                    if (diagnostics_.valid()) {
                        const auto plane = input.plane(0U).plane();
                        diagnostics_({.system = VisualSystemKind::Upscale,
                                      .operation = VisualDiagnosticOperation::UpscaleInputGeometry,
                                      .device = settings_.device,
                                      .generation = demand,
                                      .value = source_descriptor.pitch_bytes,
                                      .detail = plane.descriptor.pitch_bytes,
                                      .context = {.capacity_width = plane.descriptor.width,
                                                  .capacity_height = plane.descriptor.height,
                                                  .frame_revision = request.source.revision}});
                        diagnostics_({.system = VisualSystemKind::Upscale,
                                      .operation = VisualDiagnosticOperation::UpscaleInputAllocation,
                                      .device = settings_.device,
                                      .generation = demand,
                                      .value = plane.data,
                                      .detail = plane.descriptor.row_bytes() * plane.descriptor.height * copied_planes});
                    }
                    const auto clean_identity = [](const VisualFrame& frame) {
                        return frame.clean_revision == 0U ? frame.revision : frame.clean_revision;
                    };
                    const bool reuse_clean =
                        processed_ && processed_->kernel == request.kernel && processed_->source.source == request.source.source &&
                        processed_->source.extent == request.source.extent && processed_->source.content == request.source.content &&
                        clean_identity(processed_->source) == clean_identity(request.source);
                    runtime.Publish(
                        target.width, target.height,
                        [this, model, &input, request, reuse_clean, demand](const auto output, const auto semantic, const auto stream) {
                            if (diagnostics_.valid())
                                diagnostics_(
                                    {.system = VisualSystemKind::Upscale,
                                     .operation = VisualDiagnosticOperation::UpscaleOutputAllocation,
                                     .device = settings_.device,
                                     .generation = demand,
                                     .value = output.data,
                                     .detail = output.descriptor.pitch_bytes,
                                     .context = {.capacity_width = output.descriptor.width, .capacity_height = output.descriptor.height}});
                            if (!reuse_clean) model->Run(request.kernel, input.plane(0U).plane(), output, stream);
                            model->Semantics(input.plane(1U).plane(), semantic, stream);
                        });
                    processed_ = request;
                    if (!reuse_clean) processed_clean_revision_ = runtime.output().revision();
                    {
                        std::scoped_lock lock(mutex_);
                        if (demand != demand_) return {};
                        state_.busy = false;
                        state_.ready = true;
                        state_.input = request.source;
                        state_.kernel = request.kernel;
                        state_.scene = document->scene;
                        document_ = std::move(document);
                        state_.frame = {
                            .source =
                                {
                                    .kind = PresentationSourceKind::Upscale,
                                    .instance = 1U,
                                },
                            .extent = target,
                            .revision = runtime.output().revision(),
                            .content = {request.source.content.x * kUpscaleOutputScale, request.source.content.y * kUpscaleOutputScale,
                                        request.source.content.width * kUpscaleOutputScale,
                                        request.source.content.height * kUpscaleOutputScale},
                            .clean_revision = processed_clean_revision_,
                        };
                        AdvanceRevision();
                    }
                    diagnostics_({
                        .system = VisualSystemKind::Upscale,
                        .operation = VisualDiagnosticOperation::UpscaleModelSubmitted,
                        .device = settings_.device,
                        .generation = runtime.output().revision(),
                        .value = static_cast<std::uint64_t>(request.kernel),
                        .detail = demand,
                    });
                    return [this] { PublishChanged(); };
                })) {
                state_ = prior;
                throw contracts::UnavailableError("Upscale desired-product ingress is stopped");
            }
        }
        return snapshot();
    }
    UpscaleSnapshot snapshot() const {
        UpscaleSnapshot result;
        {
            std::scoped_lock lock(mutex_);  // CLEANUP-IGNORE: This typed snapshot projects Upscale facts while deriving
                                            // admission from the shared worker owner.
            result = state_;
        }
        return result;
    }
    void Stop() noexcept {
        {
            std::scoped_lock lock(mutex_);
            ++demand_;
            state_.busy = false;
            state_.ready = false;
            AdvanceRevision();
        }
        worker_.RequestActiveStop();
        PublishChanged();
    }
    void Shutdown() noexcept { worker_.StopAndWait(); }
    bool stopped() const noexcept { return worker_.stopped(); }
    void Publish(event_type event) noexcept { publish_visual_event_noexcept(events_, std::move(event)); }
    void PublishChanged() noexcept { Publish(event_type{UpscaleChanged{snapshot()}}); }
    void Cancelled() noexcept {
        {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            AdvanceRevision();
        }
        PublishChanged();
    }
    void AdvanceRevision() { state_.revision = mmltk::common::types::advance_monotonic_identity(state_.revision); }

   private:
    friend class UpscaleSystem;

    // CLEANUP-IGNORE: The shared checked-borrow helper owns product validation; this sealed receiver still reads its
    // own typed readiness snapshot under its own mutex.
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const {
        VisualFrame committed;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.ready) return {};
            committed = state_.frame;
        }
        return borrow_matching_visual_product(committed, worker_);
    }

    VisualDeviceSettings settings_;
    ExactVisualDocumentBorrower borrow_source_;
    SystemEventSink<event_type> events_;
    VisualDiagnosticSink diagnostics_{};
    mutable std::mutex mutex_;
    UpscaleSnapshot state_;
    std::uint64_t demand_ = 0U;
    std::optional<UpscaleRequest> processed_;
    std::uint64_t processed_clean_revision_ = 0U;
    std::shared_ptr<const VisualDocument> document_;
    bool warm_admitted_ = false;
    bool warm_attempted_ = false;
    detail::VisualRuntimeOwner worker_;
};

// CLEANUP-IGNORE: The public Upscale facade forwards construction to its one private implementation owner.
UpscaleSystem::UpscaleSystem(const VisualDeviceSettings settings, VisualRuntimeFactory factory, ExactVisualDocumentBorrower borrow_source,
                             SystemEventSink<event_type> events,
                             // CLEANUP-IGNORE: Upscale construction forwards its own diagnostic sink at the pimpl boundary.
                             VisualDiagnosticSink diagnostics)
    : impl_(std::make_unique<Impl>(settings, std::move(factory), std::move(borrow_source), std::move(events), diagnostics)) {}
UpscaleSystem::~UpscaleSystem() = default;
void UpscaleSystem::Warm() noexcept { impl_->Warm(); }
UpscaleSnapshot UpscaleSystem::Start(const UpscaleRequest request) {
    // CLEANUP-IGNORE: Upscale facade forwarding preserves its domain-specific receiver-owned transfer endpoint.
    return impl_->Start(request);
}
void UpscaleSystem::Stop() noexcept {
    // CLEANUP-IGNORE: Upscale facade forwarding preserves its void Stop semantics.
    impl_->Stop();
}
void UpscaleSystem::Shutdown() noexcept { impl_->Shutdown(); }
bool UpscaleSystem::stopped() const noexcept { return impl_->stopped(); }
UpscaleSnapshot UpscaleSystem::snapshot() const { return impl_->snapshot(); }
mmltk::frameworks::gpu::BorrowedImageProductReadView UpscaleSystem::BorrowFrame() const { return impl_->BorrowFrame(); }
VisualDocumentRead UpscaleSystem::BorrowDocument(const VisualFrame& frame) const {
    std::shared_ptr<const VisualDocument> document;
    {
        std::scoped_lock lock(impl_->mutex_);
        if (!impl_->state_.ready || impl_->state_.frame != frame) return {};
        document = impl_->document_;
    }
    return {borrow_matching_visual_product(frame, impl_->worker_), std::move(document)};
}

VisualRuntimeFactory make_native_upscale_runtime_factory(const VisualDeviceSettings settings) {
    if (!settings.valid()) throw contracts::InvalidIntentError("Upscale device settings are invalid");
    return [settings, execution = resolve_visual_device_execution(settings)] {
        return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
            .device = settings.device,
            .model = std::make_unique<NativeUpscaleModel>(settings.device),
            .context_mode = mmltk::frameworks::gpu::DeviceContextMode::PrimaryInterop,
            .input_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
            .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
            .numa_node = settings.numa_node,
            .execution = execution,
        });
    };
}

}  // namespace mmltk::controller
