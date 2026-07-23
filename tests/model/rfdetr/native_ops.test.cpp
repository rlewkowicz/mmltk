#include "mmltk/rfdetr/detection_ops.h"

#include <ATen/TensorAccessor.h>
#include <ATen/TensorIndexing.h>
#include <torch/nn/functional/vision.h>

#include <algorithm>
#include <array>
#include "support/catch2_compat.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mmltk::rfdetr {

torch::Tensor sample_packed_masks_cuda(const torch::Tensor& packed_mask_bits, int64_t height, int64_t width,
                                       const torch::Tensor& mask_indices, const torch::Tensor& point_coords);

}

namespace {

using namespace torch::indexing;
namespace F = torch::nn::functional;

template <typename T>
[[nodiscard]] const T& require_optional_ref(const std::optional<T>& value, const std::string_view message) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(message));
    }
    return *value;
}

template <typename T>
[[nodiscard]] T& require_optional_ref(std::optional<T>& value, const std::string_view message) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(message));
    }
    return *value;
}

mmltk::rfdetr::PackedTargetMasks pack_dense_masks(const torch::Tensor& dense_masks) {
    const auto dense = dense_masks.to(torch::kCPU, torch::kFloat32).contiguous();
    const int64_t height = dense.size(1);
    const int64_t width = dense.size(2);
    const int64_t words_per_mask = std::max<int64_t>(1, (height * width + 63) / 64);
    auto bits = torch::zeros({dense.size(0), words_per_mask}, torch::TensorOptions().dtype(torch::kInt64));
    auto dense_a = dense.accessor<float, 3>();
    auto* bits_ptr = reinterpret_cast<uint64_t*>(bits.data_ptr<int64_t>());
    for (int64_t mask_index = 0; mask_index < dense.size(0); ++mask_index) {
        auto* mask_words = bits_ptr + mask_index * words_per_mask;
        for (int64_t y = 0; y < height; ++y) {
            for (int64_t x = 0; x < width; ++x) {
                if (dense_a[mask_index][y][x] <= 0.0f) {
                    continue;
                }
                const int64_t pixel_index = y * width + x;
                mask_words[pixel_index >> 6] |= uint64_t{1} << (pixel_index & 63);
            }
        }
    }
    return mmltk::rfdetr::PackedTargetMasks{bits, height, width};
}

mmltk::rfdetr::PreparedTargets make_targets() {
    mmltk::rfdetr::PreparedTarget target;
    target.image_id = torch::tensor({1}, torch::TensorOptions().dtype(torch::kInt64));
    target.orig_size = torch::tensor({100, 100}, torch::TensorOptions().dtype(torch::kInt64));
    target.size = torch::tensor({100, 100}, torch::TensorOptions().dtype(torch::kInt64));
    target.boxes = torch::tensor({{0.5f, 0.5f, 0.2f, 0.2f}}, torch::TensorOptions().dtype(torch::kFloat32));
    target.labels = torch::tensor({1}, torch::TensorOptions().dtype(torch::kInt64));
    target.area = torch::tensor({400.0f}, torch::TensorOptions().dtype(torch::kFloat32));
    target.iscrowd = torch::zeros({1}, torch::TensorOptions().dtype(torch::kInt64));

    mmltk::rfdetr::PreparedTargets targets;
    targets.targets.push_back(target);
    targets.orig_sizes = torch::tensor({{100, 100}}, torch::TensorOptions().dtype(torch::kInt64));
    targets.nested_mask = torch::zeros({1, 100, 100}, torch::TensorOptions().dtype(torch::kBool));
    targets.all_boxes = target.boxes.clone();
    targets.all_labels = target.labels.clone();
    targets.all_area = target.area.clone();
    targets.all_iscrowd = target.iscrowd.clone();
    targets.offsets = {0};
    targets.counts = {1};
    return targets;
}

