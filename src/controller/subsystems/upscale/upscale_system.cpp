#include "src/controller/subsystems/upscale/upscale_system.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/cuda_error.h"

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

void rethrow_physical_upscale_failure(const std::exception_ptr& failure) {
    if (mmltk::frameworks::gpu::is_image_execution_failure(failure)) std::rethrow_exception(failure);
    try {
        if (const auto cuda = mmltk::frameworks::gpu::find_image_failure<mmltk::frameworks::gpu::CudaError>(failure))
            std::rethrow_exception(cuda);
    } catch (const mmltk::frameworks::gpu::CudaError& cuda) {
        if (cuda.shared_failure()) throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(failure);
    }
}

class UpscaleDriverFailure final : public std::runtime_error {
   public:
    UpscaleDriverFailure(const CUresult status, const std::string& detail)
        : std::runtime_error(detail + " (CUDA driver status " + std::to_string(static_cast<int>(status)) + ")"), status_(status) {}
    [[nodiscard]] CUresult status() const noexcept { return status_; }
   private:
    CUresult status_;
};

void ensure_cuda_driver_ok(const CUresult status, const char* const operation) {
    if (status == CUDA_SUCCESS) return;
    const char* detail = nullptr;
    static_cast<void>(cuGetErrorString(status, &detail));
    const auto failure = UpscaleDriverFailure(status, std::string{operation} + ": " +
                                                      (detail == nullptr ? "unknown CUDA error" : detail));
    if (mmltk::frameworks::gpu::cuda_shared_failure(status))
        throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(std::make_exception_ptr(failure));
    throw failure;
}

[[nodiscard]] CUcontext stream_context(const std::uintptr_t stream, const char* const operation) {
    CUcontext context = nullptr;
    ensure_cuda_driver_ok(cuStreamGetCtx(reinterpret_cast<CUstream>(stream), &context), operation);
    if (context == nullptr) ensure_cuda_driver_ok(CUDA_ERROR_INVALID_CONTEXT, operation);
    return context;
}

