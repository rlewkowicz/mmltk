#pragma once

#include "mmltk/rfdetr/checkpoint.h"

#include "rfdetr/backends.h"

#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

inline const std::filesystem::path& model_input_path(const ResolvedModelArtifacts& artifacts) {
    if (!artifacts.input_path.empty()) {
        return artifacts.input_path;
    }
    return artifacts.weights_path;
}

inline void validate_backend_input_resolution(const InferenceBackend& backend, const int expected_resolution) {
    const std::vector<std::int64_t>& shape = backend.info().input.shape;
    if (expected_resolution <= 0 || shape.size() < 4U) {
        return;
    }
    const std::int64_t height = shape[shape.size() - 2U];
    const std::int64_t width = shape.back();
    if ((height > 0 && height != expected_resolution) || (width > 0 && width != expected_resolution)) {
        throw std::runtime_error("RF-DETR backend static input shape does not match the selected/compiled resolution");
    }
}

inline std::string choose_backend_name(const std::string& requested_backend, const ResolvedModelArtifacts& artifacts) {
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

    throw std::runtime_error("RF-DETR backend " + requested_backend + " is unavailable for " +
                             model_input_path(artifacts).string());
}

inline std::filesystem::path default_backend_input_path(const std::filesystem::path& onnx_path,
                                                        const std::filesystem::path& tensorrt_path) {
    return onnx_path.empty() ? tensorrt_path : onnx_path;
}

struct InferenceBackendFactoryContext {
    std::filesystem::path input_path;
    std::filesystem::path onnx_path;
    std::filesystem::path tensorrt_path;
    std::filesystem::path save_engine_path;
    int device_id = 0;
    bool allow_fp16 = true;

    [[nodiscard]] std::filesystem::path onnx_model_path() const {
        if (onnx_path.empty()) {
            throw std::runtime_error("RF-DETR ONNX artifact is unavailable for " + input_path.string());
        }
        return onnx_path;
    }

    [[nodiscard]] std::filesystem::path tensorrt_model_path() const {
        if (!tensorrt_path.empty()) {
            return tensorrt_path;
        }
        if (!onnx_path.empty()) {
            return onnx_path;
        }
        throw std::runtime_error("RF-DETR TensorRT requires an engine or ONNX artifact for " + input_path.string());
    }
};

class InferenceBackendFamilyFactory {
   public:
    virtual ~InferenceBackendFamilyFactory() = default;

    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<InferenceBackend> make_backend(
        const InferenceBackendFactoryContext& context) const = 0;
    [[nodiscard]] virtual std::vector<std::unique_ptr<InferenceBackend>> make_backend_lanes(
        const InferenceBackendFactoryContext& context, int lane_count) const = 0;
};

class OnnxInferenceBackendFamilyFactory final : public InferenceBackendFamilyFactory {
   public:
    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "onnx";
    }

    [[nodiscard]] std::unique_ptr<InferenceBackend> make_backend(
        const InferenceBackendFactoryContext& context) const override {
        return make_onnx_backend(context.onnx_model_path(), context.device_id);
    }

    [[nodiscard]] std::vector<std::unique_ptr<InferenceBackend>> make_backend_lanes(
        const InferenceBackendFactoryContext& context, const int lane_count) const override {
        return make_onnx_backend_lanes(context.onnx_model_path(), context.device_id, lane_count);
    }
};

class TensorRtInferenceBackendFamilyFactory final : public InferenceBackendFamilyFactory {
   public:
    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "tensorrt";
    }

    [[nodiscard]] std::unique_ptr<InferenceBackend> make_backend(
        const InferenceBackendFactoryContext& context) const override {
        return make_tensorrt_backend(context.tensorrt_model_path(), context.device_id, context.allow_fp16,
                                     context.save_engine_path);
    }

    [[nodiscard]] std::vector<std::unique_ptr<InferenceBackend>> make_backend_lanes(
        const InferenceBackendFactoryContext& context, const int lane_count) const override {
        return make_tensorrt_backend_lanes(context.tensorrt_model_path(), context.device_id, context.allow_fp16,
                                           lane_count, context.save_engine_path);
    }
};