mmltk::rfdetr::PreparedTargets make_multi_targets() {
    mmltk::rfdetr::PreparedTargets targets;
    targets.orig_sizes = torch::tensor({{100, 100}}, torch::TensorOptions().dtype(torch::kInt64));
    targets.nested_mask = torch::zeros({1, 100, 100}, torch::TensorOptions().dtype(torch::kBool));

    mmltk::rfdetr::PreparedTarget image_targets;
    image_targets.image_id = torch::tensor({1}, torch::TensorOptions().dtype(torch::kInt64));
    image_targets.orig_size = torch::tensor({100, 100}, torch::TensorOptions().dtype(torch::kInt64));
    image_targets.size = torch::tensor({100, 100}, torch::TensorOptions().dtype(torch::kInt64));
    image_targets.boxes = torch::tensor({{0.5f, 0.5f, 0.2f, 0.2f}, {0.2f, 0.2f, 0.1f, 0.12f}},
                                        torch::TensorOptions().dtype(torch::kFloat32));
    image_targets.labels = torch::tensor({1, 2}, torch::TensorOptions().dtype(torch::kInt64));
    image_targets.area = torch::tensor({400.0f, 120.0f}, torch::TensorOptions().dtype(torch::kFloat32));
    image_targets.iscrowd = torch::zeros({2}, torch::TensorOptions().dtype(torch::kInt64));

    targets.targets.push_back(image_targets);
    targets.all_boxes = image_targets.boxes;
    targets.all_labels = image_targets.labels;
    targets.all_area = image_targets.area;
    targets.all_iscrowd = image_targets.iscrowd;
    targets.offsets = {0};
    targets.counts = {2};
    return targets;
}

mmltk::rfdetr::DetectionConfig make_config() {
    mmltk::rfdetr::DetectionConfig config;
    config.num_classes = 3;
    config.group_detr = 1;
    config.dec_layers = 3;
    config.num_select = 2;
    config.world_size = 1;
    config.aux_loss = true;
    config.two_stage = true;
    config.focal_alpha = 0.25;
    config.cls_loss_coef = 1.0;
    config.bbox_loss_coef = 5.0;
    config.giou_loss_coef = 2.0;
    config.set_cost_class = 2.0;
    config.set_cost_bbox = 5.0;
    config.set_cost_giou = 2.0;
    mmltk::rfdetr::populate_default_detection_weight_dict(config);
    return config;
}

mmltk::rfdetr::ModelOutputs make_outputs() {
    mmltk::rfdetr::ModelOutputs outputs;
    outputs.main.pred_logits =
        torch::tensor({{{-5.0f, 8.0f, -5.0f}, {6.0f, -5.0f, -5.0f}}}, torch::TensorOptions().dtype(torch::kFloat32));
    outputs.main.pred_boxes = torch::tensor({{{0.5f, 0.5f, 0.2f, 0.2f}, {0.1f, 0.1f, 0.1f, 0.1f}}},
                                            torch::TensorOptions().dtype(torch::kFloat32));
    outputs.aux_outputs.push_back(outputs.main);
    outputs.aux_outputs.push_back(outputs.main);
    outputs.enc_outputs = outputs.main;
    return outputs;
}

mmltk::rfdetr::ModelOutputs make_distinct_layer_outputs() {
    auto outputs = make_outputs();
    outputs.aux_outputs[0].pred_logits = outputs.aux_outputs[0].pred_logits + 0.3f;
    outputs.aux_outputs[0].pred_boxes = outputs.aux_outputs[0].pred_boxes + 0.01f;
    outputs.aux_outputs[1].pred_logits = outputs.aux_outputs[1].pred_logits - 0.4f;
    outputs.aux_outputs[1].pred_boxes = outputs.aux_outputs[1].pred_boxes - 0.02f;
    auto& enc_outputs = require_optional_ref(outputs.enc_outputs, "expected encoder outputs in distinct layer outputs");
    enc_outputs.pred_logits = enc_outputs.pred_logits + 0.1f;
    enc_outputs.pred_boxes = enc_outputs.pred_boxes + 0.015f;
    return outputs;
}

