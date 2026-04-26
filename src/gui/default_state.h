#pragma once

#include "view_state.h"

#include <string>
#include <string_view>

namespace mmltk::gui {

inline constexpr std::string_view kDefaultGuiPresetName = "rf-detr-seg-medium";

void apply_default_gui_state(std::string& selected_preset_name, TrainViewState& train, ValidateViewState& validate,
                             PredictViewState& predict, AnnotateViewState& annotate, ExportViewState& export_state);

}  // namespace mmltk::gui
