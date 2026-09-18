#pragma once
#include <stop_token>
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <unordered_map>
#include <torch/types.h>
#include <torch/serialize.h>
#include "src/backend/ml/cuda/tensor_readback.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
namespace mmltk::backend::models::rfdetr {
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
    torch::Tensor step;
    torch::Tensor exp_avg;
    torch::Tensor exp_avg_sq;
    torch::Tensor max_exp_avg_sq;
};
struct NativeMuonParamState {
    int64_t step = 0;
    torch::Tensor momentum_buffer;
    torch::Tensor exp_avg;
    torch::Tensor exp_avg_sq;
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
        torch::Tensor tensor;
    };
    std::vector<torch::Tensor>& parameters() { return all_params_; }
    [[nodiscard]] const std::vector<torch::Tensor>& parameters() const { return all_params_; }
    [[nodiscard]] const std::vector<std::string>& parameter_names() const { return all_param_names_; }
    [[nodiscard]] const std::vector<Group>& groups() const { return groups_; }
    void reserve_checkpoint(mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const;
    void commit(NativeOptimizerStorage candidate) noexcept {
        groups_.swap(candidate.groups_);
        state_.swap(candidate.state_);
    }

   protected:
    using ParamState = ParamStateT;
    NativeOptimizerStorage() = default;
    NativeOptimizerStorage(std::vector<Group> groups, std::vector<NamedParameter> params) : groups_(std::move(groups)), params_(std::move(params)) {}
    template <class Optimizer>
    [[nodiscard]] static std::vector<std::string> inspect_checkpoint(torch::serialize::InputArchive&, const std::unordered_map<std::string, torch::Tensor>&,
                                                                     std::stop_token stop = {});
    std::vector<Group> groups_;
    std::vector<NamedParameter> params_;
    std::vector<ParamState> state_;
    std::vector<torch::Tensor> all_params_;
    std::vector<std::string> all_param_names_;
};
class NativeAdamW : public NativeOptimizerStorage<NativeAdamWGroupConfig, NativeAdamWParamState> {
   public:
    [[nodiscard]] static std::vector<std::string> InspectCheckpoint(torch::serialize::InputArchive&, const std::unordered_map<std::string, torch::Tensor>&,
                                                                    std::stop_token stop = {});
    NativeAdamW() = default;
    NativeAdamW(std::vector<Group> groups, std::vector<NamedParameter> params, NativeOptimizerBackend backend);
    [[nodiscard]] NativeOptimizerBackend backend() const;
    [[nodiscard]] const char* backend_name() const;
    void zero_grad(bool set_to_none);
    void set_lrs(const std::vector<double>& base_lrs, double scale);
    void step();
    void save(torch::serialize::OutputArchive& archive, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const;
    void load(torch::serialize::InputArchive& archive, std::stop_token stop = {});

   private:
    friend NativeOptimizerStorage<NativeAdamWGroupConfig, NativeAdamWParamState>;
    void read_checkpoint(torch::serialize::InputArchive&, std::stop_token, bool materialize);
    void initialize_state();
    void step_group_eager(const Group& group);
    void step_group_foreach(const Group& group);
    void step_group_fused(const Group& group);
    NativeOptimizerBackend backend_ = NativeOptimizerBackend::eager;
};
bool native_optimizer_supports_foreach(const std::vector<torch::Tensor>& params);
bool native_optimizer_supports_fused(const std::vector<torch::Tensor>& params);
const char* native_optimizer_backend_name(NativeOptimizerBackend backend);
[[nodiscard]] bool muon_parameter_eligible(std::string_view name, const torch::Tensor& parameter);
class NativeMuonWithAuxAdam : public NativeOptimizerStorage<NativeMuonGroupConfig, NativeMuonParamState> {
   public:
    [[nodiscard]] static std::vector<std::string> InspectCheckpoint(torch::serialize::InputArchive&, const std::unordered_map<std::string, torch::Tensor>&,
                                                                    std::stop_token stop = {});
    NativeMuonWithAuxAdam() = default;
    NativeMuonWithAuxAdam(std::vector<Group> groups, std::vector<NamedParameter> params);
    [[nodiscard]] const char* backend_name() const;
    void zero_grad(bool set_to_none);
    void set_lrs(const std::vector<double>& base_lrs, double scale);
    void set_muon_momentum(double momentum);
    void step();
    void save(torch::serialize::OutputArchive& archive, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const;
    void load(torch::serialize::InputArchive& archive, std::stop_token stop = {});

   private:
    friend NativeOptimizerStorage<NativeMuonGroupConfig, NativeMuonParamState>;
    void read_checkpoint(torch::serialize::InputArchive&, std::stop_token, bool materialize);
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
    std::vector<torch::Tensor>& parameters();
    [[nodiscard]] const std::vector<torch::Tensor>& parameters() const;
    [[nodiscard]] const std::vector<std::string>& parameter_names() const;
    void zero_grad(bool set_to_none);
    void set_lrs(const std::vector<double>& base_lrs, double scale);
    void set_muon_momentum(double momentum);
    void step();
    void reserve_checkpoint(mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const;
    void save(torch::serialize::OutputArchive& archive, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const;
    void load(torch::serialize::InputArchive& archive);
    [[nodiscard]] NativeOptimizer stage_load(torch::serialize::InputArchive& archive) const;
    void commit(NativeOptimizer candidate) noexcept;

   private:
    std::variant<NativeAdamW, NativeMuonWithAuxAdam> storage_;
};
struct OptimizerBuildResult {
    NativeOptimizer optimizer;
    std::vector<double> base_lrs;
};
bool is_encoder_param(std::string_view name);
OptimizerBuildResult build_optimizer(NativeRfDetrModel& model, const TrainRequest& options);
}  // namespace mmltk::backend::models::rfdetr
