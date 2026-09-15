#include "prediction_test_support.h"
#include "src/backend/models/rfdetr/contract/prediction_limits.h"
#include "src/frameworks/gpu/image_failure.h"
#include <algorithm>
#include <array>
#include <cstring>
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
    return rfdetr::checked_prediction_extent(
        rfdetr::checked_prediction_extent(extent.width, extent.height, rfdetr::kMaximumEncodedMaskPixels),
        3U, rfdetr::kMaximumPredictionTensorBytes / sizeof(float));
}
struct DeviceAllocation final {
    explicit DeviceAllocation(const gpu::DeviceExecution& execution)
        : context(execution.device, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::PrimaryInterop,
            execution.placement.numa_node, execution) {}
    gpu::DeviceContext context;
    void* address = nullptr;
    static void Release(DeviceAllocation* allocation) noexcept {
        // Never free in an unrelated context. Real context/free failure retains
        // the complete aggregate; injected preview failures do not bypass this.
        CUcontext previous{};
        if (cuCtxGetCurrent(&previous) != CUDA_SUCCESS) return;
        try { allocation->context.Bind(); } catch (...) { return; }
        const auto result = allocation->address ? cudaFree(allocation->address) : cudaSuccess;
        if (result == cudaSuccess) allocation->address = nullptr;
        const auto restored = cuCtxSetCurrent(previous);
        if (result == cudaSuccess && restored == CUDA_SUCCESS) delete allocation;
    }
};
}
void StopGate::Release() {
    { std::scoped_lock lock(mutex_); released_ = true; }
    condition_.notify_all();
}
void StopGate::Reset() { std::scoped_lock lock(mutex_); released_ = false; }
bool StopGate::Wait(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    return condition_.wait(lock, stop, [this] { return released_; });
}
PredictionSource::PredictionSource(VisualExtent extent, Catalog classes)
    : extent_(extent), classes_(classes ? std::move(classes) : std::make_shared<const std::vector<std::string>>()) {}