mmltk::rfdetr::ModelOutputs make_multi_match_outputs() {
    mmltk::rfdetr::ModelOutputs outputs;
    outputs.main.pred_logits = torch::tensor({{{-5.0f, 8.0f, -5.0f}, {-5.0f, -5.0f, 8.0f}, {7.0f, -5.0f, -5.0f}}},
                                             torch::TensorOptions().dtype(torch::kFloat32));
    outputs.main.pred_boxes =
        torch::tensor({{{0.52f, 0.48f, 0.22f, 0.18f}, {0.18f, 0.22f, 0.12f, 0.08f}, {0.9f, 0.9f, 0.05f, 0.05f}}},
                      torch::TensorOptions().dtype(torch::kFloat32));
    return outputs;
}

mmltk::rfdetr::ModelOutputs make_mask_outputs(int64_t height, int64_t width) {
    auto outputs = make_outputs();
    outputs.aux_outputs.clear();
    outputs.enc_outputs.reset();
    outputs.main.pred_masks = torch::zeros({1, 2, height, width}, torch::TensorOptions().dtype(torch::kFloat32));
    auto& pred_masks = outputs.main.pred_masks.value();
    pred_masks.index_put_({0, 0, Slice(), Slice()}, 0.4f);
    pred_masks.index_put_({0, 1, Slice(), Slice()}, -0.3f);
    return outputs;
}

std::array<float, 4> cxcywh_to_xyxy(const std::array<float, 4>& box) {
    return {
        box[0] - 0.5f * box[2],
        box[1] - 0.5f * box[3],
        box[0] + 0.5f * box[2],
        box[1] + 0.5f * box[3],
    };
}

float aligned_giou(const std::array<float, 4>& lhs_cxcywh, const std::array<float, 4>& rhs_cxcywh) {
    const auto lhs = cxcywh_to_xyxy(lhs_cxcywh);
    const auto rhs = cxcywh_to_xyxy(rhs_cxcywh);
    const float inter_x1 = std::max(lhs[0], rhs[0]);
    const float inter_y1 = std::max(lhs[1], rhs[1]);
    const float inter_x2 = std::min(lhs[2], rhs[2]);
    const float inter_y2 = std::min(lhs[3], rhs[3]);
    const float inter_w = std::max(0.0f, inter_x2 - inter_x1);
    const float inter_h = std::max(0.0f, inter_y2 - inter_y1);
    const float inter = inter_w * inter_h;

    const float lhs_area = std::max(0.0f, lhs[2] - lhs[0]) * std::max(0.0f, lhs[3] - lhs[1]);
    const float rhs_area = std::max(0.0f, rhs[2] - rhs[0]) * std::max(0.0f, rhs[3] - rhs[1]);
    const float uni = lhs_area + rhs_area - inter;
    const float iou = inter / uni;

    const float enc_x1 = std::min(lhs[0], rhs[0]);
    const float enc_y1 = std::min(lhs[1], rhs[1]);
    const float enc_x2 = std::max(lhs[2], rhs[2]);
    const float enc_y2 = std::max(lhs[3], rhs[3]);
    const float enc_area = std::max(0.0f, enc_x2 - enc_x1) * std::max(0.0f, enc_y2 - enc_y1);
    return iou - (enc_area - uni) / enc_area;
}

void test_weight_dict_population() {
    const auto config = make_config();
    assert(config.weight_dict.size() == 12);
    assert(config.weight_dict[0].first == "loss_ce");
    assert(config.weight_dict[3].first == "loss_ce_0");
    assert(config.weight_dict[6].first == "loss_ce_1");
    assert(config.weight_dict[9].first == "loss_ce_enc");
}

