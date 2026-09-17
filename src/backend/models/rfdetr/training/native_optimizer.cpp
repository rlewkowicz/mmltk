#include <cctype>
#include <format>
#include <ATen/cuda/CUDAContext.h>
#include <ATen/ops/_fused_adamw.h>
#include <unordered_set>
#include <meta>
#include <type_traits>
#include <cmath>
#include <map>
#include <sstream>
#include <string_view>
#include <variant>
#include "src/backend/ml/torch/archive.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include <torch/types.h>
#include <torch/serialize.h>
#include "detail/checkpoint_private.h"
#include "detail/native_optimizer_private.h"
namespace mmltk::backend::models::rfdetr {
bool muon_parameter_eligible(const std::string_view name, const torch::Tensor& parameter) {
    if ((parameter.dim() != 2 && parameter.dim() != 4) || name.find(".weight") == std::string_view::npos) { return false; }
    if (name.find("embeddings") != std::string_view::npos || name.find("position_embeddings") != std::string_view::npos ||
        name.find("cls_token") != std::string_view::npos || name.find("mask_token") != std::string_view::npos ||
        name.find("query_feat") != std::string_view::npos || name.find("refpoint_embed") != std::string_view::npos ||
        name.find("class_embed") != std::string_view::npos || name.find("bbox_embed") != std::string_view::npos ||
        name.find("segmentation_head") != std::string_view::npos) {
        return false;
    }
    return true;
}
namespace {
constexpr const char* kNativeAdamWFormat = "mmltk.rfdetr.native_adamw";
constexpr int64_t kNativeAdamWFormatVersion = 1;
constexpr double kAdamBeta1 = 0.9;
constexpr double kAdamBeta2 = 0.999;
constexpr double kAdamEps = 1.0e-8;
constexpr const char* kNativeMuonFormat = "mmltk.rfdetr.native_muon_aux_adam";
constexpr int64_t kNativeMuonFormatVersion = 1;
constexpr double kMuonCoeffA = 3.4445;
constexpr double kMuonCoeffB = -4.7750;
constexpr double kMuonCoeffC = 2.0315;
constexpr double kMuonNormEps = 1.0e-7;
constexpr int64_t kMuonNsSteps = 5;
constexpr double kAuxAdamBeta1 = 0.9;
constexpr double kAuxAdamBeta2 = 0.95;
constexpr double kAuxAdamEps = 1.0e-10;
bool finite_equal(const double value, const double expected, const double tolerance = 1.0e-12) {
    return std::isfinite(value) && std::abs(value - expected) <= tolerance;
}
NativeOptimizerBackend parse_backend_name(const std::string& name) {
    if (name == "eager") { return NativeOptimizerBackend::eager; }
    if (name == "foreach") { return NativeOptimizerBackend::foreach; }
    if (name == "fused") { return NativeOptimizerBackend::fused; }
    throw std::runtime_error("unknown native AdamW backend in archive: " + name);
}
torch::Tensor align_tensor_like_param(const torch::Tensor& source, const torch::Tensor& param) {
    auto aligned = torch::zeros_like(param);
    if (source.defined()) { aligned.copy_(source.to(param.device(), param.scalar_type(), false, false)); }
    return aligned;
}
void ensure_aligned(torch::Tensor& t, const torch::Tensor& param) {
    if (!t.defined() || t.sizes() != param.sizes() || t.device() != param.device() || t.scalar_type() != param.scalar_type() || t.layout() != param.layout() ||
        t.strides() != param.strides()) {
        t = align_tensor_like_param(t, param);
    }
}
torch::Device step_device_for_backend(const torch::Tensor& param, NativeOptimizerBackend backend) {
    if (backend == NativeOptimizerBackend::fused || backend == NativeOptimizerBackend::foreach) { return param.device(); }
    return {torch::kCPU};
}
torch::Tensor make_step_tensor(const torch::Tensor& param, NativeOptimizerBackend backend) {
    return torch::zeros({}, torch::TensorOptions().dtype(torch::kFloat32).device(step_device_for_backend(param, backend)));
}
struct AdamWBatch {
    std::vector<torch::Tensor> params;
    std::vector<torch::Tensor> grads;
    std::vector<torch::Tensor> exp_avgs;
    std::vector<torch::Tensor> exp_avg_sqs;
    std::vector<torch::Tensor> max_exp_avg_sqs;
    std::vector<torch::Tensor> steps;
};
using AdamWBatchKey = std::pair<int, int>;
using AdamWBatchMap = std::map<AdamWBatchKey, AdamWBatch>;
template <typename GroupCollection>
void set_scaled_group_lrs(GroupCollection& groups, const std::vector<double>& base_lrs, const double scale, const char* size_mismatch_message) {
    if (base_lrs.size() != groups.size()) { throw std::runtime_error(size_mismatch_message); }
    for (size_t index = 0; index < groups.size(); ++index) { groups[index].config.lr = base_lrs[index] * scale; }
}
void zero_grad_parameters(std::vector<torch::Tensor>& params, const bool set_to_none) {
    torch::NoGradGuard no_grad;
    for (auto& param : params) {
        if (!param.grad().defined()) { continue; }
        if (set_to_none) {
            param.mutable_grad() = torch::Tensor();
            continue;
        }
        auto& grad = param.mutable_grad();
        grad.detach_();
        grad.zero_();
    }
}
template <typename Group>
void validate_group_indices(const Group& group, const size_t param_count, const char* out_of_range_message) {
    for (const size_t index : group.param_indices) {
        if (index >= param_count) { throw std::runtime_error(out_of_range_message); }
    }
}
template <typename NamedParameterCollection>
void populate_named_parameter_views(const NamedParameterCollection& params, std::vector<torch::Tensor>& all_params, std::vector<std::string>& all_param_names,
                                    const char* undefined_param_message) {
    all_params.reserve(params.size());
    all_param_names.reserve(params.size());
    for (const auto& param : params) {
        if (!param.tensor.defined()) { throw std::runtime_error(undefined_param_message); }
        all_params.push_back(param.tensor);
        all_param_names.push_back(param.name);
    }
}
template <typename GroupCollection>
void validate_group_collection_indices(const GroupCollection& groups, const size_t param_count, const char* out_of_range_message) {
    for (const auto& group : groups) { validate_group_indices(group, param_count, out_of_range_message); }
}
void write_archive_layout_counts(torch::serialize::OutputArchive& archive, const size_t group_count, const size_t param_count) {
    mmltk::backend::ml::serialization::write_int(archive, "group_count", static_cast<int64_t>(group_count));
    mmltk::backend::ml::serialization::write_int(archive, "param_count", static_cast<int64_t>(param_count));
}
void validate_archive_layout_counts(torch::serialize::InputArchive& archive, const size_t expected_group_count, const size_t expected_param_count,
                                    const char* mismatch_message) {
    const auto group_count = mmltk::backend::ml::serialization::require_int(archive, "group_count");
    const auto param_count = mmltk::backend::ml::serialization::require_int(archive, "param_count");
    if (group_count != static_cast<int64_t>(expected_group_count) || param_count != static_cast<int64_t>(expected_param_count)) {
        throw std::runtime_error(mismatch_message);
    }
}
template <typename WriteEntryFn>
void write_indexed_optimizer_archive(torch::serialize::OutputArchive& archive, const char* entry_name, const size_t entry_count, WriteEntryFn&& write_entry) {
    for (size_t index = 0; index < entry_count; ++index) {
        torch::serialize::OutputArchive entry_archive;
        write_entry(index, entry_archive);
        archive.write(mmltk::backend::ml::serialization::archive_entry_name(entry_name, index), entry_archive);
    }
}
template <typename ReadEntryFn>
void read_indexed_optimizer_archive(torch::serialize::InputArchive& archive, const char* entry_name, const size_t entry_count, ReadEntryFn&& read_entry) {
    for (size_t index = 0; index < entry_count; ++index) {
        torch::serialize::InputArchive entry_archive;
        archive.read(mmltk::backend::ml::serialization::archive_entry_name(entry_name, index), entry_archive);
        read_entry(index, entry_archive);
    }
}
void write_named_parameter_archive(torch::serialize::OutputArchive& archive, const std::string& name) {
    mmltk::backend::ml::serialization::write_string(archive, "name", name);
}
void validate_named_parameter_archive(torch::serialize::InputArchive& archive, const std::string& expected_name, const char* mismatch_message) {
    if (mmltk::backend::ml::serialization::require_string(archive, "name") != expected_name) { throw std::runtime_error(mismatch_message); }
}
torch::Tensor require_parameter_state_tensor(torch::serialize::InputArchive& archive, const char* entry_name, const torch::Tensor& param,
                                             const char* shape_mismatch_message) {
    auto state_tensor = mmltk::backend::ml::serialization::require_tensor(archive, entry_name);
    if (state_tensor.sizes() != param.sizes() || !state_tensor.device().is_cpu() || !state_tensor.is_floating_point() ||
        state_tensor.scalar_type() != param.scalar_type() || state_tensor.layout() != param.layout() || !torch::isfinite(state_tensor).all().item<bool>()) {
        throw std::runtime_error(shape_mismatch_message);
    }
    return align_tensor_like_param(state_tensor, param);
}
torch::Tensor require_adam_step_tensor(torch::serialize::InputArchive& archive, const torch::Tensor& param, const NativeOptimizerBackend backend) {
    auto step = mmltk::backend::ml::serialization::require_tensor(archive, "step");
    if (!step.defined() || !step.device().is_cpu() || step.dim() != 0 || step.scalar_type() != torch::kFloat32 || !torch::isfinite(step).item<bool>() ||
        step.item<float>() < 0.0F) {
        throw std::runtime_error("native AdamW archive step does not match the current optimizer");
    }
    return step.to(step_device_for_backend(param, backend), torch::kFloat32).contiguous();
}
template <typename ParamIndexCollection>
void write_group_param_indices(torch::serialize::OutputArchive& archive, const ParamIndexCollection& param_indices) {
    mmltk::backend::ml::serialization::write_int(archive, "param_index_count", static_cast<int64_t>(param_indices.size()));
    for (size_t param_index = 0; param_index < param_indices.size(); ++param_index) {
        mmltk::backend::ml::serialization::write_int(archive, mmltk::backend::ml::serialization::archive_entry_name("param_index", param_index).c_str(),
                                                     static_cast<int64_t>(param_indices[param_index]));
    }
}
template <typename GroupConfig>
void write_group_lr_weight_decay(torch::serialize::OutputArchive& archive, const GroupConfig& config) {
    mmltk::backend::ml::serialization::write_double(archive, "lr", config.lr);
    mmltk::backend::ml::serialization::write_double(archive, "weight_decay", config.weight_decay);
}
// Every native optimizer stamps its archive with the same format tag and version, so one reader
// rejects legacy and future archives for all of them. `label` names the optimizer in both failures.
void validate_optimizer_archive_format(torch::serialize::InputArchive& archive, const char* expected_format, const int64_t expected_version,
                                       const std::string_view label) {
    c10::IValue format_value;
    if (!archive.try_read("format", format_value) || !format_value.isString() || format_value.toStringRef() != expected_format) {
        throw std::runtime_error(
            std::format("native RF-DETR resume checkpoint uses a legacy {} archive; rebuild the "
                        "checkpoint with the current mmltk",
                        label));
    }
    const auto version = mmltk::backend::ml::serialization::require_int(archive, "format_version");
    if (version != expected_version) { throw std::runtime_error(std::format("unsupported native {} archive version: {}", label, version)); }
}
template <typename ParamIndexCollection>
void validate_group_param_indices(torch::serialize::InputArchive& archive, const ParamIndexCollection& param_indices, const char* size_mismatch_message,
                                  const char* order_mismatch_message) {
    const auto param_index_count = mmltk::backend::ml::serialization::require_int(archive, "param_index_count");
    if (param_index_count != static_cast<int64_t>(param_indices.size())) { throw std::runtime_error(size_mismatch_message); }
    for (size_t param_index = 0; param_index < param_indices.size(); ++param_index) {
        const auto stored_param_index =
            mmltk::backend::ml::serialization::require_int(archive, mmltk::backend::ml::serialization::archive_entry_name("param_index", param_index).c_str());
        if (stored_param_index != static_cast<int64_t>(param_indices[param_index])) { throw std::runtime_error(order_mismatch_message); }
    }
}
// Reads the parameter-group layout every native optimizer shares: the learning rate, the weight
// decay, whatever optimizer-specific fields `read_config` claims, and the parameter index roster.
// The mirror of write_group_lr_weight_decay plus write_group_param_indices on the save path.
template <typename GroupCollection, typename ReadConfigFn>
void read_optimizer_group_archives(torch::serialize::InputArchive& archive, GroupCollection& groups, const char* size_mismatch_message,
                                   const char* order_mismatch_message, ReadConfigFn&& read_config) {
    read_indexed_optimizer_archive(archive, "group", groups.size(), [&](const size_t index, auto& group_archive) {
        auto& group = groups[index];
        group.config.lr = mmltk::backend::ml::serialization::require_double(group_archive, "lr");
        group.config.weight_decay = mmltk::backend::ml::serialization::require_double(group_archive, "weight_decay");
        read_config(group_archive, group.config);
        validate_group_param_indices(group_archive, group.param_indices, size_mismatch_message, order_mismatch_message);
    });
}
template <typename GroupCollection, typename FlagAccessor>
std::vector<bool> collect_group_param_flags(const GroupCollection& groups, size_t param_count, FlagAccessor&& flag_accessor) {
    std::vector<bool> flags(param_count, false);
    for (const auto& group : groups) {
        const bool enabled = flag_accessor(group);
        for (const auto index : group.param_indices) { flags[index] = enabled; }
    }
    return flags;
}
// Muon routes each parameter through either the Muon or the auxiliary Adam path; state setup, save
// and load all need the same per-parameter routing decision.
template <typename GroupCollection>
std::vector<bool> collect_muon_param_flags(const GroupCollection& groups, const size_t param_count) {
    return collect_group_param_flags(groups, param_count, [](const auto& group) { return group.config.use_muon; });
}
// Shared state-initialization skeleton for the native optimizers: size the per-parameter state to
// the parameter list, resolve the per-parameter group flag once, then let the caller fill each slot.
template <typename States, typename NamedParameters, typename GroupCollection, typename FlagAccessor, typename StateInitializer>
void initialize_param_states(States& states, const NamedParameters& params, const GroupCollection& groups, FlagAccessor&& flag_accessor,
                             StateInitializer&& state_initializer) {
    states.clear();
    states.resize(params.size());
    const std::vector<bool> flags = collect_group_param_flags(groups, params.size(), std::forward<FlagAccessor>(flag_accessor));
    for (size_t index = 0; index < params.size(); ++index) { state_initializer(states[index], params[index].tensor, flags[index]); }
}
torch::Tensor muon_zeropower_via_newtonschulz5(const torch::Tensor& grad, const int64_t steps = kMuonNsSteps) {
    if (grad.dim() < 2) { throw std::runtime_error("native Muon zeropower requires tensors with rank >= 2"); }
    auto update = grad.to(torch::kBFloat16);
    const bool transpose = update.size(-2) > update.size(-1);
    if (transpose) { update = update.transpose(-2, -1); }
    const auto denom = update.flatten(-2).norm(2, -1, true).unsqueeze(-1).add(kMuonNormEps);
    update = update.div(denom);
    for (int64_t step = 0; step < steps; ++step) {
        auto gram = torch::matmul(update, update.transpose(-2, -1));
        auto poly = gram.mul(kMuonCoeffB).add(torch::matmul(gram, gram), kMuonCoeffC);
        update = update.mul(kMuonCoeffA).add_(torch::matmul(poly, update));
    }
    if (transpose) { update = update.transpose(-2, -1); }
    return update;
}
void validate_adamw_param_for_backend(const torch::Tensor& param, const torch::Tensor& grad, const NativeOptimizerBackend backend) {
    if (grad.is_sparse()) { throw std::runtime_error("native AdamW does not support sparse gradients"); }
    if (backend == NativeOptimizerBackend::fused) {
        if (!param.device().is_cuda()) { throw std::runtime_error("fused native AdamW requires CUDA parameters"); }
        if (!param.is_floating_point() || torch::is_complex(param)) { throw std::runtime_error("fused native AdamW requires real floating-point parameters"); }
        return;
    }
    if (torch::is_complex(param)) { throw std::runtime_error("native AdamW does not support complex parameters"); }
}
void align_adamw_state_tensors(torch::Tensor& step, torch::Tensor& exp_avg, torch::Tensor& exp_avg_sq, torch::Tensor& max_exp_avg_sq, torch::Tensor& grad,
                               const torch::Tensor& param, const NativeOptimizerBackend backend, const bool amsgrad) {
    const auto step_device = step_device_for_backend(param, backend);
    if (!step.defined() || step.device() != step_device) { step = step.to(step_device, torch::kFloat32).contiguous(); }
    if (step.scalar_type() != torch::kFloat32) { step = step.to(step_device, torch::kFloat32).contiguous(); }
    ensure_aligned(exp_avg, param);
    ensure_aligned(exp_avg_sq, param);
    ensure_aligned(grad, param);
    if (amsgrad) { ensure_aligned(max_exp_avg_sq, param); }
}
void collect_adamw_batch(std::map<std::pair<int, int>, AdamWBatch>& batches, const torch::Tensor& param, const torch::Tensor& grad,
                         const torch::Tensor& exp_avg, const torch::Tensor& exp_avg_sq, const torch::Tensor& max_exp_avg_sq, const torch::Tensor& step,
                         const bool amsgrad) {
    const auto device_index = static_cast<int>(param.device().index());
    const auto key = std::make_pair(device_index, static_cast<int>(param.scalar_type()));
    auto& batch = batches[key];
    batch.params.push_back(param);
    batch.grads.push_back(grad);
    batch.exp_avgs.push_back(exp_avg);
    batch.exp_avg_sqs.push_back(exp_avg_sq);
    if (amsgrad) { batch.max_exp_avg_sqs.push_back(max_exp_avg_sq); }
    batch.steps.push_back(step);
}
template <typename Params, typename States, typename Group, typename Fn>
void for_each_adamw_grad_state(Params& params, States& states, const Group& group, const NativeOptimizerBackend backend, Fn&& fn) {
    for (const auto index : group.param_indices) {
        auto& param = params[index].tensor;
        if (!param.grad().defined()) { continue; }
        auto grad = param.grad();
        validate_adamw_param_for_backend(param, grad, backend);
        fn(index, param, grad, states[index]);
    }
}
template <typename Params, typename States, typename Group>
AdamWBatchMap collect_adamw_batches(Params& params, States& states, const Group& group, const NativeOptimizerBackend backend) {
    AdamWBatchMap batches;
    for_each_adamw_grad_state(params, states, group, backend, [&](const auto, auto& param, auto grad, auto& state) {
        align_adamw_state_tensors(state.step, state.exp_avg, state.exp_avg_sq, state.max_exp_avg_sq, grad, param, backend, group.config.amsgrad);
        collect_adamw_batch(batches, param, grad, state.exp_avg, state.exp_avg_sq, state.max_exp_avg_sq, state.step, group.config.amsgrad);
    });
    return batches;
}
void apply_foreach_adamw_batch(AdamWBatch& batch, const NativeAdamWGroupConfig& config) {
    torch::_foreach_add_(batch.steps, 1.0);
    if (config.weight_decay != 0.0) { torch::_foreach_mul_(batch.params, 1.0 - config.lr * config.weight_decay); }
    torch::_foreach_mul_(batch.exp_avgs, kAdamBeta1);
    torch::_foreach_add_(batch.exp_avgs, batch.grads, 1.0 - kAdamBeta1);
    torch::_foreach_mul_(batch.exp_avg_sqs, kAdamBeta2);
    torch::_foreach_addcmul_(batch.exp_avg_sqs, batch.grads, batch.grads, 1.0 - kAdamBeta2);
    const auto step_value = batch.steps[0].item<double>();
    const double bias_correction1 = 1.0 - std::pow(kAdamBeta1, step_value);
    const double bias_correction2 = 1.0 - std::pow(kAdamBeta2, step_value);
    const double step_size = config.lr / bias_correction1;
    const double bias_correction2_sqrt = std::sqrt(bias_correction2);
    if (config.amsgrad) { torch::_foreach_maximum_(batch.max_exp_avg_sqs, batch.exp_avg_sqs); }
    auto denoms = torch::_foreach_sqrt(config.amsgrad ? batch.max_exp_avg_sqs : batch.exp_avg_sqs);
    torch::_foreach_div_(denoms, bias_correction2_sqrt);
    torch::_foreach_add_(denoms, kAdamEps);
    torch::_foreach_addcdiv_(batch.params, batch.exp_avgs, denoms, -step_size);
}
void apply_fused_adamw_batch(AdamWBatch& batch, const NativeAdamWGroupConfig& config) {
    torch::_foreach_add_(batch.steps, 1.0);
    at::_fused_adamw_(batch.params, batch.grads, batch.exp_avgs, batch.exp_avg_sqs, batch.max_exp_avg_sqs, batch.steps, config.lr, kAdamBeta1, kAdamBeta2,
                      config.weight_decay, kAdamEps, config.amsgrad, false, std::nullopt, std::nullopt);
}
template <typename Params, typename States, typename Group>
void step_adamw_group_batched(Params& params, States& states, const Group& group, const NativeOptimizerBackend backend) {
    auto batches = collect_adamw_batches(params, states, group, backend);
    for (auto& [_, batch] : batches) {
        if (batch.params.empty()) { continue; }
        if (backend == NativeOptimizerBackend::fused) {
            apply_fused_adamw_batch(batch, group.config);
        } else {
            apply_foreach_adamw_batch(batch, group.config);
        }
    }
}
torch::Tensor muon_update(const torch::Tensor& grad, torch::Tensor& momentum, const double beta, const bool nesterov) {
    momentum.lerp_(grad, 1.0 - beta);
    auto update = nesterov ? grad.lerp(momentum, beta) : momentum;
    if (update.dim() == 4) { update = update.view({update.size(0), -1}); }
    update = muon_zeropower_via_newtonschulz5(update);
    update.mul_(std::sqrt(std::max(1.0, static_cast<double>(update.size(-2)) / static_cast<double>(update.size(-1)))));
    return update;
}
torch::Tensor adam_update(const torch::Tensor& grad, torch::Tensor& exp_avg, torch::Tensor& exp_avg_sq, const int64_t step) {
    exp_avg.lerp_(grad, 1.0 - kAuxAdamBeta1);
    exp_avg_sq.lerp_(grad.square(), 1.0 - kAuxAdamBeta2);
    const double bias_correction1 = 1.0 - std::pow(kAuxAdamBeta1, static_cast<double>(step));
    const double bias_correction2 = 1.0 - std::pow(kAuxAdamBeta2, static_cast<double>(step));
    auto exp_avg_corrected = exp_avg.div(bias_correction1);
    auto exp_avg_sq_corrected = exp_avg_sq.div(bias_correction2);
    return exp_avg_corrected.div(exp_avg_sq_corrected.sqrt().add(kAuxAdamEps));
}
// Reconstruct only the saved CPU layout, then reuse the optimizer's complete
// continuation parser. No model construction, zero-state allocation, or CUDA.
template <class Groups, class Parameters>
void read_inspection_layout(torch::serialize::InputArchive& archive, const std::unordered_map<std::string, torch::Tensor>& tensors, Groups& groups,
                            Parameters& parameters) {
    const auto count = mmltk::backend::ml::serialization::require_int(archive, "param_count");
    const auto group_count = mmltk::backend::ml::serialization::require_int(archive, "group_count");
    if (count <= 0 || static_cast<std::uint64_t>(count) > tensors.size() || group_count <= 0 || group_count > count)
        throw std::runtime_error("invalid optimizer inspection inventory");
    std::unordered_set<std::string> names;
    parameters.reserve(static_cast<std::size_t>(count));
    read_indexed_optimizer_archive(archive, "param", static_cast<std::size_t>(count), [&](auto, auto& parameter) {
        const auto name = mmltk::backend::ml::serialization::require_string(parameter, "name");
        const auto found = tensors.find(name);
        if (found == tensors.end() || !names.insert(name).second || !found->second.is_cpu())
            throw std::runtime_error("optimizer parameter has no unique CPU model tensor");
        parameters.push_back({name, found->second});
    });
    groups.resize(static_cast<std::size_t>(group_count));
    std::vector<bool> assigned(static_cast<std::size_t>(count));
    read_indexed_optimizer_archive(archive, "group", groups.size(), [&](std::size_t index, auto& group) {
        const auto members = mmltk::backend::ml::serialization::require_int(group, "param_index_count");
        if (members <= 0 || members > count) throw std::runtime_error("invalid optimizer group inventory");
        auto& indices = groups[index].param_indices;
        indices.reserve(static_cast<std::size_t>(members));
        for (int64_t ordinal = 0; ordinal < members; ++ordinal) {
            const auto value =
                mmltk::backend::ml::serialization::require_int(group, mmltk::backend::ml::serialization::archive_entry_name("param_index", ordinal).c_str());
            if (value < 0 || value >= count || assigned[static_cast<std::size_t>(value)])
                throw std::runtime_error("optimizer groups do not partition the parameter inventory");
            assigned[static_cast<std::size_t>(value)] = true;
            indices.push_back(static_cast<std::size_t>(value));
        }
    });
    if (std::ranges::find(assigned, false) != assigned.end()) throw std::runtime_error("optimizer group inventory is incomplete");
}
}  // namespace
template <typename GroupConfig, typename ParamStateT>
template <class Optimizer>
std::vector<std::string> NativeOptimizerStorage<GroupConfig, ParamStateT>::inspect_checkpoint(torch::serialize::InputArchive& archive,
                                                                                              const std::unordered_map<std::string, torch::Tensor>& tensors) {
    Optimizer candidate;
    read_inspection_layout(archive, tensors, candidate.groups_, candidate.params_);
    populate_named_parameter_views(candidate.params_, candidate.all_params_, candidate.all_param_names_, "invalid CPU checkpoint tensor");
    candidate.load(archive);
    return std::move(candidate.all_param_names_);
}
const char* native_optimizer_backend_name(const NativeOptimizerBackend backend) {
    switch (backend) {
        case NativeOptimizerBackend::eager: return "eager";
        case NativeOptimizerBackend::foreach: return "foreach";
        case NativeOptimizerBackend::fused: return "fused";
    }
    return "unknown";
}
bool native_optimizer_supports_foreach(const std::vector<torch::Tensor>& params) {
    if (params.empty()) { return false; }
    for (const auto& param : params) {
        if (!param.defined() || !param.is_floating_point() || torch::is_complex(param)) { return false; }
    }
    return true;
}
bool native_optimizer_supports_fused(const std::vector<torch::Tensor>& params) {
    if (!native_optimizer_supports_foreach(params)) { return false; }
    for (const auto& param : params) {
        if (!param.device().is_cuda()) { return false; }
        const auto device_index = static_cast<int>(param.device().index());
        if (device_index < 0) { return false; }
        const auto* properties = at::cuda::getDeviceProperties(static_cast<c10::DeviceIndex>(device_index));
        if (properties == nullptr || properties->major < 8) { return false; }
    }
    return true;
}
NativeAdamW::NativeAdamW(std::vector<Group> groups, std::vector<NamedParameter> params, const NativeOptimizerBackend backend)
    : NativeOptimizerStorage(std::move(groups), std::move(params)), backend_(backend) {
    populate_named_parameter_views(params_, all_params_, all_param_names_, "native AdamW received an undefined parameter tensor");
    validate_group_collection_indices(groups_, params_.size(), "native AdamW parameter group index is out of range");
    initialize_state();
}
NativeOptimizerBackend NativeAdamW::backend() const { return backend_; }
const char* NativeAdamW::backend_name() const { return native_optimizer_backend_name(backend_); }
void NativeAdamW::initialize_state() {
    initialize_param_states(
        state_, params_, groups_, [](const auto& group) { return group.config.amsgrad; },
        [this](ParamState& state, const torch::Tensor& param, const bool needs_amsgrad) {
            state.step = make_step_tensor(param, backend_);
            state.exp_avg = torch::zeros_like(param);
            state.exp_avg_sq = torch::zeros_like(param);
            if (needs_amsgrad) { state.max_exp_avg_sq = torch::zeros_like(param); }
        });
}
void NativeAdamW::zero_grad(const bool set_to_none) { zero_grad_parameters(all_params_, set_to_none); }
void NativeAdamW::set_lrs(const std::vector<double>& base_lrs, const double scale) {
    set_scaled_group_lrs(groups_, base_lrs, scale, "native AdamW base LR count does not match param group count");
}
void NativeAdamW::step() {
    torch::NoGradGuard no_grad;
    for (const auto& group : groups_) {
        if (backend_ == NativeOptimizerBackend::fused) {
            step_group_fused(group);
        } else if (backend_ == NativeOptimizerBackend::foreach) {
            step_group_foreach(group);
        } else {
            step_group_eager(group);
        }
    }
}
void NativeAdamW::step_group_eager(const Group& group) {
    for_each_adamw_grad_state(params_, state_, group, NativeOptimizerBackend::eager, [&](const auto, auto& param, const auto& grad, auto& state) {
        state.step.add_(1.0);
        const auto step_value = state.step.template item<double>();
        if (group.config.weight_decay != 0.0) { param.mul_(1.0 - group.config.lr * group.config.weight_decay); }
        state.exp_avg.mul_(kAdamBeta1).add_(grad, 1.0 - kAdamBeta1);
        state.exp_avg_sq.mul_(kAdamBeta2).addcmul_(grad, grad, 1.0 - kAdamBeta2);
        const double bias_correction1 = 1.0 - std::pow(kAdamBeta1, step_value);
        const double bias_correction2 = 1.0 - std::pow(kAdamBeta2, step_value);
        torch::Tensor denom;
        if (group.config.amsgrad) {
            if (!state.max_exp_avg_sq.defined()) { state.max_exp_avg_sq = torch::zeros_like(param); }
            state.max_exp_avg_sq = torch::maximum(state.max_exp_avg_sq, state.exp_avg_sq);
            denom = state.max_exp_avg_sq.sqrt();
        } else {
            denom = state.exp_avg_sq.sqrt();
        }
        denom.div_(std::sqrt(bias_correction2)).add_(kAdamEps);
        param.addcdiv_(state.exp_avg, denom, -group.config.lr / bias_correction1);
    });
}
void NativeAdamW::step_group_foreach(const Group& group) { step_adamw_group_batched(params_, state_, group, NativeOptimizerBackend::foreach); }
void NativeAdamW::step_group_fused(const Group& group) { step_adamw_group_batched(params_, state_, group, NativeOptimizerBackend::fused); }
namespace {
template <class State>
void reserve_optimizer_readback(const std::vector<State>& states, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) {
    std::vector<torch::Tensor> tensors;
    for (const auto& state : states) {
        template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^State, std::meta::access_context::current()))) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(state.[:member:])>, torch::Tensor>) {
                if (state.[:member:].defined()) tensors.push_back(state.[:member:]);
            }
        }
    }
    readback.Reserve(tensors, first_slot);
}
template <class State>
void write_optimizer_state(torch::serialize::OutputArchive& archive, const State& state, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback,
                           std::size_t& slot) {
    template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^State, std::meta::access_context::current()))) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(state.[:member:])>, torch::Tensor>) {
            if (state.[:member:].defined()) archive.write(std::string(std::meta::identifier_of(member)), readback.Stage(slot++));
        }
    }
}
}  // namespace
void NativeAdamW::reserve_checkpoint(mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const {
    reserve_optimizer_readback(state_, readback, first_slot);
}
void NativeAdamW::save(torch::serialize::OutputArchive& archive, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const {
    mmltk::backend::ml::serialization::write_string(archive, "format", kNativeAdamWFormat);
    mmltk::backend::ml::serialization::write_int(archive, "format_version", kNativeAdamWFormatVersion);
    mmltk::backend::ml::serialization::write_string(archive, "backend", backend_name());
    mmltk::backend::ml::serialization::write_double(archive, "beta1", kAdamBeta1);
    mmltk::backend::ml::serialization::write_double(archive, "beta2", kAdamBeta2);
    mmltk::backend::ml::serialization::write_double(archive, "eps", kAdamEps);
    write_archive_layout_counts(archive, groups_.size(), params_.size());
    write_indexed_optimizer_archive(archive, "group", groups_.size(), [&](const size_t index, auto& group_archive) {
        write_group_lr_weight_decay(group_archive, groups_[index].config);
        mmltk::backend::ml::serialization::write_int(group_archive, "amsgrad", groups_[index].config.amsgrad ? 1 : 0);
        write_group_param_indices(group_archive, groups_[index].param_indices);
    });
    write_indexed_optimizer_archive(archive, "param", params_.size(), [&](const size_t index, auto& param_archive) {
        write_named_parameter_archive(param_archive, params_[index].name);
        write_optimizer_state(param_archive, state_[index], readback, first_slot);
        mmltk::backend::ml::serialization::write_int(param_archive, "has_max_exp_avg_sq", state_[index].max_exp_avg_sq.defined() ? 1 : 0);
    });
}
void NativeAdamW::load(torch::serialize::InputArchive& archive) {
    validate_optimizer_archive_format(archive, kNativeAdamWFormat, kNativeAdamWFormatVersion, "AdamW");
    const auto stored_backend = parse_backend_name(mmltk::backend::ml::serialization::require_string(archive, "backend"));
    (void)stored_backend;
    const auto stored_beta1 = mmltk::backend::ml::serialization::require_double(archive, "beta1");
    const auto stored_beta2 = mmltk::backend::ml::serialization::require_double(archive, "beta2");
    const auto stored_eps = mmltk::backend::ml::serialization::require_double(archive, "eps");
    if (!finite_equal(stored_beta1, kAdamBeta1) || !finite_equal(stored_beta2, kAdamBeta2) || !finite_equal(stored_eps, kAdamEps)) {
        throw std::runtime_error(
            "native AdamW archive hyperparameters do not "
            "match the compiled optimizer");
    }
    validate_archive_layout_counts(archive, groups_.size(), params_.size(), "native AdamW archive layout does not match the current optimizer");
    auto candidate_groups = groups_;
    read_optimizer_group_archives(
        archive, candidate_groups,
        "native AdamW archive parameter group size does not match the "
        "current optimizer",
        "native AdamW archive parameter group order does not match the "
        "current optimizer",
        [](auto& group_archive, auto& config) { config.amsgrad = mmltk::backend::ml::serialization::require_int(group_archive, "amsgrad") != 0; });
    for (const auto& group : candidate_groups) {
        if (!std::isfinite(group.config.lr) || group.config.lr < 0.0 || !std::isfinite(group.config.weight_decay) || group.config.weight_decay < 0.0) {
            throw std::runtime_error("native AdamW archive parameter group does not match the current optimizer");
        }
    }
    const auto uses_amsgrad = collect_group_param_flags(candidate_groups, params_.size(), [](const auto& group) { return group.config.amsgrad; });
    std::vector<NativeAdamWParamState> candidate_state(params_.size());
    read_indexed_optimizer_archive(archive, "param", params_.size(), [&](const size_t index, auto& param_archive) {
        validate_named_parameter_archive(param_archive, params_[index].name,
                                         "native AdamW archive parameter order "
                                         "does not match the current model");
        const auto& param = params_[index].tensor;
        auto loaded_step = require_adam_step_tensor(param_archive, param, backend_);
        auto loaded_exp_avg = require_parameter_state_tensor(param_archive, "exp_avg", param,
                                                             "native AdamW archive tensor shape "
                                                             "does not match the current model");
        auto loaded_exp_avg_sq = require_parameter_state_tensor(param_archive, "exp_avg_sq", param,
                                                                "native AdamW archive tensor shape "
                                                                "does not match the current model");
        const bool has_max_exp_avg_sq = mmltk::backend::ml::serialization::require_int(param_archive, "has_max_exp_avg_sq") != 0;
        if (has_max_exp_avg_sq != uses_amsgrad[index]) { throw std::runtime_error("native AdamW archive AMSGrad state does not match the current optimizer"); }
        auto& state = candidate_state[index];
        state.step = std::move(loaded_step);
        state.exp_avg = std::move(loaded_exp_avg);
        state.exp_avg_sq = std::move(loaded_exp_avg_sq);
        if (has_max_exp_avg_sq) {
            state.max_exp_avg_sq = require_parameter_state_tensor(param_archive, "max_exp_avg_sq", param,
                                                                  "native AdamW archive AMSGrad tensor shape does not match the "
                                                                  "current model");
        } else {
            state.max_exp_avg_sq = torch::Tensor();
        }
    });
    groups_.swap(candidate_groups);
    state_.swap(candidate_state);
}
std::vector<std::string> NativeAdamW::InspectCheckpoint(torch::serialize::InputArchive& archive,
                                                        const std::unordered_map<std::string, torch::Tensor>& tensors) {
    return inspect_checkpoint<NativeAdamW>(archive, tensors);
}
void NativeAdamW::commit(NativeAdamW candidate) noexcept {
    groups_.swap(candidate.groups_);
    state_.swap(candidate.state_);
}
NativeMuonWithAuxAdam::NativeMuonWithAuxAdam(std::vector<Group> groups, std::vector<NamedParameter> params)
    : NativeOptimizerStorage(std::move(groups), std::move(params)) {
    populate_named_parameter_views(params_, all_params_, all_param_names_, "native Muon received an undefined parameter tensor");
    validate_group_collection_indices(groups_, params_.size(), "native Muon parameter group index is out of range");
    initialize_state();
}
const char* NativeMuonWithAuxAdam::backend_name() const { return "eager"; }
void NativeMuonWithAuxAdam::initialize_state() {
    initialize_param_states(
        state_, params_, groups_, [](const auto& group) { return group.config.use_muon; },
        [](ParamState& state, const torch::Tensor& param, const bool use_muon) {
            if (use_muon) {
                state.momentum_buffer = torch::zeros_like(param);
            } else {
                state.exp_avg = torch::zeros_like(param);
                state.exp_avg_sq = torch::zeros_like(param);
            }
        });
}
void NativeMuonWithAuxAdam::zero_grad(const bool set_to_none) { zero_grad_parameters(all_params_, set_to_none); }
void NativeMuonWithAuxAdam::set_lrs(const std::vector<double>& base_lrs, const double scale) {
    set_scaled_group_lrs(groups_, base_lrs, scale, "native Muon base LR count does not match param group count");
}
void NativeMuonWithAuxAdam::set_muon_momentum(const double momentum) {
    for (auto& group : groups_) {
        if (!group.config.use_muon) { continue; }
        group.config.momentum = momentum;
    }
}
void NativeMuonWithAuxAdam::step() {
    torch::NoGradGuard no_grad;
    for (const auto& group : groups_) {
        for (const auto index : group.param_indices) {
            auto& param = params_[index].tensor;
            if (!param.is_floating_point() || torch::is_complex(param)) { throw std::runtime_error("native Muon requires real floating-point parameters"); }
            auto grad = param.grad().defined() ? param.grad() : torch::zeros_like(param);
            if (grad.is_sparse()) { throw std::runtime_error("native Muon does not support sparse gradients"); }
            if (grad.device() != param.device() || grad.scalar_type() != param.scalar_type()) { grad = align_tensor_like_param(grad, param); }
            auto& state = state_[index];
            if (group.config.use_muon) {
                ensure_aligned(state.momentum_buffer, param);
                auto update = muon_update(grad, state.momentum_buffer, group.config.momentum, group.config.nesterov);
                if (group.config.weight_decay != 0.0) { param.mul_(1.0 - group.config.lr * group.config.weight_decay); }
                param.add_(update.reshape_as(param), -group.config.lr);
                continue;
            }
            ensure_aligned(state.exp_avg, param);
            ensure_aligned(state.exp_avg_sq, param);
            ++state.step;
            auto update = adam_update(grad, state.exp_avg, state.exp_avg_sq, state.step);
            if (group.config.weight_decay != 0.0) { param.mul_(1.0 - group.config.lr * group.config.weight_decay); }
            param.add_(update, -group.config.lr);
        }
    }
}
void NativeMuonWithAuxAdam::reserve_checkpoint(mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const {
    reserve_optimizer_readback(state_, readback, first_slot);
}
void NativeMuonWithAuxAdam::save(torch::serialize::OutputArchive& archive, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback,
                                 std::size_t first_slot) const {
    mmltk::backend::ml::serialization::write_string(archive, "format", kNativeMuonFormat);
    mmltk::backend::ml::serialization::write_int(archive, "format_version", kNativeMuonFormatVersion);
    mmltk::backend::ml::serialization::write_int(archive, "ns_steps", kMuonNsSteps);
    mmltk::backend::ml::serialization::write_double(archive, "muon_coeff_a", kMuonCoeffA);
    mmltk::backend::ml::serialization::write_double(archive, "muon_coeff_b", kMuonCoeffB);
    mmltk::backend::ml::serialization::write_double(archive, "muon_coeff_c", kMuonCoeffC);
    mmltk::backend::ml::serialization::write_double(archive, "muon_eps", kMuonNormEps);
    mmltk::backend::ml::serialization::write_double(archive, "aux_adam_beta1", kAuxAdamBeta1);
    mmltk::backend::ml::serialization::write_double(archive, "aux_adam_beta2", kAuxAdamBeta2);
    mmltk::backend::ml::serialization::write_double(archive, "aux_adam_eps", kAuxAdamEps);
    write_archive_layout_counts(archive, groups_.size(), params_.size());
    write_indexed_optimizer_archive(archive, "group", groups_.size(), [&](const size_t index, auto& group_archive) {
        write_group_lr_weight_decay(group_archive, groups_[index].config);
        mmltk::backend::ml::serialization::write_double(group_archive, "momentum", groups_[index].config.momentum);
        mmltk::backend::ml::serialization::write_int(group_archive, "use_muon", groups_[index].config.use_muon ? 1 : 0);
        mmltk::backend::ml::serialization::write_int(group_archive, "nesterov", groups_[index].config.nesterov ? 1 : 0);
        write_group_param_indices(group_archive, groups_[index].param_indices);
    });
    const std::vector<bool> use_muon = collect_muon_param_flags(groups_, params_.size());
    write_indexed_optimizer_archive(archive, "param", params_.size(), [&](const size_t index, auto& param_archive) {
        write_named_parameter_archive(param_archive, params_[index].name);
        mmltk::backend::ml::serialization::write_int(param_archive, "use_muon", use_muon[index] ? 1 : 0);
        if (!use_muon[index]) mmltk::backend::ml::serialization::write_int(param_archive, "step", state_[index].step);
        write_optimizer_state(param_archive, state_[index], readback, first_slot);
    });
}
void NativeMuonWithAuxAdam::load(torch::serialize::InputArchive& archive) {
    validate_optimizer_archive_format(archive, kNativeMuonFormat, kNativeMuonFormatVersion, "Muon");
    if (mmltk::backend::ml::serialization::require_int(archive, "ns_steps") != kMuonNsSteps ||
        !finite_equal(mmltk::backend::ml::serialization::require_double(archive, "muon_coeff_a"), kMuonCoeffA) ||
        !finite_equal(mmltk::backend::ml::serialization::require_double(archive, "muon_coeff_b"), kMuonCoeffB) ||
        !finite_equal(mmltk::backend::ml::serialization::require_double(archive, "muon_coeff_c"), kMuonCoeffC) ||
        !finite_equal(mmltk::backend::ml::serialization::require_double(archive, "muon_eps"), kMuonNormEps) ||
        !finite_equal(mmltk::backend::ml::serialization::require_double(archive, "aux_adam_beta1"), kAuxAdamBeta1) ||
        !finite_equal(mmltk::backend::ml::serialization::require_double(archive, "aux_adam_beta2"), kAuxAdamBeta2) ||
        !finite_equal(mmltk::backend::ml::serialization::require_double(archive, "aux_adam_eps"), kAuxAdamEps)) {
        throw std::runtime_error(
            "native Muon archive hyperparameters do not match "
            "the compiled optimizer");
    }
    validate_archive_layout_counts(archive, groups_.size(), params_.size(), "native Muon archive layout does not match the current optimizer");
    auto candidate_groups = groups_;
    read_optimizer_group_archives(archive, candidate_groups,
                                  "native Muon archive parameter group size does not match the "
                                  "current optimizer",
                                  "native Muon archive parameter group order does not match the "
                                  "current optimizer",
                                  [](auto& group_archive, auto& config) {
                                      config.momentum = mmltk::backend::ml::serialization::require_double(group_archive, "momentum");
                                      config.use_muon = mmltk::backend::ml::serialization::require_int(group_archive, "use_muon") != 0;
                                      config.nesterov = mmltk::backend::ml::serialization::require_int(group_archive, "nesterov") != 0;
                                  });
    for (const auto& group : candidate_groups) {
        if (!std::isfinite(group.config.lr) || group.config.lr < 0.0 || !std::isfinite(group.config.weight_decay) || group.config.weight_decay < 0.0 ||
            !std::isfinite(group.config.momentum) || group.config.momentum < 0.0 || group.config.momentum > 1.0) {
            throw std::runtime_error("native Muon archive parameter group does not match the current optimizer");
        }
    }
    const std::vector<bool> use_muon = collect_muon_param_flags(candidate_groups, params_.size());
    std::vector<NativeMuonParamState> candidate_state(params_.size());
    read_indexed_optimizer_archive(archive, "param", params_.size(), [&](const size_t index, auto& param_archive) {
        validate_named_parameter_archive(param_archive, params_[index].name,
                                         "native Muon archive parameter order "
                                         "does not match the current model");
        const bool stored_use_muon = mmltk::backend::ml::serialization::require_int(param_archive, "use_muon") != 0;
        if (stored_use_muon != use_muon[index]) {
            throw std::runtime_error(
                "native Muon archive parameter routing does "
                "not match the current optimizer");
        }
        const auto& param = params_[index].tensor;
        auto& state = candidate_state[index];
        if (stored_use_muon) {
            auto momentum_buffer = require_parameter_state_tensor(param_archive, "momentum_buffer", param,
                                                                  "native Muon archive momentum tensor shape does not match the "
                                                                  "current model");
            state.step = 0;
            state.exp_avg = torch::Tensor();
            state.exp_avg_sq = torch::Tensor();
            state.momentum_buffer = std::move(momentum_buffer);
            return;
        }
        const auto step = mmltk::backend::ml::serialization::require_int(param_archive, "step");
        if (step < 0) { throw std::runtime_error("native Muon archive step does not match the current optimizer"); }
        auto exp_avg = require_parameter_state_tensor(param_archive, "exp_avg", param,
                                                      "native Muon archive AuxAdam tensor shape does not match the "
                                                      "current model");
        auto exp_avg_sq = require_parameter_state_tensor(param_archive, "exp_avg_sq", param,
                                                         "native Muon archive AuxAdam tensor shape does not match the "
                                                         "current model");
        state.step = step;
        state.momentum_buffer = torch::Tensor();
        state.exp_avg = std::move(exp_avg);
        state.exp_avg_sq = std::move(exp_avg_sq);
    });
    groups_.swap(candidate_groups);
    state_.swap(candidate_state);
}
std::vector<std::string> NativeMuonWithAuxAdam::InspectCheckpoint(torch::serialize::InputArchive& archive,
                                                                  const std::unordered_map<std::string, torch::Tensor>& tensors) {
    return inspect_checkpoint<NativeMuonWithAuxAdam>(archive, tensors);
}
void NativeMuonWithAuxAdam::commit(NativeMuonWithAuxAdam candidate) noexcept {
    groups_.swap(candidate.groups_);
    state_.swap(candidate.state_);
}
NativeOptimizer::NativeOptimizer(NativeAdamW optimizer) : storage_(std::move(optimizer)) {}
NativeOptimizer::NativeOptimizer(NativeMuonWithAuxAdam optimizer) : storage_(std::move(optimizer)) {}
TrainOptimizerKind NativeOptimizer::kind() const {
    return std::holds_alternative<NativeAdamW>(storage_) ? TrainOptimizerKind::AdamW : TrainOptimizerKind::Muon;
}
std::string_view NativeOptimizer::kind_name() const { return cli_enum_spelling(kind()); }
NativeOptimizer NativeOptimizer::stage_load(torch::serialize::InputArchive& archive) const {
    NativeOptimizer candidate = *this;
    candidate.load(archive);
    return candidate;
}
void NativeOptimizer::commit(NativeOptimizer candidate) noexcept {
    if (std::holds_alternative<NativeAdamW>(storage_)) {
        std::get<NativeAdamW>(storage_).commit(std::get<NativeAdamW>(std::move(candidate.storage_)));
        return;
    }
    std::get<NativeMuonWithAuxAdam>(storage_).commit(std::get<NativeMuonWithAuxAdam>(std::move(candidate.storage_)));
}
const char* NativeOptimizer::backend_name() const {
    return std::visit([](const auto& optimizer) { return optimizer.backend_name(); }, storage_);
}
std::vector<torch::Tensor>& NativeOptimizer::parameters() {
    return std::visit([](auto& optimizer) -> std::vector<torch::Tensor>& { return optimizer.parameters(); }, storage_);
}
const std::vector<torch::Tensor>& NativeOptimizer::parameters() const {
    return std::visit([](const auto& optimizer) -> const std::vector<torch::Tensor>& { return optimizer.parameters(); }, storage_);
}
const std::vector<std::string>& NativeOptimizer::parameter_names() const {
    return std::visit([](const auto& optimizer) -> const std::vector<std::string>& { return optimizer.parameter_names(); }, storage_);
}
void NativeOptimizer::zero_grad(const bool set_to_none) {
    std::visit([&](auto& optimizer) { optimizer.zero_grad(set_to_none); }, storage_);
}
void NativeOptimizer::set_lrs(const std::vector<double>& base_lrs, const double scale) {
    std::visit([&](auto& optimizer) { optimizer.set_lrs(base_lrs, scale); }, storage_);
}
void NativeOptimizer::set_muon_momentum(const double momentum) {
    std::visit(
        [&](auto& optimizer) {
            using Optimizer = std::decay_t<decltype(optimizer)>;
            if constexpr (std::is_same_v<Optimizer, NativeMuonWithAuxAdam>) {
                optimizer.set_muon_momentum(momentum);
            } else {
                (void)momentum;
            }
        },
        storage_);
}
void NativeOptimizer::step() {
    std::visit([](auto& optimizer) { optimizer.step(); }, storage_);
}
void NativeOptimizer::reserve_checkpoint(mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const {
    std::visit([&](const auto& optimizer) { optimizer.reserve_checkpoint(readback, first_slot); }, storage_);
}
void NativeOptimizer::save(torch::serialize::OutputArchive& archive, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) const {
    std::visit([&](const auto& optimizer) { optimizer.save(archive, readback, first_slot); }, storage_);
}
void NativeOptimizer::load(torch::serialize::InputArchive& archive) {
    std::visit([&](auto& optimizer) { optimizer.load(archive); }, storage_);
}
bool is_encoder_param(std::string_view name) { return name.find("backbone.0.encoder") != std::string_view::npos; }
bool encoder_zero_weight_decay(std::string_view name) {
    return name.find("gamma") != std::string_view::npos || name.find("bias") != std::string_view::npos || name.find("norm") != std::string_view::npos ||
           name.find("embeddings") != std::string_view::npos || name.find("position_embeddings") != std::string_view::npos ||
           name.find("rel_pos") != std::string_view::npos;
}
int encoder_layer_id(std::string_view name) {
    constexpr int kNumVitLayers = 13;
    if (name.find("embeddings") != std::string_view::npos) { return 0; }
    const auto layer_pos = name.find(".layer.");
    if (layer_pos != std::string_view::npos && name.find(".residual.") == std::string_view::npos) {
        const auto digits_pos = layer_pos + std::string_view(".layer.").size();
        size_t digits_len = 0;
        while (digits_pos + digits_len < name.size() && std::isdigit(static_cast<unsigned char>(name[digits_pos + digits_len]))) { ++digits_len; }
        if (digits_len > 0) { return std::stoi(std::string(name.substr(digits_pos, digits_len))) + 1; }
    }
    return kNumVitLayers + 1;
}
double parameter_lr(std::string_view name, const TrainRequest& options) {
    if (is_encoder_param(name)) {
        constexpr int kNumVitLayers = 13;
        const int layer_id = encoder_layer_id(name);
        return options.lr_encoder * std::pow(options.encoder_layer_decay, static_cast<double>(kNumVitLayers + 1 - layer_id)) * options.lr_component_decay *
               options.lr_component_decay;
    }
    if (name.find("transformer.decoder") != std::string_view::npos) { return options.lr * options.lr_component_decay; }
    return options.lr;
}
double parameter_weight_decay(std::string_view name, const TrainRequest& options) {
    if (is_encoder_param(name)) { return encoder_zero_weight_decay(name) ? 0.0 : options.weight_decay; }
    return options.weight_decay;
}
// Builds the group/parameter inputs shared by the native optimizers: one NamedParameter per trainable
// parameter and one Group per bucket, recording each bucket's base lr and moving its parameter indices into
// the optimizer group. group_config maps a bucket to the optimizer-specific group configuration.
template <typename Optimizer, typename Groups, typename NamedParams, typename GroupConfigFn>
std::pair<std::vector<typename Optimizer::Group>, std::vector<typename Optimizer::NamedParameter>> build_optimizer_inputs(Groups& groups,
                                                                                                                          const NamedParams& named_params,
                                                                                                                          std::vector<double>& base_lrs,
                                                                                                                          GroupConfigFn&& group_config) {
    std::vector<typename Optimizer::Group> optimizer_groups;
    std::vector<typename Optimizer::NamedParameter> optimizer_params;
    optimizer_groups.reserve(groups.size());
    optimizer_params.reserve(named_params.size());
    for (const auto& named_param : named_params) { optimizer_params.push_back(typename Optimizer::NamedParameter{named_param.first, named_param.second}); }
    for (auto& group : groups) {
        base_lrs.push_back(group.lr);
        optimizer_groups.push_back(typename Optimizer::Group{group_config(group), std::move(group.param_indices)});
    }
    return {std::move(optimizer_groups), std::move(optimizer_params)};
}
OptimizerBuildResult build_optimizer(NativeRfDetrModel& model, const TrainRequest& options) {
    struct GroupSpec {
        double lr = 0.0;
        double weight_decay = 0.0;
        bool use_muon = false;
        std::vector<size_t> param_indices;
    };
    std::unordered_map<std::string, size_t> group_index;
    std::vector<GroupSpec> groups;
    std::vector<std::pair<std::string, torch::Tensor>> named_params;
    for (const auto& item : model.named_parameters(true)) {
        const auto& param = item.value();
        if (!param.requires_grad()) { continue; }
        const double lr = parameter_lr(item.key(), options);
        const double weight_decay = parameter_weight_decay(item.key(), options);
        const bool use_muon = options.optimizer == TrainOptimizerKind::Muon && muon_parameter_eligible(item.key(), param);
        const std::string key = std::format("{}{}:{}", use_muon ? "muon:" : "aux:", lr, weight_decay);
        auto found = group_index.find(key);
        if (found == group_index.end()) {
            found = group_index.emplace(key, groups.size()).first;
            groups.push_back(GroupSpec{lr, weight_decay, use_muon, {}});
        }
        named_params.emplace_back(item.key(), param);
        groups[found->second].param_indices.push_back(named_params.size() - 1);
    }
    if (groups.empty() || named_params.empty()) { throw std::runtime_error("RF-DETR runtime found no trainable parameters"); }
    std::vector<double> base_lrs;
    base_lrs.reserve(groups.size());
    if (options.optimizer == TrainOptimizerKind::Muon) {
        auto [optimizer_groups, optimizer_params] = build_optimizer_inputs<NativeMuonWithAuxAdam>(
            groups, named_params, base_lrs,
            [&options](const GroupSpec& group) { return NativeMuonGroupConfig{group.lr, group.weight_decay, options.momentum, group.use_muon, true}; });
        return OptimizerBuildResult{
            NativeOptimizer(NativeMuonWithAuxAdam(std::move(optimizer_groups), std::move(optimizer_params))),
            std::move(base_lrs),
        };
    }
    auto [optimizer_groups, optimizer_params] = build_optimizer_inputs<NativeAdamW>(
        groups, named_params, base_lrs, [](const GroupSpec& group) { return NativeAdamWGroupConfig{group.lr, group.weight_decay, false}; });
    std::vector<torch::Tensor> trainable_params;
    trainable_params.reserve(optimizer_params.size());
    for (const auto& named_param : optimizer_params) { trainable_params.push_back(named_param.tensor); }
    const auto backend = (options.fused_optimizer && native_optimizer_supports_fused(trainable_params)) ? NativeOptimizerBackend::fused
                         : native_optimizer_supports_foreach(trainable_params)                          ? NativeOptimizerBackend::foreach
                                                                                                        : NativeOptimizerBackend::eager;
    return OptimizerBuildResult{
        NativeOptimizer(NativeAdamW(std::move(optimizer_groups), std::move(optimizer_params), backend)),
        std::move(base_lrs),
    };
}
}  // namespace mmltk::backend::models::rfdetr
