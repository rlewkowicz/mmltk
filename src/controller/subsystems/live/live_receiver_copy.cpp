#include "src/controller/subsystems/live/live_receiver_copy.h"
namespace mmltk::controller::detail {
namespace {
cudaError_t Wait(const std::uintptr_t stream, const std::uintptr_t event) noexcept {
    return cudaStreamWaitEvent(reinterpret_cast<cudaStream_t>(stream), reinterpret_cast<cudaEvent_t>(event), 0U);
}
cudaError_t Copy(const mmltk::frameworks::gpu::ImagePlaneView target, const std::uintptr_t source, const std::size_t source_pitch,
                 const std::uintptr_t stream) noexcept {
    return cudaMemcpy2DAsync(reinterpret_cast<void*>(target.data), target.descriptor.pitch_bytes, reinterpret_cast<const void*>(source), source_pitch,
                             target.descriptor.row_bytes(), target.descriptor.height, cudaMemcpyDeviceToDevice, reinterpret_cast<cudaStream_t>(stream));
}
cudaError_t Synchronize(const std::uintptr_t stream) noexcept { return cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(stream)); }
}  // namespace
LiveReceiverCopyOperations native_live_receiver_copy_operations() noexcept {
    return {
        .wait = &Wait,
        .copy = &Copy,
        .synchronize = &Synchronize,
    };
}
cudaError_t copy_live_receiver_frame(const mmltk::frameworks::gpu::ImagePlaneView target, const std::uintptr_t source, const std::size_t source_pitch,
                                     const std::uintptr_t ready_event, const std::uintptr_t stream, const LiveReceiverCopyOperations& operations) noexcept {
    if (!target.valid() || source == 0U || source_pitch == 0U || ready_event == 0U || stream == 0U || !operations.valid()) return cudaErrorInvalidValue;
    cudaError_t status = operations.wait(stream, ready_event);
    if (status == cudaSuccess) status = operations.copy(target, source, source_pitch, stream);
    if (status == cudaSuccess) status = operations.synchronize(stream);
    return status;
}
}  // namespace mmltk::controller::detail
