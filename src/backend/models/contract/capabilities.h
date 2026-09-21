#pragma once
namespace mmltk::backend::models {
struct ModelCapabilities final {
 bool weights = false;
 bool onnx = false;
 bool tensorrt = false;
 bool training = false;
 bool live = false;
 constexpr bool operator==(const ModelCapabilities&) const noexcept = default;
};
}  // namespace mmltk::backend::models
