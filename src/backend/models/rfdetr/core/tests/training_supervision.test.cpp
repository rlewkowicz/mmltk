// RF-DETR Match-Free mathematical and topology coverage.
#include <ATen/Context.h>
#include <cuda_runtime.h>

#include <cmath>
#include <array>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "catch2_compat.hpp"
#include "detail/detection_geometry.h"
#include "detail/detection_ops.h"
#include "detail/model_access.h"
#include "detail/modules_technical.h"
#include "detail/postprocess.h"
#include "detail/training_supervision.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "torch_api.h"
#include "src/backend/models/rfdetr/augmentation/spatial_erasure.h"

import mmltk.backend.models.rfdetr.core.model;

namespace {

namespace rfdetr = mmltk::backend::models::rfdetr;
namespace torch_api = mmltk::backend::ml::torch_api;
using namespace torch_api::indexing;

std::vector<int64_t> int64_values(const torch::Tensor& tensor) {
    const auto host = tensor.reshape({-1}).to(torch::kCPU).contiguous();
    if (host.scalar_type() != torch::kInt64) { throw std::runtime_error("expected an Int64 tensor in supervision test"); }
    const auto count = static_cast<std::size_t>(host.numel());
    const auto* values = host.data_ptr<int64_t>();
    return {values, values + count};
}

rfdetr::NativeRfDetrConfig match_free_config() {
    rfdetr::NativeRfDetrConfig config;
    config.num_classes = 3;
    config.num_queries = 3;
    config.num_select = 3;
    config.dec_layers = 1;
    config.group_detr = 1;
    config.hidden_dim = 2;
    config.aux_loss = false;
    config.two_stage = false;
    config.focal_alpha = 0.25;
    config.set_cost_class = 2.0;
    config.set_cost_bbox = 5.0;
    config.set_cost_giou = 2.0;
    config.training_supervision.assignment = rfdetr::TrainAssignmentKind::MatchFree;
    return config;
}

rfdetr::NativeRfDetrConfig denoising_config(int64_t object_classes = 3) {
    auto config = match_free_config();
    config.num_classes = static_cast<int>(object_classes + 1);
    config.num_queries = 4;
    config.num_select = 4;
    config.hidden_dim = 4;
    config.training_supervision.assignment = rfdetr::TrainAssignmentKind::Hungarian;
    config.training_supervision.denoising.enabled = true;
    config.training_supervision.denoising.groups = 2;
    config.training_supervision.denoising.label_noise_ratio = 0.5F;
    config.training_supervision.denoising.center_noise_scale = 0.4F;
    config.training_supervision.denoising.size_noise_scale = 0.4F;
    return config;
}

rfdetr::NativeRfDetrConfig production_denoising_config(const rfdetr::TrainAssignmentKind assignment, const bool bbox_reparam,
                                                       const bool segmentation = false) {
    auto config = rfdetr::native_config_from_preset(rfdetr::model_presets().front());
    config.resolution = 64;
    config.segmentation = segmentation;
    config.num_queries = 3;
    config.num_select = 3;
    config.group_detr = 1;
    config.dec_layers = 1;
    config.aux_loss = false;
    config.two_stage = segmentation;
    config.bbox_reparam = bbox_reparam;
    config.training_supervision.assignment = assignment;
    config.training_supervision.denoising.enabled = true;
    config.training_supervision.denoising.groups = 2;
    return config;
}

void set_parameter(rfdetr::TrainingSupervisionImpl& supervision, const std::string& name, const torch::Tensor& value) {
    auto parameters = supervision.named_parameters(true);
    auto* destination = parameters.find(name);
    REQUIRE(destination != nullptr);
    torch::NoGradGuard no_grad;
    destination->copy_(value);
}

void install_oracle_projections(rfdetr::TrainingSupervisionImpl& supervision) {
    set_parameter(supervision, "ground_truth_mlp.linear1.weight",
                  torch_api::tensor({{1.0F, -0.5F, 0.2F, 0.3F, -0.1F, 0.4F}, {-0.25F, 0.75F, -0.3F, 0.1F, 0.5F, 0.2F}}));
    set_parameter(supervision, "ground_truth_mlp.linear1.bias", torch_api::tensor({0.1F, 0.2F}));
    set_parameter(supervision, "ground_truth_mlp.linear2.weight", torch_api::tensor({{0.8F, -0.2F}, {0.3F, 0.7F}}));
    set_parameter(supervision, "ground_truth_mlp.linear2.bias", torch_api::tensor({0.05F, -0.1F}));
    set_parameter(supervision, "query_mlp.linear1.weight", torch::eye(2));
    set_parameter(supervision, "query_mlp.linear1.bias", torch_api::zeros({2}));
    set_parameter(supervision, "query_mlp.linear2.weight", torch::eye(2));
    set_parameter(supervision, "query_mlp.linear2.bias", torch_api::zeros({2}));
    set_parameter(supervision, "query_projection.weight", torch::eye(2));
    set_parameter(supervision, "key_projection.weight", torch::eye(2));
}

rfdetr::PreparedTargets one_image_targets(const torch::Tensor& boxes, const torch::Tensor& labels) {
    rfdetr::PreparedTargets targets;
    rfdetr::PreparedTarget target;
    target.boxes = boxes;
    target.labels = labels;
    targets.targets.push_back(std::move(target));
    targets.all_boxes = boxes;
    targets.all_labels = labels;
    targets.offsets = {0};
    targets.counts = {labels.size(0)};
    const auto integer_options = torch_api::TensorOptions().dtype(torch_api::kInt64).device(labels.device());
    targets.target_offsets = torch_api::tensor({0}, integer_options);
    targets.target_counts = torch_api::tensor({labels.size(0)}, integer_options);
    return targets;
}

rfdetr::PreparedTargets batched_targets(const std::vector<torch::Tensor>& boxes, const std::vector<torch::Tensor>& labels,
                                        int64_t resolved_queries = 4) {
    REQUIRE(boxes.size() == labels.size());
    rfdetr::PreparedTargets targets;
    targets.resolved_query_count = resolved_queries;
    int64_t offset = 0;
    for (std::size_t index = 0; index < boxes.size(); ++index) {
        rfdetr::PreparedTarget target;
        target.boxes = boxes[index];
        target.labels = labels[index];
        targets.targets.push_back(std::move(target));
        targets.offsets.push_back(offset);
        targets.counts.push_back(labels[index].size(0));
        offset += labels[index].size(0);
    }
    const auto device = labels.empty() ? torch::Device(torch::kCPU) : labels.front().device();
    const auto float_options = torch_api::TensorOptions().dtype(torch_api::kFloat32).device(device);
    const auto integer_options = torch_api::TensorOptions().dtype(torch_api::kInt64).device(device);
    targets.all_boxes = offset == 0 ? torch_api::zeros({0, 4}, float_options) : torch_api::cat(boxes, 0);
    targets.all_labels = offset == 0 ? torch_api::zeros({0}, integer_options) : torch_api::cat(labels, 0);
    targets.target_offsets = torch_api::tensor(targets.offsets, integer_options);
    targets.target_counts = torch_api::tensor(targets.counts, integer_options);
    return targets;
}

rfdetr::DetectionConfig detection_config(const rfdetr::NativeRfDetrConfig& model) {
    rfdetr::DetectionConfig config;
    config.num_classes = model.num_classes;
    config.group_detr = model.group_detr;
    config.dec_layers = model.dec_layers;
    config.num_select = model.num_select;
    config.sum_group_losses = model.sum_group_losses;
    config.use_varifocal_loss = model.use_varifocal_loss;
    config.use_position_supervised_loss = model.use_position_supervised_loss;
    config.ia_bce_loss = model.ia_bce_loss;
    config.aux_loss = model.aux_loss;
    config.two_stage = model.two_stage;
    config.focal_alpha = model.focal_alpha;
    config.cls_loss_coef = model.cls_loss_coef;
    config.bbox_loss_coef = model.bbox_loss_coef;
    config.giou_loss_coef = model.giou_loss_coef;
    config.set_cost_class = model.set_cost_class;
    config.set_cost_bbox = model.set_cost_bbox;
    config.set_cost_giou = model.set_cost_giou;
    rfdetr::populate_default_detection_weight_dict(config);
    return config;
}

void require_layer_equal(const rfdetr::OutputLayer& lhs, const rfdetr::OutputLayer& rhs) {
    REQUIRE(torch_api::equal(lhs.pred_logits, rhs.pred_logits));
    REQUIRE(torch_api::equal(lhs.pred_boxes, rhs.pred_boxes));
    REQUIRE(lhs.pred_masks.has_value() == rhs.pred_masks.has_value());
    if (lhs.pred_masks) { REQUIRE(torch_api::equal(*lhs.pred_masks, *rhs.pred_masks)); }
}

void require_float32(const std::initializer_list<torch::Tensor> tensors) {
    for (const auto& tensor : tensors) {
        REQUIRE(tensor.scalar_type() == torch::kFloat32);
    }
}

void require_finite_gradients(const std::initializer_list<torch::Tensor> tensors) {
    for (const auto& tensor : tensors) {
        REQUIRE(tensor.grad().defined());
        REQUIRE(torch_api::isfinite(tensor.grad()).all().item<bool>());
    }
}

void retain_captured_layer_gradients(rfdetr::OutputLayer& layer) {
    REQUIRE(layer.query_features.has_value());
    REQUIRE(layer.query_layout.has_value());
    REQUIRE(layer.query_layout->groups > 0);
    REQUIRE(layer.query_layout->queries_per_group > 0);
    REQUIRE(layer.query_layout->total_queries() == layer.pred_logits.size(1));
    REQUIRE(layer.query_features->size(0) == layer.pred_logits.size(0));
    REQUIRE(layer.query_features->size(1) == layer.query_layout->groups);
    REQUIRE(layer.query_features->size(2) == layer.query_layout->queries_per_group);
    layer.pred_logits.retain_grad();
    layer.pred_boxes.retain_grad();
    layer.query_features->retain_grad();
}

void retain_captured_output_gradients(rfdetr::ModelOutputs& outputs) {
    retain_captured_layer_gradients(outputs.main);
    for (auto& auxiliary : outputs.aux_outputs) {
        retain_captured_layer_gradients(auxiliary);
    }
    if (outputs.enc_outputs) { retain_captured_layer_gradients(*outputs.enc_outputs); }
}

void append_layer_gradient_evidence(const rfdetr::OutputLayer& layer, const std::string& prefix, const bool require_zero,
                                    std::vector<std::string>& names) {
    const auto append = [&](const torch::Tensor& tensor, const std::string& suffix) {
        REQUIRE(tensor.grad().defined());
        REQUIRE(torch_api::isfinite(tensor.grad()).all().item<bool>());
        if (require_zero) {
            REQUIRE(tensor.grad().abs().sum().item<float>() == 0.0F);
        } else {
            REQUIRE(tensor.grad().abs().sum().item<float>() > 0.0F);
        }
        names.push_back(prefix + suffix);
    };
    append(layer.pred_logits, ".pred_logits");
    append(layer.pred_boxes, ".pred_boxes");
    REQUIRE(layer.query_features.has_value());
    append(*layer.query_features, ".query_features");
}

std::vector<std::string> output_gradient_evidence(const rfdetr::ModelOutputs& outputs, const bool require_zero) {
    std::vector<std::string> names;
    append_layer_gradient_evidence(outputs.main, "main", require_zero, names);
    for (std::size_t index = 0; index < outputs.aux_outputs.size(); ++index) {
        append_layer_gradient_evidence(outputs.aux_outputs[index], "aux." + std::to_string(index), require_zero, names);
    }
    if (outputs.enc_outputs) { append_layer_gradient_evidence(*outputs.enc_outputs, "encoder", require_zero, names); }
    return names;
}

bool selected_model_gradient_family(const std::string& name, const bool include_encoder) {
    return name.starts_with("training_supervision.") || name.starts_with("class_embed.") || name.starts_with("bbox_embed.") ||
           (include_encoder &&
            (name.starts_with("transformer.enc_out_class_embed.") || name.starts_with("transformer.enc_out_bbox_embed.")));
}

std::vector<std::string> selected_model_gradient_evidence(torch::nn::Module& module, const bool include_encoder, const bool require_zero) {
    std::vector<std::string> expected;
    std::vector<std::string> defined;
    for (const auto& parameter : module.named_parameters(true)) {
        if (!selected_model_gradient_family(parameter.key(), include_encoder)) { continue; }
        expected.push_back(parameter.key());
        if (!parameter.value().grad().defined()) { continue; }
        REQUIRE(torch_api::isfinite(parameter.value().grad()).all().item<bool>());
        if (require_zero) { REQUIRE(parameter.value().grad().abs().sum().item<float>() == 0.0F); }
        defined.push_back(parameter.key());
    }
    REQUIRE_FALSE(expected.empty());
    REQUIRE(defined == expected);
    return defined;
}

std::vector<std::string> ordered_defined_trainable_gradients(torch::nn::Module& module) {
    std::vector<std::string> names;
    for (const auto& parameter : module.named_parameters(true)) {
        if (parameter.value().requires_grad() && parameter.value().grad().defined()) { names.push_back(parameter.key()); }
    }
    return names;
}

rfdetr::OutputLayer oracle_layer(const torch::Tensor& features) {
    rfdetr::OutputLayer layer;
    layer.pred_logits = torch_api::tensor({{{0.4F, -0.7F, 0.2F}, {-0.1F, 0.8F, -0.3F}, {0.6F, 0.1F, -0.5F}}},
                                          torch_api::TensorOptions().dtype(torch_api::kFloat32))
                            .set_requires_grad(true);
    layer.pred_boxes = torch_api::tensor({{{0.45F, 0.55F, 0.2F, 0.3F}, {0.2F, 0.25F, 0.15F, 0.1F}, {0.7F, 0.65F, 0.25F, 0.2F}}},
                                         torch_api::TensorOptions().dtype(torch_api::kFloat32))
                           .set_requires_grad(true);
    layer.query_features = features;
    layer.query_layout = rfdetr::SupervisedQueryLayout{1, 3};
    return layer;
}

void test_geometry_preserves_consumer_policies_and_batch_isolation() {
    const auto malformed = torch_api::tensor({{0.5F, 0.5F, -0.2F, -0.4F}});
    const auto criterion = rfdetr::box_cxcywh_to_xyxy(malformed, rfdetr::BoxExtentPolicy::Preserve);
    const auto postprocess = rfdetr::box_cxcywh_to_xyxy(malformed);
    REQUIRE(criterion.index({0, 0}).item<float>() > criterion.index({0, 2}).item<float>());
    REQUIRE(postprocess.index({0, 0}).item<float>() == postprocess.index({0, 2}).item<float>());

    const auto lhs = torch_api::tensor({{{0.0F, 0.0F, 1.0F, 1.0F}}, {{10.0F, 10.0F, 11.0F, 11.0F}}});
    const auto rhs = torch_api::tensor({{{0.0F, 0.0F, 1.0F, 1.0F}}, {{10.0F, 10.0F, 11.0F, 11.0F}}});
    const auto giou = rfdetr::batched_pairwise_generalized_box_iou(lhs, rhs);
    REQUIRE(giou.sizes().vec() == std::vector<int64_t>{2, 1, 1});
    REQUIRE(torch_api::allclose(giou, torch_api::ones_like(giou)));

    rfdetr::OutputTensors postprocess_input;
    postprocess_input.pred_logits = torch_api::tensor({{{10.0F}}});
    postprocess_input.pred_boxes = malformed.unsqueeze(0);
    const auto postprocessed = rfdetr::postprocess_outputs_fixed_size(postprocess_input, 100, 200, 1);
    const auto& postprocessed_box = postprocessed.front().at("boxes");
    REQUIRE(postprocessed_box.index({0, 0}).item<float>() == postprocessed_box.index({0, 2}).item<float>());
    REQUIRE(postprocessed_box.index({0, 1}).item<float>() == postprocessed_box.index({0, 3}).item<float>());

    rfdetr::ModelOutputs criterion_outputs;
    criterion_outputs.main.pred_logits = torch_api::tensor({{{10.0F, -10.0F, -10.0F}}});
    criterion_outputs.main.pred_boxes = malformed.unsqueeze(0);
    auto criterion_targets = one_image_targets(torch_api::tensor({{0.8F, 0.5F, 0.2F, 0.4F}}),
                                               torch_api::tensor({0}, torch_api::TensorOptions().dtype(torch_api::kInt64)));
    auto criterion_config = detection_config(match_free_config());
    criterion_config.aux_loss = false;
    criterion_config.two_stage = false;
    const auto losses = rfdetr::detection_loss_dict(criterion_outputs, criterion_targets, criterion_config, true, 1.0);
    const auto target_xyxy = rfdetr::box_cxcywh_to_xyxy(criterion_targets.all_boxes, rfdetr::BoxExtentPolicy::Preserve);
    const auto preserve_expected = 1.0F - rfdetr::aligned_generalized_box_iou(criterion, target_xyxy).index({0});
    const auto clamp_expected = 1.0F - rfdetr::aligned_generalized_box_iou(postprocess, target_xyxy).index({0});
    REQUIRE(torch_api::allclose(losses.at("loss_giou"), preserve_expected));
    REQUIRE_FALSE(torch_api::allclose(losses.at("loss_giou"), clamp_expected));
}

void test_gathered_ground_truth_affinity_matches_explicit_one_hot_and_sqrt_d() {
    auto config = match_free_config();
    rfdetr::TrainingSupervisionImpl supervision(config);
    supervision.initialize(17);
    install_oracle_projections(supervision);
    const auto labels = torch_api::tensor({{0, 1}}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    const auto boxes = torch_api::tensor({{{0.5F, 0.5F, 0.2F, 0.3F}, {0.2F, 0.3F, 0.1F, 0.15F}}});
    const auto valid = torch_api::ones({1, 2}, torch_api::TensorOptions().dtype(torch_api::kBool));
    const auto features = torch_api::tensor({{{{0.8F, 0.1F}, {0.2F, 0.9F}, {0.4F, 0.3F}}}});
    const auto result = supervision.correspondence(labels, boxes, valid, features);

    const auto one_hot = torch::eye(2).index({labels});
    const auto input = torch_api::cat({one_hot, boxes}, -1);
    const auto parameters = supervision.named_parameters(true);
    const auto gt_hidden = torch::relu(torch::matmul(input, parameters["ground_truth_mlp.linear1.weight"].transpose(0, 1)) +
                                       parameters["ground_truth_mlp.linear1.bias"]);
    const auto gt = torch::matmul(gt_hidden, parameters["ground_truth_mlp.linear2.weight"].transpose(0, 1)) +
                    parameters["ground_truth_mlp.linear2.bias"];
    const auto query_hidden = torch::relu(features);
    const auto expected = torch::softmax(torch::einsum("bmd,bgnd->bgmn", {gt, query_hidden}) / std::sqrt(2.0), -1);
    REQUIRE(torch_api::allclose(result.dense, expected, 1.0e-6, 1.0e-6));
}

void test_scg_excludes_inactive_columns_handles_ties_and_keeps_selected_gradients_live() {
    auto dense = torch_api::tensor({{{{0.60F, 0.40F, 0.0F}, {0.30F, 0.70F, 0.0F}, {0.25F, 0.34F, 0.0F}}}}).set_requires_grad(true);
    const auto valid = torch_api::ones({1, 3}, torch_api::TensorOptions().dtype(torch_api::kBool));
    const auto sparse = rfdetr::sparse_match_free_correspondence(dense, valid, 0.5F);
    REQUIRE(sparse.index({0, 0, Slice(), 2}).abs().sum().item<float>() == 0.0F);
    REQUIRE(sparse.index({0, 0, 0}).sum().item<float>() == 1.0F);
    REQUIRE(sparse.index({0, 0, 2}).sum().item<float>() == 0.0F);
    (sparse.index({0, 0, 0}) * torch_api::tensor({1.0F, 2.0F, 3.0F})).sum().backward();
    REQUIRE(dense.grad().index({0, 0, 0, 0}).abs().item<float>() > 0.0F);

    const auto tied = torch_api::tensor({{{{0.5F, 0.5F, 0.0F}}}});
    const auto tied_sparse =
        rfdetr::sparse_match_free_correspondence(tied, torch_api::ones({1, 1}, torch_api::TensorOptions().dtype(torch_api::kBool)), 0.99F);
    REQUIRE(tied_sparse.index({0, 0, 0, 0}).item<float>() > 0.0F);
    REQUIRE(tied_sparse.index({0, 0, 0, 1}).item<float>() == 0.0F);
}

void test_complete_focal_cost_and_both_objectives_keep_prediction_and_probe_gradients() {
    auto config = match_free_config();
    rfdetr::TrainingSupervisionImpl supervision(config);
    supervision.initialize(31);
    install_oracle_projections(supervision);
    auto features = torch_api::tensor({{{{0.8F, 0.1F}, {0.2F, 0.9F}, {0.4F, 0.3F}}}}).set_requires_grad(true);
    rfdetr::ModelOutputs outputs;
    outputs.main = oracle_layer(features);
    const auto targets = one_image_targets(torch_api::tensor({{0.5F, 0.5F, 0.2F, 0.25F}}),
                                           torch_api::tensor({1}, torch_api::TensorOptions().dtype(torch_api::kInt64)));
    const auto result = supervision.loss(outputs, targets, {torch_api::tensor(1.0F)}, true);
    REQUIRE(result.total.item<float>() >= 0.0F);
    REQUIRE(torch_api::allclose(result.total, result.classification + result.box + result.giou));
    result.total.backward();
    REQUIRE(outputs.main.pred_logits.grad().abs().sum().item<float>() > 0.0F);
    REQUIRE(outputs.main.pred_boxes.grad().abs().sum().item<float>() > 0.0F);
    REQUIRE(features.grad().abs().sum().item<float>() > 0.0F);
    const auto supervision_parameters = supervision.named_parameters(true);
    for (const auto& parameter : supervision_parameters) {
        REQUIRE(parameter.value().grad().defined());
        REQUIRE(torch_api::isfinite(parameter.value().grad()).all().item<bool>());
        REQUIRE(parameter.value().grad().abs().sum().item<float>() > 0.0F);
    }
    REQUIRE(supervision_parameters.find("value_projection.weight") == nullptr);
}

void test_focal_broadcast_cost_sums_every_channel_and_places_coefficients_once() {
    auto config = match_free_config();
    config.training_supervision.match_free.correspondence_weight = 0.7F;
    config.training_supervision.match_free.query_weight = 1.3F;
    rfdetr::TrainingSupervisionImpl supervision(config);
    supervision.initialize(37);
    install_oracle_projections(supervision);
    const auto features = torch_api::tensor({{{{0.8F, 0.1F}, {0.2F, 0.9F}, {0.4F, 0.3F}}}});
    auto layer = oracle_layer(features);
    const auto labels = torch_api::tensor({{1}}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    const auto boxes = torch_api::tensor({{{0.5F, 0.5F, 0.2F, 0.25F}}});
    const auto valid = torch_api::ones({1, 1}, torch_api::TensorOptions().dtype(torch_api::kBool));
    const auto costs = supervision.broadcast_cost(labels, boxes, valid, layer);

    const auto logits = layer.pred_logits.index({0, 0});
    const auto probabilities = logits.sigmoid();
    const auto negatives = (1.0F - config.focal_alpha) * probabilities.square() * torch::softplus(logits);
    const auto positive = config.focal_alpha * (1.0F - probabilities.index({1})).square() * torch::softplus(-logits.index({1}));
    const auto expected_classification = config.set_cost_class * (negatives.sum() + positive - negatives.index({1}));
    REQUIRE(torch_api::allclose(costs.classification.index({0, 0, 0, 0}), expected_classification));
    REQUIRE(costs.total.min().item<float>() >= 0.0F);

    const auto correspondence = supervision.correspondence(labels, boxes, valid, features);
    const auto expected = (config.training_supervision.match_free.correspondence_weight * (correspondence.dense * costs.total).sum() +
                           config.training_supervision.match_free.query_weight * (correspondence.sparse_normalized * costs.total).sum());
    rfdetr::ModelOutputs outputs;
    outputs.main = std::move(layer);
    const auto targets = one_image_targets(boxes.squeeze(0), labels.squeeze(0));
    const auto actual = supervision.loss(outputs, targets, {torch_api::tensor(1.0F)}, true);
    REQUIRE(torch_api::allclose(actual.total, expected, 1.0e-5, 1.0e-5));
}

void test_layer_group_and_device_target_reductions_follow_declared_gating() {
    auto one_group_config = match_free_config();
    rfdetr::TrainingSupervisionImpl one_group(one_group_config);
    one_group.initialize(41);
    install_oracle_projections(one_group);
    const auto features = torch_api::tensor({{{{0.8F, 0.1F}, {0.2F, 0.9F}, {0.4F, 0.3F}}}});
    rfdetr::ModelOutputs one_outputs;
    one_outputs.main = oracle_layer(features);
    one_outputs.aux_outputs = {oracle_layer(features)};
    one_outputs.enc_outputs = oracle_layer(features);
    const auto targets = one_image_targets(torch_api::tensor({{0.5F, 0.5F, 0.2F, 0.25F}}),
                                           torch_api::tensor({1}, torch_api::TensorOptions().dtype(torch_api::kInt64)));
    const auto baseline = one_group.loss(one_outputs, targets, {torch_api::tensor(1.0F)}, true).total.detach();
    const auto doubled_normalizer = one_group.loss(one_outputs, targets, {torch_api::tensor(2.0F)}, true).total.detach();
    REQUIRE(torch_api::allclose(doubled_normalizer * 2.0F, baseline));
    auto final_only = one_group.loss(one_outputs, targets, {torch_api::tensor(1.0F)}, true);
    final_only.total.backward();
    REQUIRE_FALSE(one_outputs.aux_outputs.front().pred_logits.grad().defined());
    REQUIRE_FALSE(one_outputs.enc_outputs->pred_logits.grad().defined());

    auto layered_config = one_group_config;
    layered_config.aux_loss = true;
    layered_config.two_stage = true;
    rfdetr::TrainingSupervisionImpl layered(layered_config);
    layered.initialize(41);
    install_oracle_projections(layered);
    rfdetr::ModelOutputs layered_outputs;
    layered_outputs.main = oracle_layer(features);
    layered_outputs.aux_outputs = {oracle_layer(features), oracle_layer(features)};
    layered_outputs.enc_outputs = oracle_layer(features);
    const auto layered_result = layered.loss(layered_outputs, targets, {torch_api::tensor(1.0F)}, true);
    const auto layered_loss = layered_result.total.detach();
    REQUIRE(torch_api::allclose(layered_loss, baseline * 4.0F, 1.0e-5, 1.0e-5));
    layered_result.total.backward();
    REQUIRE(layered_outputs.main.pred_logits.grad().defined());
    for (const auto& auxiliary : layered_outputs.aux_outputs) {
        REQUIRE(auxiliary.pred_logits.grad().defined());
        REQUIRE(auxiliary.pred_boxes.grad().defined());
    }
    REQUIRE(layered_outputs.enc_outputs->pred_logits.grad().defined());
    REQUIRE(layered_outputs.enc_outputs->pred_boxes.grad().defined());

    auto grouped_config = one_group_config;
    grouped_config.group_detr = 2;
    rfdetr::TrainingSupervisionImpl grouped(grouped_config);
    grouped.initialize(41);
    install_oracle_projections(grouped);
    rfdetr::ModelOutputs grouped_outputs;
    grouped_outputs.main = oracle_layer(features.repeat({1, 2, 1, 1}));
    grouped_outputs.main.pred_logits = grouped_outputs.main.pred_logits.repeat({1, 2, 1});
    grouped_outputs.main.pred_boxes = grouped_outputs.main.pred_boxes.repeat({1, 2, 1});
    grouped_outputs.main.query_layout = rfdetr::SupervisedQueryLayout{2, 3};
    const auto grouped_loss = grouped.loss(grouped_outputs, targets, {torch_api::tensor(1.0F)}, true).total.detach();
    REQUIRE(torch_api::allclose(grouped_loss, baseline, 1.0e-5, 1.0e-5));

    auto summed_group_config = grouped_config;
    summed_group_config.sum_group_losses = true;
    rfdetr::TrainingSupervisionImpl summed_groups(summed_group_config);
    summed_groups.initialize(41);
    install_oracle_projections(summed_groups);
    const auto summed_group_loss = summed_groups.loss(grouped_outputs, targets, {torch_api::tensor(1.0F)}, true).total.detach();
    REQUIRE(torch_api::allclose(summed_group_loss, baseline * 2.0F, 1.0e-5, 1.0e-5));

    rfdetr::ModelOutputs validation_outputs;
    validation_outputs.main = oracle_layer(features);
    const auto validation_loss = grouped.loss(validation_outputs, targets, {torch_api::tensor(1.0F)}, false).total.detach();
    REQUIRE(torch_api::allclose(validation_loss, baseline, 1.0e-5, 1.0e-5));
}

void test_complete_match_free_loss_is_fp32_inside_cuda_autocast() {
    if (!torch_api::cuda::is_available()) { return; }
    auto config = match_free_config();
    rfdetr::TrainingSupervisionImpl supervision(config);
    supervision.initialize(43);
    install_oracle_projections(supervision);
    supervision.to(torch::Device(torch::kCUDA));
    const auto labels = torch_api::tensor({{1}}, torch_api::TensorOptions().dtype(torch_api::kInt64).device(torch::kCUDA));
    const auto boxes =
        torch_api::tensor({{{0.5F, 0.5F, 0.2F, 0.25F}}}, torch_api::TensorOptions().dtype(torch_api::kFloat32).device(torch::kCUDA));
    const auto valid = torch_api::ones({1, 1}, torch_api::TensorOptions().dtype(torch_api::kBool).device(torch::kCUDA));
    const auto features =
        torch_api::ones({1, 1, 3, 2}, torch_api::TensorOptions().dtype(torch::kFloat16).device(torch::kCUDA)).set_requires_grad(true);
    auto layer = oracle_layer(features);
    layer.pred_logits = layer.pred_logits.detach().to(torch::kCUDA, torch::kFloat16).set_requires_grad(true);
    layer.pred_boxes = layer.pred_boxes.detach().to(torch::kCUDA, torch::kFloat16).set_requires_grad(true);
    mmltk::backend::ml::cuda::TorchAutocastScope autocast(true, torch::kFloat16);
    const auto correspondence = supervision.correspondence(labels, boxes, valid, features);
    const auto costs = supervision.broadcast_cost(labels, boxes, valid, layer);
    rfdetr::ModelOutputs outputs;
    outputs.main = layer;
    const auto targets = one_image_targets(boxes.squeeze(0), labels.squeeze(0));
    const auto loss = supervision.loss(outputs, targets, {torch_api::tensor(1.0F, boxes.options())}, true);

    require_float32({correspondence.dense, correspondence.sparse_normalized, costs.classification, costs.box, costs.giou, costs.total});
    REQUIRE(costs.total.sizes().vec() == std::vector<int64_t>{1, 1, 1, 3});
    require_float32({loss.total, loss.classification, loss.box, loss.giou, loss.correspondence});
    const auto alpha = config.training_supervision.match_free.correspondence_weight;
    const auto beta = config.training_supervision.match_free.query_weight;
    const auto expected =
        alpha * (correspondence.dense * costs.total).sum() + beta * (correspondence.sparse_normalized * costs.total).sum();
    REQUIRE(torch_api::allclose(loss.total, expected, 1.0e-5, 1.0e-5));
    REQUIRE(torch_api::allclose(loss.correspondence, alpha * (correspondence.dense * costs.total).sum(), 1.0e-5, 1.0e-5));
    REQUIRE(torch_api::allclose(loss.total - loss.correspondence, beta * (correspondence.sparse_normalized * costs.total).sum(), 1.0e-5,
                                1.0e-5));
    loss.total.backward();
    require_finite_gradients({layer.pred_logits, layer.pred_boxes, features});
    for (const auto& parameter : supervision.named_parameters(true)) {
        REQUIRE(parameter.value().grad().defined());
        REQUIRE(torch_api::isfinite(parameter.value().grad()).all().item<bool>());
    }
}

void test_timing_leases_are_explicit_bounded_and_harvested_once() {
    auto disabled_config = match_free_config();
    rfdetr::TrainingSupervisionImpl disabled(disabled_config);
    disabled.initialize(45);
    install_oracle_projections(disabled);
    disabled.configure_timing({torch::Device(torch::kCPU), 1, false});
    rfdetr::ModelOutputs disabled_outputs;
    const auto disabled_features = torch_api::ones({1, 1, 3, 2});
    disabled_outputs.main = oracle_layer(disabled_features);
    const auto disabled_targets = one_image_targets(torch_api::tensor({{0.5F, 0.5F, 0.2F, 0.25F}}),
                                                    torch_api::tensor({1}, torch_api::TensorOptions().dtype(torch_api::kInt64)));
    static_cast<void>(disabled.loss(disabled_outputs, disabled_targets, {torch_api::tensor(1.0F)}, true));
    const auto disabled_handoff = disabled.harvest_timing();
    REQUIRE(disabled_handoff.completed_leases == 0);
    REQUIRE(disabled_handoff.outstanding_leases == 0);

    if (!torch_api::cuda::is_available()) { return; }
    auto config = match_free_config();
    rfdetr::TrainingSupervisionImpl supervision(config);
    supervision.initialize(45);
    install_oracle_projections(supervision);
    supervision.to(torch::Device(torch::kCUDA));
    const auto features = torch_api::ones({1, 1, 3, 2}, torch_api::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA));
    rfdetr::ModelOutputs outputs;
    outputs.main = oracle_layer(features);
    outputs.main.pred_logits = outputs.main.pred_logits.to(torch::kCUDA);
    outputs.main.pred_boxes = outputs.main.pred_boxes.to(torch::kCUDA);
    const auto labels = torch_api::tensor({1}, torch_api::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA));
    const auto boxes =
        torch_api::tensor({{0.5F, 0.5F, 0.2F, 0.25F}}, torch_api::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA));
    const auto targets = one_image_targets(boxes, labels);
    const auto normalizer = rfdetr::DeviceLossNormalizer{torch_api::tensor(1.0F, boxes.options())};
    supervision.configure_timing({features.device(), 2, true});
    const auto record_match_free_step = [&] {
        supervision.begin_supervised_step_timing();
        supervision.begin_criterion_timing();
        static_cast<void>(supervision.loss(outputs, targets, normalizer, true));
        supervision.end_criterion_timing();
        supervision.end_supervised_step_timing();
    };
    record_match_free_step();
    record_match_free_step();
    REQUIRE_THROWS(supervision.begin_supervised_step_timing());
    REQUIRE_THROWS(supervision.configure_timing({features.device(), 1, true}));

