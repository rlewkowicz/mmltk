// RF-DETR core operation coverage.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <catch2/generators/catch_generators.hpp>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <unistd.h>

#include "catch2_compat.hpp"
#include "cuda_test_utils.hpp"
#include "detail/detection_ops.h"
#include "detail/lsap_scratch.h"
#include "detail/matcher_workspace.h"
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/common/system/numa_memory.h"
#include "src/common/system/numa_topology.h"
#include "detail/postprocess.h"
#include "detail/model_access.h"
#include "require_test_utils.hpp"
#include "torch_api.h"

import mmltk.backend.models.rfdetr.core.runtime;
import mmltk.backend.models.rfdetr.core.model;

namespace torch_api = mmltk::backend::ml::torch_api;

namespace mmltk::backend::models::rfdetr {

torch_api::Tensor sample_packed_masks_cuda(const torch_api::Tensor& packed_mask_bits, int64_t height, int64_t width,
                                           const torch_api::Tensor& mask_indices, const torch_api::Tensor& point_coords);

}

namespace {

using namespace torch_api::indexing;
namespace F = torch_api::nn::functional;

using mmltk::testsupport::require_optional_ref;

mmltk::backend::models::rfdetr::PackedTargetMasks pack_dense_masks(const torch_api::Tensor& dense_masks) {
    const auto dense = dense_masks.to(torch_api::kCPU, torch_api::kFloat32).contiguous();
    const int64_t height = dense.size(1);
    const int64_t width = dense.size(2);
    const int64_t words_per_mask = std::max<int64_t>(1, (height * width + 63) / 64);
    auto bits = torch_api::zeros({dense.size(0), words_per_mask}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    auto dense_a = dense.accessor<float, 3>();
    auto* bits_ptr = reinterpret_cast<uint64_t*>(bits.data_ptr<int64_t>());
    for (int64_t mask_index = 0; mask_index < dense.size(0); ++mask_index) {
        auto* mask_words = bits_ptr + mask_index * words_per_mask;
        for (int64_t y = 0; y < height; ++y) {
            for (int64_t x = 0; x < width; ++x) {
                if (dense_a[mask_index][y][x] <= 0.0f) { continue; }
                const int64_t pixel_index = y * width + x;
                mask_words[pixel_index >> 6] |= uint64_t{1} << (pixel_index & 63);
            }
        }
    }
    return mmltk::backend::models::rfdetr::PackedTargetMasks{bits, height, width};
}

// Builds a 100x100 single-image target set from per-instance boxes, labels, and areas.
mmltk::backend::models::rfdetr::PreparedTargets make_single_image_targets(const torch_api::Tensor& boxes, const torch_api::Tensor& labels,
                                                                          const torch_api::Tensor& area) {
    const int64_t instance_count = labels.size(0);

    mmltk::backend::models::rfdetr::PreparedTarget target;
    target.image_id = torch_api::tensor({1}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    target.orig_size = torch_api::tensor({100, 100}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    target.size = torch_api::tensor({100, 100}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    target.boxes = boxes;
    target.labels = labels;
    target.area = area;
    target.iscrowd = torch_api::zeros({instance_count}, torch_api::TensorOptions().dtype(torch_api::kInt64));

    mmltk::backend::models::rfdetr::PreparedTargets targets;
    targets.orig_sizes = torch_api::tensor({{100, 100}}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    targets.nested_mask = torch_api::zeros({1, 100, 100}, torch_api::TensorOptions().dtype(torch_api::kBool));
    targets.all_boxes = target.boxes.clone();
    targets.all_labels = target.labels.clone();
    targets.all_area = target.area.clone();
    targets.all_iscrowd = target.iscrowd.clone();
    targets.offsets = {0};
    targets.counts = {instance_count};
    targets.targets.push_back(std::move(target));
    return targets;
}

mmltk::backend::models::rfdetr::PreparedTargets make_targets() {
    return make_single_image_targets(torch_api::tensor({{0.5f, 0.5f, 0.2f, 0.2f}}, torch_api::TensorOptions().dtype(torch_api::kFloat32)),
                                     torch_api::tensor({1}, torch_api::TensorOptions().dtype(torch_api::kInt64)),
                                     torch_api::tensor({400.0f}, torch_api::TensorOptions().dtype(torch_api::kFloat32)));
}

mmltk::backend::models::rfdetr::PreparedTargets make_multi_targets() {
    return make_single_image_targets(
        torch_api::tensor({{0.5f, 0.5f, 0.2f, 0.2f}, {0.2f, 0.2f, 0.1f, 0.12f}}, torch_api::TensorOptions().dtype(torch_api::kFloat32)),
        torch_api::tensor({1, 2}, torch_api::TensorOptions().dtype(torch_api::kInt64)),
        torch_api::tensor({400.0f, 120.0f}, torch_api::TensorOptions().dtype(torch_api::kFloat32)));
}

mmltk::backend::models::rfdetr::DetectionConfig make_config() {
    mmltk::backend::models::rfdetr::DetectionConfig config;
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
    mmltk::backend::models::rfdetr::populate_default_detection_weight_dict(config);
    return config;
}

// Populates `layer` with the canonical two-query logits/boxes used across these tests.
template <typename OutputLayerT>
void fill_main_layer(OutputLayerT& layer) {
    layer.pred_logits =
        torch_api::tensor({{{-5.0f, 8.0f, -5.0f}, {6.0f, -5.0f, -5.0f}}}, torch_api::TensorOptions().dtype(torch_api::kFloat32));
    layer.pred_boxes =
        torch_api::tensor({{{0.5f, 0.5f, 0.2f, 0.2f}, {0.1f, 0.1f, 0.1f, 0.1f}}}, torch_api::TensorOptions().dtype(torch_api::kFloat32));
}

mmltk::backend::models::rfdetr::ModelOutputs make_outputs() {
    mmltk::backend::models::rfdetr::ModelOutputs outputs;
    fill_main_layer(outputs.main);
    outputs.aux_outputs.push_back(outputs.main);
    outputs.aux_outputs.push_back(outputs.main);
    outputs.enc_outputs = outputs.main;
    return outputs;
}

mmltk::backend::models::rfdetr::ModelOutputs make_distinct_layer_outputs() {
    auto outputs = make_outputs();
    outputs.aux_outputs[0].pred_logits = outputs.aux_outputs[0].pred_logits.add(0.3f);
    outputs.aux_outputs[0].pred_boxes = outputs.aux_outputs[0].pred_boxes.add(0.01f);
    outputs.aux_outputs[1].pred_logits = outputs.aux_outputs[1].pred_logits.sub(0.4f);
    outputs.aux_outputs[1].pred_boxes = outputs.aux_outputs[1].pred_boxes.sub(0.02f);
    auto& enc_outputs = require_optional_ref(outputs.enc_outputs, "expected encoder outputs in distinct layer outputs");
    enc_outputs.pred_logits = enc_outputs.pred_logits.add(0.1f);
    enc_outputs.pred_boxes = enc_outputs.pred_boxes.add(0.015f);
    return outputs;
}

mmltk::backend::models::rfdetr::ModelOutputs make_multi_match_outputs() {
    mmltk::backend::models::rfdetr::ModelOutputs outputs;
    outputs.main.pred_logits = torch_api::tensor({{{-5.0f, 8.0f, -5.0f}, {-5.0f, -5.0f, 8.0f}, {7.0f, -5.0f, -5.0f}}},
                                                 torch_api::TensorOptions().dtype(torch_api::kFloat32));
    outputs.main.pred_boxes = torch_api::tensor({{{0.52f, 0.48f, 0.22f, 0.18f}, {0.18f, 0.22f, 0.12f, 0.08f}, {0.9f, 0.9f, 0.05f, 0.05f}}},
                                                torch_api::TensorOptions().dtype(torch_api::kFloat32));
    return outputs;
}

mmltk::backend::models::rfdetr::ModelOutputs make_mask_outputs(int64_t height, int64_t width) {
    auto outputs = make_outputs();
    outputs.aux_outputs.clear();
    outputs.enc_outputs.reset();
    outputs.main.pred_masks = torch_api::zeros({1, 2, height, width}, torch_api::TensorOptions().dtype(torch_api::kFloat32));
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

float clamped_area(const float x1, const float y1, const float x2, const float y2) {
    return std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
}

float aligned_giou(const std::array<float, 4>& lhs_cxcywh, const std::array<float, 4>& rhs_cxcywh) {
    const auto lhs = cxcywh_to_xyxy(lhs_cxcywh);
    const auto rhs = cxcywh_to_xyxy(rhs_cxcywh);
    const float inter =
        clamped_area(std::max(lhs[0], rhs[0]), std::max(lhs[1], rhs[1]), std::min(lhs[2], rhs[2]), std::min(lhs[3], rhs[3]));

    const float lhs_area = clamped_area(lhs[0], lhs[1], lhs[2], lhs[3]);
    const float rhs_area = clamped_area(rhs[0], rhs[1], rhs[2], rhs[3]);
    const float uni = lhs_area + rhs_area - inter;
    const float iou = inter / uni;

    const float enc_area =
        clamped_area(std::min(lhs[0], rhs[0]), std::min(lhs[1], rhs[1]), std::max(lhs[2], rhs[2]), std::max(lhs[3], rhs[3]));
    return iou - (enc_area - uni) / enc_area;
}

// Requires exactly one image with `match_count` matched query/target rows.
template <typename MatcherIndices>
void assert_single_image_match_count(const MatcherIndices& indices, const int64_t match_count) {
    REQUIRE(indices.size() == 1);
    REQUIRE(indices[0].first.size(0) == match_count);
    REQUIRE(indices[0].second.size(0) == match_count);
}

// Requires exactly one image whose single match pairs query 0 with target 0.
template <typename MatcherIndices>
void assert_single_trivial_match(const MatcherIndices& indices) {
    assert_single_image_match_count(indices, 1);
    REQUIRE(indices[0].first.template item<int64_t>() == 0);
    REQUIRE(indices[0].second.template item<int64_t>() == 0);
}

// Single-image targets with a packed mask that sets the given (y, x) cells of a height x width grid.
mmltk::backend::models::rfdetr::PreparedTargets make_packed_mask_targets(const int64_t height, const int64_t width,
                                                                         const std::initializer_list<std::pair<int64_t, int64_t>> cells) {
    auto targets = make_targets();
    auto target_masks = torch_api::zeros({1, height, width}, torch_api::TensorOptions().dtype(torch_api::kFloat32));
    for (const auto& [y, x] : cells) {
        target_masks.index_put_({0, y, x}, 1.0f);
    }
    targets.packed_masks = pack_dense_masks(target_masks);
    return targets;
}

void test_weight_dict_population() {
    const auto config = make_config();
    REQUIRE(config.weight_dict.size() == 12);
    REQUIRE(config.weight_dict[0].first == "loss_ce");
    REQUIRE(config.weight_dict[3].first == "loss_ce_0");
    REQUIRE(config.weight_dict[6].first == "loss_ce_1");
    REQUIRE(config.weight_dict[9].first == "loss_ce_enc");
}

void test_matcher_and_losses() {
    const auto config = make_config();
    const auto outputs = make_outputs();
    const auto targets = make_targets();

    const auto indices = mmltk::backend::models::rfdetr::matcher_indices(outputs, targets, config, true);
    assert_single_trivial_match(indices);

    const auto loss_dict = mmltk::backend::models::rfdetr::detection_loss_dict(outputs, targets, config, true, false);
    REQUIRE(loss_dict.count("loss_ce") == 1);
    REQUIRE(loss_dict.count("loss_bbox") == 1);
    REQUIRE(loss_dict.count("loss_giou") == 1);
    REQUIRE(loss_dict.count("cardinality_error") == 1);
    REQUIRE(loss_dict.count("loss_ce_0") == 1);
    REQUIRE(loss_dict.count("loss_ce_1") == 1);
    REQUIRE(loss_dict.count("loss_ce_enc") == 1);

    const auto weighted = mmltk::backend::models::rfdetr::weighted_detection_loss(loss_dict, config, torch_api::Device(torch_api::kCPU));
    REQUIRE(torch_api::isfinite(weighted).item<bool>());
    REQUIRE(weighted.item<float>() > 0.0f);
}

void test_batched_matcher_transfer_matches_single_layer_losses() {
    const auto config = make_config();
    const auto targets = make_targets();
    auto outputs = make_distinct_layer_outputs();
    auto& enc_outputs = require_optional_ref(outputs.enc_outputs, "expected encoder outputs in batched matcher transfer test");
    enc_outputs.pred_logits = enc_outputs.pred_logits.narrow(1, 0, 1).clone();
    enc_outputs.pred_boxes = enc_outputs.pred_boxes.narrow(1, 0, 1).clone();
    const auto combined = mmltk::backend::models::rfdetr::detection_loss_dict(outputs, targets, config, true, false);

    mmltk::backend::models::rfdetr::DetectionConfig single_config = config;
    single_config.aux_loss = false;
    single_config.two_stage = false;
    mmltk::backend::models::rfdetr::populate_default_detection_weight_dict(single_config);

    auto main_only = outputs;
    main_only.aux_outputs.clear();
    main_only.enc_outputs.reset();
    const auto main_loss = mmltk::backend::models::rfdetr::detection_loss_dict(main_only, targets, single_config, true, false);
    REQUIRE(torch_api::allclose(combined.at("loss_ce"), main_loss.at("loss_ce"), 1.0e-6, 1.0e-6));
    REQUIRE(torch_api::allclose(combined.at("loss_bbox"), main_loss.at("loss_bbox"), 1.0e-6, 1.0e-6));
    REQUIRE(torch_api::allclose(combined.at("loss_giou"), main_loss.at("loss_giou"), 1.0e-6, 1.0e-6));

    for (size_t aux_index = 0; aux_index < outputs.aux_outputs.size(); ++aux_index) {
        mmltk::backend::models::rfdetr::ModelOutputs aux_only;
        aux_only.main = outputs.aux_outputs[aux_index];
        const auto aux_loss = mmltk::backend::models::rfdetr::detection_loss_dict(aux_only, targets, single_config, true, false);
        const std::string suffix = "_" + std::to_string(aux_index);
        REQUIRE(torch_api::allclose(combined.at("loss_ce" + suffix), aux_loss.at("loss_ce"), 1.0e-6, 1.0e-6));
        REQUIRE(torch_api::allclose(combined.at("loss_bbox" + suffix), aux_loss.at("loss_bbox"), 1.0e-6, 1.0e-6));
        REQUIRE(torch_api::allclose(combined.at("loss_giou" + suffix), aux_loss.at("loss_giou"), 1.0e-6, 1.0e-6));
    }

    mmltk::backend::models::rfdetr::ModelOutputs enc_only;
    enc_only.main = require_optional_ref(outputs.enc_outputs, "expected encoder outputs when building encoder-only loss");
    const auto enc_loss = mmltk::backend::models::rfdetr::detection_loss_dict(enc_only, targets, single_config, true, false);
    REQUIRE(torch_api::allclose(combined.at("loss_ce_enc"), enc_loss.at("loss_ce"), 1.0e-6, 1.0e-6));
    REQUIRE(torch_api::allclose(combined.at("loss_bbox_enc"), enc_loss.at("loss_bbox"), 1.0e-6, 1.0e-6));
    REQUIRE(torch_api::allclose(combined.at("loss_giou_enc"), enc_loss.at("loss_giou"), 1.0e-6, 1.0e-6));
}

void test_multi_match_box_losses() {
    const auto config = make_config();
    const auto outputs = make_multi_match_outputs();
    const auto targets = make_multi_targets();

    const auto indices = mmltk::backend::models::rfdetr::matcher_indices(outputs, targets, config, true);
    assert_single_image_match_count(indices, 2);

    const auto loss_dict = mmltk::backend::models::rfdetr::detection_loss_dict(outputs, targets, config, true, false);
    REQUIRE(loss_dict.count("loss_bbox") == 1);
    REQUIRE(loss_dict.count("loss_giou") == 1);

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
    REQUIRE(std::fabs(actual_bbox - expected_bbox) < 1.0e-5f);
    REQUIRE(std::fabs(actual_giou - expected_giou) < 1.0e-5f);
}

void test_matcher_sanitizes_nonfinite_costs() {
    auto outputs = make_outputs();
    outputs.main.pred_boxes.index_put_({0, 0, 0}, std::numeric_limits<float>::quiet_NaN());

    const auto config = make_config();
    const auto targets = make_targets();
    const auto indices = mmltk::backend::models::rfdetr::matcher_indices(outputs, targets, config, true);

    assert_single_image_match_count(indices, 1);
}

void test_rectangular_matcher_layers_preserve_all_targets() {
    namespace rfdetr = mmltk::backend::models::rfdetr;
    const int64_t target_count = GENERATE(2, 300, 404);
    const int64_t groups = GENERATE(1, 2);
    auto config = make_config();
    config.group_detr = static_cast<int>(groups);
    config.dec_layers = 2;
    rfdetr::populate_default_detection_weight_dict(config);
    const auto float_options = torch_api::TensorOptions().dtype(torch_api::kFloat32);
    const auto integer_options = float_options.dtype(torch_api::kInt64);
    auto targets = make_single_image_targets(torch_api::full({target_count + 1, 4}, 0.2F, float_options),
                                               torch_api::zeros({target_count + 1}, integer_options),
                                               torch_api::ones({target_count + 1}, float_options));
    targets.resolved_query_count = 300;
    targets.counts = {0, target_count, 1};
    targets.offsets = {0, 0, target_count};
    targets.targets.resize(3);
    for (size_t image = 0; image < targets.targets.size(); ++image) {
        targets.targets[image].boxes = targets.all_boxes.narrow(0, targets.offsets[image], targets.counts[image]);
        targets.targets[image].labels = targets.all_labels.narrow(0, targets.offsets[image], targets.counts[image]);
    }
    rfdetr::ModelOutputs outputs;
    outputs.aux_outputs.resize(1);
    outputs.enc_outputs.emplace();
    const std::array layers{&outputs.main, &outputs.aux_outputs.front(), &*outputs.enc_outputs};
    const std::array<int64_t, 3> query_counts{300, 150, 600};
    std::vector<std::pair<torch_api::Tensor, torch_api::Tensor>> retained;
    for (size_t layer_index = 0; layer_index < layers.size(); ++layer_index) {
        auto& layer = *layers[layer_index];
        const int64_t queries = query_counts[layer_index];
        layer.pred_logits = torch_api::zeros({3, queries, config.num_classes}, float_options.requires_grad(true));
        layer.pred_boxes = torch_api::full({3, queries, 4}, 0.2F, float_options);
        layer.pred_boxes.index_put_({Slice(), Slice(), 0}, 0.3F);
        layer.pred_boxes.set_requires_grad(true);
        rfdetr::ModelOutputs single;
        single.main = layer;
        const auto matches = rfdetr::matcher_indices(single, targets, config, true);
        REQUIRE(matches.size() == 3);
        for (size_t image = 0; image < matches.size(); ++image) {
            const int64_t per_group = std::min(queries / groups, targets.counts[image]);
            REQUIRE(matches[image].first.numel() == per_group * groups);
            REQUIRE(matches[image].second.numel() == per_group * groups);
            // SciPy's constant-cost rectangular assignment selects the diagonal
            // in both orientations, then concatenates groups in query order.
            const auto diagonal = torch_api::arange(per_group, integer_options);
            for (int64_t group = 0; group < groups; ++group) {
                REQUIRE(torch_api::equal(matches[image].first.narrow(0, group * per_group, per_group),
                                           diagonal + group * (queries / groups)));
                REQUIRE(torch_api::equal(matches[image].second.narrow(0, group * per_group, per_group), diagonal));
            }
        }
        if (layer_index == 0) retained = matches;
    }
    const auto losses = rfdetr::detection_loss_dict(outputs, targets, config, true, false);
    const std::array<std::string, 3> suffixes{"", "_0", "_enc"};
    for (size_t layer_index = 0; layer_index < layers.size(); ++layer_index) {
        const auto matched = std::min(query_counts[layer_index] / groups, target_count) + 1;
        const float expected = 0.1F * static_cast<float>(matched) / static_cast<float>(target_count + 1);
        REQUIRE(std::fabs(losses.at("loss_bbox" + suffixes[layer_index]).item<float>() - expected) < 1.0e-5F);
    }
    const auto weighted = rfdetr::weighted_detection_loss(losses, config, torch_api::Device(torch_api::kCPU));
    REQUIRE(torch_api::isfinite(weighted).item<bool>());
    weighted.backward();
    for (const auto* layer : layers) {
        REQUIRE(torch_api::isfinite(layer->pred_logits.grad()).all().item<bool>());
        REQUIRE(torch_api::isfinite(layer->pred_boxes.grad()).all().item<bool>());
    }
    const auto retained_count = std::min<int64_t>(300 / groups, target_count);
    REQUIRE(torch_api::equal(retained[1].second, torch_api::arange(retained_count, integer_options).repeat({groups})));
    REQUIRE(targets.all_labels.numel() == target_count + 1);
    REQUIRE(targets.counts[1] == target_count);
    if (target_count == 404) {
        auto selected = outputs;
        selected.main.pred_logits = outputs.main.pred_logits.detach().clone();
        selected.main.pred_logits.index_put_({1, 0, 1}, 10.0F);
        targets.all_labels.index_put_({403}, 1);
        const auto last_target = rfdetr::matcher_indices(selected, targets, config, true);
        REQUIRE(last_target[1].second.eq(403).any().item<bool>());
    }
    config.group_detr = 7;
    REQUIRE_THROWS(rfdetr::matcher_indices(outputs, targets, config, true));
}

auto make_mask_loss_config(const int mask_point_sample_ratio = 4) {
    auto config = make_config();
    config.include_masks = true;
    config.aux_loss = false;
    config.two_stage = false;
    config.mask_point_sample_ratio = mask_point_sample_ratio;
    mmltk::backend::models::rfdetr::populate_default_detection_weight_dict(config);
    return config;
}

void test_cpu_mask_loss_uses_upstream_keys() {
    constexpr int64_t kHeight = 4;
    constexpr int64_t kWidth = 4;

    auto targets = make_packed_mask_targets(kHeight, kWidth, {{1, 1}, {1, 2}, {2, 1}, {2, 2}});

    auto config = make_mask_loss_config();

    auto outputs = make_mask_outputs(kHeight, kWidth);
    const auto loss_dict = mmltk::backend::models::rfdetr::detection_loss_dict(outputs, targets, config, true, false);

    REQUIRE(loss_dict.count("loss_mask_ce") == 1);
    REQUIRE(loss_dict.count("loss_mask_dice") == 1);
    REQUIRE(loss_dict.count("loss_dice") == 0);

    const auto weighted = mmltk::backend::models::rfdetr::weighted_detection_loss(loss_dict, config, torch_api::Device(torch_api::kCPU));
    REQUIRE(torch_api::isfinite(weighted).item<bool>());
}

void test_sparse_mask_loss_matches_dense_reference() {
    constexpr int64_t kHeight = 8;
    constexpr int64_t kWidth = 8;
    constexpr int64_t kChannels = 2;

    auto targets = make_packed_mask_targets(kHeight, kWidth, {{2, 2}, {2, 3}, {2, 4}, {3, 2}, {3, 3}, {3, 4}});

    auto config = make_mask_loss_config();

    auto spatial_features =
        torch_api::linspace(0.05f, 1.28f, kChannels * kHeight * kWidth, torch_api::TensorOptions().dtype(torch_api::kFloat32))
            .view({1, kChannels, kHeight, kWidth});
    auto query_features = torch_api::tensor({{{0.35f, -0.15f}, {-0.2f, 0.4f}}}, torch_api::TensorOptions().dtype(torch_api::kFloat32));
    auto bias = torch_api::tensor({0.05f}, torch_api::TensorOptions().dtype(torch_api::kFloat32));
    auto dense_masks = torch_api::einsum("bchw,bnc->bnhw", {spatial_features, query_features}).add(bias);

    mmltk::backend::models::rfdetr::ModelOutputs dense_outputs;
    fill_main_layer(dense_outputs.main);
    dense_outputs.main.pred_masks = dense_masks;

    mmltk::backend::models::rfdetr::ModelOutputs sparse_outputs;
    sparse_outputs.main.pred_logits = dense_outputs.main.pred_logits.clone();
    sparse_outputs.main.pred_boxes = dense_outputs.main.pred_boxes.clone();
    sparse_outputs.main.sparse_pred_masks = mmltk::backend::models::rfdetr::OutputLayer::SparsePredMasks{
        spatial_features,
        query_features,
        bias,
    };

    torch_api::manual_seed(1234);
    const auto dense_loss = mmltk::backend::models::rfdetr::detection_loss_dict(dense_outputs, targets, config, true, false);
    torch_api::manual_seed(1234);
    const auto sparse_loss = mmltk::backend::models::rfdetr::detection_loss_dict(sparse_outputs, targets, config, true, false);

    REQUIRE(torch_api::allclose(dense_loss.at("loss_mask_ce"), sparse_loss.at("loss_mask_ce"), 1.0e-5, 1.0e-5));
    REQUIRE(torch_api::allclose(dense_loss.at("loss_mask_dice"), sparse_loss.at("loss_mask_dice"), 1.0e-5, 1.0e-5));
}

void test_matcher_mask_cost_handles_zero_point_sampling_on_cuda() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }

    auto targets = make_targets();
    targets.packed_masks = pack_dense_masks(torch_api::ones({1, 1, 1}, torch_api::TensorOptions().dtype(torch_api::kFloat32)));
    const auto& packed_masks = targets.packed_masks.value();
    targets.packed_masks = mmltk::backend::models::rfdetr::PackedTargetMasks{
        packed_masks.bits.to(torch_api::kCUDA),
        packed_masks.height,
        packed_masks.width,
    };

    auto config = make_mask_loss_config(16);

    auto outputs = make_mask_outputs(1, 1);
    outputs.main.pred_logits = outputs.main.pred_logits.to(torch_api::kCUDA);
    outputs.main.pred_boxes = outputs.main.pred_boxes.to(torch_api::kCUDA);
    outputs.main.pred_masks =
        require_optional_ref(outputs.main.pred_masks, "expected mask outputs for CUDA matcher test").to(torch_api::kCUDA);

    const auto indices = mmltk::backend::models::rfdetr::matcher_indices(outputs, targets, config, true);
    assert_single_trivial_match(indices);
}

void test_packed_mask_sampling_matches_grid_sample_nearest_boundaries() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }

    auto dense_masks = torch_api::tensor({{{0.0f, 1.0f, 0.0f, 1.0f}}}, torch_api::TensorOptions().dtype(torch_api::kFloat32));
    const auto packed = pack_dense_masks(dense_masks);
    // CLEANUP-IGNORE: Packed-mask boundary coordinates are independent from supervision CUDA fixtures.
    const auto packed_bits = packed.bits.to(torch_api::kCUDA);
    const auto mask_indices = torch_api::tensor({0}, torch_api::TensorOptions().dtype(torch_api::kInt64).device(torch_api::kCUDA));
    const auto point_coords = torch_api::tensor({{{0.0f, 0.5f},
                                                  {0.125f, 0.5f},
                                                  {0.25f, 0.5f},
                                                  {0.375f, 0.5f},
                                                  {0.5f, 0.5f},
                                                  {0.625f, 0.5f},
                                                  {0.75f, 0.5f},
                                                  {0.875f, 0.5f},
                                                  {1.0f, 0.5f}}},
                                                torch_api::TensorOptions().dtype(torch_api::kFloat32).device(torch_api::kCUDA));

    const auto sampled =
        mmltk::backend::models::rfdetr::sample_packed_masks_cuda(packed_bits, packed.height, packed.width, mask_indices, point_coords);

    const auto dense_cuda = dense_masks.unsqueeze(1).to(torch_api::kCUDA);
    const auto expected =
        F::grid_sample(dense_cuda, point_coords.unsqueeze(2).mul(2.0f).sub(1.0f),
                       F::GridSampleFuncOptions().mode(torch_api::kNearest).padding_mode(torch_api::kBorder).align_corners(false))
            .squeeze(3)
            .squeeze(1);

    REQUIRE(torch_api::equal(sampled.cpu(), expected.cpu()));
}

constexpr std::array<std::pair<int64_t, int64_t>, 5> postprocess_geometries{{
    {80, 160}, {80, 160}, {96, 40}, {32, 224}, {80, 160},
}};

using GeometrySelections = std::array<mmltk::backend::models::rfdetr::PostprocessedSelection, postprocess_geometries.size()>;

GeometrySelections select_postprocess_geometries(torch_api::Device device) {
    namespace rfdetr = mmltk::backend::models::rfdetr;
    const auto options = torch_api::TensorOptions().dtype(torch_api::kFloat32).device(device);
    rfdetr::OutputTensors outputs;
    outputs.pred_logits = torch_api::tensor({{{0.F, 2.F}, {4.F, -2.F}}}, options);
    outputs.pred_boxes = torch_api::tensor({{{0.5F, 0.5F, 0.5F, 0.25F}, {0.75F, 0.25F, 0.25F, 0.5F}}}, options);
    GeometrySelections selections;
    for (std::size_t index = 0; index < postprocess_geometries.size(); ++index) {
        const auto [height, width] = postprocess_geometries[index];
        selections[index] = rfdetr::select_output_batch_fixed_size(outputs, height, width, 3);
    }
    return selections;
}

void require_postprocess_geometries(const GeometrySelections& selections) {
    const auto expected_scores = torch_api::tensor({{1.F / (1.F + std::exp(-4.F)), 1.F / (1.F + std::exp(-2.F)), 0.5F}});
    const auto expected_labels = torch_api::tensor({{0, 1, 0}}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    const auto expected_queries = torch_api::tensor({{1, 0, 0}}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    // Check every retained result only after all replacements have been submitted.
    for (std::size_t index = 0; index < postprocess_geometries.size(); ++index) {
        const auto [height, width] = postprocess_geometries[index];
        const auto h = static_cast<float>(height);
        const auto w = static_cast<float>(width);
        const auto expected_boxes = torch_api::tensor({{{0.625F * w, 0.F, 0.875F * w, 0.5F * h},
                                                       {0.25F * w, 0.375F * h, 0.75F * w, 0.625F * h},
                                                       {0.25F * w, 0.375F * h, 0.75F * w, 0.625F * h}}});
        const auto& selected = selections[index];
        CHECK(torch_api::equal(selected.boxes.cpu(), expected_boxes));
        CHECK(torch_api::allclose(selected.scores.cpu(), expected_scores));
        CHECK(torch_api::equal(selected.labels.cpu(), expected_labels));
        CHECK(torch_api::equal(selected.query_indices.cpu(), expected_queries));
        CHECK_FALSE(selected.mask_logits.has_value());
    }
}

void test_postprocess() {
    const auto outputs = make_outputs();
    const auto target_sizes = torch_api::tensor({{100, 100}}, torch_api::TensorOptions().dtype(torch_api::kInt64));
    const auto results = mmltk::backend::models::rfdetr::postprocess_outputs(outputs, target_sizes, 2);
    REQUIRE(results.size() == 1);
    const auto& result = results.front();
    REQUIRE(result.count("scores") == 1);
    REQUIRE(result.count("labels") == 1);
    REQUIRE(result.count("boxes") == 1);
    REQUIRE(result.at("scores").size(0) == 2);
    REQUIRE(result.at("labels").size(0) == 2);
    REQUIRE(result.at("boxes").size(0) == 2);
    REQUIRE(result.at("boxes").size(1) == 4);

    const auto boxes = result.at("boxes").cpu();
    const auto box_values = boxes.accessor<float, 2>();
    REQUIRE(std::fabs(box_values[0][0] - 40.0f) < 1e-4f);
    REQUIRE(std::fabs(box_values[0][1] - 40.0f) < 1e-4f);
    REQUIRE(std::fabs(box_values[0][2] - 60.0f) < 1e-4f);
    REQUIRE(std::fabs(box_values[0][3] - 60.0f) < 1e-4f);

    require_postprocess_geometries(select_postprocess_geometries(torch_api::kCPU));
}

void test_postprocess_cuda_stream_replacement() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }

    const auto first_stream = c10::cuda::getStreamFromPool(false, 0);
    const auto second_stream = c10::cuda::getStreamFromPool(false, 0);
    c10::cuda::CUDAStreamGuard first_guard(first_stream);
    const auto first = select_postprocess_geometries(torch_api::Device(torch_api::kCUDA, 0));
    GeometrySelections second;
    {
        c10::cuda::CUDAStreamGuard second_guard(second_stream);
        second = select_postprocess_geometries(torch_api::Device(torch_api::kCUDA, 0));
    }
    const auto revisited = select_postprocess_geometries(torch_api::Device(torch_api::kCUDA, 0));
    require_postprocess_geometries(first);
    require_postprocess_geometries(revisited);
    {
        c10::cuda::CUDAStreamGuard second_guard(second_stream);
        require_postprocess_geometries(second);
    }
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_weight_dict_population);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_matcher_and_losses);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_batched_matcher_transfer_matches_single_layer_losses);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_multi_match_box_losses);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_matcher_sanitizes_nonfinite_costs);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_rectangular_matcher_layers_preserve_all_targets);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_cpu_mask_loss_uses_upstream_keys);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_sparse_mask_loss_matches_dense_reference);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_matcher_mask_cost_handles_zero_point_sampling_on_cuda);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_packed_mask_sampling_matches_grid_sample_nearest_boundaries);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops]", test_postprocess);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_ops][cuda]", test_postprocess_cuda_stream_replacement);