PredictionSource PredictionSource::Device(const gpu::DeviceExecution& execution, VisualExtent extent,
    std::span<const float> pixels, std::vector<Detection> detections, Catalog classes) {
    const auto values = pixel_values(extent);
    if (pixels.size() != values || detections.size() > contracts::kAnnotationObjectCapacity)
        throw std::invalid_argument("test source data does not match its geometry");
    const auto pixel_bytes = values * sizeof(float);
    const auto boxes_bytes = detections.size() * 4U * sizeof(float);
    const auto label_bytes = detections.size() * sizeof(std::int32_t);
    const auto bytes = pixel_bytes + boxes_bytes + label_bytes;
    if (bytes > rfdetr::kMaximumPredictionTensorBytes) throw std::invalid_argument("test source is too large");
    std::vector<std::byte> packed(bytes);
    std::memcpy(packed.data(), pixels.data(), pixel_bytes);
    for (std::size_t index = 0; index < detections.size(); ++index) {
        std::memcpy(packed.data() + pixel_bytes + index * 4U * sizeof(float), detections[index].bbox_xyxy.data(), 4U * sizeof(float));
        const std::int32_t category = detections[index].category_id - 1;
        std::memcpy(packed.data() + pixel_bytes + boxes_bytes + index * sizeof(category), &category, sizeof(category));
    }
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
    result.annotations_ = {
        .source_region = {.width = extent.width, .height = extent.height},
        .value_capacity = result.detections_.size(), .value_count = result.detections_.size(),
        .boxes_xyxy = {.address = address + pixel_bytes, .capacity_bytes = boxes_bytes},
        .category_ids = {.address = address + pixel_bytes + boxes_bytes, .capacity_bytes = label_bytes}};
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
ScopedPredictionReceiverFault::ScopedPredictionReceiverFault(PredictionReceiverFault& fault)
    : previous_(receiver_fault.exchange(&fault)) {}
ScopedPredictionReceiverFault::~ScopedPredictionReceiverFault() { receiver_fault.store(previous_); }
cudaError_t PredictionReceiverFault::Upload(void* destination, const void* source, std::size_t bytes, cudaMemcpyKind kind, cudaStream_t stream) {
    const auto copied = cudaMemcpyAsync(destination, source, bytes, kind, stream);
    if (copied != cudaSuccess) return copied;
    auto* fault = receiver_fault.load();
    if (!fault || !fault->enabled) return cudaSuccess;
    // Actual upload/pinned allocation precede injection; physical test work is
    // settled before the deliberately unobservable receiver outcome is reported.
    const auto settled = cudaStreamSynchronize(stream);
    fault->upload.receipt().ArriveAndWait();
    if (fault->terminal) throw gpu::ImageStreamExecutionFailure(
        std::make_exception_ptr(std::runtime_error("injected receiver completion failure")));
    return settled == cudaSuccess ? cudaErrorMemoryAllocation : settled;
}
int PredictionReceiverFault::Convert(const float* source, std::uint32_t width, std::uint32_t height,
    std::uint8_t* destination, std::size_t pitch, std::uintptr_t stream) noexcept {
    auto* fault = receiver_fault.load();
    if (fault && fault->partial_draw) {
        const auto command = reinterpret_cast<cudaStream_t>(stream);
        const auto written = cudaMemset2DAsync(destination, pitch, 123, width * 4U, 1U, command);
        if (written != cudaSuccess) return written;
        const auto settled = cudaStreamSynchronize(command);
        return settled == cudaSuccess ? cudaErrorMemoryAllocation : settled;
    }
    return mmltk::backend::imaging::raster::chw_float_to_rgba(source, width, height, destination, pitch, stream);
}
detail::PredictionPreviewPool::TransferOperations PredictionReceiverFault::Operations() {
    return {&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister, &Upload, {}, &Convert};
}
PredictionTransferFault::PredictionTransferFault() : previous_(std::exchange(transfer_fault, this)) {}
PredictionTransferFault::~PredictionTransferFault() { transfer_fault = previous_; }
void PredictionTransferFault::Reset() { Reset(Selection{}); }
void PredictionTransferFault::Reset(Selection selection) { selection_ = selection; copies = settlements = 0; }
CUresult PredictionTransferFault::Copy(CUdeviceptr destination, CUcontext destination_context, CUdeviceptr source,
    CUcontext source_context, std::size_t bytes, CUstream stream) {
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
detail::PredictionPreviewPool::TransferOperations PredictionTransferFault::Operations() {
    return {&Copy, &Record, &Settle, &cuMemHostRegister};
}
detail::PredictionPreviewPool::TransferOperations RefusePinnedRegistration() {
    return {&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize,
        +[](void*, std::size_t, unsigned) -> CUresult { return CUDA_ERROR_OUT_OF_MEMORY; }};
}
void CountPredictionSourceStop(void* count) noexcept { ++*static_cast<int*>(count); }
gpu::CudaContextApi PredictionContextFault::Api() noexcept { return {this, &Get, &Set}; }
CUresult PredictionContextFault::Get(void* owner, CUcontext* context) noexcept {
    auto& driver = *static_cast<PredictionContextFault*>(owner);
    ++driver.calls;
    if (driver.armed && driver.failure_ == Failure::Query) { ++driver.failures; return CUDA_ERROR_INVALID_CONTEXT; }
    return cuCtxGetCurrent(context);
}
CUresult PredictionContextFault::Set(void* owner, CUcontext context) noexcept {
    auto& driver = *static_cast<PredictionContextFault*>(owner);
    ++driver.calls;
    if (driver.observe_candidate && driver.restores == 0U) static_cast<void>(cuCtxGetCurrent(&driver.candidate));
    const auto result = cuCtxSetCurrent(context);
    ++driver.restores;
    if (driver.armed && (driver.failure_ == Failure::RestoreAlways ||
        (driver.failure_ == Failure::RestoreOnce && driver.failures == 0U))) {
        ++driver.failures;
        return CUDA_ERROR_INVALID_CONTEXT;
    }
    return result;
}
contracts::ComputeTerminal ComputeSequence::Run(std::stop_token stop, const ComputeProgressSink& progress) {
    progress(scenario_.fail ? contracts::ComputeProgress{.sequence = 0U, .completed = 2U, .total = 1U,
        .status = std::string(contracts::kComputeStatusCapacity + 1U, 'x')}
        : contracts::ComputeProgress{.sequence = 1U, .completed = 1U, .total = 2U, .status = "running"});
    if (!scenario_.gate->Wait(stop)) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
    if (scenario_.fail) return {.outcome = static_cast<contracts::ComputeOperationOutcome>(255U),
        .output = std::string(contracts::kComputePathCapacity + 1U, 'x'), .detail = std::string(contracts::kComputeErrorCapacity + 1U, 'x')};
    return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded, 0U, 2U, "result");
}
contracts::ComputeTerminal FakeNonvisualComputeRuntime::Run(rfdetr::ValidateRequest, std::stop_token stop, const ComputeProgressSink& progress) {
    return sequence_.Run(stop, progress);
}
contracts::ComputeTerminal FakeNonvisualComputeRuntime::Run(rfdetr::ModelExportRequest, std::stop_token stop, const ComputeProgressSink& progress) {
    return sequence_.Run(stop, progress);
}
FakePredictRuntime::FakePredictRuntime(PredictionScenario scenario)
    : sequence_(std::move(scenario.compute)), scenario_(std::move(scenario)) {}
contracts::ComputeTerminal FakePredictRuntime::Run(rfdetr::PredictRequest, std::stop_token stop,
    const ComputeProgressSink& progress, const ProductSink& products, const PlaybackGate&, VisualExtent,
    const ContextProvider& current_context, const PreviewRetirement& retirement) {
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
    if (!preview_) preview_ = std::make_unique<detail::PredictionPreviewPool>(execution, *context,
        PredictionReceiverFault::Operations(), retirement);
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
    auto source = PredictionSource::Device(execution, {4U, 4U}, white,
        std::vector<rfdetr::Prediction>(scenario_.labels, {.category_id = 1, .score = .75F, .bbox_xyxy = {0.0F, 0.0F, 3.0F, 3.0F}}),
        std::make_shared<const std::vector<std::string>>(std::vector<std::string>{std::string(256U, 'p')}));
    auto raw = preview_->Capture(source.pixels(), source.extent(), 0U, source.detections(), source.annotations(),
        source.classes(), 1, nullptr, source.custody());
    products(Product{.extent = source.extent(), .raw = std::move(raw), .image_id = 41,
        .source_index = scenario_.source_index ? scenario_.source_index->load() : 0});
    if (scenario_.after_product && scenario_.after_product->Wait(stop)) progress({2U, 2U, 2U, "Processed"});
    return terminal;
}
} // namespace mmltk::controller::test_support
