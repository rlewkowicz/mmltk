#include "gpu_batch_preprocessor.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment_cuda.h"
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/frameworks/gpu/cuda_error.h"
#include <stdexcept>
#include <string>
#include <string_view>
namespace mmltk::backend::models::rfdetr {
using mmltk::backend::ml::cuda::checked_device_index;
using mmltk::backend::ml::cuda::current_torch_cuda_stream_object;
using mmltk::backend::ml::cuda::TorchCudaDeviceGuard;
using mmltk::frameworks::gpu::ensure_cuda_ok;
namespace {
void require(bool condition, std::string_view message) { if (!condition) throw std::invalid_argument(std::string(message)); }
GpuPreprocessOutputType preprocess_output_type(const at::ScalarType output_type) {
    switch (output_type) {
        case at::kFloat: return GpuPreprocessOutputType::Float32;
        case at::kHalf: return GpuPreprocessOutputType::Float16;
        case torch::kBFloat16: return GpuPreprocessOutputType::BFloat16;
        default:
            require(false,
                    "GPU batch preprocessing supports only FP32, FP16, and "
                    "BF16 output");
    }
    return GpuPreprocessOutputType::Float32;
}
}
GpuBatchPreprocessor::GpuBatchPreprocessor(const std::int64_t batch_capacity, const int height, const int width, const int device_id,
                                           const at::ScalarType output_type)
    : batch_capacity_(batch_capacity), height_(height), width_(width), device_id_(device_id), output_type_(output_type) {
    require(batch_capacity_ > 0 && height_ > 0 && width_ > 0, "invalid GPU preprocessing tensor shape");
    (void)preprocess_output_type(output_type_);
    TorchCudaDeviceGuard device_guard(checked_device_index(device_id_));
    resources_->output = torch::empty({batch_capacity_, 3, height_, width_},
                           torch::TensorOptions().dtype(output_type_).device(mmltk::backend::ml::cuda::cuda_device(device_id_)));
    ensure_cuda_ok(cudaEventCreateWithFlags(&resources_->consumer_complete, cudaEventDisableTiming), "cudaEventCreateWithFlags for GPU preprocessing consumer");
}
void GpuBatchPreprocessor::Check(cudaError_t status, const char* operation) {
    if (status != cudaSuccess && resources_)
        std::move(lease_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(resources_)), status);
    ensure_cuda_ok(status, operation);
}
GpuBatchPreprocessor::~GpuBatchPreprocessor() {
    if (!resources_) return;
    int previous = -1;
    auto status = cudaGetDevice(&previous);
    if (status == cudaSuccess && previous != device_id_) status = cudaSetDevice(device_id_);
    if (status == cudaSuccess && has_run_)
        status = consumer_pending_ ? cudaEventSynchronize(resources_->consumer_complete) : cudaStreamSynchronize(producer_stream_);
    if (status == cudaSuccess && resources_->consumer_complete) status = cudaEventDestroy(resources_->consumer_complete);
    if (status != cudaSuccess)
        std::move(lease_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(resources_)), status);
    if (previous >= 0 && previous != device_id_) static_cast<void>(cudaSetDevice(previous));
}
torch::Tensor GpuBatchPreprocessor::run(const mmltk::backend::data::Batch& batch, std::int64_t output_batch_size) {
    require(resources_ != nullptr, "GPU preprocessing has terminal custody");
    const auto active_batch_size = static_cast<std::int64_t>(batch.num_images);
    if (output_batch_size == 0) { output_batch_size = active_batch_size; }
    require(active_batch_size > 0 && active_batch_size <= output_batch_size, "GPU preprocessing requires a non-empty active batch within the output batch");
    require(output_batch_size <= batch_capacity_, "GPU preprocessing batch exceeds preallocated capacity");
    require(batch.device_images != nullptr, "GPU preprocessing requires loader device images");
    const auto device_index = checked_device_index(device_id_);
    TorchCudaDeviceGuard device_guard(device_index);
    const cudaStream_t stream = current_torch_cuda_stream_object(device_index).stream();
    if (consumer_pending_) {
        Check(cudaStreamWaitEvent(stream, resources_->consumer_complete, 0), "cudaStreamWaitEvent for GPU preprocessing buffer reuse");
        consumer_pending_ = false;
    }
    producer_stream_ = stream;
    has_run_ = true;
    normalize_gpu_batch(batch.device_images, resources_->output.data_ptr(), active_batch_size, output_batch_size, height_, width_, preprocess_output_type(output_type_),
                        stream);
    return output_batch_size == batch_capacity_ ? resources_->output : resources_->output.narrow(0, 0, output_batch_size);
}
void GpuBatchPreprocessor::record_consumer(cudaStream_t stream) {
    require(resources_ != nullptr, "GPU preprocessing has terminal custody");
    TorchCudaDeviceGuard device_guard(checked_device_index(device_id_));
    Check(cudaEventRecord(resources_->consumer_complete, stream), "cudaEventRecord for GPU preprocessing consumer");
    consumer_pending_ = true;
}
}
