#pragma once

#include "mmltk/rfdetr/validate.h"

#include "rfdetr/gpu_augment.h"
#include "rfdetr/torch_cuda_utils.h"
#include "rfdetr/backends.h"
#include "rfdetr/evaluator.h"
#include "rfdetr/runtime.h"
#include "dataset_loader.h"

#include <c10/cuda/CUDAStream.h>
#include <torch/torch.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sstream>
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

inline DatasetLoader::Config make_inference_loader_config(const std::string& compiled_path, size_t batch_size,
                                                          const std::string& cpu_affinity, int device_id,
                                                          int prefetch_factor, int gather_workers) {
    DatasetLoader::Config config;
    config.compiled_path = std::filesystem::absolute(std::filesystem::path(compiled_path)).string();
    config.batch_size = batch_size;
    config.shuffle = false;
    config.prefetch_factor = checked_prefetch_factor(static_cast<size_t>(prefetch_factor));
    config.gather_workers = std::clamp(gather_workers, 1, config.prefetch_factor);
    config.cpu_affinity = cpu_affinity;
    config.device_id = device_id;
    config.seed = 42;
    config.drop_last = false;
    return config;
}

inline DatasetLoader::Config make_loader_config(const ValidationOptions& options, const RuntimeContext& runtime) {
    return make_inference_loader_config(options.compiled_path.string(), 1, runtime.loader_affinity_string(),
                                        options.device_id, checked_prefetch_factor(options.prefetch_factor),
                                        runtime.split().gather_threads);
}

inline c10::cuda::CUDAStream backend_cuda_stream(const InferenceBackend& backend, int device_id) {
    return c10::cuda::getStreamFromExternal(reinterpret_cast<cudaStream_t>(backend.stream()),
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

inline std::filesystem::path validation_profile_jsonl_path(const ValidationOptions& options) {
    const std::filesystem::path parent =
        options.report_json_path.empty() ? options.compiled_path.parent_path() : options.report_json_path.parent_path();
    return parent / "rfdetr-validation-profile.jsonl";
}

inline std::unique_ptr<EvaluationProfileRecord> make_standalone_evaluation_profile(const ValidationOptions& options,
                                                                                   const ModelInfo& model_info,
                                                                                   const CocoDataset& dataset) {
    if (!options.profile) {
        return nullptr;
    }
    auto profile = std::make_unique<EvaluationProfileRecord>();
    profile->jsonl_path = validation_profile_jsonl_path(options);
    if (!profile->jsonl_path.parent_path().empty()) {
        std::filesystem::create_directories(profile->jsonl_path.parent_path());
    }
    profile->event_source = "standalone_validation";
    profile->model_source = model_info.backend;
    profile->precision = model_info.input.dtype;
    profile->box_precision = "unknown";
    profile->expected_precision = model_info.input.dtype;
    capture_sdp_backend_flags(*profile);
    profile->metric_set = dataset.metric_set();
    profile->batch_size = options.batch_size;
    profile->image_height = model_info.input.shape.size() >= 4
                                ? static_cast<size_t>(std::max<int64_t>(0, model_info.input.shape[2]))
                                : static_cast<size_t>(options.resolution);
    profile->image_width = model_info.input.shape.size() >= 4
                               ? static_cast<size_t>(std::max<int64_t>(0, model_info.input.shape[3]))
                               : static_cast<size_t>(options.resolution);
    profile->query_count = static_cast<size_t>(std::max<int64_t>(0, model_info.num_queries));
    profile->class_count = static_cast<size_t>(std::max<int64_t>(0, model_info.num_classes));
    return profile;
}

inline nlohmann::json summary_to_json(const EvalSummary& summary) {
    return nlohmann::json{
        {"bbox_ap", summary.bbox.ap},
        {"bbox_ap50", summary.bbox.ap50},
        {"bbox_ap75", summary.bbox.ap75},
        {"mask_ap", summary.mask.has_value() ? nlohmann::json(summary.mask->ap) : nlohmann::json(nullptr)},
        {"mask_ap50", summary.mask.has_value() ? nlohmann::json(summary.mask->ap50) : nlohmann::json(nullptr)},
        {"mask_ap75", summary.mask.has_value() ? nlohmann::json(summary.mask->ap75) : nlohmann::json(nullptr)},
    };
}

inline nlohmann::json timing_to_json(const PhaseTiming& timing) {
    return nlohmann::json{
        {"seconds", timing.seconds},
        {"images", timing.images},
        {"img_per_s", timing.img_per_s},
    };
}

inline std::string format_validation_summary_line(const std::string& backend_name, const EvalSummary& summary) {
    std::ostringstream line;
    line.setf(std::ios::fixed);
    line.precision(4);
    line << backend_name << ": bbox_ap=" << summary.bbox.ap << " bbox_ap50=" << summary.bbox.ap50 << " mask_ap=";
    if (summary.mask.has_value()) {
        line << summary.mask->ap << " mask_ap50=" << summary.mask->ap50;
    } else {
        line << "null mask_ap50=null";
    }
    return line.str();
}

inline std::string format_validation_delta_summary_line(const ValidationDeltaSummary& delta) {
    std::ostringstream line;
    line.setf(std::ios::fixed);
    line.precision(4);
    line << "delta(tensorrt-onnx): bbox_ap=" << delta.bbox_ap << " bbox_ap50=" << delta.bbox_ap50 << " mask_ap=";
    if (delta.mask_ap.has_value() && delta.mask_ap50.has_value()) {
        line << *delta.mask_ap << " mask_ap50=" << *delta.mask_ap50;
    } else {
        line << "null mask_ap50=null";
    }
    return line.str();
}

}  

ValidationBackendResult run_backend_eval_parallel_impl(const ValidationOptions& options, const CocoDataset& dataset,
                                                       const InferenceBackend& backend);

ValidationBackendResult run_validation_backend(const ValidationOptions& options, const CocoDataset& source_dataset,
                                               InferenceBackend& backend);

}  
