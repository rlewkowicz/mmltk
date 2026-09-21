#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/common/system/tests/numa_topology_test_support.h"
#include <torch/utils.h>
#include <torch/nn/functional/vision.h>
#include <ATen/cuda/CUDAContext.h>
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
#include "src/backend/models/rfdetr/core/detection_ops.h"
#include "detail/lsap_scratch.h"
#include "detail/matcher_workspace.h"
#include "src/common/system/execution_policy.h"
#include "src/common/system/numa_memory.h"
#include "src/common/system/numa_topology.h"
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/core/postprocess.h"
#include "src/backend/models/rfdetr/core/model.h"
#include <torch/types.h>
#include <torch/serialize.h>
#include "src/test_support/cuda_test_utils.hpp"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/backend/models/rfdetr/core/runtime.h"
namespace mmltk::backend::models::rfdetr {
torch::Tensor sample_packed_masks_cuda(const torch::Tensor& packed_mask_bits, int64_t height, int64_t width, const torch::Tensor& mask_indices,
                                       const torch::Tensor& point_coords);
}
namespace {
using namespace torch::indexing;
namespace F = torch::nn::functional;
template <typename T>
[[nodiscard]] T& require_optional_ref(std::optional<T>& value, const std::string_view message) {
 if (!value.has_value()) throw std::runtime_error(std::string(message));
 return *value;
}
TEST_CASE("Cardinality measures computed sigmoid confidence and actual target counts", "[model][rfdetr][criterion]") {
 using mmltk::backend::models::rfdetr::cardinality_error;
 for (const auto dtype : {torch::kFloat32, torch::kFloat16, torch::kBFloat16}) {
  const auto options = torch::TensorOptions().dtype(dtype);
  // The last slot contributes to this diagnostic, independently of semantic layout.
  auto logits = torch::tensor({{{0.F, 0.F}, {-2.F, -1.F}, {-1.F, 2.F}}, {{1.F, -1.F}, {-1.F, 1.F}, {-1.F, -1.F}}}, options);
  auto counts = torch::tensor({0, 4}, torch::kInt64);
  CHECK(cardinality_error(logits, counts).item<float>() == 1.5F);
  CHECK(cardinality_error(torch::zeros({1, 300, 2}, options), torch::tensor({404}, torch::kInt64)).item<float>() == 404.F);
  CHECK(cardinality_error(torch::ones({1, 300, 2}, options), torch::tensor({404}, torch::kInt64)).item<float>() == 104.F);
  CHECK(cardinality_error(torch::empty({1, 0, 2}, options), torch::tensor({0}, torch::kInt64)).item<float>() == 0.F);
  // All three dtypes round this computed sigmoid to exactly 0.5.
  CHECK(cardinality_error(torch::full({1, 1, 2}, 1.0e-9F, options), torch::tensor({0}, torch::kInt64)).item<float>() == 0.F);
 }
 auto differentiable = torch::ones({1, 1, 2}, torch::TensorOptions().requires_grad(true));
 CHECK_FALSE(cardinality_error(differentiable, torch::tensor({0}, torch::kInt64)).requires_grad());
}
mmltk::backend::models::rfdetr::PackedTargetMasks pack_dense_masks(const torch::Tensor& dense_masks) {
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
    if (dense_a[mask_index][y][x] <= 0.0f) { continue; }
    const int64_t pixel_index = y * width + x;
    mask_words[pixel_index >> 6] |= uint64_t{1} << (pixel_index & 63);
   }
  }
 }
 return mmltk::backend::models::rfdetr::PackedTargetMasks{bits, height, width};
}
// Builds a 100x100 single-image target set from per-instance boxes, labels, and areas.
mmltk::backend::models::rfdetr::PreparedTargets make_single_image_targets(const torch::Tensor& boxes, const torch::Tensor& labels, const torch::Tensor& area) {
 const int64_t instance_count = labels.size(0);
 mmltk::backend::models::rfdetr::PreparedTarget target;
 target.image_id = torch::tensor({1}, torch::TensorOptions().dtype(torch::kInt64));
 target.orig_size = torch::tensor({100, 100}, torch::TensorOptions().dtype(torch::kInt64));
 target.size = torch::tensor({100, 100}, torch::TensorOptions().dtype(torch::kInt64));
 target.boxes = boxes;
 target.labels = labels;
 target.area = area;
 target.iscrowd = torch::zeros({instance_count}, torch::TensorOptions().dtype(torch::kInt64));
 mmltk::backend::models::rfdetr::PreparedTargets targets;
 targets.orig_sizes = torch::tensor({{100, 100}}, torch::TensorOptions().dtype(torch::kInt64));
 targets.nested_mask = torch::zeros({1, 100, 100}, torch::TensorOptions().dtype(torch::kBool));
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
 return make_single_image_targets(torch::tensor({{0.5f, 0.5f, 0.2f, 0.2f}}, torch::TensorOptions().dtype(torch::kFloat32)),
                                  torch::tensor({1}, torch::TensorOptions().dtype(torch::kInt64)),
                                  torch::tensor({400.0f}, torch::TensorOptions().dtype(torch::kFloat32)));
}
mmltk::backend::models::rfdetr::PreparedTargets make_multi_targets() {
 return make_single_image_targets(torch::tensor({{0.5f, 0.5f, 0.2f, 0.2f}, {0.2f, 0.2f, 0.1f, 0.12f}}, torch::TensorOptions().dtype(torch::kFloat32)),
                                  torch::tensor({1, 2}, torch::TensorOptions().dtype(torch::kInt64)),
                                  torch::tensor({400.0f, 120.0f}, torch::TensorOptions().dtype(torch::kFloat32)));
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
 layer.pred_logits = torch::tensor({{{-5.0f, 8.0f, -5.0f}, {6.0f, -5.0f, -5.0f}}}, torch::TensorOptions().dtype(torch::kFloat32));
 layer.pred_boxes = torch::tensor({{{0.5f, 0.5f, 0.2f, 0.2f}, {0.1f, 0.1f, 0.1f, 0.1f}}}, torch::TensorOptions().dtype(torch::kFloat32));
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
 outputs.main.pred_logits = torch::tensor({{{-5.0f, 8.0f, -5.0f}, {-5.0f, -5.0f, 8.0f}, {7.0f, -5.0f, -5.0f}}}, torch::TensorOptions().dtype(torch::kFloat32));
 outputs.main.pred_boxes =
  torch::tensor({{{0.52f, 0.48f, 0.22f, 0.18f}, {0.18f, 0.22f, 0.12f, 0.08f}, {0.9f, 0.9f, 0.05f, 0.05f}}}, torch::TensorOptions().dtype(torch::kFloat32));
 return outputs;
}
mmltk::backend::models::rfdetr::ModelOutputs make_mask_outputs(int64_t height, int64_t width) {
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
float clamped_area(const float x1, const float y1, const float x2, const float y2) { return std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1); }
float aligned_giou(const std::array<float, 4>& lhs_cxcywh, const std::array<float, 4>& rhs_cxcywh) {
 const auto lhs = cxcywh_to_xyxy(lhs_cxcywh);
 const auto rhs = cxcywh_to_xyxy(rhs_cxcywh);
 const float inter = clamped_area(std::max(lhs[0], rhs[0]), std::max(lhs[1], rhs[1]), std::min(lhs[2], rhs[2]), std::min(lhs[3], rhs[3]));
 const float lhs_area = clamped_area(lhs[0], lhs[1], lhs[2], lhs[3]);
 const float rhs_area = clamped_area(rhs[0], rhs[1], rhs[2], rhs[3]);
 const float uni = lhs_area + rhs_area - inter;
 const float iou = inter / uni;
 const float enc_area = clamped_area(std::min(lhs[0], rhs[0]), std::min(lhs[1], rhs[1]), std::max(lhs[2], rhs[2]), std::max(lhs[3], rhs[3]));
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
 auto target_masks = torch::zeros({1, height, width}, torch::TensorOptions().dtype(torch::kFloat32));
 for (const auto& [y, x] : cells) { target_masks.index_put_({0, y, x}, 1.0f); }
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
 const auto weighted = mmltk::backend::models::rfdetr::weighted_detection_loss(loss_dict, config, torch::Device(torch::kCPU));
 REQUIRE(torch::isfinite(weighted).item<bool>());
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
 REQUIRE(torch::allclose(combined.at("loss_ce"), main_loss.at("loss_ce"), 1.0e-6, 1.0e-6));
 REQUIRE(torch::allclose(combined.at("loss_bbox"), main_loss.at("loss_bbox"), 1.0e-6, 1.0e-6));
 REQUIRE(torch::allclose(combined.at("loss_giou"), main_loss.at("loss_giou"), 1.0e-6, 1.0e-6));
 for (size_t aux_index = 0; aux_index < outputs.aux_outputs.size(); ++aux_index) {
  mmltk::backend::models::rfdetr::ModelOutputs aux_only;
  aux_only.main = outputs.aux_outputs[aux_index];
  const auto aux_loss = mmltk::backend::models::rfdetr::detection_loss_dict(aux_only, targets, single_config, true, false);
  const std::string suffix = "_" + std::to_string(aux_index);
  REQUIRE(torch::allclose(combined.at("loss_ce" + suffix), aux_loss.at("loss_ce"), 1.0e-6, 1.0e-6));
  REQUIRE(torch::allclose(combined.at("loss_bbox" + suffix), aux_loss.at("loss_bbox"), 1.0e-6, 1.0e-6));
  REQUIRE(torch::allclose(combined.at("loss_giou" + suffix), aux_loss.at("loss_giou"), 1.0e-6, 1.0e-6));
 }
 mmltk::backend::models::rfdetr::ModelOutputs enc_only;
 enc_only.main = require_optional_ref(outputs.enc_outputs, "expected encoder outputs when building encoder-only loss");
 const auto enc_loss = mmltk::backend::models::rfdetr::detection_loss_dict(enc_only, targets, single_config, true, false);
 REQUIRE(torch::allclose(combined.at("loss_ce_enc"), enc_loss.at("loss_ce"), 1.0e-6, 1.0e-6));
 REQUIRE(torch::allclose(combined.at("loss_bbox_enc"), enc_loss.at("loss_bbox"), 1.0e-6, 1.0e-6));
 REQUIRE(torch::allclose(combined.at("loss_giou_enc"), enc_loss.at("loss_giou"), 1.0e-6, 1.0e-6));
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
 const auto float_options = torch::TensorOptions().dtype(torch::kFloat32);
 const auto integer_options = float_options.dtype(torch::kInt64);
 auto targets = make_single_image_targets(torch::full({target_count + 1, 4}, 0.2F, float_options), torch::zeros({target_count + 1}, integer_options),
                                          torch::ones({target_count + 1}, float_options));
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
 std::vector<std::pair<torch::Tensor, torch::Tensor>> retained;
 for (size_t layer_index = 0; layer_index < layers.size(); ++layer_index) {
  auto& layer = *layers[layer_index];
  const int64_t queries = query_counts[layer_index];
  layer.pred_logits = torch::zeros({3, queries, config.num_classes}, float_options.requires_grad(true));
  layer.pred_boxes = torch::full({3, queries, 4}, 0.2F, float_options);
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
   const auto diagonal = torch::arange(per_group, integer_options);
   for (int64_t group = 0; group < groups; ++group) {
    REQUIRE(torch::equal(matches[image].first.narrow(0, group * per_group, per_group), diagonal + group * (queries / groups)));
    REQUIRE(torch::equal(matches[image].second.narrow(0, group * per_group, per_group), diagonal));
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
 const auto weighted = rfdetr::weighted_detection_loss(losses, config, torch::Device(torch::kCPU));
 REQUIRE(torch::isfinite(weighted).item<bool>());
 weighted.backward();
 for (const auto* layer : layers) {
  REQUIRE(torch::isfinite(layer->pred_logits.grad()).all().item<bool>());
  REQUIRE(torch::isfinite(layer->pred_boxes.grad()).all().item<bool>());
 }
 const auto retained_count = std::min<int64_t>(300 / groups, target_count);
 REQUIRE(torch::equal(retained[1].second, torch::arange(retained_count, integer_options).repeat({groups})));
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
 const auto weighted = mmltk::backend::models::rfdetr::weighted_detection_loss(loss_dict, config, torch::Device(torch::kCPU));
 REQUIRE(torch::isfinite(weighted).item<bool>());
}
void test_sparse_mask_loss_matches_dense_reference() {
 constexpr int64_t kHeight = 8;
 constexpr int64_t kWidth = 8;
 constexpr int64_t kChannels = 2;
 auto targets = make_packed_mask_targets(kHeight, kWidth, {{2, 2}, {2, 3}, {2, 4}, {3, 2}, {3, 3}, {3, 4}});
 auto config = make_mask_loss_config();
 auto spatial_features =
  torch::linspace(0.05f, 1.28f, kChannels * kHeight * kWidth, torch::TensorOptions().dtype(torch::kFloat32)).view({1, kChannels, kHeight, kWidth});
 auto query_features = torch::tensor({{{0.35f, -0.15f}, {-0.2f, 0.4f}}}, torch::TensorOptions().dtype(torch::kFloat32));
 auto bias = torch::tensor({0.05f}, torch::TensorOptions().dtype(torch::kFloat32));
 auto dense_masks = torch::einsum("bchw,bnc->bnhw", {spatial_features, query_features}).add(bias);
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
 torch::manual_seed(1234);
 const auto dense_loss = mmltk::backend::models::rfdetr::detection_loss_dict(dense_outputs, targets, config, true, false);
 torch::manual_seed(1234);
 const auto sparse_loss = mmltk::backend::models::rfdetr::detection_loss_dict(sparse_outputs, targets, config, true, false);
 REQUIRE(torch::allclose(dense_loss.at("loss_mask_ce"), sparse_loss.at("loss_mask_ce"), 1.0e-5, 1.0e-5));
 REQUIRE(torch::allclose(dense_loss.at("loss_mask_dice"), sparse_loss.at("loss_mask_dice"), 1.0e-5, 1.0e-5));
}
void test_matcher_mask_cost_handles_zero_point_sampling_on_cuda() {
 if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
 auto targets = make_targets();
 targets.packed_masks = pack_dense_masks(torch::ones({1, 1, 1}, torch::TensorOptions().dtype(torch::kFloat32)));
 const auto& packed_masks = targets.packed_masks.value();
 targets.packed_masks = mmltk::backend::models::rfdetr::PackedTargetMasks{
  packed_masks.bits.to(torch::kCUDA),
  packed_masks.height,
  packed_masks.width,
 };
 auto config = make_mask_loss_config(16);
 auto outputs = make_mask_outputs(1, 1);
 outputs.main.pred_logits = outputs.main.pred_logits.to(torch::kCUDA);
 outputs.main.pred_boxes = outputs.main.pred_boxes.to(torch::kCUDA);
 outputs.main.pred_masks = require_optional_ref(outputs.main.pred_masks, "expected mask outputs for CUDA matcher test").to(torch::kCUDA);
 const auto indices = mmltk::backend::models::rfdetr::matcher_indices(outputs, targets, config, true);
 assert_single_trivial_match(indices);
}
void test_packed_mask_sampling_matches_grid_sample_nearest_boundaries() {
 if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
 auto dense_masks = torch::tensor({{{0.0f, 1.0f, 0.0f, 1.0f}}}, torch::TensorOptions().dtype(torch::kFloat32));
 const auto packed = pack_dense_masks(dense_masks);
 // CLEANUP-IGNORE: Packed-mask boundary coordinates are independent from supervision CUDA fixtures.
 const auto packed_bits = packed.bits.to(torch::kCUDA);
 const auto mask_indices = torch::tensor({0}, torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA));
 const auto point_coords =
  torch::tensor({{{0.0f, 0.5f}, {0.125f, 0.5f}, {0.25f, 0.5f}, {0.375f, 0.5f}, {0.5f, 0.5f}, {0.625f, 0.5f}, {0.75f, 0.5f}, {0.875f, 0.5f}, {1.0f, 0.5f}}},
                torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA));
 const auto sampled = mmltk::backend::models::rfdetr::sample_packed_masks_cuda(packed_bits, packed.height, packed.width, mask_indices, point_coords);
 const auto dense_cuda = dense_masks.unsqueeze(1).to(torch::kCUDA);
 const auto expected = F::grid_sample(dense_cuda, point_coords.unsqueeze(2).mul(2.0f).sub(1.0f),
                                      F::GridSampleFuncOptions().mode(torch::kNearest).padding_mode(torch::kBorder).align_corners(false))
                        .squeeze(3)
                        .squeeze(1);
 REQUIRE(torch::equal(sampled.cpu(), expected.cpu()));
}
constexpr std::array<std::pair<int64_t, int64_t>, 5> postprocess_geometries{{
 {80, 160},
 {80, 160},
 {96, 40},
 {32, 224},
 {80, 160},
}};
using GeometrySelections = std::array<mmltk::backend::models::rfdetr::PostprocessedSelection, postprocess_geometries.size()>;
GeometrySelections select_postprocess_geometries(torch::Device device) {
 namespace rfdetr = mmltk::backend::models::rfdetr;
 const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
 rfdetr::OutputTensors outputs;
 outputs.pred_logits = torch::tensor({{{0.F, 2.F}, {4.F, -2.F}}}, options);
 outputs.pred_boxes = torch::tensor({{{0.5F, 0.5F, 0.5F, 0.25F}, {0.75F, 0.25F, 0.25F, 0.5F}}}, options);
 GeometrySelections selections;
 for (std::size_t index = 0; index < postprocess_geometries.size(); ++index) {
  const auto [height, width] = postprocess_geometries[index];
  selections[index] = rfdetr::select_output_batch_fixed_size(outputs, height, width, 3);
 }
 return selections;
}
void require_postprocess_geometries(const GeometrySelections& selections) {
 const auto expected_scores = torch::tensor({{1.F / (1.F + std::exp(-4.F)), 1.F / (1.F + std::exp(-2.F)), 0.5F}});
 const auto expected_labels = torch::tensor({{0, 1, 0}}, torch::TensorOptions().dtype(torch::kInt64));
 const auto expected_queries = torch::tensor({{1, 0, 0}}, torch::TensorOptions().dtype(torch::kInt64));
 // Check every retained result only after all replacements have been submitted.
 for (std::size_t index = 0; index < postprocess_geometries.size(); ++index) {
  const auto [height, width] = postprocess_geometries[index];
  const auto h = static_cast<float>(height);
  const auto w = static_cast<float>(width);
  const auto expected_boxes =
   torch::tensor({{{0.625F * w, 0.F, 0.875F * w, 0.5F * h}, {0.25F * w, 0.375F * h, 0.75F * w, 0.625F * h}, {0.25F * w, 0.375F * h, 0.75F * w, 0.625F * h}}});
  const auto& selected = selections[index];
  CHECK(torch::equal(selected.boxes.cpu(), expected_boxes));
  CHECK(torch::allclose(selected.scores.cpu(), expected_scores));
  CHECK(torch::equal(selected.labels.cpu(), expected_labels));
  CHECK(torch::equal(selected.query_indices.cpu(), expected_queries));
  CHECK_FALSE(selected.mask_logits.has_value());
 }
}
void test_postprocess() {
 const auto outputs = make_outputs();
 const auto result = mmltk::backend::models::rfdetr::postprocess_output_batch_fixed_size(
  {outputs.main.pred_logits, outputs.main.pred_boxes, outputs.main.pred_masks}, 100, 100, 2);
 REQUIRE(result.size() == 1);
 REQUIRE(result.counts[0].item<int64_t>() == 2);
 REQUIRE(result.scores.size(1) == 2);
 REQUIRE(result.labels.size(1) == 2);
 REQUIRE(result.boxes.size(1) == 2);
 REQUIRE(result.boxes.size(2) == 4);
 const auto boxes = result.boxes[0].cpu();
 const auto box_values = boxes.accessor<float, 2>();
 REQUIRE(std::fabs(box_values[0][0] - 40.0f) < 1e-4f);
 REQUIRE(std::fabs(box_values[0][1] - 40.0f) < 1e-4f);
 REQUIRE(std::fabs(box_values[0][2] - 60.0f) < 1e-4f);
 REQUIRE(std::fabs(box_values[0][3] - 60.0f) < 1e-4f);
 require_postprocess_geometries(select_postprocess_geometries(torch::kCPU));
}
void test_postprocess_cuda_stream_replacement() {
 if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
 const auto first_stream = c10::cuda::getStreamFromPool(false, 0);
 const auto second_stream = c10::cuda::getStreamFromPool(false, 0);
 c10::cuda::CUDAStreamGuard first_guard(first_stream);
 const auto first = select_postprocess_geometries(torch::Device(torch::kCUDA, 0));
 GeometrySelections second;
 {
  c10::cuda::CUDAStreamGuard second_guard(second_stream);
  second = select_postprocess_geometries(torch::Device(torch::kCUDA, 0));
 }
 const auto revisited = select_postprocess_geometries(torch::Device(torch::kCUDA, 0));
 require_postprocess_geometries(first);
 require_postprocess_geometries(revisited);
 {
  c10::cuda::CUDAStreamGuard second_guard(second_stream);
  require_postprocess_geometries(second);
 }
}
}  // namespace
TEST_CASE("test_weight_dict_population", "[model][rfdetr][native_ops]") { test_weight_dict_population(); }
TEST_CASE("test_matcher_and_losses", "[model][rfdetr][native_ops]") { test_matcher_and_losses(); }
TEST_CASE("test_batched_matcher_transfer_matches_single_layer_losses", "[model][rfdetr][native_ops]") {
 test_batched_matcher_transfer_matches_single_layer_losses();
}
TEST_CASE("test_multi_match_box_losses", "[model][rfdetr][native_ops]") { test_multi_match_box_losses(); }
TEST_CASE("test_matcher_sanitizes_nonfinite_costs", "[model][rfdetr][native_ops]") { test_matcher_sanitizes_nonfinite_costs(); }
TEST_CASE("test_rectangular_matcher_layers_preserve_all_targets", "[model][rfdetr][native_ops]") { test_rectangular_matcher_layers_preserve_all_targets(); }
TEST_CASE("test_cpu_mask_loss_uses_upstream_keys", "[model][rfdetr][native_ops]") { test_cpu_mask_loss_uses_upstream_keys(); }
TEST_CASE("test_sparse_mask_loss_matches_dense_reference", "[model][rfdetr][native_ops]") { test_sparse_mask_loss_matches_dense_reference(); }
TEST_CASE("test_matcher_mask_cost_handles_zero_point_sampling_on_cuda", "[model][rfdetr][native_ops]") {
 test_matcher_mask_cost_handles_zero_point_sampling_on_cuda();
}
TEST_CASE("test_packed_mask_sampling_matches_grid_sample_nearest_boundaries", "[model][rfdetr][native_ops]") {
 test_packed_mask_sampling_matches_grid_sample_nearest_boundaries();
}
TEST_CASE("test_postprocess", "[model][rfdetr][native_ops]") { test_postprocess(); }
TEST_CASE("test_postprocess_cuda_stream_replacement", "[model][rfdetr][native_ops][cuda]") { test_postprocess_cuda_stream_replacement(); }
TEST_CASE("LSAP solver arrays use the owning node resource and retain capacity", "[rfdetr][lsap][numa]") {
 using namespace mmltk::backend::models::rfdetr;
 using namespace mmltk::common::system;
 const auto topology = NumaTopology::Capture();
 const auto selected = mmltk::common::system::test_support::first_permitted_cpu(topology);
 const auto node = selected.node;
 NumaMemoryResource memory(node);
 LsapScratch scratch(&memory);
 scratch.costs = {4, 1, 3, 2, 0, 5};
 scratch.row_indices.resize(2);
 scratch.col_indices.resize(2);
 REQUIRE(solve_rectangular_linear_sum_assignment(2, 3, scratch.costs.data(), false, scratch.row_indices.data(), scratch.col_indices.data(), scratch.solver) ==
         RectangularLsApStatus::kOk);
 CHECK(scratch.col_indices[0] == 1);
 CHECK(scratch.col_indices[1] == 0);
 auto* duals = scratch.solver.column_duals.data();
 const auto capacity = scratch.solver.column_duals.capacity();
 REQUIRE(solve_rectangular_linear_sum_assignment(2, 3, scratch.costs.data(), false, scratch.row_indices.data(), scratch.col_indices.data(), scratch.solver) ==
         RectangularLsApStatus::kOk);
 CHECK(scratch.solver.column_duals.data() == duals);
 CHECK(scratch.solver.column_duals.capacity() == capacity);
 CHECK(scratch.costs.get_allocator().resource() == &memory);
 CHECK(scratch.solver.column_duals.get_allocator().resource() == &memory);
 CHECK(scratch.solver.visited_columns.get_allocator().resource() == &memory);
 CHECK(scratch.row_indices.get_allocator().resource() == &memory);
 // Tall and maximizing cases exercise the PMR transpose and sorting workspaces.
 REQUIRE(solve_rectangular_linear_sum_assignment(3, 2, scratch.costs.data(), true, scratch.row_indices.data(), scratch.col_indices.data(), scratch.solver) ==
         RectangularLsApStatus::kOk);
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
 // CLEANUP-IGNORE: Criterion parity owns its device placement after its transport-specific hardware admission.
 const auto execution = mmltk::frameworks::gpu::test_support::selected_test_device(0, mmltk::common::system::NumaTopology::Capture());
 const auto& p = execution.placement;
 mmltk::common::system::ScopedExecutionPolicy policy({p.cpus, {}, 0, p.numa_node, -10, false});
 const torch::Device device(torch::kCUDA, 0);
 auto config = make_mask_loss_config();
 config.group_detr = group;
 config.aux_loss = true;
 config.two_stage = true;
 populate_default_detection_weight_dict(config);
 auto targets = make_packed_mask_targets(4, 4, {{1, 1}, {1, 2}, {2, 1}, {2, 2}});
 targets.all_boxes = targets.all_boxes.to(device);
 targets.all_labels = targets.all_labels.to(device);
 targets.packed_masks->bits = targets.packed_masks->bits.to(device);
 targets.target_offsets = torch::tensor(targets.offsets, torch::TensorOptions().dtype(torch::kInt64).device(device));
 targets.target_counts = torch::tensor(targets.counts, torch::TensorOptions().dtype(torch::kInt64).device(device));
 struct Outcome final {
  TensorMap losses;
  std::vector<torch::Tensor> gradients;
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
  std::vector<torch::Tensor> inputs;
  const auto prepare = [&](OutputLayer& layer) {
   layer.pred_logits = layer.pred_logits.to(device).detach().set_requires_grad(true);
   layer.pred_boxes = layer.pred_boxes.to(device).detach().set_requires_grad(true);
   layer.pred_masks = layer.pred_masks->to(device).detach().set_requires_grad(true);
   inputs.push_back(layer.pred_logits);
   inputs.push_back(layer.pred_boxes);
   inputs.push_back(*layer.pred_masks);
  };
  prepare(outputs.main);
  for (auto& layer : outputs.aux_outputs) prepare(layer);
  prepare(*outputs.enc_outputs);
  torch::manual_seed(7921);
  auto losses = detection_loss_dict(outputs, targets, config, true, false);
  auto total = weighted_detection_loss(losses, config, device);
  total.backward();
  workspace.complete_assignments(c10::cuda::getCurrentCUDAStream(0).stream());
  REQUIRE(workspace.statistics().completion_dependencies == 1);
  Outcome outcome;
  for (const auto& [name, loss] : losses) outcome.losses[name] = loss.detach().cpu();
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
 for (const auto& [name, expected] : baseline.losses) REQUIRE(torch::allclose(observed.losses.at(name), expected, 1e-6, 1e-6));
 REQUIRE(observed.gradients.size() == baseline.gradients.size());
 for (std::size_t index = 0; index < baseline.gradients.size(); ++index)
  REQUIRE(torch::allclose(observed.gradients[index], baseline.gradients[index], 1e-6, 1e-6));
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
 auto& owner = (model);
 owner.eval();
 torch::NoGradGuard no_grad;
 const auto image = torch::zeros({3, 64, 64});
 const auto input = rfdetr::nested_tensor_from_tensor_list({image});
 CHECK_FALSE(owner.forward(input, false).main.pred_masks.has_value());
 const auto output = owner.forward(input, true);
 REQUIRE(output.main.pred_masks.has_value());
 CHECK(output.main.pred_masks->size(1) == config.num_queries);
}
TEST_CASE("mask materialization follows selected query identity and bounded reusable chunks", "[model][rfdetr][native_ops]") {
 namespace rfdetr = mmltk::backend::models::rfdetr;
 rfdetr::OutputTensors output;
 output.pred_logits = torch::tensor({{{-10.F, 8.F}, {10.F, -10.F}, {-10.F, -10.F}}});
 output.pred_boxes = torch::ones({1, 3, 4});
 CHECK_THROWS_AS(rfdetr::select_output_batch_fixed_size(output, 1080, 1920, 500, true), std::runtime_error);
 output.pred_masks = torch::tensor({1.F, -1.F, 1.F}).view({1, 3, 1, 1});
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
 output.pred_masks = torch::ones({1, 2, 1, 1});
 CHECK_THROWS_AS(rfdetr::select_output_batch_fixed_size(output, 2, 2, 2, true), std::invalid_argument);
}
TEST_CASE("Physical ranking precedes slot filtering and keeps stable query mask identity", "[model][rfdetr][layout]") {
 namespace r = mmltk::backend::models::rfdetr;
 namespace catalog = mmltk::backend::data::catalog;
 for (std::uint32_t background = 0; background < 3; ++background) {
  auto record = r::native_training_class_layout(catalog::ClassCatalog({"cat", "dog"}));
  record.no_object = r::NoObjectEncoding::ExplicitBackground;
  std::uint32_t foreground = 0;
  for (std::uint32_t slot = 0; slot < 3; ++slot)
   record.slots[slot] =
    slot == background ? r::ModelClassSlot{r::ClassSlotRole::Background, std::nullopt} : r::ModelClassSlot{r::ClassSlotRole::Foreground, foreground++};
  r::ClassPostprocessLane classes(std::make_shared<const r::ResolvedClassLayout>(record));
  classes.Prepare(torch::kCPU);
  r::OutputTensors outputs;
  outputs.pred_logits = torch::full({1, 2, 3}, -8.F);
  outputs.pred_logits.select(2, background).fill_(100.F);
  outputs.pred_logits[0][0][background == 0 ? 1 : 0] = 4.F;
  outputs.pred_logits[0][1][background == 2 ? 1 : 2] = 3.F;
  outputs.pred_boxes = torch::tensor({.25F, .25F, .2F, .2F, .75F, .75F, .2F, .2F}).view({1, 2, 4});
  outputs.pred_masks = torch::tensor({8.F, -8.F}).view({1, 2, 1, 1});
  const auto discarded = r::select_output_batch_fixed_size(outputs, 10, 20, 2, true, &classes);
  CHECK(discarded.counts.item<int64_t>() == 0);
  CHECK(discarded.scores.size(1) == 2);
  CHECK(discarded.query_indices.size(1) == 2);
  const auto selected = r::select_output_batch_fixed_size(outputs, 10, 20, 4, true, &classes);
  REQUIRE(selected.counts.item<int64_t>() == 2);
  CHECK(selected.labels[0][0].item<int64_t>() == 0);
  CHECK(selected.labels[0][1].item<int64_t>() == 1);
  CHECK(selected.query_indices[0][0].item<int64_t>() == 0);
  CHECK(selected.query_indices[0][1].item<int64_t>() == 1);
  r::SelectedMaskWorkspace masks;
  const auto materialized = masks.Materialize(*selected.mask_logits, selected.query_indices, 10, 20);
  CHECK(materialized[0][0].all().item<bool>());
  CHECK_FALSE(materialized[0][1].any().item<bool>());
 }
 r::ClassPostprocessLane empty(std::make_shared<const r::ResolvedClassLayout>(r::native_training_class_layout(catalog::ClassCatalog{})));
 empty.Prepare(torch::kCPU);
 r::OutputTensors outputs{.pred_logits = torch::full({1, 2, 1}, 100.F), .pred_boxes = torch::zeros({1, 2, 4})};
 const auto empty_selection = r::select_output_batch_fixed_size(outputs, 10, 20, 500, false, &empty);
 CHECK(empty_selection.counts.item<int64_t>() == 0);
 CHECK(empty_selection.labels.numel() == 0);
 CHECK(empty_selection.scores.storage().nbytes() == 0);
 CHECK(empty_selection.labels.storage().nbytes() == 0);
 CHECK(empty_selection.boxes.storage().nbytes() == 0);
 CHECK(empty_selection.boxes.size(1) == 0);
 outputs.pred_masks = torch::ones({1, 2, 1, 1});
 const auto empty_masks = r::postprocess_output_batch_fixed_size(outputs, 10, 20, 500, &empty);
 REQUIRE(empty_masks.masks);
 CHECK(empty_masks.masks->numel() == 0);
 CHECK(empty_masks.masks->storage().nbytes() == 0);
 outputs.pred_logits = torch::zeros({1, 2, 2});
 CHECK_THROWS(r::select_output_batch_fixed_size(outputs, 10, 20, 500, false, &empty));
}
TEST_CASE("Class lanes retain physical ties, permutation, sparse COCO and independent final results", "[model][rfdetr][layout]") {
 namespace r = mmltk::backend::models::rfdetr;
 auto record = r::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"cat", "dog"}));
 record.slots = {{r::ClassSlotRole::Foreground, 1U}, {r::ClassSlotRole::Unused, {}}, {r::ClassSlotRole::Foreground, 0U}};
 r::ClassPostprocessLane lane(std::make_shared<const r::ResolvedClassLayout>(record));
 lane.Prepare(torch::kCPU);
 const auto logits = torch::zeros({1, 2, 3});
 CHECK(lane.ValidateLogits(logits).data_ptr() == logits.data_ptr());
 CHECK_THROWS(lane.ValidateLogits(torch::zeros({1, 2, 4})));
 r::OutputTensors outputs{.pred_logits = logits, .pred_boxes = torch::ones({1, 2, 4})};
 const auto first = r::select_output_batch_fixed_size(outputs, 8, 8, 4, false, &lane);
 REQUIRE(first.counts.item<int64_t>() == 3);
 CHECK(torch::equal(first.labels[0].narrow(0, 0, 3), torch::tensor({1L, 0L, 1L}, torch::kInt64)));
 CHECK(torch::equal(first.query_indices[0].narrow(0, 0, 3), torch::tensor({0L, 0L, 1L}, torch::kInt64)));
 const auto saved = first.labels.clone();
 outputs.pred_logits = torch::randn_like(logits);
 const auto next = r::select_output_batch_fixed_size(outputs, 8, 8, 4, false, &lane);
 CHECK(torch::equal(first.labels, saved));
 CHECK(first.labels.data_ptr() != next.labels.data_ptr());
 r::ClassPostprocessLane coco(std::make_shared<const r::ResolvedClassLayout>(r::coco_class_layout({r::ClassLayoutOrigin::VerifiedAsset, "fixture", {}})));
 coco.Prepare(torch::kCPU);
 outputs.pred_logits = torch::full({1, 1, 91}, -8.F);
 outputs.pred_logits[0][0][0] = 9.F;
 outputs.pred_logits[0][0][12] = 8.F;
 outputs.pred_logits[0][0][90] = 7.F;
 outputs.pred_boxes = torch::ones({1, 1, 4});
 const auto sparse = r::select_output_batch_fixed_size(outputs, 10, 20, 3, false, &coco);
 REQUIRE(sparse.counts.item<int64_t>() == 1);
 CHECK(sparse.labels[0][0].item<int64_t>() == 79);
 CHECK(torch::equal(sparse.boxes[0][0], torch::tensor({10.F, 5.F, 20.F, 10.F})));
 outputs.pred_boxes = torch::tensor({-.25F, 1.2F, 1.F, 1.F}).view({1, 1, 4});
 const auto clipped = r::select_output_batch_fixed_size(outputs, 10, 20, 3, false, &coco);
 CHECK(torch::allclose(clipped.boxes[0][0], torch::tensor({0.F, 7.F, 5.F, 10.F})));
 const auto discarded = r::select_output_batch_fixed_size(outputs, 10, 20, 2, false, &coco);
 CHECK(discarded.counts.item<int64_t>() == 0);
 CHECK(discarded.scores.size(1) == 2);
 r::ClassPostprocessLane raw(std::make_shared<const r::ResolvedClassLayout>(r::unresolved_class_layout(91)));
 raw.Prepare(torch::kCPU);
 const auto unresolved = r::select_output_batch_fixed_size(outputs, 10, 20, 3, false, &raw);
 CHECK(unresolved.counts.item<int64_t>() == 3);
 CHECK(torch::equal(unresolved.labels[0], torch::tensor({0L, 12L, 90L}, torch::kInt64)));
 const auto none = r::select_output_batch_fixed_size(outputs, 10, 20, 0, false, &coco);
 CHECK(none.counts.item<int64_t>() == 0);
 CHECK(none.boxes.size(1) == 0);
 auto boxes = torch::tensor({-10.F, -20.F, 50.F, 60.F}).view({1, 4});
 r::clip_prediction_boxes_(boxes, 2, 4, 18, 9);
 CHECK(torch::equal(boxes, torch::tensor({2.F, 4.F, 18.F, 9.F}).view({1, 4})));
}
TEST_CASE("Class lane device and stream rebind retains earlier final references", "[model][rfdetr][layout][gpu]") {
 namespace r = mmltk::backend::models::rfdetr;
 auto record = r::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"cat"}));
 r::ClassPostprocessLane lane(std::make_shared<const r::ResolvedClassLayout>(record));
 const auto first_stream = c10::cuda::getStreamFromPool(false, 0);
 const auto second_stream = c10::cuda::getStreamFromPool(false, 0);
 torch::Tensor first;
 {
  c10::cuda::CUDAStreamGuard guard(first_stream);
  lane.Prepare(torch::Device(torch::kCUDA, 0));
  first = lane.References(torch::zeros({1}, torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA, 0)));
 }
 {
  c10::cuda::CUDAStreamGuard guard(second_stream);
  const auto indices = torch::zeros({1}, torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA, 0));
  CHECK_THROWS(lane.References(indices));
  lane.Prepare(indices.device());
  CHECK(lane.References(indices).cpu().item<int64_t>() == 0);
 }
 {
  c10::cuda::CUDAStreamGuard guard(first_stream);
  CHECK(first.cpu().item<int64_t>() == 0);
 }
}
TEST_CASE("matcher focal alpha changes dense and CUDA assignments consistently", "[model][rfdetr][matcher]") {
 namespace r = mmltk::backend::models::rfdetr;
 const auto device = GENERATE(torch::kCPU, torch::kCUDA);
 if (device == torch::kCUDA && mmltk::testsupport::checked_cuda_device_count() == 0) SKIP("CUDA device unavailable");
 auto outputs = make_outputs();
 outputs.main.pred_logits = torch::tensor({{{0.F, 0.F, 0.F}, {0.F, 2.F, 0.F}}}).to(device);
 outputs.main.pred_boxes = torch::tensor({{{.5F, .5F, .2F, .2F}, {.7F, .5F, .2F, .2F}}}).to(device);
 auto config = make_config();
 config.set_cost_class = 1;
 config.set_cost_bbox = 5;
 config.set_cost_giou = 0;
 const auto targets = make_targets();
 for (const double alpha : {.25, .8}) {
  config.focal_alpha = alpha;
  const auto indices = r::matcher_indices(outputs, targets, config, true);
  assert_single_image_match_count(indices, 1);
  CHECK(indices[0].first.item<std::int64_t>() == (alpha == .25 ? 1 : 0));
 }
}
