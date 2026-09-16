#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::backend::models::catalog {
enum class ModelArtifactInputKind : std::uint8_t {
    Weights = 0,
    Onnx = 1,
    TensorRt = 2,
    None = 3,
};
MMLTK_REFLECT_ENUM(ModelArtifactInputKind)
[[nodiscard]] constexpr std::optional<std::string_view> artifact_kind_name(const ModelArtifactInputKind kind) noexcept {
    switch (kind) {
        case ModelArtifactInputKind::Weights: return "weights";
        case ModelArtifactInputKind::Onnx: return "onnx";
        case ModelArtifactInputKind::TensorRt: return "tensorrt";
        case ModelArtifactInputKind::None: return "none";
    }
    return std::nullopt;
}
}  // namespace mmltk::backend::models::catalog
