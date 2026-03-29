#pragma once

#include "fastloader/rfdetr/predict.h"
#include "source_selection.h"

#include <string>
#include <vector>

namespace fastloader::gui {

struct PreparedPredictSource {
    std::vector<fastloader::rfdetr::PredictImageInput> image_inputs;
};

std::string validate_predict_source(const SourceSelectionState& state);
PreparedPredictSource prepare_predict_source(const SourceSelectionState& state);

} // namespace fastloader::gui