void test_matcher_and_losses() {
    const auto config = make_config();
    const auto outputs = make_outputs();
    const auto targets = make_targets();

    const auto indices = mmltk::rfdetr::matcher_indices(outputs, targets, config, true);
    assert(indices.size() == 1);
    assert(indices[0].first.size(0) == 1);
    assert(indices[0].second.size(0) == 1);
    assert(indices[0].first.item<int64_t>() == 0);
    assert(indices[0].second.item<int64_t>() == 0);

    const auto loss_dict = mmltk::rfdetr::detection_loss_dict(outputs, targets, config, true, false);
    assert(loss_dict.count("loss_ce") == 1);
    assert(loss_dict.count("loss_bbox") == 1);
    assert(loss_dict.count("loss_giou") == 1);
    assert(loss_dict.count("cardinality_error") == 1);
    assert(loss_dict.count("loss_ce_0") == 1);
    assert(loss_dict.count("loss_ce_1") == 1);
    assert(loss_dict.count("loss_ce_enc") == 1);

    const auto weighted = mmltk::rfdetr::weighted_detection_loss(loss_dict, config, torch::Device(torch::kCPU));
    assert(torch::isfinite(weighted).item<bool>());
    assert(weighted.item<float>() > 0.0f);
}

void test_batched_matcher_transfer_matches_single_layer_losses() {
    const auto config = make_config();
    const auto targets = make_targets();
    auto outputs = make_distinct_layer_outputs();
    auto& enc_outputs =
        require_optional_ref(outputs.enc_outputs, "expected encoder outputs in batched matcher transfer test");
    enc_outputs.pred_logits = enc_outputs.pred_logits.narrow(1, 0, 1).clone();
    enc_outputs.pred_boxes = enc_outputs.pred_boxes.narrow(1, 0, 1).clone();
    const auto combined = mmltk::rfdetr::detection_loss_dict(outputs, targets, config, true, false);

    mmltk::rfdetr::DetectionConfig single_config = config;
    single_config.aux_loss = false;
    single_config.two_stage = false;
    mmltk::rfdetr::populate_default_detection_weight_dict(single_config);

    auto main_only = outputs;
    main_only.aux_outputs.clear();
    main_only.enc_outputs.reset();
    const auto main_loss = mmltk::rfdetr::detection_loss_dict(main_only, targets, single_config, true, false);
    assert(torch::allclose(combined.at("loss_ce"), main_loss.at("loss_ce"), 1.0e-6, 1.0e-6));
    assert(torch::allclose(combined.at("loss_bbox"), main_loss.at("loss_bbox"), 1.0e-6, 1.0e-6));
    assert(torch::allclose(combined.at("loss_giou"), main_loss.at("loss_giou"), 1.0e-6, 1.0e-6));

    for (size_t aux_index = 0; aux_index < outputs.aux_outputs.size(); ++aux_index) {
        mmltk::rfdetr::ModelOutputs aux_only;
        aux_only.main = outputs.aux_outputs[aux_index];
        const auto aux_loss = mmltk::rfdetr::detection_loss_dict(aux_only, targets, single_config, true, false);
        const std::string suffix = "_" + std::to_string(aux_index);
        assert(torch::allclose(combined.at("loss_ce" + suffix), aux_loss.at("loss_ce"), 1.0e-6, 1.0e-6));
        assert(torch::allclose(combined.at("loss_bbox" + suffix), aux_loss.at("loss_bbox"), 1.0e-6, 1.0e-6));
        assert(torch::allclose(combined.at("loss_giou" + suffix), aux_loss.at("loss_giou"), 1.0e-6, 1.0e-6));
    }

    mmltk::rfdetr::ModelOutputs enc_only;
    enc_only.main =
        require_optional_ref(outputs.enc_outputs, "expected encoder outputs when building encoder-only loss");
    const auto enc_loss = mmltk::rfdetr::detection_loss_dict(enc_only, targets, single_config, true, false);
    assert(torch::allclose(combined.at("loss_ce_enc"), enc_loss.at("loss_ce"), 1.0e-6, 1.0e-6));
    assert(torch::allclose(combined.at("loss_bbox_enc"), enc_loss.at("loss_bbox"), 1.0e-6, 1.0e-6));
    assert(torch::allclose(combined.at("loss_giou_enc"), enc_loss.at("loss_giou"), 1.0e-6, 1.0e-6));
}