TEST_CASE("LSAP solver arrays use the owning node resource and retain capacity", "[rfdetr][lsap][numa]") {
    using namespace mmltk::backend::models::rfdetr;
    using namespace mmltk::common::system;
    const auto topology = NumaTopology::Capture();
    const auto cpu = topology.permitted_cpus.front();
    const auto node = std::ranges::find(topology.cpus, cpu, &CpuTopology::cpu)->node;
    NumaMemoryResource memory(node);
    LsapScratch scratch(&memory);
    scratch.costs = {4, 1, 3, 2, 0, 5};
    scratch.row_indices.resize(2);
    scratch.col_indices.resize(2);
    REQUIRE(solve_rectangular_linear_sum_assignment(2, 3, scratch.costs.data(), false, scratch.row_indices.data(),
                                                    scratch.col_indices.data(), scratch.solver) == RectangularLsApStatus::kOk);
    CHECK(scratch.col_indices[0] == 1);
    CHECK(scratch.col_indices[1] == 0);
    auto* duals = scratch.solver.column_duals.data();
    const auto capacity = scratch.solver.column_duals.capacity();
    REQUIRE(solve_rectangular_linear_sum_assignment(2, 3, scratch.costs.data(), false, scratch.row_indices.data(),
                                                    scratch.col_indices.data(), scratch.solver) == RectangularLsApStatus::kOk);
    CHECK(scratch.solver.column_duals.data() == duals);
    CHECK(scratch.solver.column_duals.capacity() == capacity);
    CHECK(scratch.costs.get_allocator().resource() == &memory);
    CHECK(scratch.solver.column_duals.get_allocator().resource() == &memory);
    CHECK(scratch.solver.visited_columns.get_allocator().resource() == &memory);
    CHECK(scratch.row_indices.get_allocator().resource() == &memory);
    // Tall and maximizing cases exercise the PMR transpose and sorting workspaces.
    REQUIRE(solve_rectangular_linear_sum_assignment(3, 2, scratch.costs.data(), true, scratch.row_indices.data(),
                                                    scratch.col_indices.data(), scratch.solver) == RectangularLsApStatus::kOk);
    CHECK(scratch.solver.transposed_cost.get_allocator().resource() == &memory);
    CHECK(scratch.solver.sorted_indices.get_allocator().resource() == &memory);
}

