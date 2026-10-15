#include "src/controller/subsystems/live/live_receiver_copy.h"
#include <new>
#include <utility>
namespace mmltk::controller::detail {
namespace {
cudaError_t Wait(const std::uintptr_t stream, const std::uintptr_t event) noexcept { return cudaStreamWaitEvent(reinterpret_cast<cudaStream_t>(stream), reinterpret_cast<cudaEvent_t>(event), 0U); }
cudaError_t Copy(const mmltk::frameworks::gpu::ImagePlaneView target, const std::uintptr_t source, const std::size_t source_pitch, const std::uintptr_t stream) noexcept {
 return cudaMemcpy2DAsync(reinterpret_cast<void*>(target.data), target.descriptor.pitch_bytes, reinterpret_cast<const void*>(source), source_pitch, target.descriptor.row_bytes(),
  target.descriptor.height, cudaMemcpyDeviceToDevice, reinterpret_cast<cudaStream_t>(stream));
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
cudaError_t copy_live_receiver_frame(const mmltk::frameworks::gpu::ImagePlaneView target,
 mmltk::backend::media::live::LiveCompositeOutputLease& lease, mmltk::backend::media::live::LiveAnalysisProduct& analysis,
 const std::uintptr_t stream, const LiveReceiverCopyOperations& operations) noexcept {
 if (!target.valid() || !lease || stream == 0U || !operations.valid()) return cudaErrorInvalidValue;
 const auto& source = lease.view();
 if (source.width != target.descriptor.width || source.height != target.descriptor.height || source.pitch_bytes < target.descriptor.row_bytes() ||
     (source.analysis && (!source.analysis->ready() || source.analysis->frame() != lease.frame_revision().frame))) return cudaErrorInvalidValue;
 cudaError_t status = operations.wait(stream, source.ready_event);
 if (status == cudaSuccess) status = operations.copy(target, source.pixels, source.pitch_bytes, stream);
 // Even a failed submission may follow queued reads. Only proved settlement
 // permits release; otherwise the caller retains/abandons the terminal lease.
 const auto settled = operations.synchronize(stream);
 if (settled != cudaSuccess) return settled;
 if (status == cudaSuccess) {
  try {
   if (source.analysis) analysis.CopyFrom(*source.analysis);
   else analysis.Reset(lease.frame_revision().frame);
  } catch (const std::bad_alloc&) {
   status = cudaErrorMemoryAllocation;
  } catch (...) {
   status = cudaErrorInvalidValue;
  }
 }
 std::move(lease).Complete();
 return status;
}
}  // namespace mmltk::controller::detail
