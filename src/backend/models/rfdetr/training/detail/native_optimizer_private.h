#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <unordered_map>
#include "src/backend/ml/torch/detail/torch_api.h"
#include "src/backend/ml/cuda/tensor_readback.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
namespace mmltk::backend::models::rfdetr {
namespace torch_api = mmltk::backend::ml::torch_api;
enum class NativeOptimizerBackend : std::uint8_t {
    eager,
    foreach,
    fused,
};
struct NativeAdamWGroupConfig {
    double lr = 0.0;
    double weight_decay = 0.0;
    bool amsgrad = false;
};
struct NativeMuonGroupConfig {
    double lr = 0.0;
    double weight_decay = 0.0;
    double momentum = 0.95;
    bool use_muon = false;
    bool nesterov = true;
};
struct NativeAdamWParamState {
    torch_api::Tensor step;
    torch_api::Tensor exp_avg;
    torch_api::Tensor exp_avg_sq;
    torch_api::Tensor max_exp_avg_sq;
};
struct NativeMuonParamState {
    int64_t step = 0;
    torch_api::Tensor momentum_buffer;
    torch_api::Tensor exp_avg;
    torch_api::Tensor exp_avg_sq;
};
// Storage shared by the native optimizers: parameter groups indexing into a named parameter list, per-parameter
// optimizer state, and the flattened tensor/name views handed to schedulers and checkpointing.
// CLEANUP-IGNORE -- optimizer-specific group and state types share one storage ownership implementation.
template <typename GroupConfig, typename ParamStateT>
class NativeOptimizerStorage {
   public:
    struct Group {
        GroupConfig config;
        std::vector<size_t> param_indices;
    };
    struct NamedParameter {
        std::string name;
        torch_api::Tensor tensor;
    };
    std::vector<torch_api::Tensor>& parameters() { return all_params_; }
    [[nodiscard]] const std::vector<torch_api::Tensor>& parameters() const { return all_params_; }
    [[nodiscard]] const std::vector<std::string>& parameter_names() const { return all_param_names_; }
    [[nodiscard]] const std::vector<Group>& groups() const { return groups_; }

   protected:
    using ParamState = ParamStateT;
    NativeOptimizerStorage() = default;
    NativeOptimizerStorage(std::vector<Group> groups, std::vector<NamedParameter> params) : groups_(std::move(groups)), params_(std::move(params)) {}
    std::vector<Group> groups_;
    std::vector<NamedParameter> params_;
    std::vector<ParamState> state_;
    std::vector<torch_api::Tensor> all_params_;
    std::vector<std::string> all_param_names_;
};
class NativeAdamW : public NativeOptimizerStorage<NativeAdamWGroupConfig, NativeAdamWParamState> {
   public:
    [[nodiscard]] static std::vector<std::string> InspectCheckpoint(torch_api::InputArchive&, const std::unordered_map<std::string, torch_api::Tensor>&);
    NativeAdamW() = default;
    NativeAdamW(std::vector<Group> groups, std::vector<NamedParameter> params, NativeOptimizerBackend backend);
    [[nodiscard]] NativeOptimizerBackend backend() const;
    [[nodiscard]] const char* backend_name() const;
    void zero_grad(bool set_to_none);
    void set_lrs(const std::vector<double>& base_lrs, double scale);
    void step();
    void reserve_checkpoint(mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const;
    void save(torch_api::OutputArchive& archive, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const;
    void load(torch_api::InputArchive& archive);
    void commit(NativeAdamW candidate) noexcept;

   private:
    void initialize_state();
    void step_group_eager(const Group& group);
    void step_group_foreach(const Group& group);
    void step_group_fused(const Group& group);
    NativeOptimizerBackend backend_ = NativeOptimizerBackend::eager;
};
bool native_optimizer_supports_foreach(const std::vector<torch_api::Tensor>& params);
bool native_optimizer_supports_fused(const std::vector<torch_api::Tensor>& params);
const char* native_optimizer_backend_name(NativeOptimizerBackend backend);
[[nodiscard]] bool muon_parameter_eligible(std::string_view name, const torch_api::Tensor& parameter);
// Single source of truth for the optimizer operations that `NativeOptimizer` forwards to the
// concrete optimizer it holds. Declaring the surface from one list keeps the facade and the
// implementation from drifting apart. Rows are (name, parameter list, cv-qualifier).
#define MMLTK_NATIVE_OPTIMIZER_OPERATIONS(X)                                                                                                 \
    X(zero_grad, (bool set_to_none), )                                                                                                       \
    X(set_lrs, (const std::vector<double>& base_lrs, double scale), )                                                                        \
    X(set_muon_momentum, (double momentum), )                                                                                                \
    X(step, (), )                                                                                                                            \
    X(reserve_checkpoint, (mmltk::backend::ml::cuda::TensorReadbackBuffers & readback, std::size_t first_slot), const)                       \
    X(save, (torch_api::OutputArchive & archive, mmltk::backend::ml::cuda::TensorReadbackBuffers & readback, std::size_t first_slot), const) \
    X(load, (torch_api::InputArchive & archive), )
#define MMLTK_DECLARE_NATIVE_OPTIMIZER_OPERATION(name, params, qualifier) void name params qualifier;
class NativeMuonWithAuxAdam : public NativeOptimizerStorage<NativeMuonGroupConfig, NativeMuonParamState> {
   public:
    [[nodiscard]] static std::vector<std::string> InspectCheckpoint(torch_api::InputArchive&, const std::unordered_map<std::string, torch_api::Tensor>&);
    NativeMuonWithAuxAdam() = default;
    NativeMuonWithAuxAdam(std::vector<Group> groups, std::vector<NamedParameter> params);
    [[nodiscard]] const char* backend_name() const;
    MMLTK_NATIVE_OPTIMIZER_OPERATIONS(MMLTK_DECLARE_NATIVE_OPTIMIZER_OPERATION)
    void commit(NativeMuonWithAuxAdam candidate) noexcept;

   private:
    void initialize_state();
};
class NativeOptimizer {
   public:
    NativeOptimizer() = default;
    explicit NativeOptimizer(NativeAdamW optimizer);
    explicit NativeOptimizer(NativeMuonWithAuxAdam optimizer);
    [[nodiscard]] TrainOptimizerKind kind() const;
    [[nodiscard]] std::string_view kind_name() const;
    [[nodiscard]] const char* backend_name() const;
    std::vector<torch_api::Tensor>& parameters();
    [[nodiscard]] const std::vector<torch_api::Tensor>& parameters() const;
    [[nodiscard]] const std::vector<std::string>& parameter_names() const;
    MMLTK_NATIVE_OPTIMIZER_OPERATIONS(MMLTK_DECLARE_NATIVE_OPTIMIZER_OPERATION)
    [[nodiscard]] NativeOptimizer stage_load(torch_api::InputArchive& archive) const;
    void commit(NativeOptimizer candidate) noexcept;

   private:
    std::variant<NativeAdamW, NativeMuonWithAuxAdam> storage_;
};
#undef MMLTK_DECLARE_NATIVE_OPTIMIZER_OPERATION
#undef MMLTK_NATIVE_OPTIMIZER_OPERATIONS
}  // namespace mmltk::backend::models::rfdetr
