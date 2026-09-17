#include "prediction_test_support.h"
#include "src/backend/models/rfdetr/contract/prediction_limits.h"
#include "src/frameworks/gpu/image_failure.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>
namespace mmltk::controller::test_support {
namespace gpu = mmltk::frameworks::gpu;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace {
std::atomic<PredictionReceiverFault*> receiver_fault = nullptr;
thread_local PredictionTransferFault* transfer_fault = nullptr;
void checked(cudaError_t result) {
    if (result != cudaSuccess) throw std::runtime_error(cudaGetErrorString(result));
}
std::size_t pixel_values(VisualExtent extent) {
    if (!extent.valid()) throw std::invalid_argument("test source extent is empty");
    return rfdetr::checked_prediction_extent(rfdetr::checked_prediction_extent(extent.width, extent.height, rfdetr::kMaximumEncodedMaskPixels), 3U,
                                             rfdetr::kMaximumPredictionTensorBytes / sizeof(float));
}
struct DeviceAllocation final {
    explicit DeviceAllocation(const gpu::DeviceExecution& execution)
        : context(execution.device, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::PrimaryInterop, execution.placement.numa_node, execution) {}
    gpu::DeviceContext context;
    void* address = nullptr;
    static void Release(DeviceAllocation* allocation) noexcept {
        // Never free in an unrelated context. Real context/free failure retains
        // the complete aggregate; injected preview failures do not bypass this.
        CUcontext previous{};
        if (cuCtxGetCurrent(&previous) != CUDA_SUCCESS) return;
        try {
            allocation->context.Bind();
        } catch (...) { return; }
        const auto result = allocation->address ? cudaFree(allocation->address) : cudaSuccess;
        if (result == cudaSuccess) allocation->address = nullptr;
        const auto restored = cuCtxSetCurrent(previous);
        if (result == cudaSuccess && restored == CUDA_SUCCESS) delete allocation;
    }
};
}  // namespace
PredictionSource::PredictionSource(VisualExtent extent, Catalog classes)
    : extent_(extent), classes_(classes ? std::move(classes) : std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>()) {
    annotations_.source_region = {.width = extent.width, .height = extent.height};
    annotations_.class_catalog = classes_;
    annotations_.class_domain = classes_->empty() ? mmltk::backend::data::catalog::ClassReferenceDomain::RawOutputSlot
                                                  : mmltk::backend::data::catalog::ClassReferenceDomain::Foreground;
}
PredictionSource PredictionSource::Device(const gpu::DeviceExecution& execution, VisualExtent extent, std::span<const float> pixels,
                                          std::vector<Detection> detections, Catalog classes, std::span<const std::uint8_t> masks) {
    const auto values = pixel_values(extent);
    if (pixels.size() != values || detections.size() > contracts::kAnnotationObjectCapacity)
        throw std::invalid_argument("test source data does not match its geometry");
    const auto pixel_bytes = values * sizeof(float);
    const auto boxes_bytes = detections.size() * 4U * sizeof(float);
    const auto label_bytes = detections.size() * sizeof(std::int32_t);
    if (!masks.empty() && masks.size() != static_cast<std::size_t>(extent.width) * extent.height * detections.size())
        throw std::invalid_argument("test mask data does not match its geometry");
    const auto bytes = pixel_bytes + boxes_bytes + label_bytes + masks.size();
    if (bytes > rfdetr::kMaximumPredictionTensorBytes) throw std::invalid_argument("test source is too large");
    std::vector<std::byte> packed(bytes);
    std::memcpy(packed.data(), pixels.data(), pixel_bytes);
    for (std::size_t index = 0; index < detections.size(); ++index) {
        std::memcpy(packed.data() + pixel_bytes + index * 4U * sizeof(float), detections[index].bbox_xyxy.data(), 4U * sizeof(float));
        const std::int32_t category = detections[index].class_reference;
        std::memcpy(packed.data() + pixel_bytes + boxes_bytes + index * sizeof(category), &category, sizeof(category));
    }
    if (!masks.empty()) std::memcpy(packed.data() + pixel_bytes + boxes_bytes + label_bytes, masks.data(), masks.size());
    checked(cudaSetDevice(execution.device));
    auto allocation = std::shared_ptr<DeviceAllocation>(new DeviceAllocation(execution), &DeviceAllocation::Release);
    allocation->context.Bind();
    checked(cudaMalloc(&allocation->address, bytes));
    checked(cudaMemcpy(allocation->address, packed.data(), bytes, cudaMemcpyHostToDevice));
    PredictionSource result(extent, std::move(classes));
    result.pixels_ = static_cast<float*>(allocation->address);
    result.custody_ = std::shared_ptr<void>(allocation, allocation->address);
    result.detections_ = std::move(detections);
    const auto address = reinterpret_cast<std::uintptr_t>(allocation->address);
    result.annotations_.value_capacity = result.annotations_.value_count = result.detections_.size();
    result.annotations_.boxes_xyxy = {.address = address + pixel_bytes, .capacity_bytes = boxes_bytes};
    result.annotations_.class_references = {.address = address + pixel_bytes + boxes_bytes, .capacity_bytes = label_bytes};
    result.annotations_.masks_available = !masks.empty();
    if (!masks.empty()) result.annotations_.masks = {.address = address + pixel_bytes + boxes_bytes + label_bytes, .capacity_bytes = masks.size()};
    return result;
}
PredictionSource PredictionSource::Decoded(VisualExtent extent, std::span<const std::uint8_t> pixels, Catalog classes) {
    if (pixels.size() != pixel_values(extent)) throw std::invalid_argument("test decoded source does not match its geometry");
    auto allocation = std::make_shared_for_overwrite<std::uint8_t[]>(pixels.size());
    std::memcpy(allocation.get(), pixels.data(), pixels.size());
    PredictionSource result(extent, std::move(classes));
    result.rgb8_ = allocation.get();
    result.custody_ = std::shared_ptr<void>(allocation, allocation.get());
    return result;
}
ScopedPredictionReceiverFault::ScopedPredictionReceiverFault(PredictionReceiverFault& fault) : previous_(receiver_fault.exchange(&fault)) {}
ScopedPredictionReceiverFault::~ScopedPredictionReceiverFault() { receiver_fault.store(previous_); }
cudaError_t PredictionReceiverFault::Upload(void* destination, const void* source, std::size_t bytes, cudaMemcpyKind kind, cudaStream_t stream) {
    const auto copied = cudaMemcpyAsync(destination, source, bytes, kind, stream);
    if (copied != cudaSuccess) return copied;
    auto* fault = receiver_fault.load();
    if (!fault) return cudaSuccess;
    const auto ordinal = ++fault->uploads;
    if (!fault->enabled || (fault->fail_upload_at != 0U && fault->fail_upload_at != ordinal)) return cudaSuccess;
    // Actual upload/pinned allocation precede injection; physical test work is
    // settled before the deliberately unobservable receiver outcome is reported.
    const auto settled = cudaStreamSynchronize(stream);
    fault->upload.receipt().ArriveAndWait();
    if (fault->terminal) throw gpu::ImageStreamExecutionFailure(std::make_exception_ptr(std::runtime_error("injected receiver completion failure")));
    return settled == cudaSuccess ? cudaErrorMemoryAllocation : settled;
}
int PredictionReceiverFault::Convert(const float* source, std::uint32_t width, std::uint32_t height, std::uint8_t* destination, std::size_t pitch,
                                     cudaStream_t stream) noexcept {
    auto* fault = receiver_fault.load();
    bool fail = false;
    if (fault) {
        ++fault->draws;
        fail = fault->partial_draw;
        auto remaining = fault->draw_failures_remaining.load();
        while (remaining != 0U && !fault->draw_failures_remaining.compare_exchange_weak(remaining, remaining - 1U)) {}
        fail = fail || remaining != 0U;
    }
    if (fail) {
        const auto written = cudaMemset2DAsync(destination, pitch, 123, width * 4U, 1U, stream);
        if (written != cudaSuccess) return written;
        const auto settled = cudaStreamSynchronize(stream);
        return settled == cudaSuccess ? cudaErrorMemoryAllocation : settled;
    }
    return mmltk::backend::imaging::raster::chw_float_to_rgba(source, width, height, destination, pitch, stream);
}
detail::PredictionPreviewPool::TransferOperations PredictionReceiverFault::Operations() {
    return {&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister, &Upload, {}, &Convert};
}
namespace {
class SettlementBackend final : public gpu::ImageCopyBackend {
   public:
    explicit SettlementBackend(PredictionSettlementFault& fault) : fault_(fault) {}
    std::optional<gpu::DeviceExecution> ResolveExecution(int device, int numa) override { return native_->ResolveExecution(device, numa); }
    std::uintptr_t CreateContext(int device, gpu::DeviceContextMode mode) override { return native_->CreateContext(device, mode); }
    void DestroyContext(int device, gpu::DeviceContextMode mode, std::uintptr_t context) noexcept override {
        return native_->DestroyContext(device, mode, context);
    }
    void BindContext(std::uintptr_t context) override { return native_->BindContext(context); }
    std::uintptr_t CreateStream(std::uintptr_t context) override {
        const auto stream = native_->CreateStream(context);
        ++fault_.streams_created;
        return stream;
    }
    void DestroyStream(std::uintptr_t context, std::uintptr_t stream) noexcept override {
        ++fault_.streams_destroyed;
        return native_->DestroyStream(context, stream);
    }
    std::uintptr_t CreateEvent(std::uintptr_t context) override { return native_->CreateEvent(context); }
    void DestroyEvent(std::uintptr_t context, std::uintptr_t event) noexcept override { return native_->DestroyEvent(context, event); }
    gpu::ImagePlaneView AllocatePlane(std::uintptr_t context, gpu::ImagePlaneKind kind, std::uint32_t width, std::uint32_t height) override {
        return native_->AllocatePlane(context, kind, width, height);
    }
    void FreePlane(std::uintptr_t context, CUdeviceptr data) noexcept override { return native_->FreePlane(context, data); }
    void ClearPlane(std::uintptr_t context, std::uintptr_t stream, const gpu::ImagePlaneView& plane) override {
        return native_->ClearPlane(context, stream, plane);
    }
    std::shared_ptr<void> AllocatePinned(std::uintptr_t context, const mmltk::common::system::ExecutionPlacement* placement, std::size_t bytes) override {
        return native_->AllocatePinned(context, placement, bytes);
    }
    bool CanAccessPeer(int receiver, int source) override { return native_->CanAccessPeer(receiver, source); }
    void WaitEvent(std::uintptr_t context, std::uintptr_t stream, std::uintptr_t event) override { return native_->WaitEvent(context, stream, event); }
    void CopySameDevice(std::uintptr_t context, std::uintptr_t stream, const gpu::ImagePlaneView& destination, std::uintptr_t source_context,
                        const gpu::ImagePlaneView& source) override {
        return native_->CopySameDevice(context, stream, destination, source_context, source);
    }
    void CopyPeer(std::uintptr_t context, std::uintptr_t stream, int device, const gpu::ImagePlaneView& destination, std::uintptr_t source_context,
                  int source_device, const gpu::ImagePlaneView& source) override {
        return native_->CopyPeer(context, stream, device, destination, source_context, source_device, source);
    }
    void CopyDeviceToHost(std::uintptr_t context, const gpu::ImagePlaneView& source, void* destination, std::size_t pitch) override {
        return native_->CopyDeviceToHost(context, source, destination, pitch);
    }
    void CopyHostToDevice(std::uintptr_t context, std::uintptr_t stream, const void* source, std::size_t pitch,
                          const gpu::ImagePlaneView& destination) override {
        return native_->CopyHostToDevice(context, stream, source, pitch, destination);
    }
    void SynchronizeEvent(std::uintptr_t context, std::uintptr_t event) override { return native_->SynchronizeEvent(context, event); }
    void NotifyStream(std::uintptr_t context, std::uintptr_t stream, StreamNotification& notification) override {
        return native_->NotifyStream(context, stream, notification);
    }
    void RecordEvent(std::uintptr_t context, std::uintptr_t stream, std::uintptr_t event) override {
        native_->RecordEvent(context, stream, event);
        if (fault_.enabled) fault_.callback_returned = true;
    }
    StreamSettlement SettleStream(std::uintptr_t context, std::uintptr_t stream) noexcept override {
        auto result = native_->SettleStream(context, stream);
        if (fault_.enabled) {
            fault_.settlement.receipt().ArriveAndWait();
            return {.completion_reached = false, .failure = std::make_exception_ptr(std::runtime_error("injected outer settlement"))};
        }
        return result;
    }