void test_multi_match_box_losses() {
    const auto config = make_config();
    const auto outputs = make_multi_match_outputs();
    const auto targets = make_multi_targets();

    const auto indices = mmltk::rfdetr::matcher_indices(outputs, targets, config, true);
    assert(indices.size() == 1);
    assert(indices[0].first.size(0) == 2);
    assert(indices[0].second.size(0) == 2);

    const auto loss_dict = mmltk::rfdetr::detection_loss_dict(outputs, targets, config, true, false);
    assert(loss_dict.count("loss_bbox") == 1);
    assert(loss_dict.count("loss_giou") == 1);

    const auto pred_boxes = outputs.main.pred_boxes.squeeze(0).cpu();
    const auto target_boxes = targets.all_boxes.cpu();
    const auto row_index = indices[0].first.cpu();
    const auto col_index = indices[0].second.cpu();

    float bbox_sum = 0.0f;
    float giou_sum = 0.0f;
    for (int64_t match = 0; match < row_index.size(0); ++match) {
        const auto row = row_index[match].item<int64_t>();
        const auto col = col_index[match].item<int64_t>();
        std::array<float, 4> src{};
        std::array<float, 4> tgt{};
        for (int axis = 0; axis < 4; ++axis) {
            src[static_cast<size_t>(axis)] = pred_boxes[row][axis].item<float>();
            tgt[static_cast<size_t>(axis)] = target_boxes[col][axis].item<float>();
            bbox_sum += std::fabs(src[static_cast<size_t>(axis)] - tgt[static_cast<size_t>(axis)]);
        }
        giou_sum += 1.0f - aligned_giou(src, tgt);
    }

    const float expected_bbox = bbox_sum / static_cast<float>(row_index.size(0));
    const float expected_giou = giou_sum / static_cast<float>(row_index.size(0));
    const auto actual_bbox = loss_dict.at("loss_bbox").item<float>();
    const auto actual_giou = loss_dict.at("loss_giou").item<float>();
    assert(std::fabs(actual_bbox - expected_bbox) < 1.0e-5f);
    assert(std::fabs(actual_giou - expected_giou) < 1.0e-5f);
}

void test_matcher_sanitizes_nonfinite_costs() {
    auto outputs = make_outputs();
    outputs.main.pred_boxes.index_put_({0, 0, 0}, std::numeric_limits<float>::quiet_NaN());

    const auto config = make_config();
    const auto targets = make_targets();
    const auto indices = mmltk::rfdetr::matcher_indices(outputs, targets, config, true);

    assert(indices.size() == 1);
    assert(indices[0].first.size(0) == 1);
    assert(indices[0].second.size(0) == 1);
}

auto make_mask_loss_config() {
    auto config = make_config();
    config.include_masks = true;
    config.aux_loss = false;
    config.two_stage = false;
    config.mask_point_sample_ratio = 4;
    mmltk::rfdetr::populate_default_detection_weight_dict(config);
    return config;
}

void test_cpu_mask_loss_uses_upstream_keys() {
    constexpr int64_t kHeight = 4;
    constexpr int64_t kWidth = 4;

    auto targets = make_targets();
    auto target_masks = torch::zeros({1, kHeight, kWidth}, torch::TensorOptions().dtype(torch::kFloat32));
    target_masks.index_put_({0, 1, 1}, 1.0f);
    target_masks.index_put_({0, 1, 2}, 1.0f);
    target_masks.index_put_({0, 2, 1}, 1.0f);
    target_masks.index_put_({0, 2, 2}, 1.0f);
    targets.packed_masks = pack_dense_masks(target_masks);

    auto config = make_mask_loss_config();

    auto outputs = make_mask_outputs(kHeight, kWidth);
    const auto loss_dict = mmltk::rfdetr::detection_loss_dict(outputs, targets, config, true, false);

    assert(loss_dict.count("loss_mask_ce") == 1);
    assert(loss_dict.count("loss_mask_dice") == 1);
    assert(loss_dict.count("loss_dice") == 0);

    const auto weighted = mmltk::rfdetr::weighted_detection_loss(loss_dict, config, torch::Device(torch::kCPU));
    assert(torch::isfinite(weighted).item<bool>());
}

