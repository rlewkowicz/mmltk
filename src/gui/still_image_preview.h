#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fastloader::rfdetr {
struct PredictImageInput;
struct PredictionRecord;
}

namespace fastloader::gui {

struct StillImagePreview {
    std::string source_name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels_bgr;
};

StillImagePreview render_single_image_prediction_preview(
    const fastloader::rfdetr::PredictImageInput& input,
    const fastloader::rfdetr::PredictionRecord& record,
    int num_classes,
    int device_id);

} // namespace fastloader::gui
