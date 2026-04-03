#pragma once

#include "mmltk/rfdetr/predict.h"
#include "source_selection.h"

#include <string>
#include <vector>

namespace mmltk::gui {

struct PreparedPredictSource {
    std::vector<mmltk::rfdetr::PredictImageInput> image_inputs;
};

std::string validate_predict_source(const SourceSelectionState& state);
PreparedPredictSource prepare_predict_source(const SourceSelectionState& state);

} // namespace mmltk::gui