TEST_CASE("Criterion losses and gradients share one assignment upload under both transports", "[rfdetr][matcher][criterion][cuda][numa]") {
    using namespace mmltk::backend::models::rfdetr;
    const bool selected_h2d = GENERATE(true, false);
    const int group = GENERATE(1, 2);
    if (mmltk::testsupport::checked_cuda_device_count() == 0) SKIP("CUDA unavailable; criterion transport parity remains unverified");
    c10::cuda::CUDAGuard guard(static_cast<c10::DeviceIndex>(0));
    if (!selected_h2d) {
        int mmap = 0;
        const auto status = cuDeviceGetAttribute(&mmap, static_cast<CUdevice_attribute>(152), 0);
        REQUIRE((status == CUDA_SUCCESS || status == CUDA_ERROR_INVALID_VALUE));
        if (!(status == CUDA_SUCCESS && mmap) && ::access("/dev/gdrdrv", R_OK | W_OK) != 0)
            SKIP("GDR unavailable; criterion GDR loss and gradient parity remain unverified");
    }
    const auto execution = mmltk::frameworks::gpu::test_support::selected_test_device(0, mmltk::common::system::NumaTopology::Capture());
    const auto& p = execution.placement;
    mmltk::common::system::ScopedExecutionPolicy policy({p.cpus, {}, 0, p.numa_node, -10, false});
    const torch_api::Device device(torch_api::kCUDA, 0);
    auto config = make_mask_loss_config();
    config.group_detr = group;
    config.aux_loss = true;
    config.two_stage = true;
    populate_default_detection_weight_dict(config);
    auto targets = make_packed_mask_targets(4, 4, {{1, 1}, {1, 2}, {2, 1}, {2, 2}});
    targets.all_boxes = targets.all_boxes.to(device);
    targets.all_labels = targets.all_labels.to(device);
    targets.packed_masks->bits = targets.packed_masks->bits.to(device);
    targets.target_offsets = torch_api::tensor(targets.offsets, torch_api::TensorOptions().dtype(torch_api::kInt64).device(device));
    targets.target_counts = torch_api::tensor(targets.counts, torch_api::TensorOptions().dtype(torch_api::kInt64).device(device));

    struct Outcome final {
        TensorMap losses;
        std::vector<torch_api::Tensor> gradients;
        MatcherStatistics statistics;
    };
    const auto evaluate = [&](bool h2d) {
        MatcherWorkspace workspace(p.numa_node, h2d);
        workspace.enable_statistics();
        ScopedRuntimeContext scope(nullptr, 0, &workspace);
        auto outputs = make_mask_outputs(4, 4);
        outputs.aux_outputs = {outputs.main, outputs.main};
        outputs.enc_outputs = outputs.main;
        outputs.aux_outputs[0].pred_logits = outputs.main.pred_logits.repeat({1, 2, 1});
        outputs.aux_outputs[0].pred_boxes = outputs.main.pred_boxes.repeat({1, 2, 1});
        outputs.aux_outputs[0].pred_masks = outputs.main.pred_masks->repeat({1, 2, 1, 1});
        std::vector<torch_api::Tensor> inputs;
        const auto prepare = [&](OutputLayer& layer) {
            layer.pred_logits = layer.pred_logits.to(device).detach().set_requires_grad(true);
            layer.pred_boxes = layer.pred_boxes.to(device).detach().set_requires_grad(true);
            layer.pred_masks = layer.pred_masks->to(device).detach().set_requires_grad(true);
            inputs.push_back(layer.pred_logits);
            inputs.push_back(layer.pred_boxes);
            inputs.push_back(*layer.pred_masks);
        };
        prepare(outputs.main);
        for (auto& layer : outputs.aux_outputs)
            prepare(layer);
        prepare(*outputs.enc_outputs);
        torch_api::manual_seed(7921);
        auto losses = detection_loss_dict(outputs, targets, config, true, false);
        auto total = weighted_detection_loss(losses, config, device);
        total.backward();
        workspace.complete_assignments(c10::cuda::getCurrentCUDAStream(0).stream());
        REQUIRE(workspace.statistics().completion_dependencies == 1);
        Outcome outcome;
        for (const auto& [name, loss] : losses)
            outcome.losses[name] = loss.detach().cpu();
        for (const auto& input : inputs) {
            REQUIRE(input.grad().defined());
            outcome.gradients.push_back(input.grad().cpu());
        }
        outcome.statistics = workspace.statistics();
        return outcome;
    };
    const auto baseline = evaluate(true);
    const auto observed = evaluate(selected_h2d);
    REQUIRE(observed.losses.size() == baseline.losses.size());
    for (const auto& [name, expected] : baseline.losses)
        REQUIRE(torch_api::allclose(observed.losses.at(name), expected, 1e-6, 1e-6));
    REQUIRE(observed.gradients.size() == baseline.gradients.size());
    for (std::size_t index = 0; index < baseline.gradients.size(); ++index)
        REQUIRE(torch_api::allclose(observed.gradients[index], baseline.gradients[index], 1e-6, 1e-6));
    REQUIRE(observed.statistics.cost_submissions == 1);
    REQUIRE(observed.statistics.cost_dependencies == 1);
    REQUIRE(observed.statistics.cost_bytes == 4 * 1 * 4 * 1 * sizeof(float));
    REQUIRE(observed.statistics.materializations == 1);
    REQUIRE(observed.statistics.assignment_bytes == 4 * group * 3 * sizeof(std::int64_t));
    REQUIRE(observed.statistics.uploads == 1);
    REQUIRE(observed.statistics.h2d_submissions == (selected_h2d ? 1 : 0));
    REQUIRE(observed.statistics.gdr_writes == (selected_h2d ? 0 : 1));
}

