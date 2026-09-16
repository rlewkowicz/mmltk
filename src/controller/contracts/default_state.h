#pragma once
#include <string_view>
#include "src/controller/contracts/view_state.h"
namespace mmltk::controller::contracts {
inline constexpr std::string_view kDefaultGuiPresetName = kDefaultModelPresetName;
inline void apply_default_gui_state(TrainViewState& train, ValidateViewState& validate, PredictViewState& predict, AnnotateViewState& annotate,
                                    ExportViewState& export_state, ExploreViewState& explore) {
    train = TrainViewState{};
    validate = ValidateViewState{};
    predict = PredictViewState{};
    annotate = AnnotateViewState{};
    export_state = ExportViewState{};
    explore = ExploreViewState{};
}
}  // namespace mmltk::controller::contracts