void test_sparse_mask_loss_matches_dense_reference() {
    constexpr int64_t kHeight = 8;
    constexpr int64_t kWidth = 8;
    constexpr int64_t kChannels = 2;

    auto targets = make_targets();
    auto target_masks = torch::zeros({1, kHeight, kWidth}, torch::TensorOptions().dtype(torch::kFloat32));
    target_masks.index_put_({0, 2, 2}, 1.0f);
    target_masks.index_put_({0, 2, 3}, 1.0f);
    target_masks.index_put_({0, 2, 4}, 1.0f);
    target_masks.index_put_({0, 3, 2}, 1.0f);
    target_masks.index_put_({0, 3, 3}, 1.0f);
    target_masks.index_put_({0, 3, 4}, 1.0f);
    targets.packed_masks = pack_dense_masks(target_masks);

    auto config = make_mask_loss_config();

    auto spatial_features =
        torch::linspace(0.05f, 1.28f, kChannels * kHeight * kWidth, torch::TensorOptions().dtype(torch::kFloat32))
            .view({1, kChannels, kHeight, kWidth});
    auto query_features =
        torch::tensor({{{0.35f, -0.15f}, {-0.2f, 0.4f}}}, torch::TensorOptions().dtype(torch::kFloat32));
    auto bias = torch::tensor({0.05f}, torch::TensorOptions().dtype(torch::kFloat32));
    auto dense_masks = torch::einsum("bchw,bnc->bnhw", {spatial_features, query_features}) + bias;

    mmltk::rfdetr::ModelOutputs dense_outputs;
    dense_outputs.main.pred_logits =
        torch::tensor({{{-5.0f, 8.0f, -5.0f}, {6.0f, -5.0f, -5.0f}}}, torch::TensorOptions().dtype(torch::kFloat32));
    dense_outputs.main.pred_boxes = torch::tensor({{{0.5f, 0.5f, 0.2f, 0.2f}, {0.1f, 0.1f, 0.1f, 0.1f}}},
                                                  torch::TensorOptions().dtype(torch::kFloat32));
    dense_outputs.main.pred_masks = dense_masks;

    mmltk::rfdetr::ModelOutputs sparse_outputs;
    sparse_outputs.main.pred_logits = dense_outputs.main.pred_logits.clone();
    sparse_outputs.main.pred_boxes = dense_outputs.main.pred_boxes.clone();
    sparse_outputs.main.sparse_pred_masks = mmltk::rfdetr::OutputLayer::SparsePredMasks{
        spatial_features,
        query_features,
        bias,
    };

    torch::manual_seed(1234);
    const auto dense_loss = mmltk::rfdetr::detection_loss_dict(dense_outputs, targets, config, true, false);
    torch::manual_seed(1234);
    const auto sparse_loss = mmltk::rfdetr::detection_loss_dict(sparse_outputs, targets, config, true, false);

    assert(torch::allclose(dense_loss.at("loss_mask_ce"), sparse_loss.at("loss_mask_ce"), 1.0e-5, 1.0e-5));
    assert(torch::allclose(dense_loss.at("loss_mask_dice"), sparse_loss.at("loss_mask_dice"), 1.0e-5, 1.0e-5));
}

