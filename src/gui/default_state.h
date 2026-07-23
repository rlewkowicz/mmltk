#pragma once

#include "view_state.h"

#include <string_view>

namespace mmltk::gui {

inline constexpr std::string_view kDefaultGuiPresetName = kDefaultModelPresetName;

void apply_default_gui_state(TrainViewState& train, ValidateViewState& validate, PredictViewState& predict,
                             AnnotateViewState& annotate, ExportViewState& export_state);

}  
