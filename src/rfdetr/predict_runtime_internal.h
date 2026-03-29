#pragma once

#include "fastloader/rfdetr/model.h"
#include "fastloader/rfdetr/target_builder.h"
#include "fastloader/rfdetr/checkpoint.h"

#include "rfdetr/backends.h"
#include "rfdetr/common/tensor_utils.h"
#include "rfdetr/cuda_utils.h"
#include "rfdetr/postprocess.h"

#include <c10/cuda/CUDAStream.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fastloader::rfdetr::predict_internal {

inline const std::filesystem::path& model_input_path(const ResolvedModelArtifacts& artifacts) {
    if (!artifacts.input_path.empty()) {
        return artifacts.input_path;
    }
    return artifacts.weights_path;
}

inline std::string choose_backend_name(const std::string& requested_backend,
                                       const ResolvedModelArtifacts& artifacts) {
    if (!artifacts.weights_path.empty()) {
        if (requested_backend != "auto") {
            throw std::runtime_error("--backend is only valid with explicit --onnx or --tensorrt inputs");
        }
        return "weights";
    }
    if (requested_backend == "auto") {
        if (!artifacts.tensorrt_path.empty()) {
            return "tensorrt";
        }
        if (!artifacts.onnx_path.empty()) {
            return "onnx";
        }
    } else if (requested_backend == "tensorrt") {
        if (!artifacts.tensorrt_path.empty() || !artifacts.onnx_path.empty()) {
            return requested_backend;
        }
    } else if (requested_backend == "onnx") {
        if (!artifacts.onnx_path.empty()) {
            return requested_backend;
        }
    } else {
        throw std::runtime_error("unsupported RF-DETR backend: " + requested_backend);
    }

    throw std::runtime_error(
        "RF-DETR backend " + requested_backend + " is unavailable for " + model_input_path(artifacts).string());
}

inline std::filesystem::path backend_model_path(const ResolvedModelArtifacts& artifacts,
                                                const std::string& backend_name) {
    if (backend_name == "onnx") {
        if (artifacts.onnx_path.empty()) {
            throw std::runtime_error("RF-DETR ONNX artifact is unavailable for " + model_input_path(artifacts).string());
        }
        return artifacts.onnx_path;
    }
    if (backend_name == "tensorrt") {
        if (!artifacts.tensorrt_path.empty()) {
            return artifacts.tensorrt_path;
        }
        if (!artifacts.onnx_path.empty()) {
            return artifacts.onnx_path;
        }
        throw std::runtime_error(
            "RF-DETR TensorRT requires an engine or ONNX artifact for " + model_input_path(artifacts).string());
    }
    throw std::runtime_error("unsupported RF-DETR backend: " + backend_name);
}

inline std::unique_ptr<InferenceBackend> make_backend(const ResolvedModelArtifacts& artifacts,
                                                      const std::string& backend_name,
                                                      int device_id,
                                                      bool allow_fp16) {
    const auto model_path = backend_model_path(artifacts, backend_name);
    if (backend_name == "onnx") {
        return make_onnx_backend(model_path, device_id);
    }
    if (backend_name == "tensorrt") {
        return make_tensorrt_backend(model_path, device_id, allow_fp16);
    }
    throw std::runtime_error("unsupported RF-DETR backend: " + backend_name);
}

inline std::vector<std::unique_ptr<InferenceBackend>> make_backend_lanes(
    const ResolvedModelArtifacts& artifacts,
    const std::string& backend_name,
    int device_id,
    bool allow_fp16,
    int lane_count) {
    const auto model_path = backend_model_path(artifacts, backend_name);
    if (backend_name == "onnx") {
        return make_onnx_backend_lanes(model_path, device_id, lane_count);
    }
    if (backend_name == "tensorrt") {
        return make_tensorrt_backend_lanes(model_path, device_id, allow_fp16, lane_count);
    }
    throw std::runtime_error("unsupported RF-DETR backend: " + backend_name);
}

inline std::shared_ptr<NativeRfDetrModel> load_native_model(const ResolvedModelArtifacts& artifacts, int device_id) {
    auto model = std::make_shared<NativeRfDetrModel>(artifacts.config);
    const StateDictLoadSummary load_summary = model->load_weights(artifacts.weights_path, false);
    std::fprintf(stderr,
                 "rfdetr weights: loaded=%zu missing=%zu unexpected=%zu incompatible=%zu input=%s\n",
                 load_summary.loaded_names.size(),
                 load_summary.missing_names.size(),
                 load_summary.unexpected_names.size(),
                 load_summary.incompatible_names.size(),
                 artifacts.weights_path.c_str());
    model->eval();
    model->to(cuda_device(device_id));
    return model;
}

inline OutputTensors to_output_tensors(const ModelOutputs& outputs) {
    return OutputTensors{
        outputs.main.pred_logits,
        outputs.main.pred_boxes,
        outputs.main.pred_masks,
    };
}

inline std::pair<torch::Tensor, torch::Tensor> initialize_normalization_tensors(
    int device_id,
    const std::optional<c10::cuda::CUDAStream>& stream = std::nullopt) {
    if (!stream.has_value()) {
        return make_normalization_tensors(device_id);
    }
    c10::cuda::CUDAStreamGuard stream_guard(*stream);
    return make_normalization_tensors(device_id);
}

} // namespace fastloader::rfdetr::predict_internal
