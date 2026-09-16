#include "detail/live_device_types.h"
namespace mmltk::backend::media::live {
thread_local detail::ActiveLiveCudaContext detail::active_live_cuda_context{};
LivePhysicalCudaContext::LivePhysicalCudaContext(gpu::ResourceOwnerCommandBinding command, gpu::CudaDeviceOwner device) noexcept
    : command_(std::move(command)), device_(device) {}
bool LivePhysicalCudaContext::valid() const noexcept { return command_.valid() && device_.device >= 0 && device_.record_failure != nullptr; }
int LivePhysicalCudaContext::device() const noexcept { return device_.device; }
LiveCudaCommandScope LivePhysicalCudaContext::scope() const noexcept { return LiveCudaCommandScope{*this}; }
void LivePhysicalCudaContext::Record(const cudaError_t status) const noexcept {
    if (status != cudaSuccess && device_.record_failure != nullptr) device_.record_failure(device_.context, status);
}
gpu::ResourceOwnerCommandScope LivePhysicalCudaContext::EnterCommand() const noexcept {
    return command_.authorized() ? command_.EnterWorker() : command_.EnterMandatedOwnerThread();
}
LiveCudaCommandScope::LiveCudaCommandScope(const LivePhysicalCudaContext& context) noexcept
    : context_(&context),
      command_(context.EnterCommand()),
      device_(command_ &&
                      !(detail::active_live_cuda_context.worker == context.command_.worker_identity() &&
                        detail::active_live_cuda_context.owner == context.device_.context && detail::active_live_cuda_context.device == context.device_.device)
                  ? context.device_
                  : gpu::CudaDeviceOwner{}),
      nested_(command_ && detail::active_live_cuda_context.worker == context.command_.worker_identity() &&
              detail::active_live_cuda_context.owner == context.device_.context && detail::active_live_cuda_context.device == context.device_.device) {
    if (!command_ || (!nested_ && !device_)) {
        first_status_ = device_.status() == cudaSuccess ? cudaErrorNotPermitted : device_.status();
        context.Record(first_status_);
    } else if (!nested_) {
        detail::active_live_cuda_context = {context.command_.worker_identity(), context.device_.context, context.device_.device};
        owns_active_context_ = true;
    }
}
LiveCudaCommandScope::~LiveCudaCommandScope() noexcept {
    const cudaError_t status = nested_ ? first_status_ : device_.FinalizeStatus(first_status_);
    if (owns_active_context_) detail::active_live_cuda_context = {};
    if (context_ != nullptr) context_->Record(status);
}
LiveCudaCommandScope::operator bool() const noexcept {
    return static_cast<bool>(command_) && (nested_ || static_cast<bool>(device_)) && first_status_ == cudaSuccess;
}
cudaError_t LiveCudaCommandScope::Record(const cudaError_t status) noexcept {
    if (status != cudaSuccess && first_status_ == cudaSuccess) {
        first_status_ = status;
        if (context_ != nullptr) context_->Record(status);
    }
    return status;
}
cudaError_t LiveCudaCommandScope::Record(const std::int32_t status) noexcept { return Record(static_cast<cudaError_t>(status)); }
cudaError_t copy_device_frame(LiveCudaCommandScope& scope, const DeviceFrameView& source, const DeviceFrameCopyTarget target) noexcept {
    cudaError_t status = scope.Record(cudaStreamWaitEvent(target.stream, source.ready, 0U));
    if (status == cudaSuccess)
        status = scope.Record(cudaMemcpy2DAsync(reinterpret_cast<void*>(target.pixels), target.pitch_bytes, reinterpret_cast<const void*>(source.pixels),
                                                source.pitch_bytes, static_cast<std::size_t>(source.width) * 3U, source.height, cudaMemcpyDeviceToDevice,
                                                target.stream));
    if (status == cudaSuccess) status = scope.Record(cudaEventRecord(target.ready, target.stream));
    return status;
}
void synchronize_live_cuda_stream(LiveCudaCommandScope& scope, const cudaStream_t stream) noexcept {
    if (stream != nullptr) static_cast<void>(scope.Record(cudaStreamSynchronize(stream)));
}
void destroy_live_cuda_event(LiveCudaCommandScope& scope, cudaEvent_t& event) noexcept {
    if (event == nullptr) return;
    static_cast<void>(scope.Record(cudaEventDestroy(event)));
    event = nullptr;
}
void destroy_live_cuda_stream(LiveCudaCommandScope& scope, cudaStream_t& stream) noexcept {
    if (stream == nullptr) return;
    static_cast<void>(scope.Record(cudaStreamDestroy(stream)));
    stream = nullptr;
}
void free_live_cuda_allocation(LiveCudaCommandScope& scope, CUdeviceptr& allocation) noexcept {
    if (allocation == 0U) return;
    static_cast<void>(scope.Record(cudaFree(reinterpret_cast<void*>(allocation))));
    allocation = 0U;
}
}  // namespace mmltk::backend::media::live
