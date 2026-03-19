#pragma once

#include "rfdetr/validate.h"

#include "rfdetr/cuda_utils.h"
#include "dataset_loader.h"

#include <c10/cuda/CUDAStream.h>
#include <torch/torch.h>

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>

namespace fastloader::rfdetr {

namespace validate_detail {

inline bool interactive_logs(const ValidationOptions& options) {
    return options.log_mode == ValidationLogMode::Interactive;
}

inline int checked_prefetch_factor(size_t value) {
    if (value == 0 || value > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("native RF-DETR validation prefetch_factor exceeds loader range");
    }
    return static_cast<int>(value);
}

inline torch::Tensor normalize_batch(const Batch& batch,
                                     int device_id,
                                     int64_t image_height,
                                     int64_t image_width,
                                     const torch::Tensor& mean,
                                     const torch::Tensor& std) {
    return (torch::from_blob(
                const_cast<float*>(batch.device_images),
                {static_cast<int64_t>(batch.num_images), 3, image_height, image_width},
                torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device_id)) -
            mean) /
           std;
}

inline DatasetLoader::Config make_loader_config(const ValidationOptions& options,
                                                const RuntimeContext& runtime) {
    DatasetLoader::Config config;
    config.compiled_path = options.compiled_path.string();
    config.batch_size = 1;
    config.shuffle = false;
    config.prefetch_factor = checked_prefetch_factor(options.prefetch_factor);
    config.gather_workers = 0;
    config.cpu_affinity = runtime.loader_affinity_string();
    config.device_id = options.device_id;
    return config;
}

inline c10::cuda::CUDAStream backend_cuda_stream(const InferenceBackend& backend, int device_id) {
    return c10::cuda::getStreamFromExternal(
        reinterpret_cast<cudaStream_t>(backend.stream()),
        checked_device_index(device_id));
}

inline PhaseTiming elapsed_timing(const std::chrono::steady_clock::time_point& start, size_t images) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return PhaseTiming{
        seconds,
        seconds > 0.0 ? static_cast<double>(images) / seconds : 0.0,
        images,
    };
}

} // namespace validate_detail

// Parallel validation implementation (defined in validate_workers.cpp).
ValidationBackendResult run_backend_eval_parallel_impl(
    const ValidationOptions& options,
    const CocoDataset& source_dataset,
    const std::string& backend_name,
    const ModelInfo& model_info,
    const torch::Tensor& mean,
    const torch::Tensor& std);

} // namespace fastloader::rfdetr
