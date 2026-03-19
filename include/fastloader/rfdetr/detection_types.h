#pragma once

#include "fastloader/rfdetr/postprocess.h"

#include <torch/torch.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fastloader::rfdetr {

struct PreparedTarget {
    torch::Tensor image_id;
    torch::Tensor orig_size;
    torch::Tensor size;
    torch::Tensor boxes;
    torch::Tensor labels;
    torch::Tensor area;
    torch::Tensor iscrowd;
};

struct PackedTargetMasks {
    torch::Tensor bits;
    int64_t height = 0;
    int64_t width = 0;
};

struct PreparedTargets {
    std::vector<PreparedTarget> targets;
    torch::Tensor orig_sizes;
    torch::Tensor nested_mask;
    torch::Tensor all_boxes;
    torch::Tensor all_labels;
    torch::Tensor all_area;
    torch::Tensor all_iscrowd;
    std::optional<PackedTargetMasks> packed_masks;
    std::vector<int64_t> offsets;
    std::vector<int64_t> counts;
};

struct OutputLayer {
    struct SparsePredMasks {
        torch::Tensor spatial_features;
        torch::Tensor query_features;
        torch::Tensor bias;
    };

    torch::Tensor pred_logits;
    torch::Tensor pred_boxes;
    std::optional<torch::Tensor> pred_masks;
    std::optional<SparsePredMasks> sparse_pred_masks;
};

struct ModelOutputs {
    OutputLayer main;
    std::vector<OutputLayer> aux_outputs;
    std::optional<OutputLayer> enc_outputs;
};

struct DetectionConfig {
    int64_t num_classes = 0;
    int64_t group_detr = 1;
    int64_t dec_layers = 0;
    int64_t num_select = 300;
    int64_t world_size = 1;
    bool sum_group_losses = false;
    bool use_varifocal_loss = false;
    bool use_position_supervised_loss = false;
    bool ia_bce_loss = false;
    bool aux_loss = false;
    bool two_stage = false;
    bool include_masks = false;
    int64_t mask_point_sample_ratio = 16;
    double focal_alpha = 0.25;
    double cls_loss_coef = 1.0;
    double bbox_loss_coef = 1.0;
    double giou_loss_coef = 1.0;
    double mask_ce_loss_coef = 1.0;
    double mask_dice_loss_coef = 1.0;
    double set_cost_class = 1.0;
    double set_cost_bbox = 1.0;
    double set_cost_giou = 1.0;
    std::vector<std::pair<std::string, double>> weight_dict;
};

void populate_default_detection_weight_dict(DetectionConfig& config);

} // namespace fastloader::rfdetr
