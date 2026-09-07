#include "detail/detection_types.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace mmltk::backend::models::rfdetr {
namespace tensor_api = mmltk::backend::ml::torch_api;

void assert_inference_output_dtype(const tensor_api::Tensor& pred_logits, const tensor_api::Tensor& pred_boxes,
                                   const tensor_api::ScalarType expected_dtype, const char* context) {
    if (!pred_logits.defined() || !pred_boxes.defined()) {
        throw std::runtime_error(std::string(context) + " returned undefined detection outputs");
    }
    if (pred_logits.scalar_type() != expected_dtype) {
        throw std::runtime_error(std::string(context) + " returned logits with the wrong element type");
    }
    const bool valid_box_dtype = pred_boxes.scalar_type() == expected_dtype ||
                                 (expected_dtype != tensor_api::kFloat && pred_boxes.scalar_type() == tensor_api::kFloat);
    if (!valid_box_dtype) { throw std::runtime_error(std::string(context) + " returned boxes with the wrong element type"); }
}

void populate_default_detection_weight_dict(DetectionConfig& config) {
    config.weight_dict = {
        {"loss_ce", config.cls_loss_coef},
        {"loss_bbox", config.bbox_loss_coef},
        {"loss_giou", config.giou_loss_coef},
    };
    if (config.include_masks) {
        config.weight_dict.emplace_back("loss_mask_ce", config.mask_ce_loss_coef);
        config.weight_dict.emplace_back("loss_mask_dice", config.mask_dice_loss_coef);
    }
    if (!config.aux_loss) { return; }

    const auto base_weights = config.weight_dict;
    for (std::int64_t layer_index = 0; layer_index < config.dec_layers - 1; ++layer_index) {
        for (const auto& item : base_weights) {
            config.weight_dict.emplace_back(item.first + "_" + std::to_string(layer_index), item.second);
        }
    }
    if (config.two_stage) {
        for (const auto& item : base_weights) {
            config.weight_dict.emplace_back(item.first + "_enc", item.second);
        }
    }
}

}  // namespace mmltk::backend::models::rfdetr