TEST_CASE("native inference requests valid optional mask outputs", "[model][rfdetr][native_ops]") {
    namespace rfdetr = mmltk::backend::models::rfdetr;
    auto config = rfdetr::native_config_from_preset(rfdetr::model_presets().front());
    config.resolution = 64;
    config.segmentation = true;
    config.num_queries = 3;
    config.num_select = 3;
    config.group_detr = 1;
    config.dec_layers = 1;
    config.aux_loss = false;
    rfdetr::NativeRfDetrModel model(config);
    auto& owner = rfdetr::detail::native_model_owner(model);
    owner.module().eval();
    torch::NoGradGuard no_grad;
    const auto image = torch_api::zeros({3, 64, 64});
    const auto input = rfdetr::nested_tensor_from_tensor_list({image});
    CHECK_FALSE(owner.forward(input, false).main.pred_masks.has_value());
    const auto output = owner.forward(input, true);
    REQUIRE(output.main.pred_masks.has_value());
    CHECK(output.main.pred_masks->size(1) == config.num_queries);
}

TEST_CASE("mask materialization follows selected query identity and bounded reusable chunks", "[model][rfdetr][native_ops]") {
    namespace rfdetr = mmltk::backend::models::rfdetr;
    rfdetr::OutputTensors output;
    output.pred_logits = torch_api::tensor({{{-10.F, 8.F}, {10.F, -10.F}, {-10.F, -10.F}}});
    output.pred_boxes = torch_api::ones({1, 3, 4});
    CHECK_THROWS_AS(rfdetr::select_output_batch_fixed_size(output, 1080, 1920, 500, true), std::runtime_error);
    output.pred_masks = torch_api::tensor({1.F, -1.F, 1.F}).view({1, 3, 1, 1});
    auto selected = rfdetr::select_output_batch_fixed_size(output, 1080, 1920, 500, true);
    CHECK(selected.scores.size(1) == 6);
    CHECK(selected.labels[0][0].item<int64_t>() == 0);
    CHECK(selected.labels[0][1].item<int64_t>() == 1);
    CHECK(selected.query_indices[0][0].item<int64_t>() == 1);
    CHECK(selected.query_indices[0][1].item<int64_t>() == 0);
    CHECK(selected.mask_logits->numel() == 3);
    rfdetr::SelectedMaskWorkspace workspace;
    auto masks = workspace.Materialize(*selected.mask_logits, selected.query_indices.narrow(1, 0, 2), 1080, 1920);
    CHECK(masks.numel() == 2 * 1080 * 1920);
    CHECK_FALSE(masks[0][0].any().item<bool>());
    CHECK(masks[0][1].all().item<bool>());
    const auto address = masks.data_ptr();
    CHECK(workspace.Materialize(*selected.mask_logits, selected.query_indices.narrow(1, 0, 2), 1080, 1920).data_ptr() == address);
    CHECK_THROWS_AS(workspace.Materialize(*selected.mask_logits, selected.query_indices.narrow(1, 0, 2), 65536, 65536), std::invalid_argument);
    output.pred_masks = torch_api::ones({1, 2, 1, 1});
    CHECK_THROWS_AS(rfdetr::select_output_batch_fixed_size(output, 2, 2, 2, true), std::invalid_argument);
}

