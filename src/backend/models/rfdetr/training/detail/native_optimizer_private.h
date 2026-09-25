#pragma once
#include <stop_token>
#include <span>
#include <stdexcept>
#include "training_schedule.h"
#include <map>
#include <tuple>
#include <torch/ordered_dict.h>
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
struct DistributedContext;
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
struct NativeSGDGroupConfig {
 double lr = 0;
 double weight_decay = 0;
 double momentum = .9;
 bool nesterov = false;
 TrainingGroupRole role = TrainingGroupRole::Ordinary;
};
struct NativeSGDParamState {
 int64_t step = 0;
 torch::Tensor momentum_buffer;
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
 std::vector<torch::Tensor>& parameters() { return active_params_; }
 [[nodiscard]] const std::vector<torch::Tensor>& parameters() const { return active_params_; }
 [[nodiscard]] const std::vector<std::string>& parameter_names() const { return active_names_; }
 [[nodiscard]] const std::vector<torch::Tensor>& eligible_parameters() const { return all_params_; }
 [[nodiscard]] const std::vector<std::string>& eligible_parameter_names() const { return all_param_names_; }
 void refresh_active_parameters() {
  active_params_.clear(); active_names_.clear();
  for (std::size_t index = 0; index < all_params_.size(); ++index) {
   if (all_params_[index].requires_grad()) { active_params_.push_back(all_params_[index]); active_names_.push_back(all_param_names_[index]); }
  }
 }
 [[nodiscard]] const std::vector<Group>& groups() const { return groups_; }
 void reserve_checkpoint(mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const;
 void admit_continuation(const TrainingScheduleState&) const;
 void broadcast_state(const DistributedContext&);
 // Candidate-only leaf views admit activation without mutating live requires-grad flags.
 void bind_admission(std::span<const std::uint8_t> active) {
  if (active.size() != params_.size()) throw std::invalid_argument("optimizer admission activation inventory differs");
  for (std::size_t i = 0; i < params_.size(); ++i) params_[i].tensor = params_[i].tensor.detach().set_requires_grad(active[i] != 0);
 }
 void commit(NativeOptimizerStorage candidate) noexcept {
  groups_.swap(candidate.groups_);
  state_.swap(candidate.state_);
 }

protected:
 using ParamState = ParamStateT;
 NativeOptimizerStorage() = default;
 NativeOptimizerStorage(std::vector<Group> groups, std::vector<NamedParameter> params) : groups_(std::move(groups)), params_(std::move(params)) {}
 template <class Optimizer>
 [[nodiscard]] static std::vector<std::string> inspect_checkpoint(torch::serialize::InputArchive&, const std::unordered_map<std::string, torch::Tensor>&, std::stop_token stop = {}, const TrainingScheduleState* = nullptr);
 std::vector<Group> groups_;
 std::vector<NamedParameter> params_;
 std::vector<ParamState> state_;
 std::vector<torch::Tensor> active_params_;
 std::vector<std::string> active_names_;
 std::vector<torch::Tensor> all_params_;
 std::vector<std::string> all_param_names_;
};
class NativeAdamW : public NativeOptimizerStorage<NativeAdamWGroupConfig, NativeAdamWParamState> {
public:
 [[nodiscard]] static std::vector<std::string> InspectCheckpoint(torch::serialize::InputArchive&, const std::unordered_map<std::string, torch::Tensor>&, std::stop_token stop = {}, const TrainingScheduleState* = nullptr);
 NativeAdamW() = default;
 NativeAdamW(std::vector<Group> groups, std::vector<NamedParameter> params, NativeOptimizerBackend backend);
 [[nodiscard]] NativeOptimizerBackend backend() const;
 [[nodiscard]] const char* backend_name() const;
 void activate() { initialize_state(); refresh_active_parameters(); }
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
 [[nodiscard]] static std::vector<std::string> InspectCheckpoint(torch::serialize::InputArchive&, const std::unordered_map<std::string, torch::Tensor>&, std::stop_token stop = {}, const TrainingScheduleState* = nullptr);
 NativeMuonWithAuxAdam() = default;
 NativeMuonWithAuxAdam(std::vector<Group> groups, std::vector<NamedParameter> params);
 [[nodiscard]] const char* backend_name() const;
 void activate() { initialize_state(); refresh_active_parameters(); }
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
class NativeSGD final : public NativeOptimizerStorage<NativeSGDGroupConfig, NativeSGDParamState> {
public:
 NativeSGD() = default;
 NativeSGD(std::vector<Group>, std::vector<NamedParameter>);
 [[nodiscard]] static std::vector<std::string> InspectCheckpoint(torch::serialize::InputArchive&, const std::unordered_map<std::string, torch::Tensor>&, std::stop_token = {}, const TrainingScheduleState* = nullptr);
 [[nodiscard]] const char* backend_name() const { return "eager"; }
 void activate() { state_.resize(params_.size()); refresh_active_parameters(); }
 void zero_grad(bool);
 void set_lrs(const std::vector<double>&, double);
 void set_momentum(double);
 void step();
 void save(torch::serialize::OutputArchive&, mmltk::backend::ml::cuda::TensorReadbackBuffers&, std::size_t) const;
 void load(torch::serialize::InputArchive& archive, std::stop_token stop = {}) { read_checkpoint(archive, stop, true); }
private:
 friend NativeOptimizerStorage<NativeSGDGroupConfig, NativeSGDParamState>;
 void read_checkpoint(torch::serialize::InputArchive&, std::stop_token, bool);
};
class NativeOptimizer {
public:
 NativeOptimizer() = default;
 explicit NativeOptimizer(NativeSGD optimizer);
 explicit NativeOptimizer(NativeAdamW optimizer);
 explicit NativeOptimizer(NativeMuonWithAuxAdam optimizer);
 void activate();
 void admit_continuation(const TrainingScheduleState&) const;
 void broadcast_state(const DistributedContext&);
 [[nodiscard]] const std::vector<torch::Tensor>& eligible_parameters() const;
 [[nodiscard]] const std::vector<std::string>& eligible_parameter_names() const;
 [[nodiscard]] TrainOptimizerKind kind() const;
 [[nodiscard]] std::string_view kind_name() const;
 [[nodiscard]] const char* backend_name() const;
 std::vector<torch::Tensor>& parameters();
 [[nodiscard]] const std::vector<torch::Tensor>& parameters() const;
 [[nodiscard]] const std::vector<std::string>& parameter_names() const;
 void zero_grad(bool set_to_none);
 void set_lrs(const std::vector<double>& base_lrs, double scale);
 void set_momentum(double momentum);
 void step();
 void clip_grad_norm_(double max_norm);
 void reserve_checkpoint(mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const;
 void save(torch::serialize::OutputArchive& archive, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const;
 void load(torch::serialize::InputArchive& archive);
 [[nodiscard]] NativeOptimizer stage_load(torch::serialize::InputArchive& archive, std::span<const std::uint8_t> active = {}) const;
 void commit(NativeOptimizer candidate) noexcept;

private:
 std::variant<NativeAdamW, NativeMuonWithAuxAdam, NativeSGD> storage_;
 using GradientBucketKey = std::tuple<c10::DeviceType, c10::DeviceIndex, c10::ScalarType>;
 std::map<GradientBucketKey, std::vector<torch::Tensor>> gradient_buckets_;
 std::vector<torch::Tensor> gradient_norms_;
};
struct OptimizerBuildResult {
 NativeOptimizer optimizer;
 std::vector<double> base_lrs;
 std::vector<TrainingGroupRole> roles;
};
bool is_encoder_param(std::string_view name);
OptimizerBuildResult build_optimizer(const torch::OrderedDict<std::string, torch::Tensor>& parameters, const TrainRequest& options);
}  // namespace mmltk::backend::models::rfdetr