    const auto first_handoff = supervision.harvest_timing();
    REQUIRE(first_handoff.completed_leases + first_handoff.outstanding_leases == 14);
    static_cast<void>(cudaDeviceSynchronize());
    const auto completed_handoff = supervision.harvest_timing();
    REQUIRE(first_handoff.completed_leases + completed_handoff.completed_leases == 14);
    REQUIRE(completed_handoff.outstanding_leases == 0);
    const auto repeated_handoff = supervision.harvest_timing();
    REQUIRE(repeated_handoff.completed_leases == 0);
    REQUIRE(repeated_handoff.outstanding_leases == 0);

    supervision.configure_timing({features.device(), 1, true});
    record_match_free_step();
    static_cast<void>(cudaDeviceSynchronize());
    const auto reused_handoff = supervision.harvest_timing();
    REQUIRE(reused_handoff.completed_leases == 7);
    REQUIRE(reused_handoff.outstanding_leases == 0);

    auto dn_config = production_denoising_config(rfdetr::TrainAssignmentKind::Hungarian, true);
    dn_config.dec_layers = 2;
    dn_config.aux_loss = true;
    dn_config.two_stage = true;
    torch_api::manual_seed(46);
    rfdetr::NativeRfDetrModel dn_model(dn_config);
    auto& dn_owner = rfdetr::detail::native_model_owner(dn_model);
    dn_owner.initialize_training_supervision(46);
    dn_owner.module().to(torch::Device(torch::kCUDA));
    dn_owner.module().train(true);
    dn_owner.set_force_pytorch_deformable_attn(true);
    dn_owner.configure_supervision_timing({torch::Device(torch::kCUDA), 1, true});
    const auto dn_float_options = torch_api::TensorOptions().dtype(torch_api::kFloat32).device(torch::kCUDA);
    const auto dn_integer_options = torch_api::TensorOptions().dtype(torch_api::kInt64).device(torch::kCUDA);
    const auto dn_targets = batched_targets({torch_api::tensor({{0.5F, 0.5F, 0.2F, 0.25F}}, dn_float_options)},
                                            {torch_api::tensor({1}, dn_integer_options)}, dn_config.num_queries);
    const auto dn_image = torch_api::zeros({3, 64, 64}, dn_float_options);
    const auto record_dn_step = [&](const std::uint64_t batch_sequence) {
        dn_owner.begin_supervised_step_timing();
        const auto dn_outputs =
            dn_owner.forward_with_denoising(rfdetr::nested_tensor_from_tensor_list({dn_image}), dn_targets, {46, 1, 0, batch_sequence});
        REQUIRE(dn_outputs.denoising.has_value());
        REQUIRE(dn_outputs.denoising->aux_outputs.size() == 1);
        REQUIRE(dn_outputs.enc_outputs.has_value());
        dn_owner.begin_criterion_timing();
        static_cast<void>(dn_owner.supervision_loss(dn_outputs, dn_targets, {torch_api::tensor(1.0F, dn_float_options)}, true));
        dn_owner.end_criterion_timing();
        dn_owner.end_supervised_step_timing();
    };
    record_dn_step(1);
    REQUIRE_THROWS(dn_owner.begin_supervised_step_timing());
    static_cast<void>(cudaDeviceSynchronize());
    const auto dn_completed_handoff = dn_owner.harvest_supervision_timing();
    REQUIRE(dn_completed_handoff.completed_leases == 5);
    REQUIRE(dn_completed_handoff.outstanding_leases == 0);
    record_dn_step(2);
    REQUIRE_THROWS(dn_owner.begin_supervised_step_timing());
    static_cast<void>(cudaDeviceSynchronize());
    const auto dn_reused_handoff = dn_owner.harvest_supervision_timing();
    REQUIRE(dn_reused_handoff.completed_leases == 5);
    REQUIRE(dn_reused_handoff.outstanding_leases == 0);

