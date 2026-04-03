#pragma once

#include "gui/view_state.h"

#include <cstdint>
#include <string>

namespace mmltk::gui {

enum class ModelInputBrowseRequest : std::uint8_t {
    None = 0,
    Weights = 1,
    Onnx = 2,
    TensorRt = 3,
};

[[nodiscard]] const char* compilation_mode_label(int index) noexcept;
[[nodiscard]] const char* model_input_label(ModelInputMode mode) noexcept;
[[nodiscard]] bool draw_compile_mode_combo(const char* label, int& index);
[[nodiscard]] ModelInputBrowseRequest draw_model_input_selector(
    ModelInputMode& mode,
    std::string& weights_path,
    std::string& onnx_path,
    std::string& tensorrt_path,
    bool weights_browse_busy,
    bool onnx_browse_busy,
    bool tensorrt_browse_busy,
    bool allow_none = false);

} // namespace mmltk::gui
