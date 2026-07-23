#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace mmltk::model {

enum class ModelArtifactInputKind : std::uint8_t {
    None = 0,
    Weights = 1,
    Onnx = 2,
    TensorRt = 3,
};

struct ModelArtifactRequest {
    std::filesystem::path weights_path;
    std::filesystem::path onnx_path;
    std::filesystem::path tensorrt_path;
    std::string preset_name;
    int resolution = 0;

    [[nodiscard]] size_t selected_input_count() const noexcept {
        return static_cast<size_t>(!weights_path.empty()) + static_cast<size_t>(!onnx_path.empty()) +
               static_cast<size_t>(!tensorrt_path.empty());
    }

    [[nodiscard]] ModelArtifactInputKind selected_input_kind() const noexcept {
        if (!weights_path.empty()) {
            return ModelArtifactInputKind::Weights;
        }
        if (!onnx_path.empty()) {
            return ModelArtifactInputKind::Onnx;
        }
        if (!tensorrt_path.empty()) {
            return ModelArtifactInputKind::TensorRt;
        }
        return ModelArtifactInputKind::None;
    }
};

struct ResolvedModelArtifacts {
    std::string module_id;
    std::string preset_name;
    std::string input_kind;
    std::filesystem::path input_path;
    std::filesystem::path weights_path;
    std::filesystem::path artifact_root;
    std::filesystem::path onnx_path;
    std::filesystem::path tensorrt_path;
};

[[nodiscard]] inline const char* to_string(const ModelArtifactInputKind kind) noexcept {
    switch (kind) {
        case ModelArtifactInputKind::None:
            return "none";
        case ModelArtifactInputKind::Weights:
            return "weights";
        case ModelArtifactInputKind::Onnx:
            return "onnx";
        case ModelArtifactInputKind::TensorRt:
            return "tensorrt";
    }
    return "unknown";
}

}  
