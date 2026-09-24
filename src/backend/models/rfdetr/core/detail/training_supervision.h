#pragma once
#include <torch/torch.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/core/detail/class_tensor_axes.h"
#include "src/backend/models/rfdetr/contract/model_config.h"
namespace mmltk::backend::models::rfdetr {
struct PairwiseMaskSamples;
struct LayerMaskSamples;
inline constexpr float kSparseCorrespondenceEpsilon = 1.0e-8F;
struct MatchFreeCorrespondence {
 torch::Tensor dense;
 torch::Tensor sparse_normalized;
};
struct MatchFreeCost {
 torch::Tensor classification;
 torch::Tensor box;
 torch::Tensor giou;
 torch::Tensor total;
};
struct DenoisingVariates {
 torch::Tensor center;
 torch::Tensor size;
 torch::Tensor label_flip;
 torch::Tensor other_label;
};
struct DenoisingTransform {
 torch::Tensor labels;
 torch::Tensor boxes;
 torch::Tensor center_offsets;
 torch::Tensor extent_scales;
};
[[nodiscard]] torch::Tensor sparse_match_free_correspondence(const torch::Tensor& dense, const torch::Tensor& valid_rows, float rho);
[[nodiscard]] DenoisingTransform transform_denoising_targets(const torch::Tensor& original_labels, const torch::Tensor& original_boxes, const torch::Tensor& valid_slots, int64_t object_classes,
 const DenoisingSupervisionConfig& config, const DenoisingVariates& variates);
class TrainingSupervisionImpl final : public torch::nn::Module {
public:
 explicit TrainingSupervisionImpl(const NativeRfDetrConfig& config, std::int64_t foreground_count);
 void append_class_axes(std::vector<detail::ClassTensorAxis>& axes) const;
 ~TrainingSupervisionImpl() override;
 void initialize(std::uint64_t request_seed);
 void install_replicated_initialized_runtime(const TrainingSupervisionConfig& source_config);
 [[nodiscard]] bool initialized() const noexcept;
 [[nodiscard]] bool match_free_enabled() const noexcept;
 [[nodiscard]] bool denoising_enabled() const noexcept;
 [[nodiscard]] std::optional<DenoisingQueryBatch> prepare_denoising(
  const PreparedTargets& targets, const TrainingStepIdentity& identity, const torch::Device& device, c10::ScalarType decoder_dtype, const DenoisingVariates* injected_variates = nullptr);
 [[nodiscard]] MatchFreeCorrespondence correspondence(
  const torch::Tensor& padded_labels, const torch::Tensor& padded_boxes, const torch::Tensor& valid_rows, const torch::Tensor& query_features, const PairwiseMaskSamples* masks = nullptr);
 [[nodiscard]] MatchFreeCost broadcast_cost(const torch::Tensor& padded_labels, const torch::Tensor& padded_boxes, const torch::Tensor& valid_rows, const OutputLayer& layer) const;
 // Optional fixed DN samples follow main/auxiliary decoder order; omitted draws remain private.
 [[nodiscard]] TrainingLoss loss(
  const ModelOutputs& outputs, const PreparedTargets& targets, const DeviceLossNormalizer& normalizer, bool training_mode, std::span<const LayerMaskSamples> mask_samples = {});
 void configure_timing(const SupervisionTimingSetup& setup);
 void begin_supervised_step_timing();
 void end_supervised_step_timing();
 void begin_criterion_timing();
 void end_criterion_timing();
 void begin_denoising_decoder_timing();
 void end_denoising_decoder_timing();
 [[nodiscard]] SupervisionTimingHandoff harvest_timing();

private:
 struct PaddedTargets;
 struct DenoisingScratch;
 class ProbeMlpImpl;
 struct TimingState;
 [[nodiscard]] PaddedTargets pad_targets(const PreparedTargets& targets, const torch::Device& device, int64_t batch, bool reuse_construction_scratch);
 [[nodiscard]] torch::Tensor encode_ground_truth(const torch::Tensor& labels, const torch::Tensor& boxes);
 [[nodiscard]] torch::Tensor project_ground_truth(const torch::Tensor& encoding, const PairwiseMaskSamples* masks = nullptr);
 [[nodiscard]] torch::Tensor dense_correspondence(const torch::Tensor& probes, const torch::Tensor& query_features);
 [[nodiscard]] TrainingLoss empty_loss(const ModelOutputs& outputs) const;
 [[nodiscard]] TrainingLoss denoising_loss(
  const DenoisingOutputs& outputs, const PreparedTargets& targets, const DeviceLossNormalizer& normalizer, std::span<const LayerMaskSamples> mask_samples) const;
 NativeRfDetrConfig config_;
 std::int64_t foreground_count_;
 std::shared_ptr<ProbeMlpImpl> ground_truth_mlp_;
 std::shared_ptr<ProbeMlpImpl> query_mlp_;
 torch::nn::Linear query_projection_{nullptr};
 torch::nn::Linear key_projection_{nullptr};
 torch::nn::Linear mask_projection_{nullptr};
 torch::nn::Embedding denoising_label_embedding_{nullptr};
 torch::nn::Embedding denoising_task_embedding_{nullptr};
 std::optional<at::Generator> denoising_generator_;
 std::unique_ptr<DenoisingScratch> denoising_scratch_;
 std::unique_ptr<TimingState> timing_;
 bool initialized_ = false;
};
}  // namespace mmltk::backend::models::rfdetr