class UpscaleStreamBridge final {
   public:
    explicit UpscaleStreamBridge(CUcontext context) : context_(context) {}
    void Activate() {
        ensure_cuda_driver_ok(cuCtxSetCurrent(context_), "bind Upscale bridge context");
        if (source_ready_ == nullptr)
            ensure_cuda_driver_ok(cuEventCreate(&source_ready_, CU_EVENT_DISABLE_TIMING), "create Upscale source-ready event");
        if (model_done_ == nullptr)
            ensure_cuda_driver_ok(cuEventCreate(&model_done_, CU_EVENT_DISABLE_TIMING), "create Upscale model-done event");
    }
    ~UpscaleStreamBridge() {
        if (model_done_ != nullptr || source_ready_ != nullptr) std::terminate();
    }
    std::exception_ptr ReleaseResources() noexcept {
        std::exception_ptr failure;
        const auto record = [&](CUresult status, const char* operation) {
            if (status == CUDA_SUCCESS) return true;
            try { throw UpscaleDriverFailure(status, operation); }
            catch (...) { failure = mmltk::frameworks::gpu::combine_image_failures(failure, std::current_exception()); }
            return false;
        };
        if (!record(cuCtxSetCurrent(context_), "bind Upscale bridge release context")) return failure;
        for (auto* event : {&model_done_, &source_ready_})
            if (*event != nullptr && record(cuEventDestroy(*event), "release Upscale bridge event")) *event = nullptr;
        return failure;
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
        const auto status = mmltk::backend::imaging::raster::scale_rgba_nearest(
                {reinterpret_cast<const std::uint8_t*>(source.data), source.descriptor.pitch_bytes,
                 static_cast<int>(source.descriptor.width), static_cast<int>(source.descriptor.height)},
                {reinterpret_cast<std::uint8_t*>(target.data), target.descriptor.pitch_bytes, static_cast<int>(target.descriptor.width),
                 static_cast<int>(target.descriptor.height)},
                stream);
        mmltk::frameworks::gpu::ensure_cuda_ok(static_cast<cudaError_t>(status), "Upscale semantic scaling failed");
    }
    explicit NativeUpscaleModel(const int device, native_upscale::ImageUpscalerExecutionCheckpoint checkpoint)
        : device_(device),
          release_failure_(std::make_exception_ptr(std::runtime_error("failed to release Upscale model resources"))),
          checkpoint_(std::move(checkpoint)), models_{{
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
            std::exception_ptr physical;
            try {
                throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(release_failure_, owner_->cleanup_failure());
            } catch (...) { physical = std::current_exception(); }
            return {
                .all_released = false,
                .failure = std::move(physical),
            };
        }
        if (stream_bridge_) {
            if (auto failure = stream_bridge_->ReleaseResources()) {
                try { throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(std::move(failure)); }
                catch (...) { return {.all_released = false, .failure = std::current_exception()}; }
            }
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
    [[nodiscard]] bool GraphReplay(const UpscaleKernel kernel) const override {
        if (!owner_) return false;
        auto operation = owner_->client().ClaimOperation();
        return operation && operation.graph_replay(static_cast<native_upscale::ImageUpscalerMode>(kernel));
    }
    void Run(const UpscaleKernel kernel, const mmltk::frameworks::gpu::ImagePlaneView source,
             const mmltk::frameworks::gpu::ImagePlaneView target, const std::uintptr_t stream,
             const std::function<bool()>& current) override {
        if (current && !current()) return;
        const auto source_context = stream_context(stream, "resolve Upscale image-runtime context");
        Activate(source_context);
        const auto index = static_cast<std::size_t>(kernel);
        const auto mode = static_cast<native_upscale::ImageUpscalerMode>(index);
        const auto still_current = [&] { return !current || current(); };
        auto operation = owner_->client().ClaimOperation();
        std::uintptr_t model_stream = 0U;
        try {
            model_stream = operation ? operation.operation_stream(mode, device_, still_current) : 0U;
        } catch (const native_upscale::ImageUpscalerUnsettledFailure& failure) {
            throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(failure.failure, failure.settlement);
        } catch (const mmltk::frameworks::gpu::CudaError& failure) {
            if (failure.shared_failure())
                throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(std::current_exception());
            throw native_upscale::ImageUpscalerInitializationFailure(std::current_exception());
        }
        if (model_stream == 0U && !still_current()) return;
        if (!operation || model_stream == 0U) throw std::runtime_error("Upscale model execution failed");
        if (stream_context(model_stream, "resolve Upscale model context") != source_context)
            ensure_cuda_driver_ok(CUDA_ERROR_INVALID_CONTEXT, "Upscale model and image runtime use different CUDA contexts");
        try {
        if (!stream_bridge_) stream_bridge_.emplace(source_context);
        try {
            stream_bridge_->Activate();
        } catch (...) {
            const auto failure = std::current_exception();
            rethrow_physical_upscale_failure(failure);
            throw native_upscale::ImageUpscalerInitializationFailure(failure);
        }
        stream_bridge_->AwaitSource(stream, model_stream);
        const auto outcome = operation.run_rgba8(models_[index], mode, reinterpret_cast<const std::uint8_t*>(source.data), source.descriptor.pitch_bytes,
                                 source.descriptor.width, source.descriptor.height, reinterpret_cast<std::uint8_t*>(target.data),
                                 target.descriptor.pitch_bytes, model_stream, still_current);
        stream_bridge_->JoinModel(stream, model_stream);
        if (outcome == native_upscale::ImageUpscalerOutcome::Cancelled) return;
        } catch (const native_upscale::ImageUpscalerUnsettledFailure& failure) {
            throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(failure.failure, failure.settlement);
        } catch (...) {
            const auto failure = std::current_exception();
            const auto settled = cuStreamSynchronize(reinterpret_cast<CUstream>(model_stream));
            if (settled != CUDA_SUCCESS)
                throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(failure,
                    std::make_exception_ptr(UpscaleDriverFailure(settled, "Upscale model stream could not settle")));
            const auto residual = cudaGetLastError();
            if (residual != cudaSuccess) {
                const mmltk::frameworks::gpu::CudaError cuda(residual, "Upscale settled runtime error");
                if (cuda.shared_failure())
                    throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(failure, std::make_exception_ptr(cuda));
            }
            rethrow_physical_upscale_failure(failure);
            std::rethrow_exception(failure);
        }
    }

   private:
    void Activate(CUcontext context) {
        if (owner_) return;
        ensure_cuda_driver_ok(cuCtxSetCurrent(context), "bind Upscale image-runtime context");
        auto created = native_upscale::ImageUpscaler::Create(device_, 1U, {.models = models_, .checkpoint = checkpoint_});
        if (!created) throw std::runtime_error("failed to construct the Upscale model aggregate");
        if (!(*created)->Activate()) throw std::runtime_error("failed to activate the Upscale model aggregate");
        owner_ = std::move(*created);
    }

    int device_ = -1;
    std::exception_ptr release_failure_;
    std::optional<UpscaleStreamBridge> stream_bridge_;
    std::unique_ptr<native_upscale::ImageUpscaler> owner_;
    native_upscale::ImageUpscalerExecutionCheckpoint checkpoint_;
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
              std::optional<UpscaleRequest> request;
              {
                  std::scoped_lock lock(mutex_);
                  request = state_.pending;
                  state_.ready = false;
                  state_.busy = false;
                  state_.frame = {};
                  warm_admitted_ = false;
                  warm_attempted_ = false;
                  warm_initialized_ = false;
                  records_ = {};
                  selected_ = {};
                  input_request_.reset();
                  input_document_.reset();
                  state_.pending.reset();
                  state_.methods = {};
                  AdvanceRevision();
              }
              report_visual_worker_failure(diagnostics_, contracts::DiagnosticOwner::Upscale, settings_.device, detail);
              Publish(event_type{UpscaleFailed{snapshot(), std::move(detail), request, UpscaleFailureKind::Physical}});
          }) {
        if (!settings_.valid() || !borrow_source_) throw contracts::InvalidIntentError("Upscale device settings are invalid");
        worker_.RegisterContinuation([this](auto& runtime, auto stop) { return WarmNext(runtime, stop); });
    }
    ~Impl() { Shutdown(); }
    void Warm(const VisualExtent extent) noexcept {
        std::scoped_lock admission_lock(mutex_);
        if (!extent.valid() || extent.width > settings_.maximum_width / kUpscaleOutputScale ||
            extent.height > settings_.maximum_height / kUpscaleOutputScale) return;
        if (warm_extent_ == extent && (warm_admitted_ || warm_attempted_)) return;
        warm_extent_ = extent;
        warm_next_ = 0U;
        warm_admitted_ = true;
        diagnostics_.Emit([&] { return VisualDiagnosticFact{
            .system = contracts::DiagnosticOwner::Upscale,
            .operation = VisualDiagnosticOperation::UpscaleWarmAdmissionStarted,
            .device = settings_.device,
        }; });
        if (!worker_.NotifyContinuation()) warm_admitted_ = false;
    }
    detail::VisualRuntimeOwner::Notification WarmNext(mmltk::frameworks::gpu::SystemImageRuntime& runtime, std::stop_token stop) {
                    VisualExtent extent;
                    std::size_t index;
                    {
                        std::scoped_lock lock(mutex_);
                        if (!warm_admitted_ || warm_next_ == state_.methods.size() || stop.stop_requested()) return {};
                        extent = warm_extent_;
                        index = warm_next_++;
                    }
                    std::string failure;
                    bool graph_replay = false;
                    const auto current = [this, extent, stop] {
                        std::scoped_lock lock(mutex_);
                        return !stop.stop_requested() && warm_admitted_ && warm_extent_ == extent && !state_.busy;
                    };
                    try {
                        if (current()) {
                        auto candidate = AcquireOutput(runtime, stop);
                        if (candidate.valid() && current()) {
                        auto* const model = dynamic_cast<UpscaleAlgorithm*>(runtime.model());
                        if (model == nullptr) throw std::runtime_error("Upscale runtime model is unavailable during warm-up");
                        services::RuntimeDiagnosticSpan warm_span{diagnostics_, [&] {
                            return visual_diagnostic_boundary(
                                {.system = contracts::DiagnosticOwner::Upscale,
                                 .operation = VisualDiagnosticOperation::UpscaleWarmRuntimeStarted,
                                 .device = settings_.device},
                                VisualDiagnosticOperation::UpscaleWarmRuntimeCompleted);
                        }};
                        if (!warm_initialized_) {
                            warm_initialized_ = true;
                            model->Warm();
                        }
                        bool prepare_input = false;
                        {
                            const auto retained = runtime.BorrowInput();
                            prepare_input = !retained.valid() ||
                                retained.plane(0U).plane().descriptor.width != extent.width ||
                                retained.plane(0U).plane().descriptor.height != extent.height;
                        }
                        if (prepare_input) {
                            input_request_.reset();
                            input_document_.reset();
                            runtime.PublishInput(extent.width, extent.height, [model](auto clean, auto semantic, auto stream) {
                                model->Semantics({}, clean, stream);
                                if (semantic.valid()) model->Semantics({}, semantic, stream);
                            });
                        }
                        const auto input = runtime.BorrowInput();
                        const auto dimensions = input.plane(0U).plane().descriptor;
                        const auto target = checked_upscale_output_extent({dimensions.width, dimensions.height});
                        runtime.Publish(candidate, target.width, target.height, [model, &input, index, &current](auto clean, auto semantic, auto stream) {
                            const auto kernel = static_cast<UpscaleKernel>(index);
                            for (unsigned replay = 0U; replay < 3U && current(); ++replay)
                                model->Run(kernel, input.plane(0U).plane(), clean, stream, current);
                            if (semantic.valid() && current()) model->Semantics({}, semantic, stream);
                        });
                        warm_span.Finish();
                        graph_replay = model->GraphReplay(static_cast<UpscaleKernel>(index));
                        }
                        }
                    } catch (...) {
                        rethrow_physical_upscale_failure(std::current_exception());
                        failure = visual_failure_detail(std::current_exception(), "Upscale model warm-up failed");
                    }
                    {
                        std::scoped_lock lock(mutex_);
                        if (stop.stop_requested() || !warm_admitted_ || extent != warm_extent_ || state_.busy) {
                            // Initialization belongs to the method, not to the
                            // source that happened to preempt its warm attempt.
                            if (!failure.empty()) {
                                state_.methods[index].initialization_failed = true;
                                state_.methods[index].warm = false;
                                AdvanceRevision();
                            }
                            if (!stop.stop_requested() && warm_admitted_) {
                                if (extent == warm_extent_ && state_.busy) warm_next_ = index;
                                static_cast<void>(worker_.NotifyContinuation());
                            }
                            if (!failure.empty()) return [this] { PublishChanged(); };
                            return {};
                        }
                        state_.methods[index].warm = failure.empty();
                        state_.methods[index].initialization_failed = !failure.empty();
                        state_.methods[index].graph_replay = graph_replay;
                        warm_admitted_ = warm_next_ < state_.methods.size();
                        warm_attempted_ = !warm_admitted_;
                        AdvanceRevision();
                        if (warm_admitted_) static_cast<void>(worker_.NotifyContinuation());
                    }
                    if (failure.empty()) return [this] { PublishChanged(); };
                    return [this, failure = std::move(failure)]() mutable noexcept {
                        report_visual_worker_failure(diagnostics_, contracts::DiagnosticOwner::Upscale, settings_.device, failure);
                        Publish(event_type{UpscaleFailed{snapshot(), std::move(failure)}});
                    };
    }
    UpscaleSnapshot Start(const UpscaleRequest request) {
        if (request.kernel > UpscaleKernel::RealPlksr) throw contracts::InvalidIntentError("Upscale kernel is invalid");
        if (!request.document.valid()) throw contracts::InvalidIntentError("Upscale document facts are invalid");
        if (!request.source.valid()) throw contracts::InvalidIntentError("Upscale source frame is invalid");
        const auto target = checked_upscale_output_extent(request.source.extent);
        if (target.width > settings_.maximum_width || target.height > settings_.maximum_height)
            throw contracts::InvalidIntentError("Upscale four-times extent exceeds device bounds");
        {
            std::unique_lock admission_lock(mutex_);
            if (state_.pending == request) {
                desired_ = request;
                return state_;
            }
            auto& cached = records_[static_cast<std::size_t>(request.kernel)];
            if (cached.product.valid() && cached.request == request && cached.frame.extent == target) {
                if (state_.pending && !SameSource(*state_.pending, request)) {
                    ++demand_;
                    state_.pending.reset();
                    state_.busy = false;
                }
                Select(cached);
                desired_ = request;
                RefreshAvailability(request);
                AdvanceRevision();
                const auto selected = state_;
                diagnostics_.Emit([&] { return VisualDiagnosticFact{
                    .system = contracts::DiagnosticOwner::Upscale,
                    .operation = VisualDiagnosticOperation::UpscaleResultReused,
                    .device = settings_.device,
                    .generation = demand_,
                    .value = selected.frame.revision,
                    .detail = static_cast<std::uint64_t>(request.kernel),
                    .context = {.observation_revision = selected.revision,
                                .document_resource = request.document.resource.view(),
                                .document_revision = request.document.resource.revision,
                                .document_meaning_identity = request.document.meaning_identity,
                                .source = visual_diagnostic_source({.frame = request.source}),
                                .demand = {.demand_generation = demand_}}}; });
                admission_lock.unlock();
                Publish(event_type{UpscaleChanged{selected}});
                return selected;
            }
            const auto prior = state_;
            const auto prior_desired = desired_;
            desired_ = request;
            const auto demand = ++demand_;
            contracts::DiagnosticLink admission_link;
            diagnostics_.Emit([&] {
                admission_link = services::DiagnosticSpanIds::Next();
                return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Upscale,
                          .operation = VisualDiagnosticOperation::UpscaleRequestAdmitted,
                          .device = settings_.device,
                          .generation = demand,
                          .value = request.source.revision,
                          .detail = static_cast<std::uint64_t>(request.kernel),
                          .context = {.source = visual_diagnostic_source({.frame = request.source}),
                                      .demand = {.demand_generation = demand}, .link = admission_link}}; });
            state_.pending = request;
            state_.busy = true;
            state_.methods[static_cast<std::size_t>(request.kernel)].failed = false;
            state_.methods[static_cast<std::size_t>(request.kernel)].failure.reset();
            RefreshAvailability(request);
            AdvanceRevision();
            if (!worker_.SubmitLatest([this, request, target, demand, admission_link](
                                          mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                                          std::stop_token stop) mutable -> detail::VisualRuntimeOwner::Notification {
                    const auto current = [this, demand, stop] {
                        std::scoped_lock lock(mutex_);
                        return demand == demand_ && !stop.stop_requested();
                    };
                    if (!current()) return {};
                    contracts::DiagnosticLink worker_link;
                    diagnostics_.Emit([&] {
                        worker_link = services::DiagnosticSpanIds::Next(admission_link);
                        return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Upscale,
                                  .operation = VisualDiagnosticOperation::UpscaleWorkerStarted,
                                  .device = settings_.device,
                                  .generation = demand,
                                  .value = request.source.revision,
                                  .detail = static_cast<std::uint64_t>(request.kernel),
                                  .context = {.source = visual_diagnostic_source({.frame = request.source}),
                                              .demand = {.demand_generation = demand}, .link = worker_link}}; });
                    try {
                    VisualDocumentRead source;
                    const bool retained_input = input_request_ && SameSource(*input_request_, request);
                    if (!current()) return {};
                    if (!retained_input) source = borrow_source_(request.source);
                    if (!current()) return {};
                    if (!retained_input && (!source.valid() || source.document->facts() != request.document)) {
                        std::scoped_lock lock(mutex_);
                        if (demand != demand_) return {};
                        state_.busy = false;
                        state_.pending.reset();
                        state_.methods[static_cast<std::size_t>(request.kernel)].failed = true;
                        state_.methods[static_cast<std::size_t>(request.kernel)].failure = request;
                        AdvanceRevision();
                        return [this, request] { Publish(event_type{UpscaleFailed{snapshot(), "Upscale source is no longer available", request,
                                                                               UpscaleFailureKind::Unavailable}}); };
                    }
                    const auto source_descriptor = retained_input ?
                        runtime.BorrowInput().plane(0U).plane().descriptor : source.pixels.plane(0U).plane().descriptor;
                    if (source_descriptor.width != request.source.extent.width || source_descriptor.height != request.source.extent.height)
                        throw contracts::UnavailableError("Upscale source geometry does not match its frame");
                    auto document = retained_input ? input_document_ :
                        scale_visual_document(source.document, kUpscaleOutputScale);
                    auto* const model = dynamic_cast<UpscaleAlgorithm*>(runtime.model());
                    if (model == nullptr) throw std::runtime_error("Upscale runtime model is unavailable");
                    const auto copied_planes = diagnostics_.valid() ? source.pixels.plane_count() : 0U;
                    if (!retained_input) {
                    services::RuntimeDiagnosticSpan copy_span{diagnostics_, [&] {
                        return visual_diagnostic_boundary(
                            {.system = contracts::DiagnosticOwner::Upscale,
                             .operation = VisualDiagnosticOperation::UpscaleCopyStarted,
                             .device = settings_.device,
                             .generation = demand,
                             .value = request.source.revision,
                             .context = {.source = visual_diagnostic_source({.frame = request.source}),
                                         .demand = {.demand_generation = demand}}},
                            VisualDiagnosticOperation::CopyCompleted);
                    }, worker_link};
                        const bool preserve_clean = input_request_ &&
                            visual_clean_content_identity(input_request_->source) == visual_clean_content_identity(request.source);
                        if (!current()) return {};
                        input_request_.reset();
                        input_document_.reset();
                        if (!current()) return {};
                        const auto paths = runtime.CopyInputFrom(
                            std::move(source.pixels), [model](const auto plane, const auto stream) { model->Semantics({}, plane, stream); },
                            preserve_clean);
                        input_request_ = request;
                        input_document_ = document;
                        copy_span.FinishWith([&](auto& fact) { fact.copy_path = paths[preserve_clean ? 1U : 0U]; });
                    }
                    if (stop.stop_requested()) return {};
                    const auto input = runtime.BorrowInput();
                    if (!input.valid()) throw std::runtime_error("Upscale runtime model is unavailable");
                    if (diagnostics_.valid()) {
                        const auto plane = input.plane(0U).plane();
                        diagnostics_.Emit([&] { return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Upscale,
                                      .operation = VisualDiagnosticOperation::UpscaleInputGeometry,
                                      .device = settings_.device,
                                      .generation = demand,
                                      .value = source_descriptor.pitch_bytes,
                                      .detail = plane.descriptor.pitch_bytes,
                                      .context = {.capacity_width = plane.descriptor.width,
                                                  .capacity_height = plane.descriptor.height,
                                                  .frame_revision = request.source.revision}}; });
                        diagnostics_.Emit([&] { return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Upscale,
                                      .operation = VisualDiagnosticOperation::UpscaleInputAllocation,
                                      .device = settings_.device,
                                      .generation = demand,
                                      .value = plane.data,
                                      .detail = plane.descriptor.row_bytes() * plane.descriptor.height * copied_planes}; });
                    }
                    Record baseline;
                    {
                        std::scoped_lock lock(mutex_);
                        if (demand != demand_) return {};
                        baseline = records_[static_cast<std::size_t>(request.kernel)];
                        for (auto& record : records_) {
                            if (visual_clean_content_identity(record.request.source) != visual_clean_content_identity(request.source))
                                record = {};
                        }
                    }
                    const bool reuse_clean = baseline.product.valid() &&
                        visual_clean_content_identity(baseline.request.source) == visual_clean_content_identity(request.source);
                    auto output_candidate = AcquireOutput(runtime, stop, reuse_clean ? baseline.product :
                        mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput{},
                        mmltk::frameworks::gpu::ImagePlanePreservation::Clean);
                    if (!output_candidate.valid() || !current()) return {};
                    runtime.Publish(
                        output_candidate, target.width, target.height,
                        [this, model, &input, request, reuse_clean, demand, stop](const auto output, const auto semantic, const auto stream) {
                            if (diagnostics_.valid())
                                diagnostics_.Emit([&] { return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Upscale,
                                     .operation = VisualDiagnosticOperation::UpscaleOutputAllocation,
                                     .device = settings_.device,
                                     .generation = demand,
                                     .value = output.data,
                                     .detail = output.descriptor.pitch_bytes,
                                     .context = {.capacity_width = output.descriptor.width, .capacity_height = output.descriptor.height}}; });
                            const auto current = [this, demand, stop] {
                                std::scoped_lock lock(mutex_);
                                return demand == demand_ && !stop.stop_requested();
                            };
                            if (!current()) return;
                            if (!reuse_clean) model->Run(request.kernel, input.plane(0U).plane(), output, stream, current);
                            if (!current()) return;
                            model->Semantics(input.plane(1U).plane(), semantic, stream);
                        });
                    {
                        std::scoped_lock lock(mutex_);
                        if (demand != demand_ || stop.stop_requested()) return {};
                        auto product = runtime.CommitOutput(std::move(output_candidate));
                        const auto clean_revision = reuse_clean ? baseline.frame.clean_revision : product.revision();
                        state_.busy = false;
                        state_.pending.reset();
                        auto& record = records_[static_cast<std::size_t>(request.kernel)];
                        record.product = std::move(product);
                        record.request = request;
                        record.document = std::move(document);
                        record.frame = {
                            .source =
                                {
                                    .kind = PresentationSourceKind::Upscale,
                                    .instance = 1U,
                                },
                            .extent = target,
                            .revision = record.product.revision(),
                            .content = {request.source.content.x * kUpscaleOutputScale, request.source.content.y * kUpscaleOutputScale,
                                        request.source.content.width * kUpscaleOutputScale,
                                        request.source.content.height * kUpscaleOutputScale},
                            .clean_revision = clean_revision,
                        };
                        state_.methods[static_cast<std::size_t>(request.kernel)] = {
                            .available = true, .warm = true, .graph_replay = model->GraphReplay(request.kernel),
                            .completed = request, .frame = record.frame,
                        };
                        if (desired_ == request) Select(record);
                        if (desired_) RefreshAvailability(*desired_);
                        AdvanceRevision();
                    }
                    diagnostics_.Emit([&] { return VisualDiagnosticFact{
                        .system = contracts::DiagnosticOwner::Upscale,
                        .operation = VisualDiagnosticOperation::UpscaleModelSubmitted,
                        .device = settings_.device,
                        .generation = runtime.OutputFacts().revision,
                        .value = static_cast<std::uint64_t>(request.kernel),
                        .detail = demand,
                    }; });
                    return [this] { PublishChanged(); };
                    } catch (...) {
                        const auto failure = std::current_exception();
                        rethrow_physical_upscale_failure(failure);
                        const auto detail = visual_failure_detail(failure, "Upscale method failed");
                        const auto kind = [failure] {
                            try { std::rethrow_exception(failure); }
                            catch (const contracts::UnavailableError&) { return UpscaleFailureKind::Unavailable; }
                            catch (...) { return UpscaleFailureKind::Failed; }
                        }();
                        {
                            std::scoped_lock lock(mutex_);
                            auto& method = state_.methods[static_cast<std::size_t>(request.kernel)];
                            const bool initialization = static_cast<bool>(
                                mmltk::frameworks::gpu::find_image_failure<native_upscale::ImageUpscalerInitializationFailure>(failure));
                            if (initialization) {
                                method.initialization_failed = true;
                                method.warm = false;
                            }
                            if (demand != demand_) {
                                if (initialization) {
                                    AdvanceRevision();
                                    return [this] { PublishChanged(); };
                                }
                                return {};
                            }
                            method.failed = true;
                            method.failure = request;
                            method.warm = false;
                            state_.busy = false;
                            state_.pending.reset();
                            AdvanceRevision();
                        }
                        return [this, detail, request, kind] {
                            report_visual_worker_failure(diagnostics_, contracts::DiagnosticOwner::Upscale, settings_.device, detail);
                            Publish(event_type{UpscaleFailed{snapshot(), detail, request, kind}});
                        };
                    }
                })) {
                state_ = prior;
                desired_ = prior_desired;
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
            diagnostics_.Emit([&] { return VisualDiagnosticFact{
                .system = contracts::DiagnosticOwner::Upscale,
                .operation = VisualDiagnosticOperation::UpscaleStopRequested,
                .device = settings_.device,
                .generation = demand_,
                .context = {.observation_revision = state_.revision,
                            .source = visual_diagnostic_source({.frame = state_.input})}}; });
            ++demand_;
            state_.busy = false;
            state_.ready = false;
            state_.pending.reset();
            desired_.reset();
            warm_admitted_ = false;
            AdvanceRevision();
        }
        worker_.RequestActiveStop();
        PublishChanged();
    }
    void Shutdown() noexcept {
        worker_.StopAndWait();
        std::scoped_lock lock(mutex_);
        records_ = {};
        selected_ = {};
        input_request_.reset();
        input_document_.reset();
        document_.reset();
        state_.ready = false;
        state_.busy = false;
        state_.pending.reset();
        desired_.reset();
        warm_admitted_ = false;
    }
    bool stopped() const noexcept { return worker_.stopped(); }
    void Publish(event_type event) noexcept { publish_visual_event_noexcept(events_, std::move(event)); }
    void PublishChanged() noexcept { Publish(event_type{UpscaleChanged{snapshot()}}); }
    void AdvanceRevision() { state_.revision = mmltk::common::types::advance_monotonic_identity(state_.revision); }

   private:
    friend class UpscaleSystem;

    mmltk::frameworks::gpu::SystemImageRuntime::OutputCandidate AcquireOutput(
        mmltk::frameworks::gpu::SystemImageRuntime& runtime, std::stop_token stop,
        mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput baseline = {},
        mmltk::frameworks::gpu::ImagePlanePreservation preservation = mmltk::frameworks::gpu::ImagePlanePreservation::All) {
        services::RuntimeDiagnosticSpan admission{diagnostics_, [&] {
            return visual_diagnostic_boundary(
                {.system = contracts::DiagnosticOwner::Upscale,
                 .operation = VisualDiagnosticOperation::UpscaleOutputAdmissionStarted, .device = settings_.device},
                VisualDiagnosticOperation::UpscaleOutputAdmissionCompleted);
        }};
        auto candidate = runtime.AcquireOutput(stop, std::move(baseline), preservation);
        admission.Finish();
        return candidate;
    }

    // CLEANUP-IGNORE: The shared checked-borrow helper owns product validation; this sealed receiver still reads its
    // own typed readiness snapshot under its own mutex.
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const {
        mmltk::frameworks::gpu::ImageProductPool::Product committed;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.ready) return {};
            committed = selected_;
        }
        return committed.Borrow();
    }

    struct Record final {
        UpscaleRequest request{};
        mmltk::frameworks::gpu::ImageProductPool::Product product{};
        VisualFrame frame{};
        std::shared_ptr<const VisualDocument> document{};
    };
    static bool SameSource(const UpscaleRequest& left, const UpscaleRequest& right) {
        return left.source == right.source && left.document == right.document;
    }
    void RefreshAvailability(const UpscaleRequest& request) {
        for (std::size_t index = 0U; index < records_.size(); ++index) {
            const auto& record = records_[index];
            auto& method = state_.methods[index];
            method.available = record.product.valid() && SameSource(record.request, request) &&
                record.request.kernel == static_cast<UpscaleKernel>(index) &&
                record.frame.extent == checked_upscale_output_extent(request.source.extent);
            method.failed = method.failure && SameSource(*method.failure, request);
        }
    }
    void Select(const Record& record) {
        auto scene = record.document->scene;
        selected_ = record.product;
        state_.ready = true;
        state_.kernel = record.request.kernel;
        state_.input = record.request.source;
        state_.frame = record.frame;
        document_ = record.document;
        state_.scene = std::move(scene);
    }

    VisualDeviceSettings settings_;
    ExactVisualDocumentBorrower borrow_source_;
    SystemEventSink<event_type> events_;
    VisualDiagnosticSink diagnostics_{};
    mutable std::mutex mutex_;
    UpscaleSnapshot state_;
    std::uint64_t demand_ = 0U;
    std::optional<UpscaleRequest> desired_;
    std::array<Record, 3U> records_{};
    mmltk::frameworks::gpu::ImageProductPool::Product selected_;
    std::optional<UpscaleRequest> input_request_;
    // Shared four-times document projection for this exact receiver-owned input.
    std::shared_ptr<const VisualDocument> input_document_;
    std::shared_ptr<const VisualDocument> document_;
    bool warm_admitted_ = false;
    bool warm_attempted_ = false;
    bool warm_initialized_ = false;
    VisualExtent warm_extent_{};
    std::size_t warm_next_ = 0U;
    detail::VisualRuntimeOwner worker_;
};

