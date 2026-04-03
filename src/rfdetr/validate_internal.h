#pragma once

#include "mmltk/rfdetr/validate.h"

#include "rfdetr/cuda_utils.h"
#include "rfdetr/backends.h"
#include "rfdetr/evaluator.h"
#include "rfdetr/runtime.h"
#include "dataset_loader.h"

#include <c10/cuda/CUDAStream.h>
#include <torch/torch.h>

#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace mmltk::rfdetr {

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

inline DatasetLoader::Config make_inference_loader_config(const std::string& compiled_path,
                                                          size_t batch_size,
                                                          const std::string& cpu_affinity,
                                                          int device_id,
                                                          int prefetch_factor) {
    DatasetLoader::Config config;
    config.compiled_path = std::filesystem::absolute(std::filesystem::path(compiled_path)).string();
    config.batch_size = batch_size;
    config.shuffle = false;
    config.prefetch_factor = checked_prefetch_factor(static_cast<size_t>(prefetch_factor));
    config.gather_workers = 0;
    config.cpu_affinity = cpu_affinity;
    config.device_id = device_id;
    config.seed = 42;
    config.drop_last = true;
    return config;
}

inline DatasetLoader::Config make_loader_config(const ValidationOptions& options,
                                                const RuntimeContext& runtime) {
    return make_inference_loader_config(
        options.compiled_path.string(),
        1,
        runtime.loader_affinity_string(),
        options.device_id,
        checked_prefetch_factor(options.prefetch_factor));
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

ValidationBackendResult run_validation_backend(const ValidationOptions& options,
                                               const CocoDataset& source_dataset,
                                               InferenceBackend& backend);

} // namespace mmltk::rfdetr