    auto combined_config = production_denoising_config(rfdetr::TrainAssignmentKind::MatchFree, true);
    torch_api::manual_seed(47);
    rfdetr::NativeRfDetrModel combined_model(combined_config);
    auto& combined_owner = rfdetr::detail::native_model_owner(combined_model);
    combined_owner.initialize_training_supervision(47);
    combined_owner.module().to(torch::Device(torch::kCUDA));
    combined_owner.module().train(true);
    combined_owner.set_force_pytorch_deformable_attn(true);
    combined_owner.configure_supervision_timing({torch::Device(torch::kCUDA), 1, true});
    combined_owner.begin_supervised_step_timing();
    const auto combined_outputs =
        combined_owner.forward_with_denoising(rfdetr::nested_tensor_from_tensor_list({dn_image.clone()}), dn_targets, {47, 1, 0, 1});
    combined_owner.begin_criterion_timing();
    static_cast<void>(combined_owner.supervision_loss(combined_outputs, dn_targets, {torch_api::tensor(1.0F, dn_float_options)}, true));
    combined_owner.end_criterion_timing();
    combined_owner.end_supervised_step_timing();
    REQUIRE_THROWS(combined_owner.begin_supervised_step_timing());
    static_cast<void>(cudaDeviceSynchronize());
    const auto combined_handoff = combined_owner.harvest_supervision_timing();
    REQUIRE(combined_handoff.completed_leases == 10);
    REQUIRE(combined_handoff.outstanding_leases == 0);

    dn_owner.configure_supervision_timing({torch::Device(torch::kCUDA), 1, false});
    const auto untimed_outputs =
        dn_owner.forward_with_denoising(rfdetr::nested_tensor_from_tensor_list({dn_image}), dn_targets, {46, 1, 0, 3});
    static_cast<void>(dn_owner.supervision_loss(untimed_outputs, dn_targets, {torch_api::tensor(1.0F, dn_float_options)}, true));
    const auto untimed_handoff = dn_owner.harvest_supervision_timing();
    REQUIRE(untimed_handoff.completed_leases == 0);
    REQUIRE(untimed_handoff.outstanding_leases == 0);

    dn_owner.configure_supervision_timing({torch::Device(torch::kCUDA), 1, true});
    auto invalid_device_targets = dn_targets;
    invalid_device_targets.all_boxes = invalid_device_targets.all_boxes.to(torch::kCPU);
    invalid_device_targets.all_labels = invalid_device_targets.all_labels.to(torch::kCPU);
    REQUIRE_THROWS(
        dn_owner.forward_with_denoising(rfdetr::nested_tensor_from_tensor_list({dn_image}), invalid_device_targets, {46, 1, 0, 4}));
    static_cast<void>(cudaDeviceSynchronize());
    const auto failed_prepare_handoff = dn_owner.harvest_supervision_timing();
    REQUIRE(failed_prepare_handoff.completed_leases == 1);
    REQUIRE(failed_prepare_handoff.outstanding_leases == 0);
}

