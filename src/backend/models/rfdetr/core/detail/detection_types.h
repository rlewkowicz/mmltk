#pragma once
#include <torch/torch.h>
#include <c10/cuda/CUDAStream.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "torch_api.h"
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
};
struct PackedTargetMasks {
    PackedTargetMasks() = default;
    PackedTargetMasks(torch::Tensor packed_bits, int64_t mask_height, int64_t mask_width, torch::Tensor transforms = {}, torch::Tensor occluder_indices = {},
                      torch::Tensor occluder_transforms = {}, torch::Tensor spatial_erasure = {})
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
    std::vector<PreparedTarget> targets;
    torch::Tensor all_image_ids;
    torch::Tensor orig_sizes;
    torch::Tensor nested_mask;
    torch::Tensor all_boxes;
    torch::Tensor all_labels;
    torch::Tensor all_area;
    torch::Tensor all_iscrowd;
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
        record(all_image_ids);
        record(orig_sizes);
        record(nested_mask);
        record(all_boxes);
        record(all_labels);
        record(all_area);
        record(all_iscrowd);
        record(target_offsets);
        record(target_counts);
        record(target_indices);
        if (packed_masks) {
            record(packed_masks->bits);
            record(packed_masks->inverse_transforms);
            record(packed_masks->occluder_mask_indices);
            record(packed_masks->occluder_inverse_transforms);
            record(packed_masks->erasure);
        }
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
struct DenoisingQueryBatch {
    torch::Tensor content;
    torch::Tensor normalized_references;
    torch::Tensor original_labels;
    torch::Tensor original_boxes;
    torch::Tensor valid_slots;
    torch::Tensor target_indices;
    DecoderQueryLayout layout;
};
struct DenoisingOutputLayer {
    torch::Tensor pred_logits;
    torch::Tensor pred_boxes;
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
};
struct OutputLayer {
    struct SparsePredMasks {
        torch::Tensor spatial_features;
        torch::Tensor query_features;
        torch::Tensor bias;
    };
    torch::Tensor pred_logits;
    torch::Tensor pred_boxes;
    std::optional<torch::Tensor> query_features;
    std::optional<SupervisedQueryLayout> query_layout;
    std::optional<torch::Tensor> pred_masks;
    std::optional<SparsePredMasks> sparse_pred_masks;
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
};
struct OutputTensors {
    torch::Tensor pred_logits;
    torch::Tensor pred_boxes;
    std::optional<torch::Tensor> pred_masks{};
};
void assert_inference_output_dtype(const torch::Tensor& pred_logits, const torch::Tensor& pred_boxes, at::ScalarType expected_dtype, const char* context);
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
    std::vector<std::pair<std::string, double>> weight_dict;
};
void populate_default_detection_weight_dict(DetectionConfig& config);
}  // namespace mmltk::backend::models::rfdetr