inline const std::array<const InferenceBackendFamilyFactory*, 2>& inference_backend_family_factories() {
    static const OnnxInferenceBackendFamilyFactory onnx_factory;
    static const TensorRtInferenceBackendFamilyFactory tensorrt_factory;
    static const std::array<const InferenceBackendFamilyFactory*, 2> factories{{
        &onnx_factory,
        &tensorrt_factory,
    }};
    return factories;
}

inline const InferenceBackendFamilyFactory& inference_backend_family_factory(const std::string_view backend_name) {
    for (const InferenceBackendFamilyFactory* factory : inference_backend_family_factories()) {
        if (factory->backend_name() == backend_name) {
            return *factory;
        }
    }
    throw std::runtime_error("unsupported RF-DETR backend: " + std::string(backend_name));
}

class InferenceBackendFactory {
   public:
    InferenceBackendFactory(const ResolvedModelArtifacts& artifacts, int device_id, bool allow_fp16,
                            std::filesystem::path save_engine_path = {})
        : context_{
              model_input_path(artifacts), artifacts.onnx_path, artifacts.tensorrt_path,
              std::move(save_engine_path), device_id,           allow_fp16,
          } {}

    InferenceBackendFactory(std::filesystem::path onnx_path, std::filesystem::path tensorrt_path, int device_id,
                            bool allow_fp16, std::filesystem::path save_engine_path = {})
        : context_{
              default_backend_input_path(onnx_path, tensorrt_path),
              std::move(onnx_path),
              std::move(tensorrt_path),
              std::move(save_engine_path),
              device_id,
              allow_fp16,
          } {}

    [[nodiscard]] std::unique_ptr<InferenceBackend> make_backend(const std::string_view backend_name) const {
        return inference_backend_family_factory(backend_name).make_backend(context_);
    }

    [[nodiscard]] std::vector<std::unique_ptr<InferenceBackend>> make_backend_lanes(const std::string_view backend_name,
                                                                                    const int lane_count) const {
        return inference_backend_family_factory(backend_name).make_backend_lanes(context_, lane_count);
    }

   private:
    InferenceBackendFactoryContext context_;
};

inline std::unique_ptr<InferenceBackend> make_backend(const ResolvedModelArtifacts& artifacts,
                                                      const std::string& backend_name, int device_id, bool allow_fp16,
                                                      std::filesystem::path save_engine_path = {}) {
    InferenceBackendFactory factory(artifacts, device_id, allow_fp16, std::move(save_engine_path));
    std::unique_ptr<InferenceBackend> backend = factory.make_backend(backend_name);
    validate_backend_input_resolution(*backend, artifacts.config.resolution);
    return backend;
}

inline std::vector<std::unique_ptr<InferenceBackend>> make_backend_lanes(const ResolvedModelArtifacts& artifacts,
                                                                         const std::string& backend_name, int device_id,
                                                                         bool allow_fp16, int lane_count,
                                                                         std::filesystem::path save_engine_path = {}) {
    InferenceBackendFactory factory(artifacts, device_id, allow_fp16, std::move(save_engine_path));
    std::vector<std::unique_ptr<InferenceBackend>> lanes = factory.make_backend_lanes(backend_name, lane_count);
    for (const std::unique_ptr<InferenceBackend>& lane : lanes) {
        validate_backend_input_resolution(*lane, artifacts.config.resolution);
    }
    return lanes;
}

inline std::unique_ptr<InferenceBackend> make_backend(std::filesystem::path onnx_path,
                                                      std::filesystem::path tensorrt_path,
                                                      const std::string& backend_name, int device_id, bool allow_fp16,
                                                      std::filesystem::path save_engine_path = {}) {
    return InferenceBackendFactory(std::move(onnx_path), std::move(tensorrt_path), device_id, allow_fp16,
                                   std::move(save_engine_path))
        .make_backend(backend_name);
}

inline std::vector<std::unique_ptr<InferenceBackend>> make_backend_lanes(std::filesystem::path onnx_path,
                                                                         std::filesystem::path tensorrt_path,
                                                                         const std::string& backend_name, int device_id,
                                                                         bool allow_fp16, int lane_count,
                                                                         std::filesystem::path save_engine_path = {}) {
    return InferenceBackendFactory(std::move(onnx_path), std::move(tensorrt_path), device_id, allow_fp16,
                                   std::move(save_engine_path))
        .make_backend_lanes(backend_name, lane_count);
}

}  