void test_empty_and_retained_graphs_are_finite_and_independent() {
    auto config = match_free_config();
    rfdetr::TrainingSupervisionImpl supervision(config);
    supervision.initialize(47);
    install_oracle_projections(supervision);

    auto first_features = torch_api::rand({1, 1, 3, 2}).set_requires_grad(true);
    rfdetr::ModelOutputs first;
    first.main = oracle_layer(first_features);
    auto targets = one_image_targets(torch_api::tensor({{0.5F, 0.5F, 0.2F, 0.2F}}),
                                     torch_api::tensor({0}, torch_api::TensorOptions().dtype(torch_api::kInt64)));
    auto first_loss = supervision.loss(first, targets, {torch_api::tensor(1.0F)}, true).total;

    auto second_features = torch_api::rand({1, 1, 3, 2}).set_requires_grad(true);
    rfdetr::ModelOutputs second;
    second.main = oracle_layer(second_features);
    second.main.pred_logits = second.main.pred_logits.narrow(1, 0, 2);
    second.main.pred_boxes = second.main.pred_boxes.narrow(1, 0, 2);
    second.main.query_features = second_features.narrow(2, 0, 2);
    second.main.query_layout = rfdetr::SupervisedQueryLayout{1, 2};
    auto second_loss = supervision.loss(second, targets, {torch_api::tensor(1.0F)}, true).total;

    const auto empty_targets =
        batched_targets({torch_api::zeros({0, 4})}, {torch_api::zeros({0}, torch_api::TensorOptions().dtype(torch_api::kInt64))});
    const auto empty = supervision.loss(second, empty_targets, {torch_api::tensor(0.0F)}, true);
    REQUIRE(torch_api::isfinite(empty.total).item<bool>());
    REQUIRE(empty.total.item<float>() == 0.0F);

    second_loss.backward();
    first_loss.backward();
    REQUIRE(first_features.grad().defined());
    REQUIRE(second_features.grad().defined());
}

void test_production_match_free_boundary_captures_layers_and_empty_gradient_anchors() {
    const auto run_case = [](const bool include_auxiliary_and_encoder) {
        auto config = rfdetr::native_config_from_preset(rfdetr::model_presets().front());
        config.resolution = 64;
        config.segmentation = false;
        config.num_queries = 3;
        config.num_select = 3;
        config.group_detr = 1;
        config.dec_layers = include_auxiliary_and_encoder ? 2 : 1;
        config.aux_loss = include_auxiliary_and_encoder;
        config.two_stage = include_auxiliary_and_encoder;
        config.training_supervision.assignment = rfdetr::TrainAssignmentKind::MatchFree;

        torch_api::manual_seed(include_auxiliary_and_encoder ? 71 : 67);
        rfdetr::NativeRfDetrModel model(config);
        auto& owner = rfdetr::detail::native_model_owner(model);
        owner.module().train(true);
        owner.initialize_training_supervision(0xabcdefULL);
        const auto image = torch_api::rand({3, 64, 64});

        auto nonempty_outputs = owner.forward_for_match_free(rfdetr::nested_tensor_from_tensor_list({image}));
        REQUIRE(nonempty_outputs.aux_outputs.size() == (include_auxiliary_and_encoder ? 1U : 0U));
        REQUIRE(nonempty_outputs.enc_outputs.has_value() == include_auxiliary_and_encoder);
        retain_captured_output_gradients(nonempty_outputs);
        const auto targets = one_image_targets(torch_api::tensor({{0.5F, 0.5F, 0.25F, 0.25F}}),
                                               torch_api::tensor({0}, torch_api::TensorOptions().dtype(torch_api::kInt64)));
        const auto nonempty_loss = owner.supervision_loss(nonempty_outputs, targets, {torch_api::tensor(1.0F)}, true);
        REQUIRE(torch_api::isfinite(nonempty_loss.total).item<bool>());
        nonempty_loss.total.backward();
        const auto nonempty_output_gradients = output_gradient_evidence(nonempty_outputs, false);
        const auto nonempty_model_gradients = selected_model_gradient_evidence(owner.module(), include_auxiliary_and_encoder, false);
        const auto nonempty_optimizer_inventory = ordered_defined_trainable_gradients(owner.module());

        for (auto& parameter : owner.module().parameters(true)) {
            parameter.mutable_grad() = torch::Tensor();
        }

        auto empty_outputs = owner.forward_for_match_free(rfdetr::nested_tensor_from_tensor_list({image.clone()}));
        REQUIRE(empty_outputs.aux_outputs.size() == nonempty_outputs.aux_outputs.size());
        REQUIRE(empty_outputs.enc_outputs.has_value() == nonempty_outputs.enc_outputs.has_value());
        retain_captured_output_gradients(empty_outputs);
        const auto empty_targets = batched_targets(
            {torch_api::zeros({0, 4})}, {torch_api::zeros({0}, torch_api::TensorOptions().dtype(torch_api::kInt64))}, config.num_queries);
        const auto empty_loss = owner.supervision_loss(empty_outputs, empty_targets, {torch_api::tensor(0.0F)}, true);
        REQUIRE(torch_api::isfinite(empty_loss.total).item<bool>());
        REQUIRE(empty_loss.total.item<float>() == 0.0F);
        empty_loss.total.backward();
        const auto empty_output_gradients = output_gradient_evidence(empty_outputs, true);
        const auto empty_model_gradients = selected_model_gradient_evidence(owner.module(), include_auxiliary_and_encoder, true);
        const auto empty_optimizer_inventory = ordered_defined_trainable_gradients(owner.module());
        REQUIRE(empty_output_gradients == nonempty_output_gradients);
        REQUIRE(empty_model_gradients == nonempty_model_gradients);
        REQUIRE(empty_optimizer_inventory == nonempty_optimizer_inventory);
    };

    run_case(false);
    run_case(true);
}

void test_mixed_empty_images_use_safe_internal_padding_without_loss_contribution() {
    auto config = match_free_config();
    rfdetr::TrainingSupervisionImpl supervision(config);
    supervision.initialize(53);
    install_oracle_projections(supervision);
    // CLEANUP-IGNORE: This populated baseline is independent from the full-gradient prediction fixture.
    const auto single_features = torch_api::tensor({{{{0.8F, 0.1F}, {0.2F, 0.9F}, {0.4F, 0.3F}}}});
    // CLEANUP-IGNORE: Baseline output construction is independent from layer-reduction coverage.
    rfdetr::ModelOutputs single;
    single.main = oracle_layer(single_features);
    const auto single_targets = one_image_targets(torch_api::tensor({{0.5F, 0.5F, 0.2F, 0.25F}}),
                                                  torch_api::tensor({1}, torch_api::TensorOptions().dtype(torch_api::kInt64)));
    const auto expected = supervision.loss(single, single_targets, {torch_api::tensor(1.0F)}, true).total.detach();

    rfdetr::ModelOutputs mixed;
    mixed.main = oracle_layer(single_features.repeat({2, 1, 1, 1}));
    mixed.main.pred_logits = mixed.main.pred_logits.repeat({2, 1, 1});
    mixed.main.pred_boxes = mixed.main.pred_boxes.repeat({2, 1, 1});
    mixed.main.query_features = single_features.repeat({2, 1, 1, 1}).clone().set_requires_grad(true);
    auto mixed_targets = batched_targets(
        {torch_api::tensor({{0.5F, 0.5F, 0.2F, 0.25F}}),
         torch_api::tensor({{std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0F, -2.0F}})},
        {torch_api::tensor({1}, torch_api::TensorOptions().dtype(torch_api::kInt64)),
         torch_api::tensor({99}, torch_api::TensorOptions().dtype(torch_api::kInt64))});
    mixed_targets.counts[1] = 0;
    mixed_targets.target_counts.index_put_({1}, 0);
    mixed_targets.all_boxes = mixed_targets.all_boxes.detach().set_requires_grad(true);
    const auto actual = supervision.loss(mixed, mixed_targets, {torch_api::tensor(1.0F)}, true).total;
    REQUIRE(torch_api::isfinite(actual).item<bool>());
    REQUIRE(torch_api::allclose(actual.detach(), expected, 1.0e-5, 1.0e-5));
    actual.backward();
    require_finite_gradients({mixed_targets.all_boxes});
    REQUIRE(mixed_targets.all_boxes.grad().index({1}).abs().sum().item<float>() == 0.0F);
    REQUIRE(mixed.main.query_features->grad().index({1}).abs().sum().item<float>() == 0.0F);
}

void test_conditional_model_construction_preserves_rng_and_default_state_topology() {
    auto inactive_config = rfdetr::native_config_from_preset(rfdetr::model_presets().front());
    inactive_config.aux_loss = false;
    inactive_config.two_stage = false;
    inactive_config.segmentation = false;
    auto active_config = inactive_config;
    active_config.training_supervision.assignment = rfdetr::TrainAssignmentKind::MatchFree;

    torch_api::manual_seed(101);
    rfdetr::NativeRfDetrModel inactive(inactive_config);
    const auto inactive_rng = at::detail::getDefaultCPUGenerator().get_state().clone();
    torch_api::manual_seed(101);
    rfdetr::NativeRfDetrModel active(active_config);
    const auto active_rng = at::detail::getDefaultCPUGenerator().get_state().clone();
    REQUIRE(torch_api::equal(inactive_rng, active_rng));

    const auto inactive_parameters = rfdetr::detail::native_model_owner(inactive).module().named_parameters(true);
    const auto active_parameters = rfdetr::detail::native_model_owner(active).module().named_parameters(true);
    std::set<std::string> inactive_parameter_names;
    std::set<std::string> active_ordinary_parameter_names;
    for (const auto& parameter : inactive_parameters) {
        inactive_parameter_names.emplace(parameter.key());
        REQUIRE_FALSE(parameter.key().starts_with("training_supervision."));
        const auto* enabled_parameter = active_parameters.find(parameter.key());
        REQUIRE(enabled_parameter != nullptr);
        REQUIRE(torch_api::equal(parameter.value(), *enabled_parameter));
    }
    for (const auto& parameter : active_parameters) {
        if (!parameter.key().starts_with("training_supervision.")) { active_ordinary_parameter_names.emplace(parameter.key()); }
    }
    REQUIRE(active_ordinary_parameter_names == inactive_parameter_names);
    const auto inactive_buffers = rfdetr::detail::native_model_owner(inactive).module().named_buffers(true);
    const auto active_buffers = rfdetr::detail::native_model_owner(active).module().named_buffers(true);
    std::set<std::string> inactive_buffer_names;
    std::set<std::string> active_ordinary_buffer_names;
    for (const auto& buffer : inactive_buffers) {
        inactive_buffer_names.emplace(buffer.key());
        REQUIRE_FALSE(buffer.key().starts_with("training_supervision."));
        const auto* enabled_buffer = active_buffers.find(buffer.key());
        REQUIRE(enabled_buffer != nullptr);
        REQUIRE(torch_api::equal(buffer.value(), *enabled_buffer));
    }
    for (const auto& buffer : active_buffers) {
        if (!buffer.key().starts_with("training_supervision.")) { active_ordinary_buffer_names.emplace(buffer.key()); }
    }
    REQUIRE(active_ordinary_buffer_names == inactive_buffer_names);
    bool saw_supervision_parameter = false;
    for (const auto& parameter : active_parameters) {
        if (parameter.key().starts_with("training_supervision.")) {
            saw_supervision_parameter = true;
            REQUIRE_FALSE(parameter.key().find("value_projection") != std::string::npos);
        }
    }
    REQUIRE(saw_supervision_parameter);

    auto& inactive_owner = rfdetr::detail::native_model_owner(inactive);
    auto& active_owner = rfdetr::detail::native_model_owner(active);
    inactive_owner.module().eval();
    active_owner.module().eval();
    torch::NoGradGuard no_grad;
    const auto image = torch_api::zeros({3, 64, 64});
    const auto inactive_outputs = inactive_owner.forward(rfdetr::nested_tensor_from_tensor_list({image}), false);
    const auto active_outputs = active_owner.forward(rfdetr::nested_tensor_from_tensor_list({image.clone()}), false);
    require_layer_equal(inactive_outputs.main, active_outputs.main);
    REQUIRE(inactive_outputs.aux_outputs.size() == active_outputs.aux_outputs.size());
    REQUIRE(inactive_outputs.enc_outputs.has_value() == active_outputs.enc_outputs.has_value());
    REQUIRE_FALSE(inactive_outputs.main.query_features.has_value());
    REQUIRE_FALSE(active_outputs.main.query_features.has_value());

    const auto targets = one_image_targets(torch_api::tensor({{0.5F, 0.5F, 0.25F, 0.25F}}),
                                           torch_api::tensor({0}, torch_api::TensorOptions().dtype(torch_api::kInt64)));
    const auto retained_config = detection_config(inactive_config);
    const auto inactive_matches = rfdetr::matcher_indices(inactive_outputs, targets, retained_config, false);
    const auto active_matches = rfdetr::matcher_indices(active_outputs, targets, retained_config, false);
    REQUIRE(inactive_matches.size() == active_matches.size());
    for (std::size_t index = 0; index < inactive_matches.size(); ++index) {
        REQUIRE(torch_api::equal(inactive_matches[index].first, active_matches[index].first));
        REQUIRE(torch_api::equal(inactive_matches[index].second, active_matches[index].second));
    }
    const auto inactive_losses = rfdetr::detection_loss_dict(inactive_outputs, targets, retained_config, false, 1.0);
    const auto active_losses = rfdetr::detection_loss_dict(active_outputs, targets, retained_config, false, 1.0);
    REQUIRE(inactive_losses.size() == active_losses.size());
    for (const auto& [name, value] : inactive_losses) {
        REQUIRE(active_losses.contains(name));
        REQUIRE(torch_api::equal(value, active_losses.at(name)));
    }
}