TEST_CASE("Declared class slots are filtered before ranking without losing query masks", "[model][rfdetr][layout]") {
    namespace r = mmltk::backend::models::rfdetr;
    namespace catalog = mmltk::backend::data::catalog;
    for (std::uint32_t background = 0; background < 3; ++background) {
        auto record = r::native_training_class_layout(catalog::ClassCatalog({"cat", "dog"}));
        record.no_object = r::NoObjectEncoding::ExplicitBackground;
        std::uint32_t foreground = 0;
        for (std::uint32_t slot = 0; slot < 3; ++slot)
            record.slots[slot] = slot == background ? r::ModelClassSlot{r::ClassSlotRole::Background, std::nullopt} :
                r::ModelClassSlot{r::ClassSlotRole::Foreground, foreground++};
        auto layout = std::make_shared<const r::ResolvedClassLayout>(record);
        r::ClassPostprocessLane classes(layout);
        classes.Prepare(torch_api::kCPU);
        r::OutputTensors outputs;
        outputs.pred_logits = torch_api::full({1, 2, 3}, -8.F);
        outputs.pred_logits.select(2, background).fill_(100.F);
        const auto first_slot = background == 0 ? 1 : 0;
        const auto last_slot = background == 2 ? 1 : 2;
        outputs.pred_logits[0][0][first_slot] = 4.F;
        outputs.pred_logits[0][1][last_slot] = 3.F;
        outputs.pred_boxes = torch_api::tensor({.25F, .25F, .2F, .2F, .75F, .75F, .2F, .2F}).view({1, 2, 4});
        outputs.pred_masks = torch_api::tensor({8.F, -8.F}).view({1, 2, 1, 1});
        const auto selected = r::select_output_batch_fixed_size(outputs, 10, 20, 2, true, &classes);
        REQUIRE(selected.labels[0][0].item<std::int64_t>() == 0);
        REQUIRE(selected.labels[0][1].item<std::int64_t>() == 1);
        REQUIRE(selected.query_indices[0][0].item<std::int64_t>() == 0);
        REQUIRE(selected.query_indices[0][1].item<std::int64_t>() == 1);
        CHECK(selected.scores[0][0].item<float>() < .99F);
        r::SelectedMaskWorkspace masks;
        const auto materialized = masks.Materialize(*selected.mask_logits, selected.query_indices, 10, 20);
        CHECK(materialized[0][0].all().item<bool>());
        CHECK_FALSE(materialized[0][1].any().item<bool>());
    }
    auto empty = std::make_shared<const r::ResolvedClassLayout>(r::native_training_class_layout(catalog::ClassCatalog{}));
    r::ClassPostprocessLane classes(empty);
    classes.Prepare(torch_api::kCPU);
    r::OutputTensors outputs{.pred_logits=torch_api::full({1, 2, 1}, 100.F), .pred_boxes=torch_api::zeros({1, 2, 4})};
    const auto selected = r::select_output_batch_fixed_size(outputs, 10, 20, 500, false, &classes);
    CHECK(selected.labels.numel() == 0);
    CHECK(selected.boxes.size(1) == 0);
}

