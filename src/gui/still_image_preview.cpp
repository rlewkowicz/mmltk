#include "still_image_preview.h"

#include "mmltk/rfdetr/predict.h"

#include "stb_image.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace mmltk::gui {

namespace {

void rgb_to_bgr_in_place(std::vector<std::uint8_t>& pixels) {
    for (std::size_t offset = 0; offset + 2U < pixels.size(); offset += 3U) {
        std::swap(pixels[offset], pixels[offset + 2U]);
    }
}

}  

StillImagePreview render_single_image_prediction_preview(const mmltk::rfdetr::PredictImageInput& input,
                                                         const mmltk::rfdetr::PredictionRecord& record,
                                                         const int num_classes, const int device_id) {
    (void)num_classes;
    (void)device_id;
    int raw_width = 0;
    int raw_height = 0;
    int raw_channels = 0;
    stbi_uc* raw_pixels = stbi_load(input.image_path.c_str(), &raw_width, &raw_height, &raw_channels, 3);
    if (raw_pixels == nullptr) {
        throw std::runtime_error("failed to load preview image: " + input.image_path.string());
    }
    if (raw_width <= 0 || raw_height <= 0) {
        stbi_image_free(raw_pixels);
        throw std::runtime_error("invalid preview image dimensions: " + input.image_path.string());
    }

    StillImagePreview preview;
    preview.source_name = record.source_name.empty() ? input.image_path.string() : record.source_name;
    preview.width = static_cast<std::uint32_t>(raw_width);
    preview.height = static_cast<std::uint32_t>(raw_height);
    const std::size_t image_bytes = static_cast<std::size_t>(raw_width) * static_cast<std::size_t>(raw_height) * 3U;
    preview.pixels_bgr.assign(raw_pixels, raw_pixels + image_bytes);
    stbi_image_free(raw_pixels);
    rgb_to_bgr_in_place(preview.pixels_bgr);

    auto& overlay = preview.prediction_overlay;
    overlay.generation = 1U;
    overlay.document_generation = 1U;
    overlay.capture_width = preview.width;
    overlay.capture_height = preview.height;
    overlay.instances.reserve(record.detections.size());
    for (std::size_t index = 0; index < record.detections.size(); ++index) {
        const mmltk::rfdetr::Prediction& detection = record.detections[index];
        if (detection.category_id <= 0) {
            continue;
        }
        const int x1 = static_cast<int>(std::clamp(detection.bbox_xyxy[0], 0.0F, static_cast<float>(raw_width)));
        const int y1 = static_cast<int>(std::clamp(detection.bbox_xyxy[1], 0.0F, static_cast<float>(raw_height)));
        const int x2 = static_cast<int>(std::clamp(detection.bbox_xyxy[2], 0.0F, static_cast<float>(raw_width)));
        const int y2 = static_cast<int>(std::clamp(detection.bbox_xyxy[3], 0.0F, static_cast<float>(raw_height)));
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }
        mmltk::live::ManualOverlayInstance instance;
        instance.instance_id = "prediction-" + std::to_string(index);
        instance.box = mmltk::live::ManualOverlayBox{x1, y1, x2, y2};
        instance.category_index = static_cast<std::size_t>(detection.category_id - 1);
        if (detection.has_mask && detection.mask.width == preview.width && detection.mask.height == preview.height) {
            instance.mask_region = mmltk::live::ManualOverlayMaskRegion{0U, 0U, preview.width, preview.height};
            instance.mask_runs.reserve(detection.mask.runs.size());
            for (const auto& run : detection.mask.runs) {
                if (run.second == 0U) {
                    continue;
                }
                instance.mask_runs.push_back(mmltk::live::ManualOverlayMaskRun{run.first, run.second});
            }
        }
        overlay.instances.push_back(std::move(instance));
    }
    return preview;
}

}  
