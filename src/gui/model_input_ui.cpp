#include "gui/model_input_ui.h"

#include "gui/file_picker.h"
#include "gui/ui_controls.h"

#include <imgui.h>

namespace mmltk::gui {

const char* compilation_mode_label(const int index) noexcept {
    switch (index) {
    case 0:
        return "None";
    case 1:
        return "Selective";
    case 2:
        return "Full";
    default:
        return "Unknown";
    }
}

const char* model_input_label(const ModelInputMode mode) noexcept {
    switch (mode) {
    case ModelInputMode::Weights:
        return "Weights";
    case ModelInputMode::Onnx:
        return "ONNX";
    case ModelInputMode::TensorRt:
        return "TensorRT";
    case ModelInputMode::None:
        return "None";
    }
    return "Unknown";
}

bool draw_compile_mode_combo(const char* label, int& index) {
    bool changed = false;
    draw_labeled_combo(label, compilation_mode_label(index), 180.0f, [&index, &changed]() {
        for (int option = 0; option < 3; ++option) {
            const bool selected = option == index;
            if (ImGui::Selectable(compilation_mode_label(option), selected)) {
                index = option;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
    });
    return changed;
}

ModelInputBrowseRequest draw_model_input_selector(ModelInputMode& mode,
                                                  std::string& weights_path,
                                                  std::string& onnx_path,
                                                  std::string& tensorrt_path,
                                                  const bool weights_browse_busy,
                                                  const bool onnx_browse_busy,
                                                  const bool tensorrt_browse_busy,
                                                  const bool allow_none) {
    if (!allow_none && mode == ModelInputMode::None) {
        mode = ModelInputMode::Weights;
    }

    draw_labeled_combo("Model Input", model_input_label(mode), 180.0f, [&mode, allow_none]() {
        const auto draw_option = [&mode](const ModelInputMode option) {
            const bool selected = option == mode;
            if (ImGui::Selectable(model_input_label(option), selected)) {
                mode = option;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        };
        if (allow_none) {
            draw_option(ModelInputMode::None);
        }
        draw_option(ModelInputMode::Weights);
        draw_option(ModelInputMode::Onnx);
        draw_option(ModelInputMode::TensorRt);
    });

    switch (mode) {
    case ModelInputMode::None:
        ImGui::TextUnformatted("No model backing. Manual box seeds only.");
        return ModelInputBrowseRequest::None;
    case ModelInputMode::Weights:
        return draw_file_picker_input("Weights Path", weights_path, weights_browse_busy)
                   ? ModelInputBrowseRequest::Weights
                   : ModelInputBrowseRequest::None;
    case ModelInputMode::Onnx:
        return draw_file_picker_input("ONNX Path", onnx_path, onnx_browse_busy)
                   ? ModelInputBrowseRequest::Onnx
                   : ModelInputBrowseRequest::None;
    case ModelInputMode::TensorRt:
        return draw_file_picker_input("TensorRT Path", tensorrt_path, tensorrt_browse_busy)
                   ? ModelInputBrowseRequest::TensorRt
                   : ModelInputBrowseRequest::None;
    }
    return ModelInputBrowseRequest::None;
}

} // namespace mmltk::gui