TEST_CASE("Class lanes retain gather capacity and independently owned final labels", "[model][rfdetr][layout]") {
    namespace r = mmltk::backend::models::rfdetr;
    auto record = r::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"cat", "dog"}));
    record.slots = {{r::ClassSlotRole::Foreground, 1U}, {r::ClassSlotRole::Unused, {}}, {r::ClassSlotRole::Foreground, 0U}};
    r::ClassPostprocessLane lane(std::make_shared<const r::ResolvedClassLayout>(record));
    lane.Prepare(torch_api::kCPU);
    const auto logits = torch_api::tensor({1.F, 100.F, 2.F, 4.F, 100.F, 3.F}).view({1, 2, 3});
    const auto expected = torch_api::tensor({1.F, 2.F, 4.F, 3.F}).view({1, 2, 2});
    auto gathered = lane.Gather(logits.repeat({3, 1, 1}));
    const auto address = gathered.data_ptr();
    CHECK(torch_api::equal(gathered, expected.repeat({3, 1, 1})));
    for (const auto batch : {1, 2, 3, 1}) {
        auto value = lane.Gather(logits.repeat({batch, 1, 1}));
        CHECK(value.data_ptr() == address);
        CHECK(torch_api::equal(value, expected.repeat({batch, 1, 1})));
    }
    const auto larger = lane.Gather(logits.repeat({4, 1, 1}));
    CHECK(larger.data_ptr() != address);
    CHECK(torch_api::equal(larger, expected.repeat({4, 1, 1})));
    CHECK(lane.Gather(logits).data_ptr() == larger.data_ptr());
    r::OutputTensors output{.pred_logits = logits, .pred_boxes = torch_api::ones({1, 2, 4})};
    const auto first = r::select_output_batch_fixed_size(output, 8, 8, 2, false, &lane);
    const auto labels = first.labels.clone();
    const auto scores = first.scores.clone();
    output.pred_logits = -logits;
    const auto next = r::select_output_batch_fixed_size(output, 8, 8, 2, false, &lane);
    CHECK(torch_api::equal(first.labels, labels));
    CHECK(torch_api::equal(first.scores, scores));
    CHECK(first.labels.data_ptr() != next.labels.data_ptr());
    CHECK(first.labels[0][0].item<int64_t>() == 1);
    CHECK(first.labels[0][1].item<int64_t>() == 0);
    const auto doubled = lane.Gather(logits.to(torch_api::kFloat64));
    CHECK(doubled.data_ptr() != address);
    CHECK(torch_api::equal(doubled, expected.to(torch_api::kFloat64)));
    CHECK(lane.Gather(logits.to(torch_api::kFloat64)).data_ptr() == doubled.data_ptr());
    CHECK(lane.Gather(torch_api::zeros({0, 2, 3})).numel() == 0);
    CHECK(lane.Gather(torch_api::zeros({1, 0, 3})).numel() == 0);
    CHECK_THROWS(lane.Gather(torch_api::zeros({1, 2, 4})));
    CHECK(lane.Gather(logits.to(torch_api::kFloat64)).data_ptr() == doubled.data_ptr());
    r::ClassPostprocessLane empty(std::make_shared<const r::ResolvedClassLayout>(r::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog{})));
    empty.Prepare(torch_api::kCPU);
    const auto empty_input = torch_api::zeros({2, 3, 1});
    CHECK(empty.Gather(empty_input).numel() == 0);
    r::ClassPostprocessLane dense(std::make_shared<const r::ResolvedClassLayout>(r::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"cat", "dog"}))));
    dense.Prepare(torch_api::kCPU);
    CHECK(dense.Gather(logits).data_ptr() == logits.data_ptr());
}

