// CLEANUP-IGNORE: This module implementation owns its concrete CUDA and provider dependencies.
module;
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>
#include <torch/torch.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include "src/backend/imaging/raster/image_operations.h"
#include "src/backend/models/rfdetr/core/gpu_batch_preprocessor.h"
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/frameworks/gpu/cuda_error.h"
module mmltk.backend.models.rfdetr.inference.analysis_provider;
import mmltk.backend.models.rfdetr.inference.runtime_backend;
import mmltk.backend.ml.cuda.gpu_quiescence;
namespace mmltk::backend::models::rfdetr {
namespace runtime = mmltk::backend::ml::runtime;
class RfdetrAnalysisProvider final : public runtime::AnalysisProvider {
public:
 explicit RfdetrAnalysisProvider(const RfdetrAnalysisOptions& options);
 ~RfdetrAnalysisProvider() override;

protected:
 [[nodiscard]] ProviderWorkResult DoAnalyze(const runtime::AnalysisRequest& request) noexcept override;
 [[nodiscard]] bool ObserveCompletion(const runtime::AnalysisCompletion& completion) noexcept override;
 void RetireIssuedWork(const ProviderWorkResult&) noexcept override;
 void DoShutdown() noexcept override;

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
namespace {
[[nodiscard]] std::uint64_t steady_now_ns() noexcept {
 return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace
struct RfdetrAnalysisProvider::Impl final {
 explicit Impl(const RfdetrAnalysisOptions& options)
     : backend(make_rfdetr_runtime_backend({.artifacts = options.artifacts,
                                            .backend = options.backend,
                                            .device = options.device,
                                            .command_stream = options.command_stream,
                                            .static_resolution = options.static_resolution,
                                            .maximum_detections = options.maximum_detections,
                                            .save_compiled_model_path = {},
                                            .allow_fp16 = options.allow_fp16})),
       resolution(backend->static_resolution()),
       device(backend->device()),
       stream(reinterpret_cast<cudaStream_t>(backend->stream())) {
  using element_type = runtime::RuntimeElementType;
  if (resolution == 0U || device < 0 || stream == nullptr ||
      (backend->input_element_type() != element_type::Float16 && backend->input_element_type() != element_type::Float32)) {
   throw std::invalid_argument("RF-DETR analysis requires a static float runtime lane");
  }
  c10::cuda::CUDAGuard device_guard{c10::DeviceIndex(device)};
  c10::cuda::CUDAStreamGuard stream_guard{cuda_stream()};
  const auto float_options = torch::TensorOptions().dtype(at::kFloat).device(torch::kCUDA, device);
  input_float = torch::empty({static_cast<std::int64_t>(runtime::kMaximumAnalysisRegions), 3, resolution, resolution}, float_options);
  preprocessor = std::make_unique<GpuBatchPreprocessor>(runtime::kMaximumAnalysisRegions, resolution, resolution, device,
                                                        backend->input_element_type() == element_type::Float16 ? at::kHalf : at::kFloat);
 }
 [[nodiscard]] c10::cuda::CUDAStream cuda_stream() const noexcept { return c10::cuda::getStreamFromExternal(stream, c10::DeviceIndex(device)); }
 std::shared_ptr<RfdetrRuntimeBackend> backend;
 torch::Tensor input_float;
 std::unique_ptr<GpuBatchPreprocessor> preprocessor;
 std::optional<runtime::RuntimeSubmission> active_submission;
 std::span<runtime::AnalysisAnnotationStorage> active_annotations;
 void ClearCounts() noexcept {
  for (auto& annotation : active_annotations) {
   annotation.count.Reset();
   annotation.masks_available = false;
  }
  active_annotations = {};
 }
 std::uint32_t resolution = 0U;
 std::int32_t device = -1;
 cudaStream_t stream = nullptr;
};
RfdetrAnalysisProvider::RfdetrAnalysisProvider(const RfdetrAnalysisOptions& options) : impl_(std::make_unique<Impl>(options)) {}
RfdetrAnalysisProvider::~RfdetrAnalysisProvider() = default;
std::shared_ptr<runtime::AnalysisProvider> MakeRfdetrAnalysisProvider(const RfdetrAnalysisOptions& options) {
 return std::make_shared<RfdetrAnalysisProvider>(options);
}
RfdetrAnalysisProvider::ProviderWorkResult RfdetrAnalysisProvider::DoAnalyze(const runtime::AnalysisRequest& request) noexcept {
 try {
  if (request.source.device != impl_->device || request.source.channels != 3U || request.regions.size() > runtime::kMaximumAnalysisRegions ||
      impl_->active_submission.has_value()) {
   return {.identity = request.identity, .terminal = runtime::AnalysisTerminal::InvalidInput};
  }
  c10::cuda::CUDAGuard device_guard{c10::DeviceIndex(impl_->device)};
  c10::cuda::CUDAStreamGuard stream_guard{impl_->cuda_stream()};
  mmltk::frameworks::gpu::ensure_cuda_ok(cudaStreamWaitEvent(impl_->stream, reinterpret_cast<cudaEvent_t>(request.source_ready.event), 0U),
                                         "RF-DETR analysis source readiness");
  impl_->active_annotations = request.annotations;
  const std::size_t count = request.regions.size();
  auto input_float = impl_->input_float.narrow(0, 0, static_cast<std::int64_t>(count));
  for (std::size_t index = 0U; index < count; ++index) {
   const runtime::AnalysisRegion& region = request.regions[index];
   const auto* source = reinterpret_cast<const std::uint8_t*>(request.source.pixels.address) + static_cast<std::size_t>(region.y) * request.source.pitch_bytes +
                        static_cast<std::size_t>(region.x) * request.source.channels;
   mmltk::frameworks::gpu::ensure_cuda_ok(static_cast<cudaError_t>(mmltk::backend::imaging::raster::launch_bgr_split_to_planar_float(
                                           source, request.source.pitch_bytes, region.width, region.height, input_float[index].data_ptr<float>(),
                                           impl_->resolution, impl_->resolution, reinterpret_cast<std::uintptr_t>(impl_->stream))),
                                          "RF-DETR analysis preprocessing");
  }
  auto input = impl_->preprocessor->run({.num_images = count, .device_images = input_float.data_ptr<float>()});
  runtime::RuntimeShape shape{.rank = 4U};
  for (std::size_t axis = 0U; axis < 4U; ++axis) { shape.extents[axis] = input.size(static_cast<std::int64_t>(axis)); }
  const auto element_type = input.scalar_type() == at::kHalf ? runtime::RuntimeElementType::Float16 : runtime::RuntimeElementType::Float32;
  impl_->active_submission.emplace(impl_->backend->Run({.device_data = input.data_ptr(),
                                                        .capacity_bytes = static_cast<std::size_t>(input.numel() * input.element_size()),
                                                        .shape = shape,
                                                        .element_type = element_type},
                                                       request.annotations, {}, impl_->backend->has_masks()));
  impl_->preprocessor->record_consumer(impl_->stream);
  const auto& submission = *impl_->active_submission;
  return {
   .identity = request.identity,
   .terminal = runtime::AnalysisTerminal::Completed,
   .completed_ns = steady_now_ns(),
   .output_count = request.annotations.size(),
   .completion = {.device = submission.device(), .event = submission.completion_event(), .producer_stream = submission.stream().native_handle},
  };
 } catch (...) {
  impl_->active_submission.reset();
  impl_->ClearCounts();
  return {.identity = request.identity, .terminal = runtime::AnalysisTerminal::ExecutionFailure};
 }
}
bool RfdetrAnalysisProvider::ObserveCompletion(const runtime::AnalysisCompletion& completion) noexcept {
 if (!impl_->active_submission.has_value()) { return false; }
 const auto& submission = *impl_->active_submission;
 if (submission.device() != completion.device || submission.stream().native_handle != completion.producer_stream ||
     submission.completion_event() != completion.event) {
  return false;
 }
 try {
  auto settled = std::move(*impl_->active_submission);
  impl_->active_submission.reset();
  impl_->backend->ReleaseAfterCompletion(std::move(settled));
  for (auto& annotation : impl_->active_annotations) annotation.count.SettleAfterCompletion(annotation.value_capacity);
  impl_->active_annotations = {};
  return true;
 } catch (...) {
  impl_->active_submission.reset();
  impl_->ClearCounts();
  return false;
 }
}
void RfdetrAnalysisProvider::RetireIssuedWork(const ProviderWorkResult&) noexcept {
 if (!impl_->active_submission.has_value()) { return; }
 try {
  auto issued = std::move(*impl_->active_submission);
  impl_->active_submission.reset();
  impl_->backend->ReleaseAfterCompletion(std::move(issued));
 } catch (...) { impl_->active_submission.reset(); }
 impl_->ClearCounts();
}
void RfdetrAnalysisProvider::DoShutdown() noexcept {
 impl_->active_submission.reset();
 impl_->ClearCounts();
 impl_->preprocessor.reset();
 impl_->input_float.reset();
 impl_->backend.reset();
}
}  // namespace mmltk::backend::models::rfdetr
