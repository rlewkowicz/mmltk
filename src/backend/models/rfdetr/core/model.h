#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "model_state_load.h"
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
#include <torch/ordered_dict.h>
namespace mmltk::backend::models::rfdetr {
struct NestedTensor {
 torch::Tensor tensors;
 torch::Tensor mask;
 [[nodiscard]] NestedTensor to(const torch::Device& device) const;
 [[nodiscard]] NestedTensor pin_memory() const;
 [[nodiscard]] std::pair<torch::Tensor, torch::Tensor> decompose() const;
};
NestedTensor nested_tensor_from_tensor_list(const std::vector<torch::Tensor>& tensor_list);
class NativeRfDetrModel final {
public:
 explicit NativeRfDetrModel(const NativeRfDetrConfig& config = {}, ModelClassLayout layout = {});
 ~NativeRfDetrModel();
 NativeRfDetrModel(const NativeRfDetrModel&) = delete;
 NativeRfDetrModel& operator=(const NativeRfDetrModel&) = delete;
 NativeRfDetrModel(NativeRfDetrModel&&) noexcept;
 NativeRfDetrModel& operator=(NativeRfDetrModel&&) noexcept;
 [[nodiscard]] const NativeRfDetrConfig& config() const noexcept;
 [[nodiscard]] const std::shared_ptr<const ResolvedClassLayout>& class_layout() const noexcept;
 [[nodiscard]] bool is_compiled(bool for_training) const noexcept;
 void optimize_for_inference(std::int32_t batch_size = 1, bool for_training = false, CompilationMode mode = CompilationMode::kSelective);
 [[nodiscard]] ModelOutputs forward(const NestedTensor& batch, bool include_masks);
 [[nodiscard]] ModelOutputs forward_for_match_free(const NestedTensor& batch);
 [[nodiscard]] ModelOutputs forward_with_denoising(const NestedTensor& batch, const PreparedTargets& targets, const TrainingStepIdentity& identity);
 void initialize_training_supervision(std::uint64_t request_seed);
 void replicate_training_supervision_runtime_from(const NativeRfDetrModel& source);
 [[nodiscard]] TrainingLoss supervision_loss(const ModelOutputs& outputs, const PreparedTargets& targets, const DeviceLossNormalizer& normalizer, bool training_mode);
 void configure_supervision_timing(const SupervisionTimingSetup& setup);
 void begin_supervised_step_timing();
 void end_supervised_step_timing();
 void begin_criterion_timing();
 void end_criterion_timing();
 [[nodiscard]] SupervisionTimingHandoff harvest_supervision_timing();
 [[nodiscard]] ModelStateLoadSummary load_normalized_state(const std::vector<NormalizedModelStateEntry>& state, bool strict, const ModelClassLayout* admitted_layout = nullptr);
 [[nodiscard]] detail::NormalizedModelStateCandidate stage_normalized_state(
  const std::vector<NormalizedModelStateEntry>& state, detail::NormalizedModelStateAdmission admission, const ResolvedClassLayout* source_layout = nullptr);
 void commit_normalized_state(detail::NormalizedModelStateCandidate candidate);
 void set_force_pytorch_deformable_attn(bool value);
 void train(bool enabled = true);
 void eval();
 [[nodiscard]] bool is_training() const noexcept;
 void to(const torch::Device& device);
 [[nodiscard]] std::vector<torch::Tensor> parameters(bool recurse = true) const;
 [[nodiscard]] torch::OrderedDict<std::string, torch::Tensor> named_parameters(bool recurse = true) const;
 [[nodiscard]] torch::OrderedDict<std::string, torch::Tensor> named_buffers(bool recurse = true) const;

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::models::rfdetr