void test_matcher_mask_cost_handles_zero_point_sampling_on_cuda() {
    if (!torch::cuda::is_available()) {
        return;
    }

    auto targets = make_targets();
    targets.packed_masks = pack_dense_masks(torch::ones({1, 1, 1}, torch::TensorOptions().dtype(torch::kFloat32)));
    const auto& packed_masks = targets.packed_masks.value();
    targets.packed_masks = mmltk::rfdetr::PackedTargetMasks{
        packed_masks.bits.to(torch::kCUDA),
        packed_masks.height,
        packed_masks.width,
    };

    auto config = make_config();
    config.include_masks = true;
    config.aux_loss = false;
    config.two_stage = false;
    config.mask_point_sample_ratio = 16;
    mmltk::rfdetr::populate_default_detection_weight_dict(config);

    auto outputs = make_mask_outputs(1, 1);
    outputs.main.pred_logits = outputs.main.pred_logits.to(torch::kCUDA);
    outputs.main.pred_boxes = outputs.main.pred_boxes.to(torch::kCUDA);
    outputs.main.pred_masks =
        require_optional_ref(outputs.main.pred_masks, "expected mask outputs for CUDA matcher test").to(torch::kCUDA);

    const auto indices = mmltk::rfdetr::matcher_indices(outputs, targets, config, true);
    assert(indices.size() == 1);
    assert(indices[0].first.size(0) == 1);
    assert(indices[0].second.size(0) == 1);
    assert(indices[0].first.item<int64_t>() == 0);
    assert(indices[0].second.item<int64_t>() == 0);
}

void test_packed_mask_sampling_matches_grid_sample_nearest_boundaries() {
    if (!torch::cuda::is_available()) {
        return;
    }

    auto dense_masks = torch::tensor({{{0.0f, 1.0f, 0.0f, 1.0f}}}, torch::TensorOptions().dtype(torch::kFloat32));
    const auto packed = pack_dense_masks(dense_masks);
    const auto packed_bits = packed.bits.to(torch::kCUDA);
    const auto mask_indices = torch::tensor({0}, torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA));
    const auto point_coords = torch::tensor({{{0.0f, 0.5f},
                                              {0.125f, 0.5f},
                                              {0.25f, 0.5f},
                                              {0.375f, 0.5f},
                                              {0.5f, 0.5f},
                                              {0.625f, 0.5f},
                                              {0.75f, 0.5f},
                                              {0.875f, 0.5f},
                                              {1.0f, 0.5f}}},
                                            torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA));

    const auto sampled =
        mmltk::rfdetr::sample_packed_masks_cuda(packed_bits, packed.height, packed.width, mask_indices, point_coords);

    const auto dense_cuda = dense_masks.unsqueeze(1).to(torch::kCUDA);
    const auto expected =
        F::grid_sample(
            dense_cuda, 2.0f * point_coords.unsqueeze(2) - 1.0f,
            F::GridSampleFuncOptions().mode(torch::kNearest).padding_mode(torch::kBorder).align_corners(false))
            .squeeze(3)
            .squeeze(1);

    assert(torch::equal(sampled.cpu(), expected.cpu()));
}

void test_postprocess() {
    const auto outputs = make_outputs();
    const auto target_sizes = torch::tensor({{100, 100}}, torch::TensorOptions().dtype(torch::kInt64));
    const auto results = mmltk::rfdetr::postprocess_outputs(outputs, target_sizes, 2);
    assert(results.size() == 1);
    const auto& result = results.front();
    assert(result.count("scores") == 1);
    assert(result.count("labels") == 1);
    assert(result.count("boxes") == 1);
    assert(result.at("scores").size(0) == 2);
    assert(result.at("labels").size(0) == 2);
    assert(result.at("boxes").size(0) == 2);
    assert(result.at("boxes").size(1) == 4);

    const auto boxes = result.at("boxes").cpu();
    const auto box_values = boxes.accessor<float, 2>();
    assert(std::fabs(box_values[0][0] - 40.0f) < 1e-4f);
    assert(std::fabs(box_values[0][1] - 40.0f) < 1e-4f);
    assert(std::fabs(box_values[0][2] - 60.0f) < 1e-4f);
    assert(std::fabs(box_values[0][3] - 60.0f) < 1e-4f);
}

}  

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_weight_dict_population);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_matcher_and_losses);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_batched_matcher_transfer_matches_single_layer_losses);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_multi_match_box_losses);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_matcher_sanitizes_nonfinite_costs);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_cpu_mask_loss_uses_upstream_keys);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_sparse_mask_loss_matches_dense_reference);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_matcher_mask_cost_handles_zero_point_sampling_on_cuda);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]",
                         test_packed_mask_sampling_matches_grid_sample_nearest_boundaries);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_postprocess);