TEST_CASE("Class lane device and stream rebind retains earlier final results", "[model][rfdetr][layout][gpu]") {
    namespace r = mmltk::backend::models::rfdetr;
    auto record = r::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"cat"}));
    record.slots = {{r::ClassSlotRole::Unused, {}}, {r::ClassSlotRole::Foreground, 0U}};
    r::ClassPostprocessLane lane(std::make_shared<const r::ResolvedClassLayout>(record));
    lane.Prepare(torch_api::kCPU);
    const auto cpu = torch_api::tensor({99.F, 3.F}).view({1, 1, 2});
    auto previous = lane.Gather(cpu);
    const auto device = torch_api::Device(torch_api::kCUDA, 0);
    const auto first_stream = c10::cuda::getStreamFromPool(false, 0);
    const auto second_stream = c10::cuda::getStreamFromPool(false, 0);
    torch_api::Tensor first_labels, first_scratch;
    {
        c10::cuda::CUDAStreamGuard guard(first_stream);
        lane.Prepare(device);
        const auto input = cpu.to(device);
        first_scratch = lane.Gather(input);
        const auto& scratch = first_scratch;
        CHECK(scratch.data_ptr() != previous.data_ptr());
        CHECK(lane.Gather(input).data_ptr() == scratch.data_ptr());
        first_labels = lane.References(torch_api::zeros({1, 1}, input.options().dtype(torch_api::kInt64)));
    }
    {
        c10::cuda::CUDAStreamGuard guard(second_stream);
        const auto input = cpu.to(device);
        CHECK_THROWS(lane.Gather(input));
        lane.Prepare(device);
        const auto scratch = lane.Gather(input);
        CHECK(scratch.data_ptr() != first_scratch.data_ptr());
        CHECK(scratch.cpu().item<float>() == 3.F);
        CHECK(lane.Gather(input).data_ptr() == scratch.data_ptr());
    }
    {
        c10::cuda::CUDAStreamGuard guard(first_stream);
        CHECK(first_labels.cpu().item<int64_t>() == 0);
    }
    lane.Prepare(torch_api::kCPU);
    CHECK(lane.Gather(cpu).item<float>() == 3.F);
    CHECK(previous.item<float>() == 3.F);
}
