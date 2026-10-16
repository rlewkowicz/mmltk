#pragma once
#include <c10/cuda/CUDAGuard.h>
#include <torch/types.h>
#include <c10/cuda/CUDAStream.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <meta>
#include <type_traits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <memory>
#include "src/backend/data/catalog/class_catalog.h"
#include "src/backend/models/rfdetr/contract/output_roles.h"
#include "reflected_tensor_packet.h"
#include "criterion_terms.h"
namespace mmltk::backend::models::rfdetr {
using TensorMap = std::map<std::string, torch::Tensor>;
struct PreparedTarget {
 torch::Tensor image_id;
 torch::Tensor orig_size;
 torch::Tensor size;
 torch::Tensor boxes;
 torch::Tensor labels;
 torch::Tensor area;
 torch::Tensor iscrowd;
 // Normalized x,y plus source annotation visibility (0/1/2); padded rows are zero.
 torch::Tensor keypoints;
 torch::Tensor keypoint_counts;
};
struct PackedTargetMasks {
 static constexpr bool tensor_packet = true;
 PackedTargetMasks() = default;
 PackedTargetMasks(torch::Tensor packed_bits, int64_t mask_height, int64_t mask_width, torch::Tensor transforms = {}, torch::Tensor occluder_indices = {}, torch::Tensor occluder_transforms = {},
  torch::Tensor spatial_erasure = {})
     : bits(std::move(packed_bits)),
       height(mask_height),
       width(mask_width),
       inverse_transforms(std::move(transforms)),
       occluder_mask_indices(std::move(occluder_indices)),
       occluder_inverse_transforms(std::move(occluder_transforms)),
       erasure(std::move(spatial_erasure)) {}
 torch::Tensor bits;
 int64_t height = 0;
 int64_t width = 0;
 torch::Tensor inverse_transforms;
 torch::Tensor occluder_mask_indices;
 torch::Tensor occluder_inverse_transforms;
 // Contiguous canonical AugmentationSpatialErasure object bytes, one per target.
 torch::Tensor erasure;
};
struct PreparedTargets {
 torch::Tensor sampling_keys;
 std::uint64_t microbatch_key = 0;
 std::vector<PreparedTarget> targets;
 torch::Tensor all_image_ids;
 torch::Tensor orig_sizes;
 torch::Tensor nested_mask;
 torch::Tensor all_boxes;
 torch::Tensor all_labels;
 torch::Tensor all_area;
 torch::Tensor all_iscrowd;
 torch::Tensor all_keypoints;
 torch::Tensor all_keypoint_counts;
 std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> class_catalog;
 torch::Tensor target_offsets;
 torch::Tensor target_counts;
 torch::Tensor target_indices;
 std::optional<PackedTargetMasks> packed_masks;
 std::vector<int64_t> offsets;
 std::vector<int64_t> counts;
 std::string_view split;
 int64_t resolved_query_count = 0;
 void record_stream(const c10::cuda::CUDAStream& stream) const {
  const auto record = [&stream](const torch::Tensor& tensor) {
   if (tensor.defined() && tensor.device().is_cuda()) { tensor.record_stream(stream); }
  };
  // Per-image targets are aliases of the aggregate tensors, already recorded.
  visit_tensor_packet<false>(*this, record);
 }
};
struct SupervisedQueryLayout {
 int64_t groups = 1;
 int64_t queries_per_group = 0;
 [[nodiscard]] int64_t total_queries() const noexcept { return groups * queries_per_group; }
};
struct DecoderQueryLayout {
 SupervisedQueryLayout ordinary;
 int64_t denoising_groups = 0;
 int64_t denoising_queries_per_group = 0;
 torch::Tensor denoising_key_padding;
 torch::Tensor denoising_valid_slots;
 [[nodiscard]] bool has_denoising() const noexcept { return denoising_groups > 0 && denoising_queries_per_group > 0; }
 [[nodiscard]] int64_t denoising_queries() const noexcept { return denoising_groups * denoising_queries_per_group; }
 [[nodiscard]] int64_t total_queries() const noexcept { return ordinary.total_queries() + denoising_queries(); }
};
struct SparsePredMasks {
 torch::Tensor spatial_features;
 torch::Tensor query_features;
 torch::Tensor bias;
};
struct DenoisingQueryBatch {
 torch::Tensor content;
 torch::Tensor normalized_references;
 torch::Tensor original_labels;
 torch::Tensor original_boxes;
 torch::Tensor valid_slots;
 torch::Tensor target_indices;
 DecoderQueryLayout layout;
 std::uint64_t mask_sampling_seed = 0;
};
struct DenoisingOutputLayer {
 torch::Tensor pred_logits;
 torch::Tensor pred_boxes;
 std::optional<SparsePredMasks> sparse_pred_masks{};
};
struct DenoisingOutputs {
 DenoisingOutputLayer main;
 std::vector<DenoisingOutputLayer> aux_outputs;
 torch::Tensor original_labels;
 torch::Tensor original_boxes;
 torch::Tensor valid_slots;
 torch::Tensor target_indices;
 int64_t groups = 0;
 int64_t queries_per_group = 0;
 std::uint64_t mask_sampling_seed = 0;
};
struct OutputLayer {
 torch::Tensor pred_logits;
 torch::Tensor pred_boxes;
 std::optional<torch::Tensor> query_features;
 std::optional<SupervisedQueryLayout> query_layout;
 std::optional<torch::Tensor> pred_masks;
 // [batch, queries, KeypointTensorLayout::padded_count(), KeypointPredictionPacket::size].
 // keypoint_layout.h owns physical schema slots; keypoint_prediction.h owns channels.
 std::optional<torch::Tensor> pred_keypoints;
 std::optional<SparsePredMasks> sparse_pred_masks;
 template <class Self, class Visitor>
 void for_each_required_tensor(this Self&& self, Visitor&& visit) {
  template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^OutputLayer, std::meta::access_context::current()))) {
   if constexpr (std::is_same_v<std::remove_cvref_t<decltype(self.[:member:])>, torch::Tensor>) visit(self.[:member:]);
  }
 }
};
struct ModelOutputs {
 OutputLayer main;
 std::vector<OutputLayer> aux_outputs;
 std::optional<OutputLayer> enc_outputs;
 std::optional<DenoisingOutputs> denoising;
};
struct DeviceLossNormalizer {
 torch::Tensor target_count;
};
struct SupervisionTimingSetup {
 torch::Device device = torch::kCPU;
 std::size_t maximum_accumulated_losses = 1;
 bool profiling_enabled = false;
};
struct SupervisionTimingHandoff {
 std::size_t completed_leases = 0;
 std::size_t outstanding_leases = 0;
};
struct TrainingStepIdentity {
 std::uint64_t seed = 0;
 std::uint64_t epoch = 0;
 std::uint32_t rank = 0;
 std::uint64_t batch_sequence = 0;
};
struct TrainingLossComponents {
 torch::Tensor classification;
 torch::Tensor box;
 torch::Tensor giou;
 torch::Tensor mask_ce{};
 torch::Tensor mask_dice{};
};
struct TrainingLoss {
 torch::Tensor total;
 torch::Tensor classification;
 torch::Tensor box;
 torch::Tensor giou;
 torch::Tensor correspondence;
 torch::Tensor denoising;
 torch::Tensor auxiliary;
 TrainingLossComponents main;
 torch::Tensor mask_ce{};
 torch::Tensor mask_dice{};
};
struct OutputTensors {
 torch::Tensor pred_logits;
 torch::Tensor pred_boxes;
 std::optional<torch::Tensor> pred_masks{};
 std::optional<torch::Tensor> pred_keypoints{};
 std::span<const std::uint16_t> keypoints_per_class{};
};
namespace detail {
template <class Owner>
consteval auto output_role_members() {
 std::array<std::meta::info, kRfdetrExecutableOutputCount> result{};
 RfdetrOutputSchema::visit([&]<RfdetrOutputRole Role>(RfdetrOutputPolicy facts) {
  template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^Owner, std::meta::access_context::current()))) {
   if (std::meta::identifier_of(member) == facts.name) {
    using Tensor = typename[:std::meta::type_of(member):];
    if (facts.task ? !std::is_same_v<Tensor, std::optional<torch::Tensor>> : !std::is_same_v<Tensor, torch::Tensor>) throw "RF-DETR output role has the wrong required/optional tensor type";
    result[RfdetrOutputSchema::index<Role>()] = member;
    return;
   }
  }
  throw "RF-DETR output role has no matching tensor member";
 });
 return result;
}
// Materialize both structural boundaries here; consumers do not reflect their
// own tensor inventories. Tensor handles and their storage stay with the caller.
inline constexpr auto kOutputLayerRoleMembers = output_role_members<OutputLayer>();
inline constexpr auto kOutputTensorRoleMembers = output_role_members<OutputTensors>();
}  // namespace detail
struct RfdetrOutputProjection final {
 template <class Self, class Visitor>
 static void visit(Self&& output, Visitor&& visitor) {
  using Owner = std::remove_cvref_t<Self>;
  static_assert(std::is_same_v<Owner, OutputLayer> || std::is_same_v<Owner, OutputTensors>);
  RfdetrOutputSchema::visit([&]<RfdetrOutputRole Role>(RfdetrOutputPolicy facts) {
   constexpr auto index = RfdetrOutputSchema::index<Role>();
   constexpr auto member = std::is_same_v<Owner, OutputLayer> ? detail::kOutputLayerRoleMembers[index] : detail::kOutputTensorRoleMembers[index];
   visitor.template operator()<Role>(facts, output.[:member:]);
  });
 }
};
void assert_inference_output_dtype(const OutputLayer&, at::ScalarType expected_dtype, const char* context);
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
 std::vector<std::uint16_t> keypoints_per_class;
 double keypoint_l1_loss_coef = 0.0;
 double keypoint_findable_loss_coef = 0.0;
 double keypoint_visible_loss_coef = 0.0;
 double keypoint_nll_loss_coef = 0.0;
 bool use_jit_traced_loss_ops = false;
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
 std::vector<CriterionWeight> weights;
};
void populate_default_detection_weights(DetectionConfig& config);
}  // namespace mmltk::backend::models::rfdetr
