#pragma once

#include "mmltk/rfdetr/checkpoint.h"

#include "rfdetr/backends.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

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

class InferenceBackendFactory {
public:
    InferenceBackendFactory(const ResolvedModelArtifacts& artifacts,
                            int device_id,
                            bool allow_fp16,
                            std::filesystem::path save_engine_path = {})
        : input_path_(model_input_path(artifacts)),
          onnx_path_(artifacts.onnx_path),
          tensorrt_path_(artifacts.tensorrt_path),
          save_engine_path_(std::move(save_engine_path)),
          device_id_(device_id),
          allow_fp16_(allow_fp16) {}

    InferenceBackendFactory(std::filesystem::path onnx_path,
                            std::filesystem::path tensorrt_path,
                            int device_id,
                            bool allow_fp16,
                            std::filesystem::path save_engine_path = {})
        : input_path_(onnx_path.empty() ? tensorrt_path : onnx_path),
          onnx_path_(std::move(onnx_path)),
          tensorrt_path_(std::move(tensorrt_path)),
          save_engine_path_(std::move(save_engine_path)),
          device_id_(device_id),
          allow_fp16_(allow_fp16) {}

    [[nodiscard]] std::unique_ptr<InferenceBackend> make_backend(const std::string& backend_name) const {
        const std::filesystem::path model_path = backend_model_path(backend_name);
        if (backend_name == "onnx") {
            return make_onnx_backend(model_path, device_id_);
        }
        if (backend_name == "tensorrt") {
            return make_tensorrt_backend(model_path, device_id_, allow_fp16_, save_engine_path_);
        }
        throw std::runtime_error("unsupported RF-DETR backend: " + backend_name);
    }

    [[nodiscard]] std::vector<std::unique_ptr<InferenceBackend>> make_backend_lanes(
        const std::string& backend_name,
        int lane_count) const {
        const std::filesystem::path model_path = backend_model_path(backend_name);
        if (backend_name == "onnx") {
            return make_onnx_backend_lanes(model_path, device_id_, lane_count);
        }
        if (backend_name == "tensorrt") {
            return make_tensorrt_backend_lanes(model_path, device_id_, allow_fp16_, lane_count, save_engine_path_);
        }
        throw std::runtime_error("unsupported RF-DETR backend: " + backend_name);
    }

private:
    [[nodiscard]] std::filesystem::path backend_model_path(const std::string& backend_name) const {
        if (backend_name == "onnx") {
            if (onnx_path_.empty()) {
                throw std::runtime_error("RF-DETR ONNX artifact is unavailable for " + input_path_.string());
            }
            return onnx_path_;
        }
        if (backend_name == "tensorrt") {
            if (!tensorrt_path_.empty()) {
                return tensorrt_path_;
            }
            if (!onnx_path_.empty()) {
                return onnx_path_;
            }
            throw std::runtime_error(
                "RF-DETR TensorRT requires an engine or ONNX artifact for " + input_path_.string());
        }
        throw std::runtime_error("unsupported RF-DETR backend: " + backend_name);
    }

    std::filesystem::path input_path_;
    std::filesystem::path onnx_path_;
    std::filesystem::path tensorrt_path_;
    std::filesystem::path save_engine_path_;
    int device_id_ = 0;
    bool allow_fp16_ = true;
};

inline std::unique_ptr<InferenceBackend> make_backend(const ResolvedModelArtifacts& artifacts,
                                                      const std::string& backend_name,
                                                      int device_id,
                                                      bool allow_fp16,
                                                      std::filesystem::path save_engine_path = {}) {
    return InferenceBackendFactory(artifacts, device_id, allow_fp16, std::move(save_engine_path)).make_backend(
        backend_name);
}

inline std::vector<std::unique_ptr<InferenceBackend>> make_backend_lanes(
    const ResolvedModelArtifacts& artifacts,
    const std::string& backend_name,
    int device_id,
    bool allow_fp16,
    int lane_count,
    std::filesystem::path save_engine_path = {}) {
    return InferenceBackendFactory(artifacts, device_id, allow_fp16, std::move(save_engine_path)).make_backend_lanes(
        backend_name,
        lane_count);
}

inline std::unique_ptr<InferenceBackend> make_backend(std::filesystem::path onnx_path,
                                                      std::filesystem::path tensorrt_path,
                                                      const std::string& backend_name,
                                                      int device_id,
                                                      bool allow_fp16,
                                                      std::filesystem::path save_engine_path = {}) {
    return InferenceBackendFactory(std::move(onnx_path),
                                   std::move(tensorrt_path),
                                   device_id,
                                   allow_fp16,
                                   std::move(save_engine_path))
        .make_backend(backend_name);
}

inline std::vector<std::unique_ptr<InferenceBackend>> make_backend_lanes(
    std::filesystem::path onnx_path,
    std::filesystem::path tensorrt_path,
    const std::string& backend_name,
    int device_id,
    bool allow_fp16,
    int lane_count,
    std::filesystem::path save_engine_path = {}) {
    return InferenceBackendFactory(std::move(onnx_path),
                                   std::move(tensorrt_path),
                                   device_id,
                                   allow_fp16,
                                   std::move(save_engine_path))
        .make_backend_lanes(backend_name, lane_count);
}

} // namespace mmltk::rfdetr