   private:
    PredictionSettlementFault& fault_;
    std::shared_ptr<gpu::ImageCopyBackend> native_ = gpu::cuda_image_copy_backend();
};
}  // namespace
std::shared_ptr<gpu::ImageCopyBackend> PredictionSettlementFault::Backend() { return std::make_shared<SettlementBackend>(*this); }
PredictionTransferFault::PredictionTransferFault() : previous_(std::exchange(transfer_fault, this)) {}
PredictionTransferFault::~PredictionTransferFault() { transfer_fault = previous_; }
void PredictionTransferFault::Reset() { Reset(Selection{}); }
void PredictionTransferFault::Reset(Selection selection) {
    selection_ = selection;
    copies = settlements = waits = 0;
    waited_stream = nullptr;
}
CUresult PredictionTransferFault::Copy(CUdeviceptr destination, CUcontext destination_context, CUdeviceptr source, CUcontext source_context, std::size_t bytes,
                                       CUstream stream) {
    if (transfer_fault && ++transfer_fault->copies == transfer_fault->selection_.fail_copy) return CUDA_ERROR_INVALID_VALUE;
    return cuMemcpyPeerAsync(destination, destination_context, source, source_context, bytes, stream);
}
cudaError_t PredictionTransferFault::Record(cudaEvent_t event, cudaStream_t stream) {
    if (transfer_fault && transfer_fault->selection_.fail_record) return cudaErrorInvalidResourceHandle;
    return cudaEventRecord(event, stream);
}
cudaError_t PredictionTransferFault::Settle(cudaStream_t stream) {
    if (transfer_fault) {
        ++transfer_fault->settlements;
        if (transfer_fault->selection_.fail_settle) return cudaErrorUnknown;
    }
    return cudaStreamSynchronize(stream);
}
cudaError_t PredictionTransferFault::Wait(cudaStream_t stream, cudaEvent_t event, unsigned flags) {
    if (transfer_fault) {
        ++transfer_fault->waits;
        transfer_fault->waited_stream = stream;
        if (transfer_fault->selection_.fail_wait) return cudaErrorInvalidResourceHandle;
    }
    return cudaStreamWaitEvent(stream, event, flags);
}
detail::PredictionPreviewPool::TransferOperations PredictionTransferFault::Operations() {
    auto operations = detail::PredictionPreviewPool::TransferOperations{&Copy, &Record, &Settle, &cuMemHostRegister};
    operations.wait = &Wait;
    return operations;
}
detail::PredictionPreviewPool::TransferOperations RefusePinnedRegistration() {
    return {&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, +[](void*, std::size_t, unsigned) -> CUresult { return CUDA_ERROR_OUT_OF_MEMORY; }};
}
void CountPredictionSourceStop(void* count) noexcept { ++*static_cast<int*>(count); }
gpu::CudaContextApi PredictionContextFault::Api() noexcept { return {this, &Get, &Set}; }
CUresult PredictionContextFault::Get(void* owner, CUcontext* context) noexcept {
    auto& driver = *static_cast<PredictionContextFault*>(owner);
    ++driver.calls;
    if (driver.armed && driver.failure_ == Failure::Query) {
        ++driver.failures;
        return CUDA_ERROR_INVALID_CONTEXT;
    }
    return cuCtxGetCurrent(context);
}
CUresult PredictionContextFault::Set(void* owner, CUcontext context) noexcept {
    auto& driver = *static_cast<PredictionContextFault*>(owner);
    ++driver.calls;
    if (driver.observe_candidate && driver.restores == 0U) static_cast<void>(cuCtxGetCurrent(&driver.candidate));
    const auto result = cuCtxSetCurrent(context);
    ++driver.restores;
    if (driver.armed && (driver.failure_ == Failure::RestoreAlways || (driver.failure_ == Failure::RestoreOnce && driver.failures == 0U))) {
        ++driver.failures;
        return CUDA_ERROR_INVALID_CONTEXT;
    }
    return result;
}
contracts::ComputeTerminal ComputeSequence::Run(std::stop_token stop, const ComputeProgressSink& progress) {
    progress(scenario_.fail
                 ? contracts::ComputeProgress{.sequence = 0U, .completed = 2U, .total = 1U, .status = std::string(contracts::kComputeStatusCapacity + 1U, 'x')}
                 : contracts::ComputeProgress{.sequence = 1U, .completed = 1U, .total = 2U, .status = "running"});
    if (!scenario_.gate->Wait(stop)) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
    if (scenario_.fail)
        return {.outcome = static_cast<contracts::ComputeOperationOutcome>(255U),
                .output = std::string(contracts::kComputePathCapacity + 1U, 'x'),
                .detail = std::string(contracts::kComputeErrorCapacity + 1U, 'x')};
    return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded, 0U, 2U, "result");
}
ValidationRuntimeResult FakeNonvisualComputeRuntime::Run(rfdetr::ValidateRequest, std::stop_token stop, const ComputeProgressSink& progress,
                                                         const rfdetr::ValidationDelivery&) {
    ValidationRuntimeResult result{.terminal = sequence_.Run(stop, progress)};
    if (result.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded) {
        result.evaluation.emplace();
        result.evaluation->summary.bbox.available = true;
        result.evaluation->summary.bbox.ap = 0.75;
        result.evaluation->class_catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{
            "last in model", "absent", "middle", "first in model", std::string(mmltk::backend::data::catalog::kClassNameCapacity, 'z')});
        result.evaluation->details.resize(5U);
        for (std::uint32_t index = 0U; index < 5U; ++index) result.evaluation->details[index].category = index;
    }
    return result;
}
contracts::ComputeTerminal FakeNonvisualComputeRuntime::Run(rfdetr::ModelExportRequest, std::stop_token stop, const ComputeProgressSink& progress) {
    return sequence_.Run(stop, progress);
}
FakePredictRuntime::FakePredictRuntime(PredictionScenario scenario) : sequence_(std::move(scenario.compute)), scenario_(std::move(scenario)) {}
contracts::ComputeTerminal FakePredictRuntime::Run(rfdetr::PredictRequest, std::stop_token stop, const ComputeProgressSink& progress,
                                                   const ProductSink& products, const PlaybackGate&, VisualExtent, const ContextProvider& current_context,
                                                   const PreviewRetirement& retirement) {
    if (scenario_.predictions) ++*scenario_.predictions;
    auto terminal = sequence_.Run(stop, progress);
    if (terminal.outcome != contracts::ComputeOperationOutcome::Succeeded) return terminal;
    if (scenario_.refuse_preview) {
        products(std::unexpected{std::string{"Prediction preview exceeds the visual object capacity"}});
        return terminal;
    }
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    const auto context = current_context();
    if (!context) return terminal;
    if (!preview_) preview_ = std::make_unique<detail::PredictionPreviewPool>(execution, *context, PredictionReceiverFault::Operations(), retirement);
    if (scenario_.receiver_fault && scenario_.receiver_fault->enabled) {
        scenario_.receiver_fault->retirement = retirement;
        std::array<std::uint8_t, 48U> white;
        white.fill(255U);
        auto source = PredictionSource::Decoded({4U, 4U}, white);
        scenario_.receiver_fault->decoded = source.custody();
        auto raw = preview_->Capture(nullptr, source.extent(), 0U, {}, {}, source.classes(), 1, source.rgb8(), source.custody());
        products(Product{.extent = source.extent(), .raw = std::move(raw), .image_id = 41});
        return terminal;
    }
    std::array<float, 48U> white;
    white.fill(1.0F);
    auto source = PredictionSource::Device(
        execution, {4U, 4U}, white,
        std::vector<rfdetr::Prediction>(scenario_.labels, {.class_reference = 0, .score = .75F, .bbox_xyxy = {0.0F, 0.0F, 3.0F, 3.0F}}),
        std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{std::string(256U, 'p')}));
    auto raw =
        preview_->Capture(source.pixels(), source.extent(), 0U, source.detections(), source.annotations(), source.classes(), 1, nullptr, source.custody());
    products(
        Product{.extent = source.extent(), .raw = std::move(raw), .image_id = 41, .source_index = scenario_.source_index ? scenario_.source_index->load() : 0});
    if (scenario_.after_product && scenario_.after_product->Wait(stop)) progress({2U, 2U, 2U, "Processed"});
    return terminal;
}
}  // namespace mmltk::controller::test_support
