#pragma once
#include "src/backend/models/rfdetr/core/detail/class_tensor_axes.h"
#include <torch/torch.h>
#include <string>
#include <utility>
#include <vector>
#include "detection_types.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
namespace mmltk::backend::models::rfdetr {
struct NormalizedModelStateEntry {
    std::string name;
    torch::Tensor tensor;
};
struct ModelStateLoadSummary {
    std::vector<std::string> loaded_names;
    std::vector<std::string> missing_names;
    std::vector<std::string> unexpected_names;
    std::vector<std::string> incompatible_names;
};
struct NestedTensor {
    torch::Tensor tensors;
    torch::Tensor mask;
    [[nodiscard]] NestedTensor to(const torch::Device& device) const;
    [[nodiscard]] NestedTensor pin_memory() const;
    [[nodiscard]] std::pair<torch::Tensor, torch::Tensor> decompose() const;
};
NestedTensor nested_tensor_from_tensor_list(const std::vector<torch::Tensor>& tensor_list);
namespace detail {
struct NormalizedModelStateCandidate {
    ModelStateLoadSummary summary;
    std::vector<torch::Tensor> destinations;
    std::vector<torch::Tensor> tensors;
};
enum class NormalizedModelStateAdmission {
    Exact,
    PartialExact,
    FreshTransfer,
    PartialFreshTransfer,
};
class NativeModelTechnicalOwner {
   public:
    virtual ~NativeModelTechnicalOwner() = default;
    [[nodiscard]] virtual torch::nn::Module& module() noexcept = 0;
    [[nodiscard]] virtual const torch::nn::Module& module() const noexcept = 0;
    [[nodiscard]] virtual ModelOutputs forward(const NestedTensor& batch, bool include_masks) = 0;
    [[nodiscard]] virtual ModelOutputs forward_for_match_free(const NestedTensor& batch) = 0;
    [[nodiscard]] virtual ModelOutputs forward_with_denoising(const NestedTensor& batch, const PreparedTargets& targets,
                                                              const TrainingStepIdentity& identity) = 0;
    virtual void initialize_training_supervision(std::uint64_t request_seed) = 0;
    void replicate_training_supervision_runtime_from(const NativeModelTechnicalOwner& source) {
        import_training_supervision_runtime(source.export_training_supervision_runtime());
    }
    [[nodiscard]] virtual TrainingLoss supervision_loss(const ModelOutputs& outputs, const PreparedTargets& targets, const DeviceLossNormalizer& normalizer,
                                                        bool training_mode) = 0;
    virtual void configure_supervision_timing(const SupervisionTimingSetup& setup) = 0;
    virtual void begin_supervised_step_timing() = 0;
    virtual void end_supervised_step_timing() = 0;
    virtual void begin_criterion_timing() = 0;
    virtual void end_criterion_timing() = 0;
    [[nodiscard]] virtual SupervisionTimingHandoff harvest_supervision_timing() = 0;
    [[nodiscard]] virtual ModelStateLoadSummary load_normalized_state(const std::vector<NormalizedModelStateEntry>& state, bool strict,
                                                                      const ModelClassLayout* admitted_layout = nullptr) = 0;
    [[nodiscard]] virtual NormalizedModelStateCandidate stage_normalized_state(const std::vector<NormalizedModelStateEntry>& state,
                                                                               NormalizedModelStateAdmission admission,
                                                                               const ResolvedClassLayout* source_layout = nullptr) = 0;
    virtual void commit_normalized_state(NormalizedModelStateCandidate candidate) = 0;
    virtual void set_force_pytorch_deformable_attn(bool value) = 0;

   protected:
    struct TrainingSupervisionRuntimeState {
        TrainingSupervisionConfig config;
        bool has_owner = false;
        bool initialized = false;
    };
    [[nodiscard]] virtual TrainingSupervisionRuntimeState export_training_supervision_runtime() const = 0;
    virtual void import_training_supervision_runtime(const TrainingSupervisionRuntimeState& state) = 0;
};
}  // namespace detail
}  // namespace mmltk::backend::models::rfdetr
