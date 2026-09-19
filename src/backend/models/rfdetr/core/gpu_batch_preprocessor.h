#pragma once
#include <cstdint>
#include <memory>
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include <cuda_runtime_api.h>
#include <torch/types.h>
#include "src/backend/data/dataset_loader.h"
namespace mmltk::backend::models::rfdetr {
class GpuBatchPreprocessor final {
   public:
    GpuBatchPreprocessor(std::int64_t batch_capacity, int height, int width, int device_id, at::ScalarType output_type);
    ~GpuBatchPreprocessor();
    GpuBatchPreprocessor(const GpuBatchPreprocessor&) = delete;
    GpuBatchPreprocessor& operator=(const GpuBatchPreprocessor&) = delete;
    [[nodiscard]] torch::Tensor run(const mmltk::backend::data::Batch& batch, std::int64_t output_batch_size = 0);
    void record_consumer(cudaStream_t stream);
    [[nodiscard]] inline std::int64_t batch_capacity() const noexcept { return batch_capacity_; }
    [[nodiscard]] inline at::ScalarType output_type() const noexcept { return output_type_; }

   private:
    struct Resources final {
        torch::Tensor output;
        cudaEvent_t consumer_complete = nullptr;
    };
    void Check(cudaError_t status, const char* operation);
    mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_{1U};
    mmltk::frameworks::gpu::TerminalCudaRetirementLease lease_{mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement_)};
    std::shared_ptr<Resources> resources_ = std::make_shared<Resources>();
    std::int64_t batch_capacity_ = 0;
    int height_ = 0;
    int width_ = 0;
    int device_id_ = -1;
    at::ScalarType output_type_ = at::kFloat;
    cudaStream_t producer_stream_ = nullptr;
    bool consumer_pending_ = false;
    bool has_run_ = false;
};
}