// CLEANUP-IGNORE: The public Upscale facade forwards construction to its one private implementation owner.
UpscaleSystem::UpscaleSystem(const VisualDeviceSettings settings, VisualRuntimeFactory factory, ExactVisualDocumentBorrower borrow_source,
                             SystemEventSink<event_type> events,
                             // CLEANUP-IGNORE: Upscale construction forwards its own diagnostic sink at the pimpl boundary.
                             VisualDiagnosticSink diagnostics)
    : impl_(std::make_unique<Impl>(settings, std::move(factory), std::move(borrow_source), std::move(events), diagnostics)) {}
UpscaleSystem::~UpscaleSystem() = default;
void UpscaleSystem::Warm(const VisualExtent extent) noexcept { impl_->Warm(extent); }
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
    mmltk::frameworks::gpu::ImageProductPool::Product product;
    {
        std::scoped_lock lock(impl_->mutex_);
        if (!impl_->state_.ready || impl_->state_.frame != frame) return {};
        document = impl_->document_;
        product = impl_->selected_;
    }
    return {product.Borrow(), std::move(document)};
}

VisualRuntimeFactory make_native_upscale_runtime_factory(
    const VisualDeviceSettings settings, native_upscale::ImageUpscalerExecutionCheckpoint checkpoint) {
    if (!settings.valid()) throw contracts::InvalidIntentError("Upscale device settings are invalid");
    return [settings, execution = resolve_visual_device_execution(settings), checkpoint = std::move(checkpoint)](auto revisions) {
        return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
            .device = settings.device,
            .model = std::make_unique<NativeUpscaleModel>(settings.device, checkpoint),
            .context_mode = mmltk::frameworks::gpu::DeviceContextMode::PrimaryInterop,
            .input_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
            .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
            .output_buffer_count = 4U,
            .numa_node = settings.numa_node,
            .execution = execution,
            .product_revisions = std::move(revisions),
        });
    };
}

}  // namespace mmltk::controller
