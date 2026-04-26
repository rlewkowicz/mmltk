#pragma once

#include "mmltk/model/artifacts.h"
#include "mmltk/rfdetr/model_config.h"

#include <filesystem>
#include <string>

namespace mmltk::rfdetr {

struct NativeRfDetrConfig {
    std::string preset_name;
    int resolution = 0;
    bool segmentation = false;
    int num_classes = 0;
    int num_queries = 0;
    int num_select = 0;
    int dec_layers = 0;
    int group_detr = 1;
    bool two_stage = true;
    int hidden_dim = 256;
    int patch_size = 16;
    int num_windows = 2;
    int positional_encoding_size = 24;
    int sa_nheads = 8;
    int ca_nheads = 16;
    int dec_n_points = 2;
    int dim_feedforward = 2048;
    bool bbox_reparam = true;
    bool lite_refpoint_refine = true;
    double cls_loss_coef = 1.0;
    double bbox_loss_coef = 5.0;
    double giou_loss_coef = 2.0;
    double mask_ce_loss_coef = 1.0;
    double mask_dice_loss_coef = 1.0;
    bool sum_group_losses = false;
    bool use_varifocal_loss = false;
    bool use_position_supervised_loss = false;
    bool ia_bce_loss = true;
    bool aux_loss = true;
    int64_t mask_point_sample_ratio = 16;
    double focal_alpha = 0.25;
    double set_cost_class = 2.0;
    double set_cost_bbox = 5.0;
    double set_cost_giou = 2.0;
};

struct ModelArtifactRequest : mmltk::model::ModelArtifactRequest {};

struct ResolvedModelArtifacts : mmltk::model::ResolvedModelArtifacts {
    NativeRfDetrConfig config;
};

ResolvedModelArtifacts resolve_model_artifacts(const ModelArtifactRequest& request);
ResolvedModelArtifacts resolve_upstream_weight_artifacts(const std::filesystem::path& weights_path);

}  // namespace mmltk::rfdetr
