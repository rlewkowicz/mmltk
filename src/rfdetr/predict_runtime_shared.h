#pragma once

#include "rfdetr/backends.h"
#include "rfdetr/gpu_augment.h"
#include "rfdetr/torch_cuda_utils.h"
#include "rfdetr/evaluator.h"

#include <c10/cuda/CUDAStream.h>

#include "rfdetr/runtime.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace mmltk::rfdetr::predict_internal {

struct BackendExecutionSpec {
    c10::cuda::CUDAStream stream = c10::cuda::getDefaultCUDAStream();
    std::int64_t num_queries = 300;
    std::size_t category_count = 0;
};

struct BackendInferenceLane {
    std::unique_ptr<InferenceBackend> backend;
    std::shared_ptr<PredictionBufferSlotPool> slot_pool;
    std::unique_ptr<GpuBatchPreprocessor> preprocessor;
    c10::cuda::CUDAStream stream = c10::cuda::getDefaultCUDAStream();
    std::int64_t num_queries = 300;
    std::size_t category_count = 0;
};

inline std::size_t prediction_lane_slot_count(const RuntimeSplit& split, std::size_t batch_size = 1) {
    const std::size_t lane_threads = static_cast<std::size_t>(std::max(1, split.lane_threads));
    const std::size_t cpu_threads = static_cast<std::size_t>(std::max(1, split.cpu_threads));
    const std::size_t images_per_wave = lane_threads * std::max<std::size_t>(1, batch_size);
    return std::max<std::size_t>(2, 1 + ((cpu_threads + images_per_wave - 1) / images_per_wave));
}

inline std::size_t prediction_cpu_batch_limit(const RuntimeSplit& split, std::size_t batch_size) {
    const std::size_t cpu_threads = static_cast<std::size_t>(std::max(1, split.cpu_threads));
    const std::size_t images_per_batch = std::max<std::size_t>(1, batch_size);
    return std::max<std::size_t>(2, (cpu_threads + images_per_batch - 1) / images_per_batch);
}

inline int effective_lane_count(const std::size_t batch_size, const int lanes) {
    return std::max(1, lanes > 0 ? lanes : static_cast<int>(std::max<std::size_t>(1, batch_size)));
}

inline BackendExecutionSpec describe_backend_execution(const InferenceBackend& backend, const int device_id,
                                                       const std::size_t fallback_category_count = 0) {
    const ModelInfo& info = backend.info();
    return BackendExecutionSpec{
        c10::cuda::getStreamFromExternal(reinterpret_cast<cudaStream_t>(backend.stream()),
                                         checked_device_index(device_id)),
        info.num_queries > 0 ? info.num_queries : 300,
        info.num_classes > 0 ? static_cast<std::size_t>(info.num_classes) : fallback_category_count,
    };
}

inline std::vector<BackendInferenceLane> make_backend_inference_lanes(
    std::vector<std::unique_ptr<InferenceBackend>>&& backends, const int device_id, const std::size_t slot_count,
    const std::size_t fallback_category_count, const int64_t batch_capacity,
    const std::optional<std::pair<uint32_t, uint32_t>>& mask_shape = std::nullopt,
    const bool allow_mask_reconfiguration = false) {
    std::vector<BackendInferenceLane> lanes;
    lanes.reserve(backends.size());
    for (auto& backend : backends) {
        BackendExecutionSpec spec = describe_backend_execution(*backend, device_id, fallback_category_count);
        lanes.push_back(BackendInferenceLane{
            std::move(backend),
            std::make_shared<PredictionBufferSlotPool>(slot_count,
                                                       PredictionBufferConfig{
                                                           batch_capacity,
                                                           spec.num_queries,
                                                           mask_shape,
                                                           device_id,
                                                           allow_mask_reconfiguration,
                                                       }),
            nullptr,
            spec.stream,
            spec.num_queries,
            spec.category_count,
        });
    }
    return lanes;
}

}  