void test_dn_endpoint_transform_mapping_padding_and_one_class_behavior() {
    auto config = denoising_config();
    config.training_supervision.denoising.label_noise_ratio = 1.0F;
    rfdetr::TrainingSupervisionImpl supervision(config);
    supervision.initialize(101);
    const auto boxes =
        std::vector<torch::Tensor>{torch_api::tensor({{0.02F, 0.98F, 0.2F, 0.4F}, {0.5F, 0.5F, 0.1F, 0.2F}}), torch_api::zeros({0, 4})};
    const auto labels = std::vector<torch::Tensor>{torch_api::tensor({0, 2}, torch_api::TensorOptions().dtype(torch_api::kInt64)),
                                                   torch_api::zeros({0}, torch_api::TensorOptions().dtype(torch_api::kInt64))};
    const auto targets = batched_targets(boxes, labels);
    const std::vector<int64_t> slot_shape{2, 2, 2};
    rfdetr::DenoisingVariates variates;
    variates.center = torch_api::tensor({0.0F, 1.0F}).view({1, 1, 1, 2}).expand({2, 2, 2, 2}).clone();
    variates.size = torch_api::tensor({0.0F, 1.0F}).view({1, 1, 1, 2}).expand({2, 2, 2, 2}).clone();
    variates.label_flip = torch_api::zeros(slot_shape);
    variates.other_label =
        torch_api::tensor({0, 1, 1, 0, 0, 1, 1, 0}, torch_api::TensorOptions().dtype(torch_api::kInt64)).view(slot_shape);
    const auto prepared = supervision.prepare_denoising(targets, {9, 3, 2, 7}, torch::Device(torch::kCPU), torch::kFloat32, &variates);
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->layout.ordinary.total_queries() == 4);
    REQUIRE(prepared->layout.denoising_queries() == 4);
    REQUIRE(prepared->content.sizes().vec() == std::vector<int64_t>{2, 4, 4});
    REQUIRE(prepared->normalized_references.sizes().vec() == std::vector<int64_t>{2, 4, 4});
    const auto transformed = rfdetr::transform_denoising_targets(prepared->original_labels, prepared->original_boxes, prepared->valid_slots,
                                                                 config.num_classes - 1, config.training_supervision.denoising, variates);
    const auto expanded_extents = prepared->original_boxes.slice(-1, 2, 4);
    const auto sampled_bounds = expanded_extents * (0.5F * config.training_supervision.denoising.center_noise_scale);
    const auto valid_coordinates = prepared->valid_slots.unsqueeze(-1).expand_as(sampled_bounds);
    REQUIRE((transformed.center_offsets.abs().index({valid_coordinates}) < sampled_bounds.index({valid_coordinates})).all().item<bool>());
    REQUIRE(transformed.extent_scales.min().item<float>() >= 1.0F - config.training_supervision.denoising.size_noise_scale);
    REQUIRE(transformed.extent_scales.max().item<float>() <=
            std::nextafter(1.0F + config.training_supervision.denoising.size_noise_scale, std::numeric_limits<float>::infinity()));
    REQUIRE(prepared->valid_slots.index({0}).all().item<bool>());
    REQUIRE_FALSE(prepared->valid_slots.index({1}).any().item<bool>());
    REQUIRE(int64_values(prepared->target_indices.index({0, 0})) == std::vector<int64_t>{0, 1});
    REQUIRE(int64_values(prepared->target_indices.index({0, 1})) == std::vector<int64_t>{0, 1});
    REQUIRE((prepared->target_indices.index({1}) == -1).all().item<bool>());
    REQUIRE(prepared->layout.denoising_key_padding.index({1, 0, 0}).item<bool>() == false);
    REQUIRE(prepared->layout.denoising_key_padding.index({1, 0, 1}).item<bool>());
    REQUIRE(prepared->content.index({1}).abs().sum().item<float>() == 0.0F);
    auto sentinel_attention = torch::nn::MultiheadAttention(torch::nn::MultiheadAttentionOptions(4, 2).dropout(0.0));
    const auto decoder_target = torch_api::cat({torch_api::zeros({2, 4, 4}), prepared->content}, 1);
    const auto sentinel_output =
        rfdetr::isolated_group_self_attention(sentinel_attention, decoder_target, torch_api::zeros_like(decoder_target), prepared->layout);
    REQUIRE(torch_api::isfinite(sentinel_output).all().item<bool>());

    const auto source_extent = boxes[0].index({0, Slice(2, 4)});
    const auto center_bound = source_extent * (0.5F * config.training_supervision.denoising.center_noise_scale);
    const auto actual_offset = prepared->normalized_references.index({0, 0, Slice(0, 2)}) - boxes[0].index({0, Slice(0, 2)});
    REQUIRE(actual_offset.index({0}).item<float>() > -center_bound.index({0}).item<float>());
    REQUIRE(actual_offset.index({1}).item<float>() < center_bound.index({1}).item<float>());
    const auto second_bound = boxes[0].index({1, Slice(2, 4)}) * (0.5F * config.training_supervision.denoising.center_noise_scale);
    REQUIRE(transformed.center_offsets.index({0, 0, 1, 0}).item<float>() == -std::nextafter(second_bound.index({0}).item<float>(), 0.0F));
    REQUIRE(transformed.center_offsets.index({0, 0, 1, 1}).item<float>() == std::nextafter(second_bound.index({1}).item<float>(), 0.0F));
    const auto expected_extent =
        boxes[0].index({1, Slice(2, 4)}) * torch_api::tensor({1.0F - config.training_supervision.denoising.size_noise_scale,
                                                              1.0F + config.training_supervision.denoising.size_noise_scale});
    REQUIRE(torch_api::allclose(prepared->normalized_references.index({0, 1, Slice(2, 4)}), expected_extent));
    REQUIRE(prepared->normalized_references.min().item<float>() >= 0.0F);
    REQUIRE(prepared->normalized_references.max().item<float>() <= 1.0F);
    REQUIRE(torch_api::allclose(prepared->original_boxes.index({0, 0}), boxes[0]));
    REQUIRE(torch_api::equal(prepared->original_labels.index({0, 0}), labels[0]));
    REQUIRE(int64_values(transformed.labels.index({0, 0})) == std::vector<int64_t>{1, 1});
    REQUIRE(int64_values(transformed.labels.index({0, 1})) == std::vector<int64_t>{2, 0});
    REQUIRE(transformed.labels.max().item<int64_t>() < config.num_classes - 1);
    REQUIRE(prepared->original_labels.index({0, 0, 0}).item<int64_t>() == 0);
    REQUIRE(prepared->original_labels.index({0, 0, 1}).item<int64_t>() == 2);
    auto open_upper = variates;
    open_upper.label_flip = torch_api::ones(slot_shape);
    const auto open_upper_unflipped =
        rfdetr::transform_denoising_targets(prepared->original_labels, prepared->original_boxes, prepared->valid_slots,
                                            config.num_classes - 1, config.training_supervision.denoising, open_upper);
    REQUIRE(torch_api::equal(open_upper_unflipped.labels, prepared->original_labels));

    const auto distinct_labels = torch_api::tensor({{{0, 2}}}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    const auto distinct_boxes = torch_api::tensor({{{{0.4F, 0.6F, 0.2F, 0.4F}, {0.7F, 0.3F, 0.1F, 0.2F}}}});
    const auto distinct_valid = torch_api::ones({1, 1, 2}, torch_api::TensorOptions().dtype(torch_api::kBool));
    rfdetr::DenoisingVariates distinct;
    distinct.center = torch_api::tensor({0.125F, 0.875F, 0.25F, 0.75F}).view({1, 1, 2, 2});
    distinct.size = torch_api::tensor({0.2F, 0.8F, 0.3F, 0.7F}).view({1, 1, 2, 2});
    distinct.label_flip = torch_api::tensor({{{0.49F, 1.0F}}});
    distinct.other_label = torch_api::zeros({1, 1, 2}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    const auto independently_noised = rfdetr::transform_denoising_targets(distinct_labels, distinct_boxes, distinct_valid, 3,
                                                                          config.training_supervision.denoising, distinct);
    const auto expected_center_offsets = (distinct.center * 2.0F - 1.0F) * distinct_boxes.slice(-1, 2, 4) *
                                         (0.5F * config.training_supervision.denoising.center_noise_scale);
    const auto expected_extent_scales = 1.0F - config.training_supervision.denoising.size_noise_scale +
                                        2.0F * config.training_supervision.denoising.size_noise_scale * distinct.size;
    REQUIRE(torch_api::allclose(independently_noised.center_offsets, expected_center_offsets));
    REQUIRE(torch_api::allclose(independently_noised.extent_scales, expected_extent_scales));
    REQUIRE(int64_values(independently_noised.labels) == std::vector<int64_t>{1, 2});

    auto one_class_config = denoising_config(1);
    one_class_config.training_supervision.denoising.label_noise_ratio = 1.0F;
    rfdetr::TrainingSupervisionImpl one_class(one_class_config);
    // CLEANUP-IGNORE: The one-class noise oracle and reference-convention test exercise different invariants.
    one_class.initialize(102);
    const auto one_targets = batched_targets({torch_api::tensor({{0.5F, 0.5F, 0.2F, 0.2F}})},
                                             {torch_api::tensor({0}, torch_api::TensorOptions().dtype(torch_api::kInt64))});
    rfdetr::DenoisingVariates one_variates;
    one_variates.center = torch_api::full({1, 2, 1, 2}, 0.5F);
    one_variates.size = torch_api::full({1, 2, 1, 2}, 0.5F);
    one_variates.label_flip = torch_api::zeros({1, 2, 1});
    const auto one_prepared =
        one_class.prepare_denoising(one_targets, {1, 2, 3, 4}, torch::Device(torch::kCPU), torch::kFloat32, &one_variates);
    REQUIRE(one_prepared.has_value());
    REQUIRE((one_prepared->original_labels == 0).all().item<bool>());
    const auto one_transformed =
        rfdetr::transform_denoising_targets(one_prepared->original_labels, one_prepared->original_boxes, one_prepared->valid_slots, 1,
                                            one_class_config.training_supervision.denoising, one_variates);
    REQUIRE((one_transformed.labels == 0).all().item<bool>());

    auto zero_flip_config = config;
    zero_flip_config.training_supervision.denoising.label_noise_ratio = 0.0F;
    const auto zero_ratio_unflipped =
        rfdetr::transform_denoising_targets(prepared->original_labels, prepared->original_boxes, prepared->valid_slots,
                                            zero_flip_config.num_classes - 1, zero_flip_config.training_supervision.denoising, variates);
    REQUIRE(torch_api::equal(zero_ratio_unflipped.labels, prepared->original_labels));

    const auto all_empty = batched_targets({torch_api::zeros({0, 4}), torch_api::zeros({0, 4})},
                                           {torch_api::zeros({0}, torch_api::TensorOptions().dtype(torch_api::kInt64)),
                                            torch_api::zeros({0}, torch_api::TensorOptions().dtype(torch_api::kInt64))});
    REQUIRE_FALSE(supervision.prepare_denoising(all_empty, {9, 3, 2, 7}, torch::Device(torch::kCPU), torch::kFloat32));
}

void test_dn_center_underflow_other_label_bijection_and_step_determinism() {
    auto config = denoising_config();
    config.training_supervision.denoising.center_noise_scale = rfdetr::kSupervisionOpenUnitMinimum;
    config.training_supervision.denoising.label_noise_ratio = 1.0F;
    rfdetr::TrainingSupervisionImpl supervision(config);
    supervision.initialize(103);
    const auto targets =
        batched_targets({torch_api::tensor({{0.5F, 0.5F, 0.2F, 0.3F}, {0.4F, 0.6F, 0.1F, 0.15F}, {0.7F, 0.3F, 0.25F, 0.2F}})},
                        {torch_api::tensor({0, 1, 2}, torch_api::TensorOptions().dtype(torch_api::kInt64))});
    rfdetr::DenoisingVariates variates;
    variates.center = torch_api::zeros({1, 2, 3, 2});
    variates.size = torch_api::full({1, 2, 3, 2}, 0.5F);
    variates.label_flip = torch_api::zeros({1, 2, 3});
    variates.other_label = torch_api::tensor({0, 0, 1, 1, 1, 0}, torch_api::TensorOptions().dtype(torch_api::kInt64)).view({1, 2, 3});
    const auto prepared = supervision.prepare_denoising(targets, {1, 0, 0, 0}, torch::Device(torch::kCPU), torch::kFloat32, &variates);
    REQUIRE(prepared.has_value());
    const auto transformed = rfdetr::transform_denoising_targets(prepared->original_labels, prepared->original_boxes, prepared->valid_slots,
                                                                 config.num_classes - 1, config.training_supervision.denoising, variates);
    const auto transformed_centers = transformed.boxes.view({1, 2, 3, 4}).slice(-1, 0, 2);
    const auto source_centers = targets.all_boxes.view({1, 1, 3, 4}).slice(-1, 0, 2).expand({1, 2, 3, 2});
    REQUIRE(torch_api::equal(transformed_centers, source_centers));
    REQUIRE(int64_values(transformed.labels.index({0, 0})) == std::vector<int64_t>{1, 0, 1});
    REQUIRE(int64_values(transformed.labels.index({0, 1})) == std::vector<int64_t>{2, 2, 0});
    const auto noised_labels = supervision.named_parameters(true)["denoising_label_embedding.weight"];
    REQUIRE(noised_labels.size(0) == 3);

    auto deterministic_config = denoising_config();
    rfdetr::TrainingSupervisionImpl deterministic(deterministic_config);
    deterministic.initialize(104);
    const rfdetr::TrainingStepIdentity identity{55, 8, 3, 144};
    const auto first = deterministic.prepare_denoising(targets, identity, torch::Device(torch::kCPU), torch::kFloat32);
    const auto intervening = deterministic.prepare_denoising(targets, {55, 8, 3, 145}, torch::Device(torch::kCPU), torch::kFloat32);
    const auto repeated = deterministic.prepare_denoising(targets, identity, torch::Device(torch::kCPU), torch::kFloat32);
    REQUIRE(first.has_value());
    REQUIRE(intervening.has_value());
    REQUIRE(repeated.has_value());
    REQUIRE(torch_api::equal(first->content, repeated->content));
    REQUIRE(torch_api::equal(first->normalized_references, repeated->normalized_references));
    REQUIRE(torch_api::equal(first->target_indices, repeated->target_indices));
    REQUIRE_FALSE(torch_api::equal(first->normalized_references, intervening->normalized_references));
}

torch::Tensor dense_group_attention(torch::nn::MultiheadAttention& attention, const torch::Tensor& target, const torch::Tensor& position,
                                    const std::vector<int64_t>& group_widths, bool allow_dn_to_ordinary) {
    const int64_t total = target.size(1);
    auto mask = torch_api::full({total, total}, -std::numeric_limits<float>::infinity(), target.options());
    int64_t offset = 0;
    for (const auto width : group_widths) {
        mask.index_put_({Slice(offset, offset + width), Slice(offset, offset + width)}, 0.0F);
        offset += width;
    }
    if (allow_dn_to_ordinary && group_widths.size() > 1) {
        const int64_t ordinary = group_widths.front();
        mask.index_put_({Slice(ordinary, total), Slice(0, ordinary)}, 0.0F);
    }
    const auto q = (target + position).transpose(0, 1);
    const auto v = target.transpose(0, 1);
    return std::get<0>(attention->forward(q, q, v, {}, false, mask)).transpose(0, 1);
}

void test_split_self_attention_matches_equation_seven_and_symmetric_group_layout() {
    torch_api::manual_seed(105);
    // CLEANUP-IGNORE: Equation-seven and symmetric layouts are independent topology oracles in this proof.
    auto attention = torch::nn::MultiheadAttention(torch::nn::MultiheadAttentionOptions(4, 2).dropout(0.0));
    const auto target = torch_api::rand({1, 6, 4}).set_requires_grad(true);
    const auto position = torch_api::rand({1, 6, 4});
    rfdetr::DecoderQueryLayout equation_layout;
    equation_layout.ordinary = {1, 2};
    equation_layout.denoising_groups = 2;
    equation_layout.denoising_queries_per_group = 2;
    equation_layout.denoising_key_padding = torch_api::zeros({1, 2, 2}, torch_api::TensorOptions().dtype(torch_api::kBool));
    equation_layout.denoising_valid_slots = torch_api::ones({1, 2, 2}, torch_api::TensorOptions().dtype(torch_api::kBool));
    const auto split = rfdetr::isolated_group_self_attention(attention, target, position, equation_layout);
    const auto equation_seven = dense_group_attention(attention, target, position, {2, 2, 2}, true);
    CAPTURE((split.narrow(1, 0, 2) - equation_seven.narrow(1, 0, 2)).abs().max().item<float>());
    REQUIRE(torch_api::allclose(split.narrow(1, 0, 2), equation_seven.narrow(1, 0, 2), 1.0e-6, 1.0e-6));

    const auto grouped_target = torch_api::rand({1, 10, 4}).set_requires_grad(true);
    const auto grouped_position = torch_api::rand({1, 10, 4});
    rfdetr::DecoderQueryLayout grouped_layout;
    grouped_layout.ordinary = {2, 3};
    grouped_layout.denoising_groups = 2;
    grouped_layout.denoising_queries_per_group = 2;
    grouped_layout.denoising_key_padding = torch_api::zeros({1, 2, 2}, torch_api::TensorOptions().dtype(torch_api::kBool));
    grouped_layout.denoising_valid_slots = torch_api::ones({1, 2, 2}, torch_api::TensorOptions().dtype(torch_api::kBool));
    const auto grouped_split = rfdetr::isolated_group_self_attention(attention, grouped_target, grouped_position, grouped_layout);
    const auto symmetric = dense_group_attention(attention, grouped_target, grouped_position, {3, 3, 2, 2}, false);
    REQUIRE(torch_api::allclose(grouped_split, symmetric, 1.0e-6, 1.0e-6));

    auto mutated = grouped_target.detach().clone();
    mutated.index_put_({0, Slice(6, 8)}, mutated.index({0, Slice(6, 8)}) + 100.0F);
    const auto mutated_output = rfdetr::isolated_group_self_attention(attention, mutated, grouped_position, grouped_layout);
    REQUIRE(torch_api::allclose(grouped_split.narrow(1, 0, 6), mutated_output.narrow(1, 0, 6), 1.0e-6, 1.0e-6));
    REQUIRE(torch_api::allclose(grouped_split.narrow(1, 8, 2), mutated_output.narrow(1, 8, 2), 1.0e-6, 1.0e-6));
    auto ordinary_mutated = grouped_target.detach().clone();
    ordinary_mutated.index_put_({0, Slice(0, 3)}, ordinary_mutated.index({0, Slice(0, 3)}) - 100.0F);
    const auto ordinary_mutated_output =
        rfdetr::isolated_group_self_attention(attention, ordinary_mutated, grouped_position, grouped_layout);
    REQUIRE(torch_api::allclose(grouped_split.narrow(1, 3, 7), ordinary_mutated_output.narrow(1, 3, 7), 1.0e-6, 1.0e-6));
    grouped_split.narrow(1, 6, 2).sum().backward();
    REQUIRE(grouped_target.grad().narrow(1, 6, 2).abs().sum().item<float>() > 0.0F);
    REQUIRE(grouped_target.grad().narrow(1, 0, 6).abs().sum().item<float>() == 0.0F);
    REQUIRE(grouped_target.grad().narrow(1, 8, 2).abs().sum().item<float>() == 0.0F);
}

void test_dn_direct_loss_auxiliary_gating_padding_and_retained_graphs() {
    auto config = denoising_config();
    config.aux_loss = false;
    config.cls_loss_coef = 2.0;
    config.bbox_loss_coef = 5.0;
    config.giou_loss_coef = 2.0;
    rfdetr::TrainingSupervisionImpl supervision(config);
    supervision.initialize(106);
    const auto targets = batched_targets({torch_api::tensor({{0.5F, 0.5F, 0.2F, 0.2F}}), torch_api::zeros({0, 4})},
                                         {torch_api::tensor({1}, torch_api::TensorOptions().dtype(torch_api::kInt64)),
                                          torch_api::zeros({0}, torch_api::TensorOptions().dtype(torch_api::kInt64))});
    const auto prepared = supervision.prepare_denoising(targets, {7, 2, 1, 9}, torch::Device(torch::kCPU), torch::kFloat32);
    REQUIRE(prepared.has_value());

    auto make_outputs = [&](const rfdetr::DenoisingQueryBatch& prepared_batch, float shift) {
        rfdetr::ModelOutputs outputs;
        rfdetr::DenoisingOutputs denoising;
        denoising.groups = prepared_batch.layout.denoising_groups;
        denoising.queries_per_group = prepared_batch.layout.denoising_queries_per_group;
        denoising.original_labels = prepared_batch.original_labels;
        denoising.original_boxes = prepared_batch.original_boxes;
        denoising.valid_slots = prepared_batch.valid_slots;
        denoising.target_indices = prepared_batch.target_indices;
        const int64_t batch = prepared_batch.valid_slots.size(0);
        const int64_t groups = prepared_batch.layout.denoising_groups;
        const int64_t queries = prepared_batch.layout.denoising_queries_per_group;
        denoising.main.pred_logits = torch_api::full({batch, groups, queries, 4}, shift).set_requires_grad(true);
        denoising.main.pred_boxes = (prepared_batch.original_boxes + shift).clone().set_requires_grad(true);
        denoising.aux_outputs.push_back({torch_api::full({batch, groups, queries, 4}, shift + 0.1F).set_requires_grad(true),
                                         (prepared_batch.original_boxes + shift + 0.1F).clone().set_requires_grad(true)});
        outputs.denoising = std::move(denoising);
        outputs.main.pred_logits = torch_api::zeros({batch, 4, 4}).set_requires_grad(true);
        outputs.main.pred_boxes = torch_api::zeros({batch, 4, 4}).set_requires_grad(true);
        outputs.main.pred_masks = torch_api::zeros({batch, 4, 2, 2}).set_requires_grad(true);
        rfdetr::OutputLayer encoder;
        encoder.pred_logits = torch_api::zeros({batch, 4, 4}).set_requires_grad(true);
        encoder.pred_boxes = torch_api::zeros({batch, 4, 4}).set_requires_grad(true);
        outputs.enc_outputs = std::move(encoder);
        return outputs;
    };

    auto first_outputs = make_outputs(*prepared, 0.05F);
    const auto first = supervision.loss(first_outputs, targets, {torch_api::tensor(1.0F)}, true);
    REQUIRE(torch_api::allclose(first.total, first.classification + first.box + first.giou));
    REQUIRE(torch_api::equal(first.total, first.denoising));
    first.total.backward();
    REQUIRE(first_outputs.denoising->main.pred_logits.grad().defined());
    REQUIRE(first_outputs.denoising->main.pred_boxes.grad().defined());
    REQUIRE_FALSE(first_outputs.denoising->aux_outputs.front().pred_logits.grad().defined());
    REQUIRE(first_outputs.denoising->main.pred_logits.grad().index({1}).abs().sum().item<float>() == 0.0F);
    REQUIRE_FALSE(first_outputs.main.pred_logits.grad().defined());
    REQUIRE_FALSE(first_outputs.main.pred_masks->grad().defined());
    REQUIRE_FALSE(first_outputs.enc_outputs->pred_logits.grad().defined());

    auto auxiliary_config = config;
    auxiliary_config.aux_loss = true;
    rfdetr::TrainingSupervisionImpl auxiliary_supervision(auxiliary_config);
    auxiliary_supervision.initialize(106);
    auto auxiliary_outputs = make_outputs(*prepared, 0.05F);
    const auto auxiliary_loss = auxiliary_supervision.loss(auxiliary_outputs, targets, {torch_api::tensor(1.0F)}, true);
    REQUIRE(auxiliary_loss.total.item<float>() > first.total.item<float>());
    auxiliary_loss.total.backward();
    REQUIRE(auxiliary_outputs.denoising->aux_outputs.front().pred_logits.grad().defined());
    REQUIRE(auxiliary_outputs.denoising->aux_outputs.front().pred_boxes.grad().defined());

    // CLEANUP-IGNORE: Retained output creation is independent from production DN gradient retention.
    auto retained_first = make_outputs(*prepared, 0.02F);
    // CLEANUP-IGNORE: This retained graph transition has its own target topology and lifetime assertions.
    const auto retained_first_loss = supervision.loss(retained_first, targets, {torch_api::tensor(1.0F)}, true).total;
    const auto other_targets =
        batched_targets({torch_api::tensor({{0.4F, 0.4F, 0.1F, 0.1F}, {0.7F, 0.7F, 0.2F, 0.2F}}), torch_api::zeros({0, 4})},
                        {torch_api::tensor({0, 2}, torch_api::TensorOptions().dtype(torch_api::kInt64)),
                         torch_api::zeros({0}, torch_api::TensorOptions().dtype(torch_api::kInt64))});
    const auto other_prepared = supervision.prepare_denoising(other_targets, {7, 2, 1, 10}, torch::Device(torch::kCPU), torch::kFloat32);
    REQUIRE(other_prepared.has_value());
    auto retained_second = make_outputs(*other_prepared, 0.08F);
    const auto retained_second_loss = supervision.loss(retained_second, other_targets, {torch_api::tensor(2.0F)}, true).total;
    static_cast<void>(supervision.prepare_denoising(targets, {7, 2, 1, 11}, torch::Device(torch::kCPU), torch::kFloat32));
    retained_second_loss.backward();
    retained_first_loss.backward();
    REQUIRE(retained_first.denoising->main.pred_boxes.grad().defined());
    REQUIRE(retained_second.denoising->main.pred_boxes.grad().defined());
}

void test_production_dn_forward_preserves_reference_and_output_boundaries() {
    const auto run_case = [](const rfdetr::TrainAssignmentKind assignment, const bool bbox_reparam, const bool segmentation,
                             const std::uint64_t seed) {
        auto config = production_denoising_config(assignment, bbox_reparam, segmentation);
        torch_api::manual_seed(seed);
        rfdetr::NativeRfDetrModel model(config);
        auto& owner = rfdetr::detail::native_model_owner(model);
        owner.module().train(true);
        owner.initialize_training_supervision(seed + 1U);

        const auto boxes = torch_api::tensor({{0.25F, 0.75F, 0.2F, 0.4F}, {0.7F, 0.35F, 0.15F, 0.1F}});
        const auto labels = torch_api::tensor({0, 1}, torch_api::TensorOptions().dtype(torch_api::kInt64));
        const auto targets = batched_targets({boxes}, {labels}, config.num_queries);
        const rfdetr::TrainingStepIdentity identity{seed, 7, 2, 19};
        const auto image = torch_api::rand({3, 64, 64});

        auto outputs = owner.forward_with_denoising(rfdetr::nested_tensor_from_tensor_list({image}), targets, identity);
        REQUIRE(outputs.main.pred_logits.size(1) == config.num_queries);
        REQUIRE(outputs.main.pred_boxes.size(1) == config.num_queries);
        REQUIRE(outputs.denoising.has_value());
        const auto& denoising = *outputs.denoising;
        REQUIRE(denoising.groups == 2);
        REQUIRE(denoising.queries_per_group == 2);
        REQUIRE(denoising.main.pred_logits.sizes().vec() == std::vector<int64_t>{1, 2, 2, config.num_classes});
        REQUIRE(denoising.main.pred_boxes.sizes().vec() == std::vector<int64_t>{1, 2, 2, 4});
        REQUIRE(torch_api::equal(denoising.original_labels.index({0, 0}), labels));
        REQUIRE(torch_api::allclose(denoising.original_boxes.index({0, 0}), boxes));
        REQUIRE(int64_values(denoising.target_indices.index({0, 0})) == std::vector<int64_t>{0, 1});
        REQUIRE(int64_values(denoising.target_indices.index({0, 1})) == std::vector<int64_t>{0, 1});
        REQUIRE(outputs.main.query_features.has_value() == (assignment == rfdetr::TrainAssignmentKind::MatchFree));
        if (outputs.main.query_features) { REQUIRE(outputs.main.query_features->size(2) == config.num_queries); }
        REQUIRE(outputs.enc_outputs.has_value() == segmentation);
        if (outputs.enc_outputs) { REQUIRE(outputs.enc_outputs->pred_logits.size(1) == config.num_queries); }
        REQUIRE(outputs.main.sparse_pred_masks.has_value() == segmentation);
        if (outputs.main.sparse_pred_masks) { REQUIRE(outputs.main.sparse_pred_masks->query_features.size(1) == config.num_queries); }

        rfdetr::TrainingSupervisionImpl oracle(config);
        oracle.initialize(seed + 1U);
        const auto prepared = oracle.prepare_denoising(targets, identity, torch::Device(torch::kCPU), torch::kFloat32);
        REQUIRE(prepared.has_value());
        // The production bbox head starts with a zero final layer. Equality to
        // the normalized preparation proves that direct references reach
        // reparameterized refinement and inverse-sigmoid references reach
        // additive refinement before the common normalized output.
        REQUIRE(torch_api::allclose(denoising.main.pred_boxes, prepared->normalized_references.view_as(denoising.main.pred_boxes), 1.0e-6,
                                    1.0e-6));

        const auto changed_identity = rfdetr::TrainingStepIdentity{seed, 7, 2, 20};
        const auto changed =
            owner.forward_with_denoising(rfdetr::nested_tensor_from_tensor_list({image.clone()}), targets, changed_identity);
        REQUIRE(torch_api::allclose(outputs.main.pred_logits, changed.main.pred_logits, 1.0e-6, 1.0e-6));
        REQUIRE(torch_api::allclose(outputs.main.pred_boxes, changed.main.pred_boxes, 1.0e-6, 1.0e-6));
        REQUIRE_FALSE(torch_api::equal(outputs.denoising->main.pred_boxes, changed.denoising->main.pred_boxes));

        owner.module().eval();
        torch::NoGradGuard no_grad;
        const auto inference = owner.forward(rfdetr::nested_tensor_from_tensor_list({image.clone()}), segmentation);
        REQUIRE_FALSE(inference.denoising.has_value());
        REQUIRE(inference.main.pred_logits.size(1) == config.num_queries);
        rfdetr::OutputTensors postprocess_input;
        postprocess_input.pred_logits = inference.main.pred_logits;
        postprocess_input.pred_boxes = inference.main.pred_boxes;
        const auto postprocessed = rfdetr::postprocess_outputs_fixed_size(postprocess_input, 64, 64, config.num_select);
        REQUIRE(postprocessed.size() == 1);
        REQUIRE(postprocessed.front().at("boxes").size(0) == config.num_select);
    };

    run_case(rfdetr::TrainAssignmentKind::Hungarian, true, true, 112);
    run_case(rfdetr::TrainAssignmentKind::MatchFree, false, false, 113);
}

void test_production_dn_retained_graph_scratch_padding_and_gradients() {
    auto config = production_denoising_config(rfdetr::TrainAssignmentKind::MatchFree, true);
    config.aux_loss = true;
    config.dec_layers = 2;
    torch_api::manual_seed(114);
    rfdetr::NativeRfDetrModel model(config);
    auto& owner = rfdetr::detail::native_model_owner(model);
    owner.module().train(true);
    owner.initialize_training_supervision(0x5a5aULL);
    const auto image = torch_api::rand({3, 64, 64});

    const auto first_targets =
        batched_targets({torch_api::tensor({{0.4F, 0.6F, 0.2F, 0.25F}})},
                        {torch_api::tensor({0}, torch_api::TensorOptions().dtype(torch_api::kInt64))}, config.num_queries);
    auto first = owner.forward_with_denoising(rfdetr::nested_tensor_from_tensor_list({image}), first_targets, {71, 3, 1, 4});
    REQUIRE(first.denoising.has_value());
    first.denoising->main.pred_logits.retain_grad();
    first.denoising->main.pred_boxes.retain_grad();
    REQUIRE(first.denoising->aux_outputs.size() == 1);
    first.denoising->aux_outputs.front().pred_logits.retain_grad();
    first.denoising->aux_outputs.front().pred_boxes.retain_grad();
    const auto first_loss = owner.supervision_loss(first, first_targets, {torch_api::tensor(1.0F)}, true).total;

    const auto second_targets =
        batched_targets({torch_api::tensor({{0.2F, 0.3F, 0.1F, 0.15F}, {0.75F, 0.7F, 0.2F, 0.2F}}), torch_api::zeros({0, 4})},
                        {torch_api::tensor({1, 2}, torch_api::TensorOptions().dtype(torch_api::kInt64)),
                         torch_api::zeros({0}, torch_api::TensorOptions().dtype(torch_api::kInt64))},
                        config.num_queries);
    auto second =
        owner.forward_with_denoising(rfdetr::nested_tensor_from_tensor_list({image.clone(), image.clone()}), second_targets, {71, 3, 1, 5});
    REQUIRE(second.denoising.has_value());
    REQUIRE(second.denoising->queries_per_group == 2);
    REQUIRE_FALSE(second.denoising->valid_slots.index({1}).any().item<bool>());
    REQUIRE((second.denoising->target_indices.index({1}) == -1).all().item<bool>());
    REQUIRE(torch_api::isfinite(second.denoising->main.pred_logits.index({1})).all().item<bool>());
    REQUIRE(torch_api::isfinite(second.denoising->main.pred_boxes.index({1})).all().item<bool>());
    second.denoising->main.pred_logits.retain_grad();
    second.denoising->main.pred_boxes.retain_grad();
    const auto second_loss = owner.supervision_loss(second, second_targets, {torch_api::tensor(2.0F)}, true).total;

    const auto high_water_targets =
        batched_targets({torch_api::tensor({{0.2F, 0.2F, 0.1F, 0.1F}, {0.5F, 0.5F, 0.2F, 0.2F}, {0.8F, 0.8F, 0.1F, 0.1F}})},
                        {torch_api::tensor({0, 1, 2}, torch_api::TensorOptions().dtype(torch_api::kInt64))}, config.num_queries);
    static_cast<void>(
        owner.forward_with_denoising(rfdetr::nested_tensor_from_tensor_list({image.clone()}), high_water_targets, {71, 3, 1, 6}));

    second_loss.backward();
    first_loss.backward();
    require_finite_gradients({first.denoising->main.pred_logits, first.denoising->main.pred_boxes,
                              first.denoising->aux_outputs.front().pred_logits, first.denoising->aux_outputs.front().pred_boxes,
                              second.denoising->main.pred_logits, second.denoising->main.pred_boxes});
    REQUIRE(second.denoising->main.pred_logits.grad().index({1}).abs().sum().item<float>() == 0.0F);
    REQUIRE(second.denoising->main.pred_boxes.grad().index({1}).abs().sum().item<float>() == 0.0F);
    for (const auto& name : {"training_supervision.denoising_label_embedding.weight",
                             "training_supervision.denoising_task_embedding.weight", "training_supervision.ground_truth_mlp.linear1.weight",
                             "class_embed.weight", "bbox_embed.layers.2.weight", "transformer.decoder.layers.0.self_attn.in_proj_weight"}) {
        const auto parameters = owner.module().named_parameters(true);
        const auto* parameter = parameters.find(name);
        REQUIRE(parameter != nullptr);
        REQUIRE(parameter->grad().defined());
        REQUIRE(torch_api::isfinite(parameter->grad()).all().item<bool>());
        REQUIRE(parameter->grad().abs().sum().item<float>() > 0.0F);
    }

    for (auto& parameter : owner.module().parameters(true)) {
        parameter.mutable_grad() = torch::Tensor();
    }
    const auto empty_targets = batched_targets(
        {torch_api::zeros({0, 4})}, {torch_api::zeros({0}, torch_api::TensorOptions().dtype(torch_api::kInt64))}, config.num_queries);
    const auto empty = owner.forward_with_denoising(rfdetr::nested_tensor_from_tensor_list({image.clone()}), empty_targets, {71, 3, 1, 7});
    REQUIRE_FALSE(empty.denoising.has_value());
    const auto ordinary_empty = owner.forward_for_match_free(rfdetr::nested_tensor_from_tensor_list({image.clone()}));
    require_layer_equal(empty.main, ordinary_empty.main);
    REQUIRE(torch_api::equal(*empty.main.query_features, *ordinary_empty.main.query_features));
    REQUIRE(empty.aux_outputs.size() == ordinary_empty.aux_outputs.size());
    for (std::size_t index = 0; index < empty.aux_outputs.size(); ++index) {
        require_layer_equal(empty.aux_outputs[index], ordinary_empty.aux_outputs[index]);
        REQUIRE(torch_api::equal(*empty.aux_outputs[index].query_features, *ordinary_empty.aux_outputs[index].query_features));
    }
    const auto empty_loss = owner.supervision_loss(empty, empty_targets, {torch_api::tensor(0.0F)}, true);
    REQUIRE(empty_loss.total.item<float>() == 0.0F);
    empty_loss.total.backward();
    const auto parameters = owner.module().named_parameters(true);
    for (const auto& name :
         {"training_supervision.denoising_label_embedding.weight", "training_supervision.denoising_task_embedding.weight"}) {
        const auto* parameter = parameters.find(name);
        REQUIRE(parameter != nullptr);
        REQUIRE(parameter->grad().defined());
        REQUIRE(parameter->grad().abs().sum().item<float>() == 0.0F);
    }

    const auto over_capacity_targets = batched_targets(
        {torch_api::zeros({config.num_queries + 1, 4})},
        {torch_api::zeros({config.num_queries + 1}, torch_api::TensorOptions().dtype(torch_api::kInt64))}, config.num_queries);
    REQUIRE_THROWS(
        owner.forward_with_denoising(rfdetr::nested_tensor_from_tensor_list({image.clone()}), over_capacity_targets, {71, 3, 1, 8}));
    REQUIRE_FALSE(
        rfdetr::training_supervision_query_layout_valid(config.training_supervision, std::numeric_limits<std::size_t>::max(), 2U, 0U));
    REQUIRE_FALSE(
        rfdetr::training_supervision_query_layout_valid(config.training_supervision, 0U, 1U, std::numeric_limits<std::size_t>::max()));
    REQUIRE_FALSE(
        rfdetr::training_supervision_query_layout_valid(config.training_supervision, std::numeric_limits<std::size_t>::max() - 1U, 1U, 1U));
}

void test_dn_reference_conventions_inference_removal_and_disabled_exact_path() {
    auto dn_config = denoising_config();
    rfdetr::TrainingSupervisionImpl supervision(dn_config);
    supervision.initialize(108);
    const auto targets = batched_targets({torch_api::tensor({{0.25F, 0.75F, 0.2F, 0.4F}})},
                                         {torch_api::tensor({1}, torch_api::TensorOptions().dtype(torch_api::kInt64))});
    rfdetr::DenoisingVariates variates;
    variates.center = torch_api::full({1, 2, 1, 2}, 0.5F);
    variates.size = torch_api::full({1, 2, 1, 2}, 0.5F);
    variates.label_flip = torch_api::full({1, 2, 1}, 1.0F);
    variates.other_label = torch_api::zeros({1, 2, 1}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    const auto prepared = supervision.prepare_denoising(targets, {2, 4, 6, 8}, torch::Device(torch::kCPU), torch::kFloat32, &variates);
    REQUIRE(prepared.has_value());
    const auto reparameterized_reference = prepared->normalized_references;
    const auto additive_reference = rfdetr::inverse_sigmoid(prepared->normalized_references);
    REQUIRE(torch_api::allclose(additive_reference.sigmoid(), reparameterized_reference, 1.0e-6, 1.0e-6));
    const auto zero_delta = torch_api::zeros_like(reparameterized_reference);
    const auto reparameterized_result =
        torch_api::cat({zero_delta.slice(-1, 0, 2) * reparameterized_reference.slice(-1, 2, 4) + reparameterized_reference.slice(-1, 0, 2),
                        zero_delta.slice(-1, 2, 4).exp() * reparameterized_reference.slice(-1, 2, 4)},
                       -1);
    REQUIRE(torch_api::equal(reparameterized_result, reparameterized_reference));

    auto inactive_config = rfdetr::native_config_from_preset(rfdetr::model_presets().front());
    inactive_config.resolution = 64;
    inactive_config.segmentation = false;
    inactive_config.aux_loss = false;
    inactive_config.two_stage = false;
    inactive_config.group_detr = 1;
    inactive_config.num_queries = 3;
    inactive_config.num_select = 3;
    auto active_config = inactive_config;
    active_config.training_supervision.denoising.enabled = true;
    torch_api::manual_seed(109);
    rfdetr::NativeRfDetrModel inactive(inactive_config);
    torch_api::manual_seed(109);
    rfdetr::NativeRfDetrModel active(active_config);
    auto& inactive_owner = rfdetr::detail::native_model_owner(inactive);
    auto& active_owner = rfdetr::detail::native_model_owner(active);
    inactive_owner.module().eval();
    active_owner.module().eval();
    const auto image = torch_api::zeros({3, 64, 64});
    torch::NoGradGuard no_grad;
    const auto inactive_output = inactive_owner.forward(rfdetr::nested_tensor_from_tensor_list({image}), false);
    const auto active_output = active_owner.forward(rfdetr::nested_tensor_from_tensor_list({image.clone()}), false);
    require_layer_equal(inactive_output.main, active_output.main);
    REQUIRE_FALSE(inactive_output.denoising.has_value());
    REQUIRE_FALSE(active_output.denoising.has_value());
    for (const auto& parameter : inactive_owner.module().named_parameters(true)) {
        REQUIRE_FALSE(parameter.key().starts_with("training_supervision."));
    }
    const auto active_parameters = active_owner.module().named_parameters(true);
    REQUIRE(active_parameters.find("training_supervision.denoising_label_embedding.weight") != nullptr);
    REQUIRE(active_parameters.find("training_supervision.denoising_task_embedding.weight") != nullptr);
    REQUIRE(active_parameters.find("training_supervision.ground_truth_mlp.linear1.weight") == nullptr);
}

void test_dn_all_empty_loss_is_finite_and_parameter_anchored() {
    auto config = denoising_config();
    rfdetr::TrainingSupervisionImpl supervision(config);
    supervision.initialize(110);
    rfdetr::ModelOutputs outputs;
    outputs.main.pred_logits = torch_api::zeros({2, 4, 4}).set_requires_grad(true);
    // CLEANUP-IGNORE: All-empty prediction tensors prove DN parameter anchoring, not nonempty loss setup.
    outputs.main.pred_boxes = torch_api::zeros({2, 4, 4}).set_requires_grad(true);
    const auto targets = batched_targets({torch_api::zeros({0, 4}), torch_api::zeros({0, 4})},
                                         {torch_api::zeros({0}, torch_api::TensorOptions().dtype(torch_api::kInt64)),
                                          // CLEANUP-IGNORE: Two empty rows specifically exercise the zero-target path.
                                          torch_api::zeros({0}, torch_api::TensorOptions().dtype(torch_api::kInt64))});
    const auto loss = supervision.loss(outputs, targets, {torch_api::tensor(0.0F)}, true);
    REQUIRE(torch_api::isfinite(loss.total).item<bool>());
    REQUIRE(loss.total.item<float>() == 0.0F);
    loss.total.backward();
    for (const auto& parameter : supervision.named_parameters(true)) {
        REQUIRE(parameter.value().grad().defined());
        REQUIRE(parameter.value().grad().abs().sum().item<float>() == 0.0F);
    }
}

void test_active_empty_loss_anchors_every_selected_mask_operand() {
    const auto run_case = [](const bool auxiliary_and_encoder) {
        auto config = denoising_config();
        config.segmentation = true;
        config.aux_loss = auxiliary_and_encoder;
        config.two_stage = auxiliary_and_encoder;
        config.dec_layers = auxiliary_and_encoder ? 2 : 1;
        rfdetr::TrainingSupervisionImpl supervision(config);
        supervision.initialize(112);

        const auto make_layer = [](const bool dense) {
            rfdetr::OutputLayer layer;
            layer.pred_logits = torch_api::zeros({1, 4, 4}).set_requires_grad(true);
            layer.pred_boxes = torch_api::zeros({1, 4, 4}).set_requires_grad(true);
            if (dense) {
                layer.pred_masks = torch_api::zeros({1, 4, 2, 2}).set_requires_grad(true);
            } else {
                layer.sparse_pred_masks = rfdetr::OutputLayer::SparsePredMasks{torch_api::zeros({1, 8, 2, 2}).set_requires_grad(true),
                                                                               torch_api::zeros({1, 4, 8}).set_requires_grad(true),
                                                                               torch_api::zeros({1, 4, 1}).set_requires_grad(true)};
            }
            return layer;
        };
        rfdetr::ModelOutputs outputs;
        outputs.main = make_layer(false);
        if (auxiliary_and_encoder) {
            outputs.aux_outputs.push_back(make_layer(true));
            outputs.enc_outputs = make_layer(false);
        }
        const auto targets =
            batched_targets({torch_api::zeros({0, 4})}, {torch_api::zeros({0}, torch_api::TensorOptions().dtype(torch_api::kInt64))});
        const auto loss = supervision.loss(outputs, targets, {torch_api::tensor(0.0F)}, true);
        REQUIRE(loss.total.item<float>() == 0.0F);
        loss.total.backward();
        const auto require_zero_gradient = [](const torch::Tensor& tensor) {
            REQUIRE(tensor.grad().defined());
            REQUIRE(tensor.grad().abs().sum().item<float>() == 0.0F);
        };
        const auto require_layer_masks = [&](const rfdetr::OutputLayer& layer) {
            if (layer.pred_masks) { require_zero_gradient(*layer.pred_masks); }
            if (layer.sparse_pred_masks) {
                require_zero_gradient(layer.sparse_pred_masks->spatial_features);
                require_zero_gradient(layer.sparse_pred_masks->query_features);
                require_zero_gradient(layer.sparse_pred_masks->bias);
            }
        };
        require_layer_masks(outputs.main);
        for (const auto& layer : outputs.aux_outputs) {
            require_layer_masks(layer);
        }
        if (outputs.enc_outputs) { require_layer_masks(*outputs.enc_outputs); }
    };

    run_case(false);
    run_case(true);
}

void test_dn_preparation_and_objective_remain_fp32_under_cuda_autocast() {
    if (!torch_api::cuda::is_available()) { return; }
    auto config = denoising_config();
    rfdetr::TrainingSupervisionImpl supervision(config);
    supervision.initialize(111);
    // CLEANUP-OFF: This CUDA-autocast fixture proves DN dtype boundaries, not timing-lease setup.
    supervision.to(torch::Device(torch::kCUDA));
    const auto float_options = torch_api::TensorOptions().dtype(torch_api::kFloat32).device(torch::kCUDA);
    const auto integer_options = torch_api::TensorOptions().dtype(torch_api::kInt64).device(torch::kCUDA);
    const auto targets =
        batched_targets({torch_api::tensor({{0.5F, 0.5F, 0.2F, 0.2F}}, float_options)}, {torch_api::tensor({1}, integer_options)});
    // CLEANUP-ON
    mmltk::backend::ml::cuda::TorchAutocastScope autocast(true, torch::kFloat16);
    const auto prepared = supervision.prepare_denoising(targets, {4, 3, 2, 1}, targets.all_boxes.device(), torch::kFloat16);
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->content.scalar_type() == torch::kFloat16);
    REQUIRE(prepared->normalized_references.scalar_type() == torch::kFloat32);
    rfdetr::ModelOutputs outputs;
    rfdetr::DenoisingOutputs denoising;
    denoising.groups = prepared->layout.denoising_groups;
    denoising.queries_per_group = prepared->layout.denoising_queries_per_group;
    denoising.original_labels = prepared->original_labels;
    denoising.original_boxes = prepared->original_boxes;
    denoising.valid_slots = prepared->valid_slots;
    denoising.target_indices = prepared->target_indices;
    denoising.main.pred_logits =
        torch_api::zeros({1, 2, 1, 4}, torch_api::TensorOptions().dtype(torch::kFloat16).device(torch::kCUDA)).set_requires_grad(true);
    denoising.main.pred_boxes = prepared->original_boxes.to(torch::kFloat16).clone().set_requires_grad(true);
    outputs.denoising = std::move(denoising);
    outputs.main.pred_logits =
        torch_api::zeros({1, 4, 4}, torch_api::TensorOptions().dtype(torch::kFloat16).device(torch::kCUDA)).set_requires_grad(true);
    outputs.main.pred_boxes =
        torch_api::zeros({1, 4, 4}, torch_api::TensorOptions().dtype(torch::kFloat16).device(torch::kCUDA)).set_requires_grad(true);
    const auto loss = supervision.loss(outputs, targets, {torch_api::tensor(1.0F, float_options)}, true);
    require_float32({loss.total, loss.classification, loss.box, loss.giou});
    loss.total.backward();
    require_finite_gradients({outputs.denoising->main.pred_logits, outputs.denoising->main.pred_boxes});
}

void test_feature_initialization_is_reproducible_and_independent_of_dn_toggle() {
    auto match_only_config = match_free_config();
    auto combined_config = match_only_config;
    combined_config.training_supervision.denoising.enabled = true;
    auto dn_only_config = combined_config;
    dn_only_config.training_supervision.assignment = rfdetr::TrainAssignmentKind::Hungarian;
    rfdetr::TrainingSupervisionImpl match_only(match_only_config);
    rfdetr::TrainingSupervisionImpl combined(combined_config);
    rfdetr::TrainingSupervisionImpl dn_only(dn_only_config);
    const auto rng_before = at::detail::getDefaultCPUGenerator().get_state().clone();
    match_only.initialize(0x12345678ULL);
    combined.initialize(0x12345678ULL);
    dn_only.initialize(0x12345678ULL);
    REQUIRE(torch_api::equal(rng_before, at::detail::getDefaultCPUGenerator().get_state()));
    const auto match_parameters = match_only.named_parameters(true);
    const auto combined_parameters = combined.named_parameters(true);
    for (const auto& parameter : match_parameters) {
        const auto* other = combined_parameters.find(parameter.key());
        REQUIRE(other != nullptr);
        REQUIRE(torch_api::equal(parameter.value(), *other));
    }
    REQUIRE(combined_parameters.find("denoising_label_embedding.weight") != nullptr);
    REQUIRE(combined_parameters.find("denoising_task_embedding.weight") != nullptr);
    const auto dn_parameters = dn_only.named_parameters(true);
    REQUIRE(torch_api::equal(combined_parameters["denoising_label_embedding.weight"], dn_parameters["denoising_label_embedding.weight"]));
    REQUIRE(torch_api::equal(combined_parameters["denoising_task_embedding.weight"], dn_parameters["denoising_task_embedding.weight"]));
    REQUIRE_THROWS(match_only.initialize(0x12345678ULL));
}

void test_sparse_masks_share_erasure_after_geometry_and_donor_composition() {
    if (!torch_api::cuda::is_available()) { return; }
    const auto floats = torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA);
    const auto integers = floats.dtype(torch::kInt64);
    const std::array<rfdetr::AugmentationSpatialErasure, 4U> erasures{
        rfdetr::AugmentationSpatialErasure{.x1 = 0.5F, .y1 = 0.5F, .rectangular = 1U},
        rfdetr::AugmentationSpatialErasure{.x1 = 0.5F, .y1 = 0.5F, .rectangular = 1U},
        rfdetr::AugmentationSpatialErasure{.dropout_probability = 1.0F},
        rfdetr::AugmentationSpatialErasure{},
    };
    auto bytes = torch::empty({4, static_cast<int64_t>(sizeof(erasures.front()))}, torch::TensorOptions().dtype(torch::kUInt8));
    std::memcpy(bytes.data_ptr<std::uint8_t>(), erasures.data(), sizeof(erasures));
    auto transforms = torch::tensor({1.F, 0.F, 0.F, 0.F, 1.F, 0.F}, floats).repeat({4, 1});
    transforms.index_put_({3, 2}, -0.5F);
    auto occluder_transforms = torch::tensor({1.F, 0.F, 0.F, 0.F, 1.F, 0.F}, floats).repeat({4, 1});
    const rfdetr::PackedTargetMasks masks{torch::full({4, 1}, 0xFFFF, integers),
                                          4,
                                          4,
                                          transforms,
                                          torch::tensor({1, -1, -1, -1}, integers),
                                          occluder_transforms,
                                          bytes.to(torch::kCUDA)};
    const auto points = torch::tensor({0.125F, 0.125F, 0.375F, 0.375F, 0.625F, 0.125F, 0.125F, 0.625F, 1.0F, 1.0F}, floats).view({1, 5, 2});
    const auto sampled = rfdetr::sample_target_masks(masks, torch::arange(4, integers), points, "erasure test");
    const auto expected =
        torch::tensor({0.F, 0.F, 0.F, 0.F, 0.F, 0.F, 0.F, 1.F, 1.F, 1.F, 0.F, 0.F, 0.F, 0.F, 0.F, 0.F, 0.F, 1.F, 0.F, 1.F}, floats)
            .view({4, 5});
    REQUIRE(torch::equal(sampled, expected));
    auto invalid = masks;
    invalid.erasure = bytes;
    REQUIRE_THROWS(rfdetr::sample_target_masks(invalid, torch::arange(4, integers), points, "invalid erasure"));
    const auto empty = rfdetr::sample_target_masks(masks, torch::empty({0}, integers), points, "empty erasure");
    CHECK(empty.numel() == 0);
}

}  // namespace

