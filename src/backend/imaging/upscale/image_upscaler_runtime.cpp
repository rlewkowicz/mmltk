module;
#include <cuda_runtime.h>
#include <array>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include "detail/image_upscaler_cuda.h"
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/common/system/runtime_paths.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/image_failure.h"
#include "upscale_execution.h"
module mmltk.backend.imaging.upscale.image_upscaler;
#include "detail/image_upscaler_internal.h"
namespace mmltk::backend::imaging::upscale {
using mmltk::frameworks::gpu::ensure_cuda_ok;
namespace {
constexpr std::array<ImageUpscalerDescriptor, 2U> kDescriptors{{
 {
  .kind = ImageUpscalerKind::ShiftLUT,
  .filename = "ShiftLUT_fp32.onnx",
  .input_name = "image",
  .output_name = "upscaled",
  .sha256 = "811f4e42557d549a06352c477f2ec4047cd84c57ca345955d8996bb271e727f7",
  .cache_name = "shiftlut-fp32",
  .label = "ShiftLUT",
  .halo = 32U,
  .tensor_rt_enabled = false,
  .allow_fp16 = false,
  .allow_tf32 = false,
 },
 {
  .kind = ImageUpscalerKind::RealPLKSR,
  .filename = "RealPLKSR_fp16.onnx",
  .input_name = "input",
  .output_name = "output",
  .sha256 = "294d117eb8e7093417cc972d41c7c5b84f3d452193c952e58bc7c94e2143692d",
  .cache_name = "realplksr-fp16",
  .label = "RealPLKSR",
  .halo = 16U,
  .tensor_rt_enabled = true,
  .allow_fp16 = true,
  .allow_tf32 = true,
 },
}};
}
const ImageUpscalerDescriptor& image_upscaler_descriptor(const ImageUpscalerKind kind) noexcept {
 const std::size_t index = static_cast<std::size_t>(kind);
 if (index >= kDescriptors.size()) { std::terminate(); }
 return kDescriptors[index];
}
std::filesystem::path image_upscaler_model_path(const ImageUpscalerDescriptor& descriptor) {
 std::filesystem::path repository = mmltk::common::system::runtime_paths::repository_root() / "src" / "backend" / "imaging" / "upscale" / "assets" / descriptor.filename;
 if (std::filesystem::is_regular_file(repository)) { return repository; }
 std::filesystem::path installed = mmltk::common::system::runtime_paths::install_prefix() / "models" / descriptor.filename;
 if (std::filesystem::is_regular_file(installed)) { return installed; }
 throw std::runtime_error("missing Image upscaler model " + std::string(descriptor.filename));
}
std::size_t checked_upscaler_elements(const std::uint32_t width, const std::uint32_t height, const std::size_t channels) {
 if (width == 0U || height == 0U || static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / width) { throw std::overflow_error("Image upscaler dimensions overflow"); }
 const std::size_t pixels = static_cast<std::size_t>(width) * height;
 if (channels == 0U || pixels > std::numeric_limits<std::size_t>::max() / channels) { throw std::overflow_error("Image upscaler tensor size overflow"); }
 return pixels * channels;
}
cudaError_t settle_upscaler_stream(cudaStream_t stream, cudaGraph_t& abandoned_capture) noexcept {
 if (stream == nullptr) return cudaSuccess;
 cudaStreamCaptureStatus capture = cudaStreamCaptureStatusNone;
 auto status = cudaStreamIsCapturing(stream, &capture);
 if (status != cudaSuccess) return status;
 if (capture != cudaStreamCaptureStatusNone) {
  status = cudaStreamEndCapture(stream, &abandoned_capture);
  if (status != cudaSuccess && status != cudaErrorStreamCaptureInvalidated) return status;
  status = cudaStreamIsCapturing(stream, &capture);
  if (status != cudaSuccess) return status;
  if (capture != cudaStreamCaptureStatusNone) return cudaErrorStreamCaptureInvalidated;
 }
 return cudaStreamSynchronize(stream);
}
UpscalerFloatBuffer::~UpscalerFloatBuffer() {
 if (data_ != nullptr || replacement_ != nullptr) std::terminate();
}
void UpscalerFloatBuffer::ensure(const std::size_t elements, const char* context) {
 if (elements <= capacity_) { return; }
 if (elements > std::numeric_limits<std::size_t>::max() / sizeof(float)) { throw std::overflow_error(std::string(context) + " byte size overflow"); }
 if (replacement_ != nullptr) throw std::logic_error("failed Image upscaler buffer requires retirement");
 ensure_cuda_ok(cudaMalloc(reinterpret_cast<void**>(&replacement_), elements * sizeof(float)), context);
 if (data_ != nullptr) { ensure_cuda_ok(cudaFree(data_), "cudaFree while growing Image upscaler buffer"); }
 data_ = std::exchange(replacement_, nullptr);
 capacity_ = elements;
}
cudaError_t UpscalerFloatBuffer::Release(UpscalerCleanup* cleanup) noexcept {
 cudaError_t failure = cudaSuccess;
 if (data_ != nullptr) {
  failure = cudaFree(data_);
  if (cleanup) cleanup->Record(failure, "release neural float allocation");
  if (failure == cudaSuccess) {
   data_ = nullptr;
   capacity_ = 0U;
  }
 }
 if (replacement_ != nullptr) {
  const auto replacement_failure = cudaFree(replacement_);
  if (cleanup) cleanup->Record(replacement_failure, "release neural replacement allocation");
  if (replacement_failure == cudaSuccess) replacement_ = nullptr;
  if (failure == cudaSuccess) failure = replacement_failure;
 }
 return failure;
}
TiledImageUpscalerRuntimeState::TiledImageUpscalerRuntimeState(ImageUpscalerDescriptor descriptor, const int device_id) : descriptor_(descriptor), device_id_(device_id) {}
ImageUpscalerOutcome TiledImageUpscalerRuntimeState::Activate(const ImageUpscalerExecutionCheckpoint& checkpoint, ImageUpscalerCurrent current) {
 if (!current()) return ImageUpscalerOutcome::Cancelled;
 ensure_cuda_ok(cudaSetDevice(device_id_), "cudaSetDevice for Image upscaler runtime");
 ensure_cuda_ok(cudaEventCreateWithFlags(&consumer_done_, cudaEventDisableTiming), "cudaEventCreate for Image upscaler output consumption");
 if (checkpoint) checkpoint(ImageUpscalerExecutionStage::EventCreated);
 if (!current()) return ImageUpscalerOutcome::Cancelled;
 ensure_cuda_ok(cudaStreamCreateWithFlags(&cleanup_stream_, cudaStreamNonBlocking), "cudaStreamCreate for Image upscaler cleanup");
 if (checkpoint) checkpoint(ImageUpscalerExecutionStage::StreamCreated);
 return current() ? ImageUpscalerOutcome::Completed : ImageUpscalerOutcome::Cancelled;
}
TiledImageUpscalerRuntimeState::~TiledImageUpscalerRuntimeState() {
 std::lock_guard lock(mutex_);
 if ((!stopped_ && std::uncaught_exceptions() == 0) || awaiting_consumer_ || consumer_fatal_) std::terminate();
 if (consumer_done_ != nullptr || cleanup_stream_ != nullptr) std::terminate();
}
ImageUpscalerRuntimeOutput TiledImageUpscalerRuntimeState::enqueue(
 const ImageUpscalerRequest& request, const cudaStream_t consumer_stream, void* backend, const ImageUpscalerSubmitTiles submit_tiles, const ImageUpscalerExecutionCheckpoint& checkpoint) {
 std::lock_guard lock(mutex_);
 if (request.device_pixels == nullptr || request.target_pixels == nullptr || consumer_stream == nullptr || request.source_pitch < static_cast<std::size_t>(request.source_width) * 4U ||
     request.target_pitch < static_cast<std::size_t>(request.crop_width) * 16U || request.crop_width == 0U || request.crop_height == 0U || request.source_width == 0U || request.source_height == 0U ||
     request.crop_x > request.source_width || request.crop_width > request.source_width - request.crop_x || request.crop_y > request.source_height ||
     request.crop_height > request.source_height - request.crop_y) {
  throw std::invalid_argument("Image upscaler request has invalid source geometry");
 }
 if (awaiting_consumer_) { throw std::runtime_error("Image upscaler output was not consumed"); }
 ensure_cuda_ok(cudaSetDevice(device_id_), "cudaSetDevice for Image upscaler enqueue");
 if (consumer_pending_) {
  ensure_cuda_ok(cudaStreamWaitEvent(consumer_stream, consumer_done_, 0U), "cudaStreamWaitEvent before reusing Image upscaler output");
  consumer_pending_ = false;
 }
 if (request.crop_width > std::numeric_limits<std::uint32_t>::max() / 4U || request.crop_height > std::numeric_limits<std::uint32_t>::max() / 4U) {
  throw std::overflow_error("Image upscaler output dimensions overflow");
 }
 const std::uint32_t restored_width = request.crop_width * 4U;
 const std::uint32_t restored_height = request.crop_height * 4U;
 static_cast<void>(checked_upscaler_elements(restored_width, restored_height, 4U));
 if (request.source_pitch > std::numeric_limits<std::size_t>::max() / request.source_height || request.target_pitch > std::numeric_limits<std::size_t>::max() / restored_height)
  throw std::overflow_error("Image upscaler pitched image size overflow");
 if (!image_upscaler_admitted(checkpoint, ImageUpscalerExecutionStage::TargetAdmitted, request.current)) return {.outcome = ImageUpscalerOutcome::Cancelled};
 if (!request.current() || !submit_tiles(backend, request, consumer_stream, restored_width, restored_height)) return {.outcome = ImageUpscalerOutcome::Cancelled};
 awaiting_consumer_ = true;
 return {
  .device_pixels = request.target_pixels,
  .width = restored_width,
  .height = restored_height,
 };
}
void TiledImageUpscalerRuntimeState::mark_consumed(const cudaStream_t stream) {
 std::lock_guard lock(mutex_);
 if (!awaiting_consumer_) { return; }
 ensure_cuda_ok(cudaEventRecord(consumer_done_, stream), "cudaEventRecord for Image upscaler output consumption");
 awaiting_consumer_ = false;
 consumer_pending_ = true;
}
void TiledImageUpscalerRuntimeState::abandon_consumer() noexcept {
 std::lock_guard lock(mutex_);
 if (!awaiting_consumer_) { return; }
 consumer_fatal_ = true;
}
cudaError_t TiledImageUpscalerRuntimeState::Stop(void* backend, const ImageUpscalerReleaseBackend release_backend, const ImageUpscalerExecutionCheckpoint& checkpoint) noexcept {
 std::lock_guard lock(mutex_);
 if (stopped_) return cudaSuccess;
 if (!cleanup_.Record(cudaSetDevice(device_id_), "bind neural cleanup device")) return cleanup_.status();
 bool settled = cleanup_.Record(awaiting_consumer_ || consumer_fatal_ ? cudaErrorNotReady : cudaSuccess, "settle neural consumer ownership");
 if (consumer_pending_) settled = cleanup_.Record(cudaEventSynchronize(consumer_done_), "settle neural consumer fence") && settled;
 if (cleanup_stream_ != nullptr) settled = cleanup_.Record(cudaStreamSynchronize(cleanup_stream_), "settle neural cleanup stream") && settled;
 // Backend buffers are still referenced by an unsettled consumer.
 if (!settled) return cleanup_.status();
 cleanup_.Record(release_backend(backend), "release neural backend");
 if (consumer_done_ != nullptr) {
  if (cleanup_.Record(cudaEventDestroy(consumer_done_), "destroy neural consumer fence")) consumer_done_ = nullptr;
  cleanup_.Checkpoint(checkpoint, ImageUpscalerExecutionStage::EventDestroyed);
 }
 if (cleanup_stream_ != nullptr) {
  if (cleanup_.Record(cudaStreamDestroy(cleanup_stream_), "destroy neural cleanup stream")) cleanup_stream_ = nullptr;
  cleanup_.Checkpoint(checkpoint, ImageUpscalerExecutionStage::StreamDestroyed);
 }
 consumer_pending_ = false;
 stopped_ = cleanup_.status() == cudaSuccess;
 return cleanup_.status();
}
}  // namespace mmltk::backend::imaging::upscale
