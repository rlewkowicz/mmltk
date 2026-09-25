#include "src/backend/models/rfdetr/core/tests/training_fixture.h"
#include "src/backend/models/rfdetr/core/detail/training_mask_loss.h"
#include "src/backend/models/rfdetr/core/detail/training_supervision.h"
#include "src/backend/ml/torch/tests/tensor_fixture.h"
#include "src/backend/models/rfdetr/core/detection_ops.h"
#include "src/backend/models/rfdetr/core/detail/detection_sampling.h"
#include "src/backend/models/rfdetr/core/detail/detection_geometry.h"
#include "src/backend/models/rfdetr/core/detail/modules_technical.h"
#include "src/backend/models/rfdetr/core/detail/segmentation_depthwise.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <ATen/Context.h>
#include <torch/nn/functional/vision.h>
#include <torch/nn/functional/conv.h>
#include <torch/nn/functional/normalization.h>
#include "src/backend/models/rfdetr/core/detail/traced_loss_cache.h"
#include "src/backend/models/rfdetr/core/detail/matcher_workspace.h"
#include "src/backend/models/rfdetr/core/runtime.h"
#include "src/common/system/execution_policy.h"
#include "src/common/system/numa_topology.h"
#include <limits>
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
namespace {
namespace rf = mmltk::backend::models::rfdetr;
namespace tensor_fixture = mmltk::backend::ml::testsupport;
namespace F = torch::nn::functional;
using namespace torch::indexing;
// Literal independent equations from RF-DETR e9a138f70cc14e3fddca029a0683055e34ab8f35:
// utilities/box_ops.py, models/criterion.py and models/heads/segmentation.py.
// These fixtures use native full-precision matrix/convolution policy. They
// establish mathematical VJP equivalence, not upstream default-kernel bitwise
// identity, dataset-epoch identity, convergence or AP evidence.
torch::Tensor reference_mask_sample(const torch::Tensor& image, const torch::Tensor& points, bool nearest) {
 auto options = F::GridSampleFuncOptions().padding_mode(torch::kBorder).align_corners(false);
 if (nearest)
  options.mode(torch::kNearest);
 else
  options.mode(torch::kBilinear);
 return F::grid_sample(image, (2 * points - 1).unsqueeze(2), options).squeeze(3).squeeze(1);
}
torch::Tensor reference_corners(const torch::Tensor& boxes) {
 const auto coordinates = boxes.unbind(-1);
 const auto x = coordinates[0], y = coordinates[1], w = coordinates[2], h = coordinates[3];
 return torch::stack({x - 0.5 * w.clamp_min(0), y - 0.5 * h.clamp_min(0), x + 0.5 * w.clamp_min(0), y + 0.5 * h.clamp_min(0)}, -1);
}
torch::Tensor reference_giou(const torch::Tensor& a, const torch::Tensor& b, bool generalized = true, bool pairwise = false) {
 const auto upcast = [](const torch::Tensor& value) { return value.scalar_type() == torch::kFloat16 || value.scalar_type() == torch::kBFloat16 ? value.to(torch::kFloat32) : value; };
 const auto aa = upcast(a), bb = upcast(b);
 const auto area_a = (aa.select(-1, 2) - aa.select(-1, 0)) * (aa.select(-1, 3) - aa.select(-1, 1));
 const auto area_b = (bb.select(-1, 2) - bb.select(-1, 0)) * (bb.select(-1, 3) - bb.select(-1, 1));
 const auto left = [&](int64_t start) { return pairwise ? a.slice(-1, start, start + 2).unsqueeze(-2) : a.slice(-1, start, start + 2); };
 const auto right = [&](int64_t start) { return pairwise ? b.slice(-1, start, start + 2).unsqueeze(-3) : b.slice(-1, start, start + 2); };
 const auto intersection_lower = torch::maximum(left(0), right(0));
 const auto intersection_upper = torch::minimum(left(2), right(2));
 const auto intersection_wh = (intersection_upper - intersection_lower).clamp_min(0);
 const auto intersection = intersection_wh.select(-1, 0) * intersection_wh.select(-1, 1);
 const auto area = (pairwise ? area_a.unsqueeze(-1) + area_b.unsqueeze(-2) : area_a + area_b) - intersection;
 const auto iou = intersection / area.clamp_min(1e-7);
 if (!generalized) return iou;
 const auto enclosing_lower = torch::minimum(left(0), right(0));
 const auto enclosing_upper = torch::maximum(left(2), right(2));
 const auto enclosing_wh = (enclosing_upper - enclosing_lower).clamp_min(0);
 const auto enclosing = enclosing_wh.select(-1, 0) * enclosing_wh.select(-1, 1);
 return iou - (enclosing - area) / enclosing.clamp_min(1e-7);
}
rf::PreparedTargets targets(const torch::Tensor& boxes, const torch::Tensor& labels) {
 rf::PreparedTargets result;
 rf::PreparedTarget target;
 target.boxes = boxes;
 target.labels = labels;
 result.targets.push_back(target);
 result.all_boxes = boxes;
 result.all_labels = labels;
 result.offsets = {0};
 result.counts = {labels.numel()};
 return result;
}
void require_stock_loss_vocabulary(const rf::TensorMap& losses, std::size_t auxiliaries, bool encoder, bool masks) {
 std::vector<std::string> suffixes{""};
 for (std::size_t layer = 0; layer < auxiliaries; ++layer) suffixes.push_back("_" + std::to_string(layer));
 if (encoder) suffixes.push_back("_enc");
 REQUIRE(losses.contains("class_error"));
 for (const auto& suffix : suffixes) {
  for (const auto key : {"loss_ce", "loss_bbox", "loss_giou", "cardinality_error"}) REQUIRE(losses.contains(key + suffix));
  if (masks) for (const auto key : {"loss_mask_ce", "loss_mask_dice"}) REQUIRE(losses.contains(key + suffix));
 }
 // Exhaust the upstream vocabulary: sufficient statistics never become map
 // entries, with or without a typed main-output consumer.
 REQUIRE(losses.size() == 1 + suffixes.size() * (masks ? 6 : 4));
}
TEST_CASE("Stock geometry matches literal area promotion clamps and gradients", "[rfdetr][parity]") {
 tensor_fixture::FullMatrixPrecision precision;
 for (const auto device : tensor_fixture::available_devices())
  for (const auto dtype : {torch::kFloat16, torch::kBFloat16, torch::kFloat32, torch::kFloat64}) {
   CAPTURE(device, dtype);
   const auto options = torch::TensorOptions().device(device).dtype(dtype);
   auto boxes = torch::tensor({{0., 0., 0., 0.}, {0., 0., 0.0002, 0.0002}, {0., 0., -0.3, 0.2}, {0.5, 0.5, 0.2, 0.4}, {2., 2., 0.4, 0.2}}, options).set_requires_grad(true);
   auto independent = boxes.detach().clone().set_requires_grad(true);
   const auto target = torch::tensor({{0., 0., 0., 0.}, {0., 0., 0.0002, 0.0002}, {0., 0., 0., 0.2}, {0.5, 0.5, 0.2, 0.4}, {0., 0., 0.4, 0.2}}, options);
   const auto actual = rf::aligned_generalized_box_iou(rf::box_cxcywh_to_xyxy(boxes), rf::box_cxcywh_to_xyxy(target));
   const auto expected = reference_giou(reference_corners(independent), reference_corners(target));
   REQUIRE(torch::allclose(actual, expected, 1e-5, 1e-6));
   actual.sum().backward();
   expected.sum().backward();
   CAPTURE(boxes.grad(), independent.grad());
   REQUIRE(torch::allclose(boxes.grad(), independent.grad(), 1e-4, 1e-5, true));
   const auto pairwise = rf::pairwise_generalized_box_iou(reference_corners(boxes.detach()), reference_corners(target));
   REQUIRE(torch::allclose(pairwise.diag(), expected.detach(), 1e-4, 1e-5));
  }
}
TEST_CASE("Stock categorical samples retain actual grid_sample half-pixel identities", "[rfdetr][parity]") {
 for (const auto device : tensor_fixture::available_devices())
  for (const int64_t width : {3, 16, 673}) {
   CAPTURE(device, width);
   auto mask = torch::arange(width, torch::kInt64).remainder(2).to(torch::kFloat32).view({1, 1, 1, width}).to(device);
   auto bits = torch::zeros({1, (width + 63) / 64}, torch::kInt64);
   auto* words = reinterpret_cast<uint64_t*>(bits.data_ptr<int64_t>());
   for (int64_t x = 1; x < width; x += 2) words[x / 64] |= uint64_t{1} << (x % 64);
   std::vector<float> coordinates{-0.1f, 0.f, 1.f, 1.1f};
   for (int64_t x = 1; x < width; ++x) {
    const float tie = static_cast<float>(x) / static_cast<float>(width);
    coordinates.push_back(std::nextafter(tie, 0.f));
    coordinates.push_back(tie);
    coordinates.push_back(std::nextafter(tie, 1.f));
   }
   auto x = torch::tensor(coordinates, torch::kFloat32).to(device);
   auto points = torch::stack({x, torch::full_like(x, 0.5)}, -1).unsqueeze(0);
   rf::PackedTargetMasks packed{bits.to(device), 1, width};
   const auto actual = rf::sample_target_masks(packed, torch::zeros({1}, torch::TensorOptions().device(device).dtype(torch::kInt64)), points, "upstream categorical fixture");
   const auto expected = F::grid_sample(mask, (2 * points - 1).unsqueeze(2), F::GridSampleFuncOptions().mode(torch::kNearest).padding_mode(torch::kBorder).align_corners(false)).reshape_as(actual);
   CAPTURE(points.index({0, actual[0] != expected[0]}));
   REQUIRE(torch::equal(actual, expected));
  }
}
TEST_CASE("Empty stock masks retain representation-specific connected expressions", "[rfdetr][parity]") {
 for (const bool sparse : {false, true})
  for (const bool nonfinite : {false, true}) {
   rf::ModelOutputs outputs;
   outputs.main.pred_logits = torch::tensor({{{0.3, -0.7}, {0.8, 0.2}}}, torch::kFloat32).set_requires_grad(true);
   outputs.main.pred_boxes = torch::ones({1, 2, 4}).set_requires_grad(true);
   auto spatial = torch::ones({1, 2, 2, 2});
   if (nonfinite) spatial.flatten()[5] = std::numeric_limits<float>::infinity();
   spatial.set_requires_grad(true);
   auto query = torch::ones({1, 2, 2}).set_requires_grad(true);
   auto bias = torch::zeros({1}).set_requires_grad(true);
   if (sparse)
    outputs.main.sparse_pred_masks = rf::SparsePredMasks{spatial, query, bias};
   else
    outputs.main.pred_masks = spatial;
   outputs.aux_outputs = {outputs.main};
   outputs.enc_outputs = outputs.main;
   rf::DetectionConfig config;
   config.num_classes = 2;
   config.group_detr = 1;
   config.include_masks = true;
   config.use_jit_traced_loss_ops = false;
   const auto empty = targets(torch::empty({0, 4}), torch::empty({0}, torch::kInt64));
   rf::DetectionStatisticsPacket::Tensors statistics;
   const auto losses = rf::detection_loss_dict(outputs, empty, config, true, 1.0, &statistics);
   require_stock_loss_vocabulary(losses, 1, true, true);
   REQUIRE(losses.at("loss_ce").item<float>() > 0.0F);
   REQUIRE(losses.at("class_error").item<float>() == 100.F);
   REQUIRE(rf::DetectionStatisticsPacket::get<^^rf::DetectionSufficientStatistics::matched_count>(statistics).item<float>() == 0.F);
   REQUIRE(rf::DetectionStatisticsPacket::get<^^rf::DetectionSufficientStatistics::class_error_sum>(statistics).item<float>() == 0.F);
   REQUIRE(rf::DetectionStatisticsPacket::get<^^rf::DetectionSufficientStatistics::image_count>(statistics).item<float>() == 1.F);
   for (const auto& statistic : statistics) REQUIRE_FALSE(statistic.requires_grad());
   for (const auto suffix : {"", "_0", "_enc"}) {
    const auto mask_loss = losses.at(std::string("loss_mask_ce") + suffix) + losses.at(std::string("loss_mask_dice") + suffix);
    REQUIRE(mask_loss.requires_grad());
    if (sparse && nonfinite)
     REQUIRE(torch::isnan(mask_loss).all().item<bool>());
    else
     REQUIRE(mask_loss.item<float>() == 0.0F);
   }
   losses.at("loss_mask_ce").backward();
   REQUIRE(spatial.grad().defined());
   REQUIRE(spatial.grad().count_nonzero().item<int64_t>() == 0);
   if (sparse) {
    REQUIRE(query.grad().defined());
    REQUIRE(bias.grad().defined());
   }
  }
}
TEST_CASE("Depthwise VJP uses original weight precision outside forward autocast", "[rfdetr][parity]") {
 tensor_fixture::FullMatrixPrecision precision;
 for (const auto& [device, amp] : tensor_fixture::available_amp_precisions()) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat32);
  auto input = (torch::arange(18, options).reshape({1, 2, 3, 3}) * 0.07137 - 0.3).set_requires_grad(true);
  auto weight = (torch::arange(18, options).reshape({2, 1, 3, 3}) * 0.02317 - 0.1).set_requires_grad(true);
  auto bias = torch::tensor({0.03713, -0.06217}, options).set_requires_grad(true);
  torch::Tensor output;
  {
   mmltk::backend::ml::cuda::TorchAutocastScope scope(amp != torch::kFloat32, amp);
   output = rf::segmentation_depthwise(input, weight, bias);
  }
  auto incoming = (torch::arange(18, options).reshape_as(output) * 0.04113 + 0.037).to(output.scalar_type());
  output.backward(incoming);
  // Independent literal convolution, differentiated in original FP32. This
  // bypasses native custom backward and avoids an im2col reference expansion.
  auto x = input.detach().clone().set_requires_grad(true), w = weight.detach().clone().set_requires_grad(true), b = bias.detach().clone().set_requires_grad(true);
  std::vector<torch::Tensor> channels;
  for (int64_t channel = 0; channel < 2; ++channel) {
   std::vector<torch::Tensor> rows;
   for (int64_t y = 0; y < 3; ++y) {
    std::vector<torch::Tensor> cells;
    for (int64_t col = 0; col < 3; ++col) {
     auto cell = b[channel];
     for (int64_t ky = 0; ky < 3; ++ky)
      for (int64_t kx = 0; kx < 3; ++kx) {
       const auto iy = y + ky - 1, ix = col + kx - 1;
       if (iy >= 0 && iy < 3 && ix >= 0 && ix < 3) cell = cell + x.index({0, channel, iy, ix}) * w.index({channel, 0, ky, kx});
      }
     cells.push_back(cell);
    }
    rows.push_back(torch::stack(cells));
   }
   channels.push_back(torch::stack(rows));
  }
  const auto reference = torch::stack(channels).unsqueeze(0);
  reference.backward(incoming.to(torch::kFloat32));
  REQUIRE(torch::allclose(input.grad(), x.grad(), 1e-5, 1e-6));
  REQUIRE(torch::allclose(weight.grad(), w.grad(), 1e-5, 1e-6));
  REQUIRE(torch::allclose(bias.grad(), b.grad(), 1e-5, 1e-6));
 }
}
TEST_CASE("Encoder mask projection follows learned upstream skip-block equations", "[rfdetr][parity]") {
 tensor_fixture::FullMatrixPrecision precision;
 for (const bool selective : {false, true})
  for (const auto device : tensor_fixture::available_devices()) {
   rf::SegmentationHead head(2, 1, 1, 1);
   head->to(device);
   head->prepare_selective(true, selective, 1);
   {
    torch::NoGradGuard guard;
    for (const auto& parameter : head->named_parameters()) {
     parameter.value().copy_((torch::arange(parameter.value().numel(), parameter.value().options()) * 0.0317 - 0.17).reshape_as(parameter.value()));
    }
   }
   auto spatial = torch::tensor({{{{0.2, -0.4}, {0.7, 0.1}}, {{-0.3, 0.9}, {0.4, -0.8}}}}, torch::kFloat32).to(device).set_requires_grad(true);
   auto query = torch::tensor({{{0.17, -0.39}, {0.71, 0.23}, {-0.47, 0.83}}}, torch::kFloat32).to(device).set_requires_grad(true);
   auto native = head->forward(spatial, {query}, {2, 2}, true).front();
   std::unordered_map<std::string, torch::Tensor> weights;
   for (const auto& parameter : head->named_parameters()) weights.emplace(parameter.key(), parameter.value().detach().clone().set_requires_grad(true));
   auto x = spatial.detach().clone().set_requires_grad(true), q = query.detach().clone().set_requires_grad(true);
   const auto linear = [&](const torch::Tensor& input, const std::string& name) { return torch::matmul(input, weights.at(name + ".weight").transpose(0, 1)) + weights.at(name + ".bias"); };
   const auto mean = q.mean(-1, true);
   auto normalized = (q - mean) / torch::sqrt((q - mean).square().mean(-1, true) + 1e-5);
   normalized = normalized * weights.at("query_features_block.norm_in.weight") + weights.at("query_features_block.norm_in.bias");
   auto hidden = linear(normalized, "query_features_block.layers.0");
   hidden = 0.5 * hidden * (1 + torch::erf(hidden / std::sqrt(2.0)));
   const auto projected_query = linear(q + linear(hidden, "query_features_block.layers.2"), "query_features_proj");
   const auto projected_spatial = torch::matmul(weights.at("spatial_features_proj.weight").reshape({2, 2}), x.flatten(2)) + weights.at("spatial_features_proj.bias").reshape({1, 2, 1});
   const auto expected = (torch::bmm(projected_query, projected_spatial) + weights.at("bias")).reshape_as(native);
   REQUIRE(torch::allclose(native, expected, 1e-5, 1e-6));
   const auto sparse = head->sparse_forward(spatial, {query}, {2, 2}, true).front();
   REQUIRE(torch::allclose(sparse.spatial_features.flatten(2), projected_spatial, 1e-5, 1e-6));
   REQUIRE(torch::allclose(sparse.query_features, projected_query, 1e-5, 1e-6));
   native.square().sum().backward();
   expected.square().sum().backward();
   REQUIRE(torch::allclose(spatial.grad(), x.grad(), 1e-5, 1e-6));
   REQUIRE(torch::allclose(query.grad(), q.grad(), 1e-5, 1e-6));
   for (const auto& parameter : head->named_parameters()) {
    if (parameter.key().starts_with("blocks.")) {
     REQUIRE_FALSE(parameter.value().grad().defined());
     continue;
    }
    REQUIRE(parameter.value().grad().defined());
    REQUIRE(torch::allclose(parameter.value().grad(), weights.at(parameter.key()).grad(), 1e-5, 1e-6));
   }
  }
}
TEST_CASE("Stock matched classification and box losses follow independent grouped equations", "[rfdetr][parity]") {
 rf::testsupport::MatcherExecutionFixture fixture;
 rf::ScopedRuntimeContext runtime(nullptr, 0, &fixture.workspace);
 tensor_fixture::FullMatrixPrecision precision;
 for (const auto device : tensor_fixture::available_devices())
  for (const bool ia : {false, true})
   for (const int groups : {1, 2}) {
    rf::DetectionConfig config;
    config.num_classes = 2;
    config.group_detr = groups;
    config.ia_bce_loss = ia;
    config.include_masks = false;
    config.aux_loss = false;
    config.two_stage = false;
    config.use_jit_traced_loss_ops = false;
    config.set_cost_class = 2;
    config.set_cost_bbox = 5;
    config.set_cost_giou = 2;
    config.cls_loss_coef = 1.3;
    config.bbox_loss_coef = 4.1;
    config.giou_loss_coef = 2.7;
    rf::populate_default_detection_weight_dict(config);
    auto logits = torch::tensor({{{1.1, -0.4}, {-0.7, 0.8}}}, torch::kFloat32).repeat({1, groups, 1}).to(device).set_requires_grad(true);
    auto boxes = torch::tensor({{{0.31, 0.42, 0.21, 0.19}, {0.8, 0.7, 0.1, 0.2}}}, torch::kFloat32).repeat({1, groups, 1}).to(device).set_requires_grad(true);
    const auto gt_box = torch::tensor({{0.3, 0.4, 0.2, 0.2}}, torch::kFloat32).to(device);
    const auto gt = targets(gt_box, torch::zeros({1}, torch::TensorOptions().device(device).dtype(torch::kInt64)));
    rf::ModelOutputs output;
    output.main.pred_logits = logits;
    output.main.pred_boxes = boxes;
    const auto pairs = rf::matcher_indices(output, gt, config, true);
    REQUIRE(torch::equal(pairs[0].first, torch::arange(0, groups * 2, 2, torch::kInt64)));
    rf::DetectionStatisticsPacket::Tensors statistics;
    const auto losses = rf::detection_loss_dict(output, gt, config, true, torch::tensor(double(groups), boxes.options()), &statistics);
    require_stock_loss_vocabulary(losses, 0, false, false);
    auto l = logits.detach().clone().set_requires_grad(true), b = boxes.detach().clone().set_requires_grad(true);
    const auto selected = torch::arange(0, groups * 2, 2, torch::TensorOptions().device(device).dtype(torch::kInt64));
    const auto selected_boxes = b[0].index_select(0, selected);
    const auto corners = reference_corners(selected_boxes), gt_corners = reference_corners(gt_box.expand_as(selected_boxes));
    const auto giou = reference_giou(corners, gt_corners);
    const auto l1 = (selected_boxes - gt_box).abs().sum() / groups;
    const auto box_loss = (1 - giou).sum() / groups;
    const auto probability = l.sigmoid();
    auto labels = torch::zeros_like(l);
    labels.index_put_({0, selected, 0}, 1.0);
    torch::Tensor classification;
    if (ia) {
     // Positive quality uses IoU, not generalized IoU. Derive area independently.
     const auto wh = (torch::minimum(corners.slice(-1, 2, 4), gt_corners.slice(-1, 2, 4)) - torch::maximum(corners.slice(-1, 0, 2), gt_corners.slice(-1, 0, 2))).clamp_min(0);
     const auto inter = wh.select(-1, 0) * wh.select(-1, 1);
     const auto iou = inter / (selected_boxes.select(-1, 2) * selected_boxes.select(-1, 3) + 0.04 - inter).clamp_min(1e-7);
     const auto quality = (probability.index({0, selected, 0}).pow(config.focal_alpha) * iou.detach().pow(1 - config.focal_alpha)).clamp_min(0.01).detach();
     auto positive = torch::zeros_like(l);
     positive.index_put_({0, selected, 0}, quality);
     auto negative = probability.square();
     negative.index_put_({0, selected, 0}, 1 - quality);
     classification = (positive * torch::softplus(-l) + negative * torch::softplus(l)).sum() / groups;
    } else {
     const auto probability_target = probability * labels + (1 - probability) * (1 - labels);
     classification =
      ((labels * torch::softplus(-l) + (1 - labels) * torch::softplus(l)) * (1 - probability_target).square() * (config.focal_alpha * labels + (1 - config.focal_alpha) * (1 - labels))).sum() / groups;
    }
    REQUIRE(rf::DetectionStatisticsPacket::get<^^rf::DetectionSufficientStatistics::matched_count>(statistics).item<float>() == groups);
    REQUIRE(rf::DetectionStatisticsPacket::get<^^rf::DetectionSufficientStatistics::class_error_sum>(statistics).item<float>() == 0.F);
    REQUIRE(rf::DetectionStatisticsPacket::get<^^rf::DetectionSufficientStatistics::image_count>(statistics).item<float>() == 1.F);
    REQUIRE(rf::DetectionStatisticsPacket::get<^^rf::DetectionSufficientStatistics::cardinality_error_sum>(statistics).item<float>() == 2 * groups - 1);
    for (const auto& statistic : statistics) REQUIRE_FALSE(statistic.requires_grad());
    REQUIRE(torch::allclose(losses.at("loss_ce"), classification, 1e-5, 1e-6));
    REQUIRE(torch::allclose(losses.at("loss_bbox"), l1, 1e-5, 1e-6));
    REQUIRE(torch::allclose(losses.at("loss_giou"), box_loss, 1e-5, 1e-6));
    auto distributed_config = config;
    distributed_config.world_size = 2;
    int reductions = 0;
    rf::DetectionStatisticsPacket::Tensors distributed_statistics;
    const auto distributed = rf::detection_loss_dict(output, gt, distributed_config, true, true, [&](torch::Tensor& count) {
     ++reductions;
     count.add_(3 * groups);
    }, &distributed_statistics);
    REQUIRE(reductions == 1);
    for (std::size_t field = 0; field < statistics.size(); ++field) REQUIRE(torch::equal(statistics[field], distributed_statistics[field]));
    REQUIRE(torch::allclose(distributed.at("loss_ce"), classification / 4, 1e-5, 1e-6));
    REQUIRE(torch::allclose(distributed.at("loss_bbox"), l1 / 4, 1e-5, 1e-6));
    auto summed_config = config;
    summed_config.sum_group_losses = true;
    const auto summed = rf::detection_loss_dict(output, gt, summed_config, true, false);
    require_stock_loss_vocabulary(summed, 0, false, false);
    REQUIRE(torch::allclose(summed.at("loss_ce"), classification * groups, 1e-5, 1e-6));
    auto layered = output;
    auto background = output.main;
    background.pred_logits = torch::full_like(logits, -2);
    layered.aux_outputs = {background};
    layered.enc_outputs = background;
    rf::DetectionStatisticsPacket::Tensors main_statistics;
    const auto layered_losses = rf::detection_loss_dict(layered, gt, config, true, 1.0, &main_statistics);
    require_stock_loss_vocabulary(layered_losses, 1, true, false);
    REQUIRE(layered_losses.at("cardinality_error_0").item<float>() == 1.F);
    REQUIRE(layered_losses.at("cardinality_error_enc").item<float>() == 1.F);
    for (std::size_t field = 0; field < statistics.size(); ++field) REQUIRE(torch::equal(statistics[field], main_statistics[field]));
    const auto actual = rf::weighted_detection_loss(losses, config, device);
    const auto expected = 1.3 * classification + 4.1 * l1 + 2.7 * box_loss;
    actual.backward();
    expected.backward();
    REQUIRE(torch::allclose(logits.grad(), l.grad(), 1e-5, 1e-6));
    REQUIRE(torch::allclose(boxes.grad(), b.grad(), 1e-5, 1e-6));
   }
}
TEST_CASE("Stock dense and sparse masks share explicit CPU CUDA samples and retained trace VJPs", "[rfdetr][parity]") {
 tensor_fixture::FullMatrixPrecision precision;
 rf::testsupport::MatcherExecutionFixture fixture;
 auto& workspace = fixture.workspace;
 rf::ScopedRuntimeContext runtime(nullptr, 0, &workspace);
 for (const auto device : tensor_fixture::available_devices())
  for (bool sparse : {false, true})
   for (int change : {0, 1, 2, 3, 4, 5, 6}) {
    CAPTURE(device, sparse, change);
    const auto prior_identity = workspace.loss_cache().sigmoid_focal.identity();
    const auto amp = change == 1 || change == 3 ? torch::kFloat16 : change == 2 ? torch::kBFloat16 : torch::kFloat32;
    const int64_t ratio = change == 1 ? 4 : 2;
    const double alpha = change == 3 ? 0.6 : 0.25, denominator = change == 5 ? 3.0 : 1.0;
    rf::DetectionConfig config;
    config.num_classes = 2;
    config.group_detr = 1;
    config.include_masks = true;
    config.ia_bce_loss = false;
    config.mask_point_sample_ratio = ratio;
    config.focal_alpha = alpha;
    for (const auto suffix : {"", "_0", "_enc"}) {
     config.weight_dict.emplace_back(std::string("loss_ce") + suffix, 1.3);
     config.weight_dict.emplace_back(std::string("loss_mask_ce") + suffix, 2.1);
     config.weight_dict.emplace_back(std::string("loss_mask_dice") + suffix, 0.7);
    }
    const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
    auto gt = targets(torch::tensor({{0.5, 0.5, 0.2, 0.2}}, options), torch::zeros({1}, torch::kInt64).to(device));
    gt.packed_masks = rf::PackedTargetMasks{torch::tensor({{int64_t{0x5a5a}}}, torch::kInt64).to(device), 4, 4};
    const auto target_mask = torch::tensor({{0., 1., 0., 1.}, {1., 0., 1., 0.}, {0., 1., 0., 1.}, {1., 0., 1., 0.}}, options).view({1, 1, 4, 4});
    const int64_t count = std::max<int64_t>(4, 16 / ratio), selected_count = static_cast<int64_t>(0.75 * static_cast<double>(count));
    // Explicit nonuniform draws are identical for native/reference and every device.
    const auto coordinates = [&](int64_t n) { return (torch::arange(n * 2, torch::kFloat32) * 0.173 + 0.037).remainder(1).reshape({1, n, 2}).to(device); };
    rf::LayerMaskSamples supplied{coordinates(16 / ratio), coordinates(count * 3), coordinates(count - selected_count)};
    std::array<rf::LayerMaskSamples, 3> samples{supplied, supplied, supplied};
    std::array<std::vector<torch::Tensor>, 3> leaves;
    std::array<torch::Tensor, 3> totals;
    for (int route = 0; route < 3; ++route) {
     mmltk::backend::ml::cuda::TorchAutocastScope forward_scope(amp != torch::kFloat32, amp);
     auto logits = torch::tensor({{{0.37, -0.61}}}, options).set_requires_grad(true);
     auto spatial = (torch::arange(16, options).reshape({1, 1, 4, 4}) * 0.13 - 0.9).set_requires_grad(true);
     auto query = torch::full({1, 1, 1}, 0.83, options).set_requires_grad(true);
     auto bias = torch::full({1}, 0.17, options).set_requires_grad(true);
     leaves[route] = {logits, spatial};
     if (sparse) {
      leaves[route].push_back(query);
      leaves[route].push_back(bias);
     }
     if (route < 2) {
      config.use_jit_traced_loss_ops = route == 0;
      rf::ModelOutputs output;
      output.main.pred_logits = logits;
      output.main.pred_boxes = gt.all_boxes.unsqueeze(0);
      if (sparse)
       output.main.sparse_pred_masks = rf::SparsePredMasks{spatial, query, bias};
      else
       output.main.pred_masks = spatial;
      output.aux_outputs = {output.main};
      output.enc_outputs = output.main;
      rf::DetectionStatisticsPacket::Tensors statistics;
      const auto losses = rf::detection_loss_dict(output, gt, config, true, torch::full({}, denominator, options), samples, &statistics);
      require_stock_loss_vocabulary(losses, 1, true, true);
      REQUIRE(rf::DetectionStatisticsPacket::get<^^rf::DetectionSufficientStatistics::matched_count>(statistics).item<float>() == 1.F);
      REQUIRE(rf::DetectionStatisticsPacket::get<^^rf::DetectionSufficientStatistics::image_count>(statistics).item<float>() == 1.F);
      for (const auto& statistic : statistics) { REQUIRE(statistic.defined()); REQUIRE_FALSE(statistic.requires_grad()); }
      totals[route] = rf::weighted_detection_loss(losses, config, device);
      if (route == 0) {
       // Exercise failure and old-signature reuse in this very criterion workspace.
       auto& slot = workspace.loss_cache().sigmoid_focal;
       const auto original = slot.identity();
       if (change > 0) {
        const bool changed_context = device.is_cuda() && change <= 4;
        const bool changed_alpha = change == 3 || change == 4;
        if (changed_context || changed_alpha)
         REQUIRE(original != prior_identity);
        else
         REQUIRE(original == prior_identity);
       }
       const auto labels = torch::tensor({{{1., 0.}}}, options);
       {
        mmltk::backend::ml::cuda::TorchAutocastScope rejected_scope(amp == torch::kFloat32, torch::kBFloat16);
        REQUIRE_THROWS(slot.invoke(
         "__torch__.RejectedCriterionTrace", {alpha + 0.1, 3.0}, [](const torch::Tensor&, const torch::Tensor&) -> torch::Tensor { throw std::runtime_error("replacement failure"); },
         logits.to(torch::kFloat64), labels.to(torch::kFloat64)));
       }
       REQUIRE(slot.identity() == original);
       const auto reused = rf::detection_loss_dict(output, gt, config, true, torch::full({}, denominator, options), samples);
       require_stock_loss_vocabulary(reused, 1, true, true);
       REQUIRE(slot.identity() == original);
       REQUIRE(torch::allclose(totals[route], rf::weighted_detection_loss(reused, config, device), 1e-5, 1e-6));
       // Gamma is fixed at two by the criterion. Mutate the same slot through its
       // ordinary semantic API, then prove criterion restoration replaces it.
       if (change == 3) {
        const auto changed = slot.invoke(
         "__torch__.ChangedCriterionGamma", {alpha, 3.0},
         [alpha](const torch::Tensor& x, const torch::Tensor& y) {
          const auto p = x.sigmoid();
          return ((y * torch::softplus(-x) + (1 - y) * torch::softplus(x)) * torch::pow(1 - (p * y + (1 - p) * (1 - y)), 3) * (alpha * y + (1 - alpha) * (1 - y))).mean(1).sum();
         },
         logits, labels);
        REQUIRE(slot.identity() != original);
        const auto p = logits.sigmoid();
        const auto reference =
         ((labels * torch::softplus(-logits) + (1 - labels) * torch::softplus(logits)) * torch::pow(1 - (p * labels + (1 - p) * (1 - labels)), 3) * (alpha * labels + (1 - alpha) * (1 - labels)))
          .mean(1)
          .sum();
        REQUIRE(torch::allclose(changed, reference, 1e-5, 1e-6));
        mmltk::backend::ml::cuda::TorchAutocastScope backward_scope(false, torch::kFloat32);
        REQUIRE(torch::allclose(torch::autograd::grad({changed}, {logits}, {}, true)[0], torch::autograd::grad({reference}, {logits}, {}, true)[0], 1e-5, 1e-6));
       }
       const auto restored = rf::detection_loss_dict(output, gt, config, true, torch::full({}, denominator, options), samples);
       if (change != 3) REQUIRE(slot.identity() == original);
       const auto restored_total = rf::weighted_detection_loss(restored, config, device), reused_total = rf::weighted_detection_loss(reused, config, device);
       REQUIRE(torch::allclose(totals[route], restored_total, 1e-5, 1e-6));
       mmltk::backend::ml::cuda::TorchAutocastScope backward_scope(false, torch::kFloat32);
       const auto reused_gradients = torch::autograd::grad({reused_total}, leaves[route], {}, true), restored_gradients = torch::autograd::grad({restored_total}, leaves[route], {}, true);
       for (size_t i = 0; i < leaves[route].size(); ++i) REQUIRE(torch::allclose(reused_gradients[i], restored_gradients[i], 2e-5, 2e-6));
       const auto original_gradients = torch::autograd::grad({totals[route]}, leaves[route], {}, true);
       for (size_t i = 0; i < leaves[route].size(); ++i) REQUIRE(torch::allclose(original_gradients[i], restored_gradients[i], 2e-5, 2e-6));
      }
     } else {
      // Upstream projects each matched layer independently. Reusing one AMP
      // contraction with a tripled cotangent moves its Half rounding boundary.
      totals[route] = torch::zeros({}, options);
      for (const auto& layer_samples : samples) {
       const auto dense = sparse ? torch::matmul(query[0], spatial[0].flatten(1)).reshape({1, 1, 4, 4}) + bias : spatial;
       const auto uncertainty = reference_mask_sample(dense.detach(), layer_samples.uncertain_candidates, false).abs().neg();
       const auto indices = std::get<1>(uncertainty.topk(selected_count, 1));
       const auto coords = torch::cat({layer_samples.uncertain_candidates[0].index_select(0, indices.flatten()).unsqueeze(0), layer_samples.uncertain_random}, 1);
       const auto prediction = reference_mask_sample(dense, coords, false), labels = reference_mask_sample(target_mask, coords, true);
       REQUIRE(torch::equal(rf::sample_target_masks(*gt.packed_masks, torch::zeros({1}, torch::kInt64).to(device), coords, "uncertain labels"), labels));
       const auto bce = (labels * torch::softplus(-prediction) + (1 - labels) * torch::softplus(prediction)).mean(1).sum() / denominator;
       const auto probability = prediction.sigmoid();
       const auto dice = (1 - (2 * (probability * labels).sum(1) + 1) / (probability.sum(1) + labels.sum(1) + 1)).sum() / denominator;
       const auto classes = torch::tensor({{{1., 0.}}}, options), p = logits.sigmoid();
       const auto ce =
        ((classes * torch::softplus(-logits) + (1 - classes) * torch::softplus(logits)) * torch::pow(1 - (p * classes + (1 - p) * (1 - classes)), 2) * (alpha * classes + (1 - alpha) * (1 - classes)))
         .sum() /
        denominator;
       totals[route] = totals[route] + 1.3 * ce + 2.1 * bce + 0.7 * dice;
      }
     }
    }
    std::array<std::vector<torch::Tensor>, 3> gradients;
    for (int route = 0; route < 3; ++route) gradients[route] = torch::autograd::grad({totals[route]}, leaves[route]);
    for (int route = 0; route < 2; ++route) {
     REQUIRE(torch::allclose(totals[route], totals[2], 2e-5, 2e-6));
     for (size_t i = 0; i < leaves[route].size(); ++i) {
      CAPTURE(route, i, gradients[route][i], gradients[2][i]);
      REQUIRE(torch::allclose(gradients[route][i], gradients[2][i], 2e-5, 2e-6));
     }
    }
   }
}
TEST_CASE("Loss contraction cache follows effective CUDA precision with outstanding backwards", "[rfdetr][parity]") {
 tensor_fixture::FullMatrixPrecision precision;
 rf::testsupport::MatcherExecutionFixture fixture;
 auto& workspace = fixture.workspace;
 struct Context {
  bool enabled;
  c10::ScalarType dtype;
 };
 const std::array<Context, 7> contexts{
  {{false, torch::kFloat16}, {false, torch::kBFloat16}, {true, torch::kFloat16}, {true, torch::kFloat16}, {true, torch::kBFloat16}, {true, torch::kFloat16}, {false, torch::kFloat16}}};
 const auto recorded = [](const torch::Tensor& x, const torch::Tensor& y) {
  const auto p = x.sigmoid();
  return 1 - (2 * torch::einsum("nc,mc->nm", {p, y}) + 1) / (p.sum(1).unsqueeze(1) + y.sum(1).unsqueeze(0) + 1);
 };
 for (const auto device : tensor_fixture::available_devices()) {
  struct Pending {
   std::vector<torch::Tensor> executions;
   torch::Tensor expected;
   std::vector<torch::Tensor> inputs, reference;
   double relative, absolute;
  };
  std::vector<Pending> pending;
  auto& slot = workspace.loss_cache().batch_dice;
  for (size_t index = 0; index < contexts.size(); ++index) {
   const auto context = contexts[index];
   CAPTURE(device, index, context.enabled, context.dtype);
   const bool amp = device.is_cuda() && context.enabled;
   const double relative = amp ? (context.dtype == torch::kBFloat16 ? 0.02 : 0.002) : 2e-5;
   const double absolute = relative * 0.1;
   const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat32);
   const int64_t points = 3 + static_cast<int64_t>(index % 2);
   auto x = (torch::arange(2 * points, options).reshape({2, points}) * 0.13713 - 0.61917).set_requires_grad(true);
   auto y = (torch::arange(3 * points, options).reshape({3, points}) * 0.03171 + 0.17319).set_requires_grad(true);
   auto a = x.detach().clone().set_requires_grad(true), b = y.detach().clone().set_requires_grad(true);
   torch::Tensor actual, expected;
   std::vector<torch::Tensor> executions;
   {
    // Set the inactive dtype explicitly as well: it must not cause replacement.
    mmltk::backend::ml::cuda::TorchAutocastScope dtype_scope(true, context.dtype);
    mmltk::backend::ml::cuda::TorchAutocastScope forward_scope(context.enabled, context.dtype);
    const auto previous = slot.identity();
    actual = slot.invoke("__torch__.PrecisionBatchDice", recorded, x, y);
    if (index > 0) {
     const auto prior = contexts[index - 1];
     const bool incompatible = device.is_cuda() && (prior.enabled != context.enabled || (context.enabled && prior.dtype != context.dtype));
     if (incompatible)
      REQUIRE(slot.identity() != previous);
     else
      REQUIRE(slot.identity() == previous);
    }
    const auto identity = slot.identity();
    const auto reused = slot.invoke("__torch__.PrecisionBatchDice", recorded, x, y);
    REQUIRE(slot.identity() == identity);
    REQUIRE(torch::allclose(actual, reused, relative, absolute));
    executions = {actual, reused};
    const auto p = a.sigmoid();
    expected = 1 - (2 * torch::matmul(p, b.transpose(0, 1)) + 1) / (p.sum(1).unsqueeze(1) + b.sum(1).unsqueeze(0) + 1);
    REQUIRE(torch::allclose(actual, expected, relative, absolute));
    if (device.is_cuda()) {
     // Change only execution context: failure must leave the full prior key usable.
     {
      mmltk::backend::ml::cuda::TorchAutocastScope rejected_scope(!context.enabled, torch::kBFloat16);
      REQUIRE_THROWS(
       slot.invoke("__torch__.RejectedPrecisionBatchDice", [](const torch::Tensor&, const torch::Tensor&) -> torch::Tensor { throw std::runtime_error("precision candidate failure"); }, x, y));
     }
     REQUIRE(slot.identity() == identity);
     const auto after_failure = slot.invoke("__torch__.PrecisionBatchDice", recorded, x, y);
     REQUIRE(slot.identity() == identity);
     CAPTURE(actual, after_failure, (after_failure - actual).abs().max().item<double>());
     // Optimized execution contracts Half/BFloat16 arithmetic differently;
     // the cache identity is exact, while equation/VJP parity is dtype bounded.
     REQUIRE(torch::allclose(after_failure, expected, relative, absolute));
     executions.push_back(after_failure);
    }
   }
   pending.push_back({std::move(executions), expected, {x, y}, {a, b}, relative, absolute});
  }
  // Every old graph remains differentiable after its slot has been replaced.
  for (const auto& value : pending) {
   const auto expected = torch::autograd::grad({value.expected.sum()}, value.reference);
   for (const auto& execution : value.executions) {
    const auto actual = torch::autograd::grad({execution.sum()}, value.inputs);
    for (size_t i = 0; i < actual.size(); ++i) REQUIRE(torch::allclose(actual[i], expected[i], value.relative, value.absolute));
   }
  }
 }
}
TEST_CASE("Weighted matcher preserves nonfinite costs until full-domain sanitization", "[rfdetr][parity]") {
 tensor_fixture::FullMatrixPrecision precision;
 for (const auto device : tensor_fixture::available_devices())
  for (int scenario = 0; scenario < 8; ++scenario)
   for (bool sparse : {false, true}) {
    if (sparse && scenario >= 3) continue;
    auto logits_cpu = torch::tensor({{{0.4F, -0.2F}, {0.8F, 0.1F}}, {{-0.3F, 0.7F}, {0.2F, -0.9F}}});
    if (scenario == 1) logits_cpu.index_put_({0, 0, 0}, std::numeric_limits<float>::quiet_NaN());
    if (scenario == 2) {
     logits_cpu.index_put_({0, 0, 0}, std::numeric_limits<float>::infinity());
     logits_cpu.index_put_({1, 1, 1}, -std::numeric_limits<float>::infinity());
    }
    if (scenario == 6) logits_cpu.add_(4);  // Entire finite classification domain is negative.
    if (scenario == 7) {
     logits_cpu = torch::tensor({{{-20.F, 5.F}, {-15.F, 3.F}}, {{4.F, -18.F}, {5.F, -16.F}}});
     logits_cpu.index_put_({0, 0, 0}, std::numeric_limits<float>::quiet_NaN());
    }
    if (scenario == 3) logits_cpu.fill_(std::numeric_limits<float>::quiet_NaN());
    rf::ModelOutputs outputs;
    outputs.main.pred_logits = logits_cpu.repeat({1, 2, 1}).to(device);
    outputs.main.pred_boxes = torch::tensor({{{0.2F, 0.2F, 0.1F, 0.1F}, {0.8F, 0.8F, 0.2F, 0.2F}}, {{0.3F, 0.7F, 0.2F, 0.3F}, {0.7F, 0.2F, 0.1F, 0.2F}}}).to(device);
    outputs.main.pred_boxes = outputs.main.pred_boxes.repeat({1, 2, 1});
    rf::PreparedTargets gt;
    gt.all_boxes = torch::tensor({{0.2F, 0.2F, 0.1F, 0.1F}, {0.8F, 0.8F, 0.2F, 0.2F}, {0.3F, 0.7F, 0.2F, 0.3F}, {0.7F, 0.2F, 0.1F, 0.2F}}).to(device);
    gt.all_labels = torch::tensor({0, 1, 0, 1}, torch::kInt64).to(device);
    gt.counts = {2, 2};
    gt.offsets = {0, 2};
    for (int image = 0; image < 2; ++image) {
     rf::PreparedTarget target;
     target.boxes = gt.all_boxes.narrow(0, image * 2, 2);
     target.labels = gt.all_labels.narrow(0, image * 2, 2);
     gt.targets.push_back(target);
    }
    rf::DetectionConfig config;
    config.num_classes = 2;
    config.group_detr = 2;
    config.set_cost_class = scenario == 4 ? std::numeric_limits<float>::max() : scenario == 7 ? std::numeric_limits<float>::max() / 8 : 2;
    config.set_cost_bbox = scenario >= 6 ? 0 : 5;
    config.set_cost_giou = scenario >= 6 ? 0 : 2;
    config.use_jit_traced_loss_ops = false;
    const auto l = logits_cpu.repeat({1, 2, 1}).flatten(0, 1), probability = l.sigmoid();
    const auto class_cost = (0.25 * (1 - probability).square() * torch::softplus(-l) - 0.75 * probability.square() * torch::softplus(l)).index_select(1, gt.all_labels.cpu());
    const auto boxes = outputs.main.pred_boxes.cpu().flatten(0, 1), target_boxes = gt.all_boxes.cpu();
    const auto delta = (boxes.unsqueeze(1) - target_boxes.unsqueeze(0)).abs().sum(-1);
    const auto a = reference_corners(boxes).unsqueeze(1).expand({8, 4, 4}), b = reference_corners(target_boxes).unsqueeze(0).expand({8, 4, 4});
    auto cost = config.set_cost_bbox * delta + config.set_cost_class * class_cost - config.set_cost_giou * reference_giou(a, b);
    rf::LayerMaskSamples samples;
    if (scenario < 3) {
     config.include_masks = true;
     config.mask_point_sample_ratio = 4;
     samples.matcher = torch::tensor({{{0.125F, 0.125F}, {0.375F, 0.375F}, {0.625F, 0.625F}, {0.875F, 0.875F}}}).to(device);
     outputs.main.pred_masks = (torch::arange(128, torch::kFloat32).reshape({2, 4, 4, 4}) * 0.03 - 1.7).to(device);
     gt.packed_masks = rf::PackedTargetMasks{torch::tensor({{int64_t{0x5a5a}}, {int64_t{0xa5a5}}, {int64_t{0xffff}}, {int64_t{0}}}, torch::kInt64).to(device), 4, 4};
     const auto predictions = F::grid_sample(outputs.main.pred_masks->cpu().reshape({8, 1, 4, 4}), (2 * samples.matcher.cpu() - 1).unsqueeze(2).expand({8, 4, 1, 2}),
      F::GridSampleFuncOptions().mode(torch::kBilinear).padding_mode(torch::kBorder).align_corners(false))
                               .reshape({8, 4});
     const auto labels = torch::tensor({{0.F, 0.F, 0.F, 0.F}, {1.F, 1.F, 1.F, 1.F}, {1.F, 1.F, 1.F, 1.F}, {0.F, 0.F, 0.F, 0.F}});
     REQUIRE(torch::equal(rf::sample_target_masks(*gt.packed_masks, torch::arange(4, torch::kInt64).to(device), samples.matcher, "matcher fixture").cpu(), labels));
     const auto ce = (torch::softplus(-predictions).unsqueeze(1) * labels.unsqueeze(0) + torch::softplus(predictions).unsqueeze(1) * (1 - labels).unsqueeze(0)).mean(-1);
     const auto probabilities = predictions.sigmoid();
     const auto dice = 1 - (2 * (probabilities.unsqueeze(1) * labels.unsqueeze(0)).sum(-1) + 1) / (probabilities.sum(-1).unsqueeze(1) + labels.sum(-1).unsqueeze(0) + 1);
     cost += config.mask_ce_loss_coef * ce + config.mask_dice_loss_coef * dice;
     if (sparse) {
      outputs.main.sparse_pred_masks =
       rf::SparsePredMasks{*outputs.main.pred_masks, torch::eye(4, outputs.main.pred_logits.options()).unsqueeze(0).expand({2, 4, 4}), torch::zeros({1}, outputs.main.pred_logits.options())};
      outputs.main.pred_masks.reset();
     }
    }
    if (scenario == 5) {
     // A positive ratio above area produces zero matcher points: BCE divides
     // zero by zero, and its weighted nonfinite term participates even at zero.
     config.include_masks = true;
     config.mask_point_sample_ratio = 17;
     config.mask_ce_loss_coef = 0;
     config.mask_dice_loss_coef = 1;
     outputs.main.pred_masks = torch::ones({2, 4, 4, 4}, outputs.main.pred_logits.options());
     gt.packed_masks = rf::PackedTargetMasks{torch::zeros({4, 1}, torch::TensorOptions().device(device).dtype(torch::kInt64)), 4, 4};
     cost = cost + torch::full_like(cost, std::numeric_limits<float>::quiet_NaN());
    }
    if (scenario == 6) REQUIRE(cost.lt(0).all().item<bool>());
    const auto finite = torch::isfinite(cost);
    if (!finite.all().item<bool>()) {
     float sentinel = std::numeric_limits<float>::max();
     if (finite.any().item<bool>()) {
      const auto values = cost.index({finite});
      const auto proposed = values.max() + values.abs().max() + 1;
      if (scenario == 7) REQUIRE_FALSE(torch::isfinite(proposed).item<bool>());
      if (torch::isfinite(proposed).item<bool>()) sentinel = proposed.item<float>();
     }
     cost = torch::where(finite, cost, torch::full_like(cost, sentinel));
    }
    const auto pairs = rf::matcher_indices(outputs, gt, config, true, samples);
    for (int image = 0; image < 2; ++image)
     for (int group = 0; group < 2; ++group) {
      const auto matrix = cost.narrow(0, image * 4 + group * 2, 2).narrow(1, image * 2, 2).to(torch::kFloat64);
      const double diagonal = matrix.index({0, 0}).item<double>() + matrix.index({1, 1}).item<double>();
      const double crossed = matrix.index({0, 1}).item<double>() + matrix.index({1, 0}).item<double>();
      const auto row = pairs[image].first.cpu().narrow(0, group * 2, 2), column = pairs[image].second.cpu().narrow(0, group * 2, 2);
      REQUIRE(torch::equal(row, torch::tensor({group * 2, group * 2 + 1}, torch::kInt64)));
      REQUIRE(torch::equal(column, torch::tensor(diagonal <= crossed ? std::vector<int64_t>{0, 1} : std::vector<int64_t>{1, 0}, torch::kInt64)));
     }
   }
}
TEST_CASE("Selected encoder memory reduces pointwise head work with equal all-position VJPs", "[rfdetr][parity]") {
 tensor_fixture::FullMatrixPrecision precision;
 for (int64_t spatial : {2, 3, 7})
  for (int64_t groups : {1, 2})
   for (bool reparam : {false, true}) {
    rf::RfDetrMlp head(2, 3, 4, 3);
    {
     torch::NoGradGuard guard;
     for (const auto& parameter : head->named_parameters()) parameter.value().copy_((torch::arange(parameter.value().numel(), torch::kFloat32) * 0.037 + 0.11).reshape_as(parameter.value()));
    }
    std::unordered_map<std::string, torch::Tensor> reference_weights;
    for (const auto& parameter : head->named_parameters()) reference_weights.emplace(parameter.key(), parameter.value().detach().clone().set_requires_grad(true));
    auto memory = (torch::arange(groups * spatial * 2, torch::kFloat32).reshape({groups, spatial, 2}) * 0.071 - 0.3).set_requires_grad(true);
    auto proposals = (torch::arange(groups * spatial * 4, torch::kFloat32).reshape({groups, spatial, 4}) * 0.003 + 0.2).set_requires_grad(true);
    auto reference_memory = memory.detach().clone().set_requires_grad(true), reference_proposals = proposals.detach().clone().set_requires_grad(true);
    const auto scores = memory.detach().select(-1, 0) * -0.7 + memory.detach().select(-1, 1) * 0.2;
    const auto selected = std::get<1>(scores.topk(std::min<int64_t>(spatial, 3), 1));
    const auto gather = [&](const torch::Tensor& tensor) { return tensor.gather(1, selected.unsqueeze(-1).expand({groups, selected.size(1), tensor.size(2)})); };
    const auto refine = [=](const torch::Tensor& base, const torch::Tensor& delta) {
     return reparam ? torch::cat({delta.slice(-1, 0, 2) * base.slice(-1, 2, 4) + base.slice(-1, 0, 2), delta.slice(-1, 2, 4).exp() * base.slice(-1, 2, 4)}, -1) : (delta + base).sigmoid();
    };
    const auto actual = refine(gather(proposals), head->forward(gather(memory)));
    auto all_delta = reference_memory;
    for (int layer = 0; layer < 3; ++layer) {
     const auto prefix = "layers." + std::to_string(layer);
     all_delta = torch::matmul(all_delta, reference_weights.at(prefix + ".weight").t()) + reference_weights.at(prefix + ".bias");
     if (layer < 2) all_delta = torch::relu(all_delta);
    }
    const auto expected = gather(refine(reference_proposals, all_delta));
    REQUIRE(torch::allclose(actual, expected, 1e-5, 1e-6));
    REQUIRE_FALSE(gather(proposals).detach().requires_grad());
    actual.square().sum().backward();
    expected.square().sum().backward();
    REQUIRE(torch::allclose(memory.grad(), reference_memory.grad(), 1e-5, 1e-6));
    REQUIRE(torch::allclose(proposals.grad(), reference_proposals.grad(), 1e-5, 1e-6));
    for (const auto& parameter : head->named_parameters()) REQUIRE(torch::allclose(parameter.value().grad(), reference_weights.at(parameter.key()).grad(), 1e-5, 1e-6));
   }
}
TEST_CASE("Full mask block retains upstream AMP derivatives through an AdamW update", "[rfdetr][parity]") {
 tensor_fixture::FullMatrixPrecision precision;
 for (const bool selective : {false, true})
  for (const auto device : tensor_fixture::available_devices())
   for (const auto dtype : {torch::kFloat32, torch::kFloat16, torch::kBFloat16}) {
    if (device.is_cpu() && dtype != torch::kFloat32) continue;
    CAPTURE(selective, device, dtype);
    rf::DepthwiseConvBlock block(2, 0.3);
    rf::detail::SelectiveTensorRegion region("depthwise_tail_oracle");
    region.prepare(true, selective, 1);
    block->to(device);
    {
     torch::NoGradGuard guard;
     for (const auto& parameter : block->named_parameters())
      parameter.value().copy_((torch::arange(parameter.value().numel(), parameter.value().options()) * 0.031713 + 0.113719).reshape_as(parameter.value()));
    }
    auto input = (torch::arange(18, torch::TensorOptions().device(device).dtype(torch::kFloat32)).reshape({1, 2, 3, 3}) * 0.071371 - 0.3).set_requires_grad(true);
    auto x = input.detach().clone().set_requires_grad(true);
    std::unordered_map<std::string, torch::Tensor> weights;
    for (const auto& parameter : block->named_parameters()) weights.emplace(parameter.key(), parameter.value().detach().clone().set_requires_grad(true));
    torch::Tensor actual, rounded;
    {
     mmltk::backend::ml::cuda::TorchAutocastScope scope(dtype != torch::kFloat32, dtype);
     const auto convolved = rf::segmentation_depthwise(input, block->dwconv->weight, block->dwconv->bias);
     for (int execution = 0; execution < (selective ? 4 : 1); ++execution) {
      actual =
       region.invoke(true, {input, convolved}, {block.get()}, [&](const auto& tensors) { return rf::detail::SelectiveTensorRegion::Tensors{block->pointwise_tail(tensors[0], tensors[1])}; }).front();
     }
     rounded = F::conv2d(x, weights.at("dwconv.weight"), F::Conv2dFuncOptions().bias(weights.at("dwconv.bias")).padding(1).groups(2)).detach();
    }
    // Reference custom derivative: original FP32 operands differentiated by
    // ordinary convolution, with the actual autocast forward value substituted.
    const auto convolution = F::conv2d(x, weights.at("dwconv.weight"), F::Conv2dFuncOptions().bias(weights.at("dwconv.bias")).padding(1).groups(2));
    auto tail = (convolution + (rounded.to(torch::kFloat32) - convolution.detach())).to(rounded.scalar_type()).permute({0, 2, 3, 1});
    torch::Tensor expected;
    {
     mmltk::backend::ml::cuda::TorchAutocastScope scope(dtype != torch::kFloat32, dtype);
     tail = F::layer_norm(tail, F::LayerNormFuncOptions({2}).weight(weights.at("norm.weight")).bias(weights.at("norm.bias")).eps(1e-6));
     tail = torch::linear(tail, weights.at("pwconv1.weight"), weights.at("pwconv1.bias"));
     tail = torch::gelu(tail);
     expected = (tail * weights.at("gamma")).permute({0, 3, 1, 2}) + x;
    }
    // Use the upstream layer-normalization primitive: a manual variance VJP's
    // near-zero rounding can change the first AdamW update's sign.
    const double tolerance = dtype == torch::kBFloat16 ? 0.02 : dtype == torch::kFloat16 ? 0.002 : 1e-5;
    REQUIRE(torch::allclose(actual, expected, tolerance, tolerance));
    actual.square().sum().backward();
    expected.square().sum().backward();
    REQUIRE(torch::allclose(input.grad(), x.grad(), tolerance, tolerance));
    std::unordered_map<std::string, torch::Tensor> updated;
    for (const auto& parameter : block->named_parameters()) {
     const auto& reference = weights.at(parameter.key());
     REQUIRE(torch::allclose(parameter.value().grad(), reference.grad(), tolerance, tolerance));
     updated.emplace(parameter.key(), reference.detach() * (1 - 0.003 * 0.07) - 0.003 * reference.grad() / (reference.grad().abs() + 1e-8));
    }
    torch::optim::AdamW optimizer(block->parameters(), torch::optim::AdamWOptions(0.003).weight_decay(0.07));
    optimizer.step();
    for (const auto& parameter : block->named_parameters()) {
     CAPTURE(parameter.key(), parameter.value(), updated.at(parameter.key()));
     REQUIRE(torch::allclose(parameter.value(), updated.at(parameter.key()), tolerance, tolerance));
    }
   }
}
TEST_CASE("Depthwise saved operands support accumulation optional bias and version checks", "[rfdetr][parity]") {
 auto x = torch::ones({1, 2, 3, 3}).set_requires_grad(true), weight = torch::full({2, 1, 3, 3}, 0.13717).set_requires_grad(true);
 const auto first = rf::segmentation_depthwise(x, weight, {});
 const auto second = rf::segmentation_depthwise(x * 2, weight, {});
 (second.sum() + first.sum()).backward();
 REQUIRE(weight.grad().defined());
 REQUIRE(x.grad().defined());
 auto frozen = weight.detach();
 auto live = x.detach().clone().set_requires_grad(true);
 rf::segmentation_depthwise(live, frozen, {}).sum().backward();
 REQUIRE(live.grad().defined());
 REQUIRE_FALSE(frozen.grad().defined());
 const auto outstanding = rf::segmentation_depthwise(live, weight, {});
 {
  torch::NoGradGuard guard;
  weight.add_(0.1);
 }
 REQUIRE_THROWS(outstanding.sum().backward());
}
}  // namespace
TEST_CASE("Packed mask admission rejects malformed extents and CPU logical indices", "[rfdetr][parity]") {
 for (const auto device : tensor_fixture::available_devices())
  for (int malformed = 0; malformed < 3; ++malformed) {
   auto gt = targets(torch::ones({2, 4}).to(device), torch::zeros({2}, torch::kInt64).to(device));
   gt.packed_masks = rf::PackedTargetMasks{torch::zeros({malformed == 0 ? 1 : 2, malformed == 1 ? 0 : 1}, torch::kInt64).to(device), 4, malformed == 2 ? 17 : 4};
   rf::ModelOutputs output;
   output.main.pred_logits = torch::zeros({1, 2, 1}).to(device);
   output.main.pred_boxes = torch::ones({1, 2, 4}).to(device);
   output.main.pred_masks = torch::ones({1, 2, 4, 4}).to(device);
   rf::DetectionConfig config;
   config.include_masks = true;
   config.num_classes = 1;
   config.group_detr = 1;
   REQUIRE_THROWS_AS(rf::matcher_indices(output, gt, config, true), std::invalid_argument);
  }
 rf::PackedTargetMasks packed{torch::zeros({1, 1}, torch::kInt64), 4, 4};
 for (int64_t index : {-1, 1, 2}) REQUIRE_THROWS_AS(rf::sample_target_masks(packed, torch::tensor({index}, torch::kInt64), torch::zeros({1, 1, 2}), "invalid fixture"), std::invalid_argument);
 packed.bits = torch::empty({0, 1}, torch::kInt64);
 REQUIRE_THROWS_AS(rf::sample_target_masks(packed, torch::zeros({1}, torch::kInt64), torch::zeros({1, 0, 2}), "empty invalid fixture"), std::invalid_argument);
 REQUIRE(rf::sample_target_masks(packed, torch::empty({0}, torch::kInt64), torch::zeros({1, 0, 2}), "empty valid fixture").sizes() == torch::IntArrayRef({0, 0}));
}
TEST_CASE("Stock geometry covers epsilon neighborhoods mixed promotion empty domains and tiny updates", "[rfdetr][parity]") {
 tensor_fixture::FullMatrixPrecision precision;
 for (const auto device : tensor_fixture::available_devices())
  for (const auto lhs_type : {torch::kFloat16, torch::kBFloat16, torch::kFloat32, torch::kFloat64})
   for (const auto rhs_type : {torch::kFloat32, torch::kFloat64}) {
    const auto lhs_options = torch::TensorOptions().dtype(lhs_type).device(device), rhs_options = torch::TensorOptions().dtype(rhs_type).device(device);
    // Areas straddle the exact 1e-7 denominator clamp, including touching and
    // disjoint/degenerate extents. Casting precedes each literal reference op.
    auto a = torch::tensor({{0., 0., 0., 1.}, {0., 0., 0.999e-7, 1.}, {0., 0., 1e-7, 1.}, {0., 0., 1.001e-7, 1.}, {2., 0., 3., 1.}}, lhs_options).set_requires_grad(true);
    auto b = torch::tensor({{0., 0., 0., 1.}, {0., 0., 0.999e-7, 1.}, {0., 0., 1e-7, 1.}, {0., 0., 1.001e-7, 1.}, {3., 0., 4., 1.}}, rhs_options).set_requires_grad(true);
    for (bool generalized : {false, true}) {
     CAPTURE(device, lhs_type, rhs_type, generalized);
     const auto actual = generalized ? rf::pairwise_generalized_box_iou(a, b) : rf::pairwise_box_iou(a, b);
     auto ra = a.detach().clone().set_requires_grad(true), rb = b.detach().clone().set_requires_grad(true);
     const auto reference = reference_giou(ra, rb, generalized, true);
     REQUIRE(torch::allclose(actual, reference, 1e-5, 1e-6));
     const auto aligned = generalized ? rf::aligned_generalized_box_iou(a, b) : rf::aligned_box_iou(a, b);
     REQUIRE(torch::allclose(aligned, reference.diag(), 1e-5, 1e-6));
     const auto gradients = torch::autograd::grad({actual.sum() + aligned.sum()}, {a, b});
     // Aligned and pairwise routes are separate forward graphs upstream too;
     // combining their cotangents before a Half cast changes VJP rounding.
     const auto expected = torch::autograd::grad({reference.sum() + reference_giou(ra, rb, generalized).sum()}, {ra, rb});
     // Half derivatives at 1e-7 may overflow in both implementations. This is
     // equality of the supported dtype expression, not a finite-half claim.
     for (int i = 0; i < 2; ++i) {
      CAPTURE(i, gradients[i], expected[i]);
      REQUIRE(torch::allclose(gradients[i], expected[i], 1e-4, 1e-5, true));
     }
     const auto empty = generalized ? rf::pairwise_generalized_box_iou(a.narrow(0, 0, 0), b) : rf::pairwise_box_iou(a.narrow(0, 0, 0), b);
     REQUIRE(empty.sizes() == torch::IntArrayRef({0, 5}));
     const auto empty_aligned = generalized ? rf::aligned_generalized_box_iou(a.narrow(0, 0, 0), b.narrow(0, 0, 0)) : rf::aligned_box_iou(a.narrow(0, 0, 0), b.narrow(0, 0, 0));
     REQUIRE(empty_aligned.numel() == 0);
     const auto fast = generalized ? rf::pairwise_generalized_box_iou(a.detach(), b.detach()) : rf::pairwise_box_iou(a.detach(), b.detach());
     REQUIRE(torch::allclose(fast, reference.detach(), 1e-5, 1e-6));
    }
   }
 for (const auto device : tensor_fixture::available_devices()) {
  auto boxes = torch::tensor({{0., 0., 0., 0.}, {0., 0., 2e-4, 2e-4}}, torch::TensorOptions().dtype(torch::kFloat64).device(device)).set_requires_grad(true);
  auto reference = boxes.detach().clone().set_requires_grad(true);
  const auto target = torch::tensor({{0., 0., 0., 0.}, {0., 0., 3e-4, 3e-4}}, boxes.options());
  (1 - rf::aligned_generalized_box_iou(rf::box_cxcywh_to_xyxy(boxes), rf::box_cxcywh_to_xyxy(target))).sum().backward();
  (1 - reference_giou(reference_corners(reference), reference_corners(target))).sum().backward();
  const auto expected = reference.detach() - 0.0001 * reference.grad() / (reference.grad().abs() + 1e-8);
  torch::optim::AdamW optimizer({boxes}, torch::optim::AdamWOptions(0.0001).weight_decay(0));
  optimizer.step();
  REQUIRE(torch::isfinite(boxes).all().item<bool>());
  REQUIRE(torch::allclose(boxes, expected, 1e-10, 1e-12));
 }
}
TEST_CASE("Explicit criterion samples reject incompatible invocation metadata", "[rfdetr][parity]") {
 rf::ModelOutputs output;
 output.main.pred_logits = torch::zeros({1, 1, 1});
 output.main.pred_boxes = torch::ones({1, 1, 4});
 output.main.pred_masks = torch::zeros({1, 1, 4, 4});
 auto gt = targets(torch::ones({1, 4}), torch::zeros({1}, torch::kInt64));
 gt.packed_masks = rf::PackedTargetMasks{torch::zeros({1, 1}, torch::kInt64), 4, 4};
 rf::DetectionConfig config;
 config.include_masks = true;
 config.num_classes = 1;
 config.mask_point_sample_ratio = 4;
 std::array<rf::LayerMaskSamples, 2> excessive;
 REQUIRE_THROWS_AS(rf::detection_loss_dict(output, gt, config, true, torch::tensor(1.F), excessive), std::invalid_argument);
 for (int fault : {0, 1}) {
  rf::LayerMaskSamples samples;
  samples.matcher = fault == 0 ? torch::zeros({1, 3, 2}) : torch::zeros({1, 4, 2}, torch::kFloat64);
  REQUIRE_THROWS_AS(rf::matcher_indices(output, gt, config, true, samples), std::invalid_argument);
 }
}
TEST_CASE("DN masks use stock direct equations for fixed samples gradients and updates", "[rfdetr][parity][training_supervision]") {
 tensor_fixture::FullMatrixPrecision precision;
 for (const auto& [device, amp] : tensor_fixture::available_amp_precisions()) {
  for (const int64_t population : {1, 3, 5})
   for (const int groups : {1, 2})
    for (const int mode : {0, 1, 2, 3}) {
     const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat32);
     const auto integers = options.dtype(torch::kInt64);
     rf::NativeRfDetrConfig config;
     config.num_classes = 2;
     config.num_queries = 3;
     config.hidden_dim = 4;
     config.group_detr = 1;
     config.segmentation = true;
     config.aux_loss = true;
     config.dec_layers = 2;
     config.two_stage = true;
     config.training_supervision.denoising.enabled = true;
     config.training_supervision.denoising.groups = static_cast<std::uint16_t>(groups);
     config.mask_ce_loss_coef = mode & 1 ? 1.7 : 0.0;
     config.mask_dice_loss_coef = mode & 2 ? 2.3 : 0.0;
     config.mask_point_sample_ratio = population == 1 ? 32 : 2;
     rf::TrainingSupervisionImpl owner(config, 1);
     owner.initialize(173);
     owner.to(device);
     auto gt = targets(torch::full({population + 1, 4}, 0.2F, options), torch::zeros({population + 1}, integers));
     gt.counts = {population, 0, 1};
     gt.offsets = {0, population, population};
     gt.targets.clear();
     gt.target_counts = torch::tensor(gt.counts, integers);
     gt.target_offsets = torch::tensor(gt.offsets, integers);
     auto words = torch::zeros({population + 1, 1}, torch::kInt64);
     auto dense_targets = torch::zeros({population + 1, 1, 4, 4});
     for (int64_t row = 0; row <= population; ++row)
      if (row % 2 == 0) {
       words[row][0] = int64_t{0x5a5a};
       dense_targets[row][0] = torch::tensor({{0.F, 1.F, 0.F, 1.F}, {1.F, 0.F, 1.F, 0.F}, {0.F, 1.F, 0.F, 1.F}, {1.F, 0.F, 1.F, 0.F}});
      }
     gt.packed_masks = rf::PackedTargetMasks{words.to(device), 4, 4};
     dense_targets = dense_targets.to(device);
     const auto prepared = owner.prepare_denoising(gt, {173, 2, 1, 7}, device, torch::kFloat32);
     REQUIRE(prepared);
     const auto make_output = [&] {
      rf::ModelOutputs result;
      rf::DenoisingOutputs dn;
      dn.original_labels = prepared->original_labels;
      dn.original_boxes = prepared->original_boxes;
      dn.valid_slots = prepared->valid_slots;
      dn.target_indices = prepared->target_indices;
      dn.groups = groups;
      dn.queries_per_group = population;
      dn.mask_sampling_seed = prepared->mask_sampling_seed;
      const auto layer = [&](float shift) {
       rf::DenoisingOutputLayer value;
       value.pred_logits = torch::full({3, groups, population, 2}, shift, options).set_requires_grad(true);
       value.pred_boxes = (prepared->original_boxes + shift * 0.1).detach().set_requires_grad(true);
       value.sparse_pred_masks = rf::SparsePredMasks{(torch::arange(96, options).reshape({3, 2, 4, 4}) * 0.037 - 1.1 + shift).set_requires_grad(true),
        (torch::arange(3 * groups * population * 2, options).reshape({3, groups * population, 2}) * 0.043 - 0.7).set_requires_grad(true), torch::full({1}, 0.173, options).set_requires_grad(true)};
       return value;
      };
      dn.main = layer(0.31F);
      dn.aux_outputs.push_back(layer(0.47F));
      result.denoising = std::move(dn);
      result.enc_outputs = rf::OutputLayer{};
      result.enc_outputs->pred_masks = torch::ones({3, 3, 4, 4}, options).set_requires_grad(true);
      return result;
     };
     auto actual = make_output(), reference = make_output();
     const int64_t rows = (population + 1) * groups;
     const int64_t points = std::max<int64_t>(4, 16 / config.mask_point_sample_ratio);
     const int64_t important = static_cast<int64_t>(0.75 * static_cast<double>(points));
     const auto coordinates = [&](int64_t count, float shift) { return (torch::arange(rows * count * 2, options) * 0.137 + shift).remainder(1.2).sub(0.1).reshape({rows, count, 2}); };
     const std::array<rf::LayerMaskSamples, 2> samples{
      {{{}, coordinates(points * 3, 0.03F), coordinates(points - important, 0.21F)}, {{}, coordinates(points * 3, 0.17F), coordinates(points - important, 0.41F)}}};
     const auto count = torch::full({}, population + 1, options);
     rf::TrainingLoss loss;
     torch::Tensor expected = torch::zeros({}, options), expected_ce = torch::zeros({}, options), expected_dice = torch::zeros({}, options);
     {
      mmltk::backend::ml::cuda::TorchAutocastScope scope(amp != torch::kFloat32, amp);
      loss = owner.loss(actual, gt, {count}, true, samples);
      const std::array<const rf::DenoisingOutputLayer*, 2> layers{&reference.denoising->main, &reference.denoising->aux_outputs.front()};
      for (size_t index = 0; index < layers.size(); ++index) {
       const auto& layer = *layers[index];
       {
        mmltk::backend::ml::cuda::TorchAutocastScope fp32(false, torch::kFloat32);
        const auto probabilities = layer.pred_logits.sigmoid();
        const auto labels = torch::tensor({1.F, 0.F}, options);
        const auto classes = labels * config.focal_alpha * (1 - probabilities).square() * torch::softplus(-layer.pred_logits) +
                             (1 - labels) * (1 - config.focal_alpha) * probabilities.square() * torch::softplus(layer.pred_logits);
        const auto valid = prepared->valid_slots;
        const auto class_rows = torch::where(valid, classes.sum(-1), torch::zeros_like(valid, options));
        const auto box_rows = torch::where(valid, (layer.pred_boxes - prepared->original_boxes).abs().sum(-1), torch::zeros_like(valid, options));
        const auto giou_rows = torch::where(valid, 1 - reference_giou(reference_corners(layer.pred_boxes), reference_corners(prepared->original_boxes)), torch::zeros_like(valid, options));
        expected = expected + (config.cls_loss_coef * class_rows.sum() + config.bbox_loss_coef * box_rows.sum() + config.giou_loss_coef * giou_rows.sum()) / (count * groups);
       }
       const auto& sparse = *layer.sparse_pred_masks;
       std::vector<torch::Tensor> selected, selected_targets;
       for (int64_t image = 0; image < 3; ++image)
        for (int64_t group = 0; group < groups; ++group) {
         const auto n = gt.counts[image];
         if (n == 0) continue;
         const auto query = sparse.query_features[image].narrow(0, group * population, n);
         selected.push_back(torch::matmul(query, sparse.spatial_features[image].flatten(1)).reshape({n, 1, 4, 4}) + sparse.bias);
         selected_targets.push_back(dense_targets.narrow(0, gt.offsets[image], n));
        }
       auto masks = torch::cat(selected), labels = torch::cat(selected_targets);
       const auto uncertainty = -reference_mask_sample(masks.detach(), samples[index].uncertain_candidates, false).abs();
       const auto top = std::get<1>(uncertainty.topk(important, 1));
       const auto coords = torch::cat({samples[index].uncertain_candidates.gather(1, top.unsqueeze(-1).expand({rows, important, 2})), samples[index].uncertain_random}, 1);
       const auto logits = reference_mask_sample(masks, coords, false), truth = reference_mask_sample(labels, coords, true);
       const auto ce = (truth * torch::softplus(-logits) + (1 - truth) * torch::softplus(logits)).mean(1).sum() / (count * groups);
       const auto probabilities = logits.sigmoid();
       const auto dice = (1 - (2 * (probabilities * truth).sum(1) + 1) / (probabilities.sum(1) + truth.sum(1) + 1)).sum() / (count * groups);
       const auto zero = (sparse.spatial_features.sum() + sparse.query_features.sum() + sparse.bias.sum()) * 0.0;
       expected_ce = expected_ce + (config.mask_ce_loss_coef == 0 ? zero : config.mask_ce_loss_coef * ce);
       expected_dice = expected_dice + (config.mask_dice_loss_coef == 0 ? zero : config.mask_dice_loss_coef * dice);
      }
      expected = expected + expected_ce + expected_dice;
     }
     const double tolerance = amp == torch::kFloat32 ? 2e-5 : 3e-2;
     REQUIRE(torch::allclose(loss.mask_ce, expected_ce, tolerance, tolerance));
     REQUIRE(torch::allclose(loss.mask_dice, expected_dice, tolerance, tolerance));
     REQUIRE(torch::allclose(loss.total, expected, tolerance, tolerance));
     REQUIRE(torch::equal(loss.total, loss.denoising));
     loss.total.backward();
     expected.backward();
     REQUIRE_FALSE(actual.enc_outputs->pred_masks->grad().defined());
     const auto check_layer = [&](const rf::DenoisingOutputLayer& a, const rf::DenoisingOutputLayer& b) {
      const std::array left{a.pred_logits, a.pred_boxes, a.sparse_pred_masks->spatial_features, a.sparse_pred_masks->query_features, a.sparse_pred_masks->bias};
      const std::array right{b.pred_logits, b.pred_boxes, b.sparse_pred_masks->spatial_features, b.sparse_pred_masks->query_features, b.sparse_pred_masks->bias};
      for (size_t i = 0; i < left.size(); ++i) {
       REQUIRE(left[i].grad().defined());
       REQUIRE(torch::isfinite(left[i].grad()).all().item<bool>());
       REQUIRE(torch::allclose(left[i].grad(), right[i].grad(), tolerance, tolerance));
       torch::NoGradGuard no_grad;
       REQUIRE(torch::allclose(left[i] - 0.01 * left[i].grad(), right[i] - 0.01 * right[i].grad(), tolerance, tolerance));
      }
      const auto query_gradient = a.sparse_pred_masks->query_features.grad().view({3, groups, population, 2});
      REQUIRE(query_gradient[1].count_nonzero().item<int64_t>() == 0);
      if (population > 1) REQUIRE(query_gradient[2].narrow(1, 1, population - 1).count_nonzero().item<int64_t>() == 0);
     };
     check_layer(actual.denoising->main, reference.denoising->main);
     check_layer(actual.denoising->aux_outputs.front(), reference.denoising->aux_outputs.front());
     auto missing = gt;
     missing.packed_masks.reset();
     if (mode == 0) {
      auto disabled = make_output();
      const auto without_masks = owner.loss(disabled, missing, {count}, true);
      REQUIRE(torch::equal(without_masks.total, without_masks.classification + without_masks.box + without_masks.giou));
      REQUIRE(torch::allclose(without_masks.total, loss.total, tolerance, tolerance));
      without_masks.total.backward();
      for (const auto* layer : {&disabled.denoising->main, &disabled.denoising->aux_outputs.front()}) {
       const auto& masks = *layer->sparse_pred_masks;
       mmltk::backend::ml::testsupport::require_zero_gradients({masks.spatial_features, masks.query_features, masks.bias});
      }
     } else
      REQUIRE_THROWS(owner.loss(actual, missing, {count}, true));
    }
 }
}