// CLEANUP-IGNORE: These separately registered mathematical cases have different test bodies and acceptance obligations.
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_sparse_masks_share_erasure_after_geometry_and_donor_composition);

// CLEANUP-IGNORE: Registration is an exhaustive inventory of semantically independent mathematical tests.
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_geometry_preserves_consumer_policies_and_batch_isolation);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_gathered_ground_truth_affinity_matches_explicit_one_hot_and_sqrt_d);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]",
                         test_scg_excludes_inactive_columns_handles_ties_and_keeps_selected_gradients_live);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]",
                         test_complete_focal_cost_and_both_objectives_keep_prediction_and_probe_gradients);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]",
                         test_focal_broadcast_cost_sums_every_channel_and_places_coefficients_once);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_layer_group_and_device_target_reductions_follow_declared_gating);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_complete_match_free_loss_is_fp32_inside_cuda_autocast);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_timing_leases_are_explicit_bounded_and_harvested_once);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_empty_and_retained_graphs_are_finite_and_independent);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]",
                         test_production_match_free_boundary_captures_layers_and_empty_gradient_anchors);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]",
                         test_mixed_empty_images_use_safe_internal_padding_without_loss_contribution);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]",
                         test_conditional_model_construction_preserves_rng_and_default_state_topology);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_dn_endpoint_transform_mapping_padding_and_one_class_behavior);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_dn_center_underflow_other_label_bijection_and_step_determinism);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]",
                         test_split_self_attention_matches_equation_seven_and_symmetric_group_layout);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_dn_direct_loss_auxiliary_gating_padding_and_retained_graphs);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_production_dn_forward_preserves_reference_and_output_boundaries);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_production_dn_retained_graph_scratch_padding_and_gradients);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_dn_reference_conventions_inference_removal_and_disabled_exact_path);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_dn_all_empty_loss_is_finite_and_parameter_anchored);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_active_empty_loss_anchors_every_selected_mask_operand);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_dn_preparation_and_objective_remain_fp32_under_cuda_autocast);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision]", test_feature_initialization_is_reproducible_and_independent_of_dn_toggle);
