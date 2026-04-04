#include "rfdetr/native_optimizer.h"

#include "rfdetr/archive_utils.h"
#include "rfdetr/checkpoint_internal.h"

#include <ATen/cuda/CUDAContext.h>
#include <ATen/ops/_fused_adamw.h>

#include <c10/util/Exception.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <format>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace mmltk::rfdetr {

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

NativeOptimizerBackend parse_backend_name(const std::string& name) {
    if (name == "eager") {
        return NativeOptimizerBackend::eager;
    }
    if (name == "foreach") {
        return NativeOptimizerBackend::foreach;
    }
    if (name == "fused") {
        return NativeOptimizerBackend::fused;
    }
    throw std::runtime_error("unknown native AdamW backend in archive: " + name);
}

std::string tensor_signature(const torch::Tensor& tensor) {
    std::ostringstream stream;
    stream << "dtype=" << tensor.scalar_type()
           << " device=" << tensor.device()
           << " layout=" << tensor.layout()
           << " sizes=" << tensor.sizes()
           << " strides=" << tensor.strides();
    return std::move(stream).str();
}

torch::Tensor align_tensor_like_param(const torch::Tensor& source, const torch::Tensor& param) {
    auto aligned = torch::zeros_like(param);
    if (source.defined()) {
        aligned.copy_(source.to(param.device(), param.scalar_type(), false, false));
    }
    return aligned;
}

void ensure_aligned(torch::Tensor& t, const torch::Tensor& param) {
    if (!t.defined() || t.sizes() != param.sizes())
        t = torch::zeros_like(param);
    if (t.device() != param.device() || t.scalar_type() != param.scalar_type())
        t = align_tensor_like_param(t, param);
}

torch::Device step_device_for_backend(const torch::Tensor& param, NativeOptimizerBackend backend) {
    if (backend == NativeOptimizerBackend::fused || backend == NativeOptimizerBackend::foreach) {
        return param.device();
    }
    return {torch::kCPU};
}

torch::Tensor make_step_tensor(const torch::Tensor& param, NativeOptimizerBackend backend) {
    return torch::zeros({}, torch::TensorOptions().dtype(torch::kFloat32).device(step_device_for_backend(param, backend)));
}

void validate_group_indices(const NativeAdamW::Group& group, size_t param_count) {
    for (const size_t index : group.param_indices) {
        if (index >= param_count) {
            throw std::runtime_error("native AdamW parameter group index is out of range");
        }
    }
}

void validate_group_indices(const NativeMuonWithAuxAdam::Group& group, size_t param_count) {
    for (const size_t index : group.param_indices) {
        if (index >= param_count) {
            throw std::runtime_error("native Muon parameter group index is out of range");
        }
    }
}

torch::Tensor muon_zeropower_via_newtonschulz5(const torch::Tensor& grad, const int64_t steps = kMuonNsSteps) {
    if (grad.dim() < 2) {
        throw std::runtime_error("native Muon zeropower requires tensors with rank >= 2");
    }

    auto update = grad.to(torch::kBFloat16);
    const bool transpose = update.size(-2) > update.size(-1);
    if (transpose) {
        update = update.transpose(-2, -1);
    }
    const auto denom = update.flatten(-2).norm(2, -1, true).unsqueeze(-1).add(kMuonNormEps);
    update = update / denom;
    for (int64_t step = 0; step < steps; ++step) {
        auto gram = torch::matmul(update, update.transpose(-2, -1));
        auto poly = gram.mul(kMuonCoeffB).add(torch::matmul(gram, gram), kMuonCoeffC);
        update = update.mul(kMuonCoeffA).add_(torch::matmul(poly, update));
    }
    if (transpose) {
        update = update.transpose(-2, -1);
    }
    return update;
}

torch::Tensor muon_update(const torch::Tensor& grad,
                          torch::Tensor& momentum,
                          const double beta,
                          const bool nesterov) {
    momentum.lerp_(grad, 1.0 - beta);
    auto update = nesterov ? grad.lerp(momentum, beta) : momentum;
    if (update.dim() == 4) {
        update = update.view({update.size(0), -1});
    }
    update = muon_zeropower_via_newtonschulz5(update);
    update.mul_(std::sqrt(std::max(1.0, static_cast<double>(update.size(-2)) / static_cast<double>(update.size(-1)))));
    return update;
}

torch::Tensor adam_update(const torch::Tensor& grad,
                          torch::Tensor& exp_avg,
                          torch::Tensor& exp_avg_sq,
                          const int64_t step) {
    exp_avg.lerp_(grad, 1.0 - kAuxAdamBeta1);
    exp_avg_sq.lerp_(grad.square(), 1.0 - kAuxAdamBeta2);
    const double bias_correction1 = 1.0 - std::pow(kAuxAdamBeta1, static_cast<double>(step));
    const double bias_correction2 = 1.0 - std::pow(kAuxAdamBeta2, static_cast<double>(step));
    auto exp_avg_corrected = exp_avg / bias_correction1;
    auto exp_avg_sq_corrected = exp_avg_sq / bias_correction2;
    return exp_avg_corrected / (exp_avg_sq_corrected.sqrt() + kAuxAdamEps);
}

} // namespace

const char* train_optimizer_kind_name(const TrainOptimizerKind kind) {
    switch (kind) {
    case TrainOptimizerKind::AdamW:
        return "adamw";
    case TrainOptimizerKind::Muon:
        return "muon";
    }
    return "unknown";
}

const char* native_optimizer_backend_name(const NativeOptimizerBackend backend) {
    switch (backend) {
    case NativeOptimizerBackend::eager:
        return "eager";
    case NativeOptimizerBackend::foreach:
        return "foreach";
    case NativeOptimizerBackend::fused:
        return "fused";
    }
    return "unknown";
}

bool native_optimizer_supports_foreach(const std::vector<torch::Tensor>& params) {
    if (params.empty()) {
        return false;
    }
    for (const auto& param : params) {
        if (!param.defined() || !param.is_floating_point() || torch::is_complex(param)) {
            return false;
        }
    }
    return true;
}

bool native_optimizer_supports_fused(const std::vector<torch::Tensor>& params) {
    if (!native_optimizer_supports_foreach(params)) {
        return false;
    }
    for (const auto& param : params) {
        if (!param.device().is_cuda()) {
            return false;
        }
        const auto device_index = static_cast<int>(param.device().index());
        if (device_index < 0) {
            return false;
        }
        const auto* properties = at::cuda::getDeviceProperties(static_cast<c10::DeviceIndex>(device_index));
        if (properties == nullptr || properties->major < 8) {
            return false;
        }
    }
    return true;
}

NativeAdamW::NativeAdamW(std::vector<Group> groups,
                         std::vector<NamedParameter> params,
                         const NativeOptimizerBackend backend)
    : groups_(std::move(groups)), params_(std::move(params)), backend_(backend) {
    all_params_.reserve(params_.size());
    all_param_names_.reserve(params_.size());
    for (const auto& param : params_) {
        if (!param.tensor.defined()) {
            throw std::runtime_error("native AdamW received an undefined parameter tensor");
        }
        all_params_.push_back(param.tensor);
        all_param_names_.push_back(param.name);
    }
    for (const auto& group : groups_) {
        validate_group_indices(group, params_.size());
    }
    initialize_state();
}

std::vector<torch::Tensor>& NativeAdamW::parameters() {
    return all_params_;
}

const std::vector<torch::Tensor>& NativeAdamW::parameters() const {
    return all_params_;
}

const std::vector<std::string>& NativeAdamW::parameter_names() const {
    return all_param_names_;
}

const std::vector<NativeAdamW::Group>& NativeAdamW::groups() const {
    return groups_;
}

NativeOptimizerBackend NativeAdamW::backend() const {
    return backend_;
}

const char* NativeAdamW::backend_name() const {
    return native_optimizer_backend_name(backend_);
}

void NativeAdamW::initialize_state() {
    state_.clear();
    state_.reserve(params_.size());

    std::vector<bool> needs_amsgrad(params_.size(), false);
    for (const auto& group : groups_) {
        for (const auto index : group.param_indices) {
            needs_amsgrad[index] = group.config.amsgrad;
        }
    }

    for (size_t index = 0; index < params_.size(); ++index) {
        const auto& param = params_[index].tensor;
        ParamState state;
        state.step = make_step_tensor(param, backend_);
        state.exp_avg = torch::zeros_like(param);
        state.exp_avg_sq = torch::zeros_like(param);
        if (needs_amsgrad[index]) {
            state.max_exp_avg_sq = torch::zeros_like(param);
        }
        state_.push_back(std::move(state));
    }
}

void NativeAdamW::zero_grad(const bool set_to_none) {
    torch::NoGradGuard no_grad;
    for (auto& param : all_params_) {
        if (!param.grad().defined()) {
            continue;
        }
        if (set_to_none) {
            param.mutable_grad() = torch::Tensor();
            continue;
        }
        auto& grad = param.mutable_grad();
        grad.detach_();
        grad.zero_();
    }
}

void NativeAdamW::set_lrs(const std::vector<double>& base_lrs, const double scale) {
    if (base_lrs.size() != groups_.size()) {
        throw std::runtime_error("native AdamW base LR count does not match param group count");
    }
    for (size_t index = 0; index < groups_.size(); ++index) {
        groups_[index].config.lr = base_lrs[index] * scale;
    }
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
    for (const auto index : group.param_indices) {
        auto& param = params_[index].tensor;
        if (!param.grad().defined()) {
            continue;
        }
        auto grad = param.grad();
        if (grad.is_sparse()) {
            throw std::runtime_error("native AdamW does not support sparse gradients");
        }
        if (torch::is_complex(param)) {
            throw std::runtime_error("native AdamW does not support complex parameters");
        }

        auto& state = state_[index];
        state.step.add_(1.0);
        const auto step_value = state.step.item<double>();

        if (group.config.weight_decay != 0.0) {
            param.mul_(1.0 - group.config.lr * group.config.weight_decay);
        }

        state.exp_avg.mul_(kAdamBeta1).add_(grad, 1.0 - kAdamBeta1);
        state.exp_avg_sq.mul_(kAdamBeta2).addcmul_(grad, grad, 1.0 - kAdamBeta2);

        const double bias_correction1 = 1.0 - std::pow(kAdamBeta1, step_value);
        const double bias_correction2 = 1.0 - std::pow(kAdamBeta2, step_value);
        torch::Tensor denom;
        if (group.config.amsgrad) {
            if (!state.max_exp_avg_sq.defined()) {
                state.max_exp_avg_sq = torch::zeros_like(param);
            }
            state.max_exp_avg_sq = torch::maximum(state.max_exp_avg_sq, state.exp_avg_sq);
            denom = state.max_exp_avg_sq.sqrt();
        } else {
            denom = state.exp_avg_sq.sqrt();
        }
        denom.div_(std::sqrt(bias_correction2)).add_(kAdamEps);
        param.addcdiv_(state.exp_avg, denom, -group.config.lr / bias_correction1);
    }
}

void NativeAdamW::step_group_foreach(const Group& group) {
    struct ForeachBatch {
        std::vector<torch::Tensor> params;
        std::vector<torch::Tensor> grads;
        std::vector<torch::Tensor> exp_avgs;
        std::vector<torch::Tensor> exp_avg_sqs;
        std::vector<torch::Tensor> max_exp_avg_sqs;
        std::vector<torch::Tensor> steps;
    };

    std::map<std::pair<int, int>, ForeachBatch> batches;
    for (const auto index : group.param_indices) {
        auto& param = params_[index].tensor;
        if (!param.grad().defined()) {
            continue;
        }
        auto grad = param.grad();
        if (grad.is_sparse()) {
            throw std::runtime_error("native AdamW does not support sparse gradients");
        }
        if (torch::is_complex(param)) {
            throw std::runtime_error("native AdamW does not support complex parameters");
        }

        auto& state = state_[index];
        if (!state.step.defined() || state.step.device() != param.device()) {
            state.step = state.step.to(param.device(), torch::kFloat32).contiguous();
        }
        if (state.step.scalar_type() != torch::kFloat32) {
            state.step = state.step.to(param.device(), torch::kFloat32).contiguous();
        }
        ensure_aligned(state.exp_avg, param);
        ensure_aligned(state.exp_avg_sq, param);
        ensure_aligned(grad, param);

        const auto device_index = static_cast<int>(param.device().index());
        const auto key = std::make_pair(device_index, static_cast<int>(param.scalar_type()));
        auto& batch = batches[key];
        batch.params.push_back(param);
        batch.grads.push_back(grad);
        batch.exp_avgs.push_back(state.exp_avg);
        batch.exp_avg_sqs.push_back(state.exp_avg_sq);
        if (group.config.amsgrad) {
            ensure_aligned(state.max_exp_avg_sq, param);
            batch.max_exp_avg_sqs.push_back(state.max_exp_avg_sq);
        }
        batch.steps.push_back(state.step);
    }

    for (auto& [_, batch] : batches) {
        if (batch.params.empty()) {
            continue;
        }

        torch::_foreach_add_(batch.steps, 1.0);

        if (group.config.weight_decay != 0.0) {
            torch::_foreach_mul_(batch.params, 1.0 - group.config.lr * group.config.weight_decay);
        }

        torch::_foreach_mul_(batch.exp_avgs, kAdamBeta1);
        torch::_foreach_add_(batch.exp_avgs, batch.grads, 1.0 - kAdamBeta1);
        torch::_foreach_mul_(batch.exp_avg_sqs, kAdamBeta2);
        torch::_foreach_addcmul_(batch.exp_avg_sqs, batch.grads, batch.grads, 1.0 - kAdamBeta2);

        const auto step_val = batch.steps[0].item<double>();
        const double bias_correction1 = 1.0 - std::pow(kAdamBeta1, step_val);
        const double bias_correction2 = 1.0 - std::pow(kAdamBeta2, step_val);
        const double step_size = group.config.lr / bias_correction1;
        const double bias_correction2_sqrt = std::sqrt(bias_correction2);

        if (group.config.amsgrad) {
            torch::_foreach_maximum_(batch.max_exp_avg_sqs, batch.exp_avg_sqs);
            auto denoms = torch::_foreach_sqrt(batch.max_exp_avg_sqs);
            torch::_foreach_div_(denoms, bias_correction2_sqrt);
            torch::_foreach_add_(denoms, kAdamEps);
            torch::_foreach_addcdiv_(batch.params, batch.exp_avgs, denoms, -step_size);
        } else {
            auto denoms = torch::_foreach_sqrt(batch.exp_avg_sqs);
            torch::_foreach_div_(denoms, bias_correction2_sqrt);
            torch::_foreach_add_(denoms, kAdamEps);
            torch::_foreach_addcdiv_(batch.params, batch.exp_avgs, denoms, -step_size);
        }
    }
}

void NativeAdamW::step_group_fused(const Group& group) {
    struct FusedBatch {
        std::vector<torch::Tensor> params;
        std::vector<torch::Tensor> grads;
        std::vector<torch::Tensor> exp_avgs;
        std::vector<torch::Tensor> exp_avg_sqs;
        std::vector<torch::Tensor> max_exp_avg_sqs;
        std::vector<torch::Tensor> steps;
    };

    std::map<std::pair<int, int>, FusedBatch> batches;
    for (const auto index : group.param_indices) {
        auto& param = params_[index].tensor;
        if (!param.grad().defined()) {
            continue;
        }
        auto grad = param.grad();
        if (grad.is_sparse()) {
            throw std::runtime_error("native AdamW does not support sparse gradients");
        }
        if (!param.device().is_cuda()) {
            throw std::runtime_error("fused native AdamW requires CUDA parameters");
        }
        if (!param.is_floating_point() || torch::is_complex(param)) {
            throw std::runtime_error("fused native AdamW requires real floating-point parameters");
        }

        auto& state = state_[index];
        if (!state.step.defined() || !state.step.device().is_cuda()) {
            state.step = state.step.to(param.device(), torch::kFloat32).contiguous();
        }
        if (state.step.scalar_type() != torch::kFloat32) {
            state.step = state.step.to(param.device(), torch::kFloat32).contiguous();
        }
        ensure_aligned(state.exp_avg, param);
        ensure_aligned(state.exp_avg_sq, param);
        ensure_aligned(grad, param);

        const auto device_index = static_cast<int>(param.device().index());
        const auto key = std::make_pair(device_index, static_cast<int>(param.scalar_type()));
        auto& batch = batches[key];
        batch.params.push_back(param);
        batch.grads.push_back(grad);
        batch.exp_avgs.push_back(state.exp_avg);
        batch.exp_avg_sqs.push_back(state.exp_avg_sq);
        if (group.config.amsgrad) {
            ensure_aligned(state.max_exp_avg_sq, param);
            batch.max_exp_avg_sqs.push_back(state.max_exp_avg_sq);
        }
        batch.steps.push_back(state.step);
    }

    for (auto& [_, batch] : batches) {
        if (batch.params.empty()) {
            continue;
        }
        torch::_foreach_add_(batch.steps, 1);
        at::_fused_adamw_(batch.params,
                          batch.grads,
                          batch.exp_avgs,
                          batch.exp_avg_sqs,
                          batch.max_exp_avg_sqs,
                          batch.steps,
                          group.config.lr,
                          kAdamBeta1,
                          kAdamBeta2,
                          group.config.weight_decay,
                          kAdamEps,
                          group.config.amsgrad,
                          false,
                          std::nullopt,
                          std::nullopt);
    }
}

void NativeAdamW::save(torch::serialize::OutputArchive& archive) const {
    write_string(archive, "format", kNativeAdamWFormat);
    write_int(archive, "format_version", kNativeAdamWFormatVersion);
    write_string(archive, "backend", backend_name());
    write_double(archive, "beta1", kAdamBeta1);
    write_double(archive, "beta2", kAdamBeta2);
    write_double(archive, "eps", kAdamEps);
    write_int(archive, "group_count", static_cast<int64_t>(groups_.size()));
    write_int(archive, "param_count", static_cast<int64_t>(params_.size()));

    for (size_t index = 0; index < groups_.size(); ++index) {
        torch::serialize::OutputArchive group_archive;
        write_double(group_archive, "lr", groups_[index].config.lr);
        write_double(group_archive, "weight_decay", groups_[index].config.weight_decay);
        write_int(group_archive, "amsgrad", groups_[index].config.amsgrad ? 1 : 0);
        write_int(group_archive, "param_index_count", static_cast<int64_t>(groups_[index].param_indices.size()));
        for (size_t param_index = 0; param_index < groups_[index].param_indices.size(); ++param_index) {
            write_int(group_archive,
                      archive_entry_name("param_index", param_index).c_str(),
                      static_cast<int64_t>(groups_[index].param_indices[param_index]));
        }
        archive.write(archive_entry_name("group", index), group_archive);
    }

    for (size_t index = 0; index < params_.size(); ++index) {
        torch::serialize::OutputArchive param_archive;
        write_string(param_archive, "name", params_[index].name);
        param_archive.write("step", detail::prepare_tensor_for_checkpoint_write(state_[index].step));
        param_archive.write("exp_avg", detail::prepare_tensor_for_checkpoint_write(state_[index].exp_avg));
        param_archive.write("exp_avg_sq", detail::prepare_tensor_for_checkpoint_write(state_[index].exp_avg_sq));
        write_int(param_archive, "has_max_exp_avg_sq", state_[index].max_exp_avg_sq.defined() ? 1 : 0);
        if (state_[index].max_exp_avg_sq.defined()) {
            param_archive.write("max_exp_avg_sq", detail::prepare_tensor_for_checkpoint_write(state_[index].max_exp_avg_sq));
        }
        archive.write(archive_entry_name("param", index), param_archive);
    }
}

void NativeAdamW::load(torch::serialize::InputArchive& archive) {
    c10::IValue format_value;
    if (!archive.try_read("format", format_value) || !format_value.isString() ||
        format_value.toStringRef() != kNativeAdamWFormat) {
        throw std::runtime_error(
            "native RF-DETR resume checkpoint uses a legacy optimizer archive; rebuild the checkpoint with the current mmltk");
    }
    const auto version = require_int(archive, "format_version");
    if (version != kNativeAdamWFormatVersion) {
        throw std::runtime_error(std::format("unsupported native AdamW archive version: {}", version));
    }

    const auto stored_backend = parse_backend_name(require_string(archive, "backend"));
    (void)stored_backend;
    const auto stored_beta1 = require_double(archive, "beta1");
    const auto stored_beta2 = require_double(archive, "beta2");
    const auto stored_eps = require_double(archive, "eps");
    if (std::abs(stored_beta1 - kAdamBeta1) > 1.0e-12 || std::abs(stored_beta2 - kAdamBeta2) > 1.0e-12 ||
        std::abs(stored_eps - kAdamEps) > 1.0e-12) {
        throw std::runtime_error("native AdamW archive hyperparameters do not match the compiled optimizer");
    }

    const auto group_count = require_int(archive, "group_count");
    const auto param_count = require_int(archive, "param_count");
    if (group_count != static_cast<int64_t>(groups_.size()) || param_count != static_cast<int64_t>(params_.size())) {
        throw std::runtime_error("native AdamW archive layout does not match the current optimizer");
    }

    for (size_t index = 0; index < groups_.size(); ++index) {
        torch::serialize::InputArchive group_archive;
        archive.read(archive_entry_name("group", index), group_archive);
        groups_[index].config.lr = require_double(group_archive, "lr");
        groups_[index].config.weight_decay = require_double(group_archive, "weight_decay");
        groups_[index].config.amsgrad = require_int(group_archive, "amsgrad") != 0;
        const auto param_index_count = require_int(group_archive, "param_index_count");
        if (param_index_count != static_cast<int64_t>(groups_[index].param_indices.size())) {
            throw std::runtime_error("native AdamW archive parameter group size does not match the current optimizer");
        }
        for (size_t param_index = 0; param_index < groups_[index].param_indices.size(); ++param_index) {
            const auto stored_param_index = require_int(group_archive, archive_entry_name("param_index", param_index).c_str());
            if (stored_param_index != static_cast<int64_t>(groups_[index].param_indices[param_index])) {
                throw std::runtime_error("native AdamW archive parameter group order does not match the current optimizer");
            }
        }
    }

    for (size_t index = 0; index < params_.size(); ++index) {
        torch::serialize::InputArchive param_archive;
        archive.read(archive_entry_name("param", index), param_archive);
        const auto stored_name = require_string(param_archive, "name");
        if (stored_name != params_[index].name) {
            throw std::runtime_error("native AdamW archive parameter order does not match the current model");
        }

        const auto& param = params_[index].tensor;
        auto loaded_step = require_tensor(param_archive, "step");
        auto loaded_exp_avg = require_tensor(param_archive, "exp_avg");
        auto loaded_exp_avg_sq = require_tensor(param_archive, "exp_avg_sq");
        const bool has_max_exp_avg_sq = require_int(param_archive, "has_max_exp_avg_sq") != 0;

        if (loaded_exp_avg.sizes() != param.sizes() || loaded_exp_avg_sq.sizes() != param.sizes()) {
            throw std::runtime_error("native AdamW archive tensor shape does not match the current model");
        }

        state_[index].step = loaded_step.to(step_device_for_backend(param, backend_), torch::kFloat32).contiguous();
        state_[index].exp_avg = loaded_exp_avg.to(param.device(), param.scalar_type()).contiguous();
        state_[index].exp_avg_sq = loaded_exp_avg_sq.to(param.device(), param.scalar_type()).contiguous();
        if (has_max_exp_avg_sq) {
            auto loaded_max = require_tensor(param_archive, "max_exp_avg_sq");
            if (loaded_max.sizes() != param.sizes()) {
                throw std::runtime_error("native AdamW archive AMSGrad tensor shape does not match the current model");
            }
            state_[index].max_exp_avg_sq = loaded_max.to(param.device(), param.scalar_type()).contiguous();
        } else {
            state_[index].max_exp_avg_sq = torch::Tensor();
        }
    }
}

NativeMuonWithAuxAdam::NativeMuonWithAuxAdam(std::vector<Group> groups,
                                             std::vector<NamedParameter> params)
    : groups_(std::move(groups)), params_(std::move(params)) {
    all_params_.reserve(params_.size());
    all_param_names_.reserve(params_.size());
    for (const auto& param : params_) {
        if (!param.tensor.defined()) {
            throw std::runtime_error("native Muon received an undefined parameter tensor");
        }
        all_params_.push_back(param.tensor);
        all_param_names_.push_back(param.name);
    }
    for (const auto& group : groups_) {
        validate_group_indices(group, params_.size());
    }
    initialize_state();
}

std::vector<torch::Tensor>& NativeMuonWithAuxAdam::parameters() {
    return all_params_;
}

const std::vector<torch::Tensor>& NativeMuonWithAuxAdam::parameters() const {
    return all_params_;
}

const std::vector<std::string>& NativeMuonWithAuxAdam::parameter_names() const {
    return all_param_names_;
}

const std::vector<NativeMuonWithAuxAdam::Group>& NativeMuonWithAuxAdam::groups() const {
    return groups_;
}

const char* NativeMuonWithAuxAdam::backend_name() const {
    return "eager";
}

void NativeMuonWithAuxAdam::initialize_state() {
    state_.clear();
    state_.resize(params_.size());

    std::vector<bool> use_muon(params_.size(), false);
    for (const auto& group : groups_) {
        for (const auto index : group.param_indices) {
            use_muon[index] = group.config.use_muon;
        }
    }

    for (size_t index = 0; index < params_.size(); ++index) {
        const auto& param = params_[index].tensor;
        auto& state = state_[index];
        if (use_muon[index]) {
            state.momentum_buffer = torch::zeros_like(param);
        } else {
            state.exp_avg = torch::zeros_like(param);
            state.exp_avg_sq = torch::zeros_like(param);
        }
    }
}

void NativeMuonWithAuxAdam::zero_grad(const bool set_to_none) {
    torch::NoGradGuard no_grad;
    for (auto& param : all_params_) {
        if (!param.grad().defined()) {
            continue;
        }
        if (set_to_none) {
            param.mutable_grad() = torch::Tensor();
            continue;
        }
        auto& grad = param.mutable_grad();
        grad.detach_();
        grad.zero_();
    }
}

void NativeMuonWithAuxAdam::set_lrs(const std::vector<double>& base_lrs, const double scale) {
    if (base_lrs.size() != groups_.size()) {
        throw std::runtime_error("native Muon base LR count does not match param group count");
    }
    for (size_t index = 0; index < groups_.size(); ++index) {
        groups_[index].config.lr = base_lrs[index] * scale;
    }
}

void NativeMuonWithAuxAdam::set_muon_momentum(const double momentum) {
    for (auto& group : groups_) {
        if (!group.config.use_muon) {
            continue;
        }
        group.config.momentum = momentum;
    }
}

void NativeMuonWithAuxAdam::step() {
    torch::NoGradGuard no_grad;
    for (const auto& group : groups_) {
        for (const auto index : group.param_indices) {
            auto& param = params_[index].tensor;
            if (!param.is_floating_point() || torch::is_complex(param)) {
                throw std::runtime_error("native Muon requires real floating-point parameters");
            }

            auto grad = param.grad().defined() ? param.grad() : torch::zeros_like(param);
            if (grad.is_sparse()) {
                throw std::runtime_error("native Muon does not support sparse gradients");
            }
            if (grad.device() != param.device() || grad.scalar_type() != param.scalar_type()) {
                grad = align_tensor_like_param(grad, param);
            }

            auto& state = state_[index];
            if (group.config.use_muon) {
                ensure_aligned(state.momentum_buffer, param);
                auto update = muon_update(grad, state.momentum_buffer, group.config.momentum, group.config.nesterov);
                if (group.config.weight_decay != 0.0) {
                    param.mul_(1.0 - group.config.lr * group.config.weight_decay);
                }
                param.add_(update.reshape_as(param), -group.config.lr);
                continue;
            }

            ensure_aligned(state.exp_avg, param);
            ensure_aligned(state.exp_avg_sq, param);
            ++state.step;
            auto update = adam_update(grad, state.exp_avg, state.exp_avg_sq, state.step);
            if (group.config.weight_decay != 0.0) {
                param.mul_(1.0 - group.config.lr * group.config.weight_decay);
            }
            param.add_(update, -group.config.lr);
        }
    }
}

void NativeMuonWithAuxAdam::save(torch::serialize::OutputArchive& archive) const {
    write_string(archive, "format", kNativeMuonFormat);
    write_int(archive, "format_version", kNativeMuonFormatVersion);
    write_int(archive, "ns_steps", kMuonNsSteps);
    write_double(archive, "muon_coeff_a", kMuonCoeffA);
    write_double(archive, "muon_coeff_b", kMuonCoeffB);
    write_double(archive, "muon_coeff_c", kMuonCoeffC);
    write_double(archive, "muon_eps", kMuonNormEps);
    write_double(archive, "aux_adam_beta1", kAuxAdamBeta1);
    write_double(archive, "aux_adam_beta2", kAuxAdamBeta2);
    write_double(archive, "aux_adam_eps", kAuxAdamEps);
    write_int(archive, "group_count", static_cast<int64_t>(groups_.size()));
    write_int(archive, "param_count", static_cast<int64_t>(params_.size()));

    for (size_t index = 0; index < groups_.size(); ++index) {
        torch::serialize::OutputArchive group_archive;
        write_double(group_archive, "lr", groups_[index].config.lr);
        write_double(group_archive, "weight_decay", groups_[index].config.weight_decay);
        write_double(group_archive, "momentum", groups_[index].config.momentum);
        write_int(group_archive, "use_muon", groups_[index].config.use_muon ? 1 : 0);
        write_int(group_archive, "nesterov", groups_[index].config.nesterov ? 1 : 0);
        write_int(group_archive, "param_index_count", static_cast<int64_t>(groups_[index].param_indices.size()));
        for (size_t param_index = 0; param_index < groups_[index].param_indices.size(); ++param_index) {
            write_int(group_archive,
                      archive_entry_name("param_index", param_index).c_str(),
                      static_cast<int64_t>(groups_[index].param_indices[param_index]));
        }
        archive.write(archive_entry_name("group", index), group_archive);
    }

    for (size_t index = 0; index < params_.size(); ++index) {
        const auto use_muon = [&]() {
            for (const auto& group : groups_) {
                if (std::ranges::find(group.param_indices, index) != group.param_indices.end()) {
                    return group.config.use_muon;
                }
            }
            return false;
        }();

        torch::serialize::OutputArchive param_archive;
        write_string(param_archive, "name", params_[index].name);
        write_int(param_archive, "use_muon", use_muon ? 1 : 0);
        if (use_muon) {
            param_archive.write(
                "momentum_buffer",
                detail::prepare_tensor_for_checkpoint_write(state_[index].momentum_buffer));
        } else {
            write_int(param_archive, "step", state_[index].step);
            param_archive.write("exp_avg", detail::prepare_tensor_for_checkpoint_write(state_[index].exp_avg));
            param_archive.write("exp_avg_sq", detail::prepare_tensor_for_checkpoint_write(state_[index].exp_avg_sq));
        }
        archive.write(archive_entry_name("param", index), param_archive);
    }
}

void NativeMuonWithAuxAdam::load(torch::serialize::InputArchive& archive) {
    c10::IValue format_value;
    if (!archive.try_read("format", format_value) || !format_value.isString() ||
        format_value.toStringRef() != kNativeMuonFormat) {
        throw std::runtime_error(
            "native RF-DETR resume checkpoint uses a legacy Muon archive; rebuild the checkpoint with the current mmltk");
    }
    const auto version = require_int(archive, "format_version");
    if (version != kNativeMuonFormatVersion) {
        throw std::runtime_error(std::format("unsupported native Muon archive version: {}", version));
    }
    if (require_int(archive, "ns_steps") != kMuonNsSteps ||
        std::abs(require_double(archive, "muon_coeff_a") - kMuonCoeffA) > 1.0e-12 ||
        std::abs(require_double(archive, "muon_coeff_b") - kMuonCoeffB) > 1.0e-12 ||
        std::abs(require_double(archive, "muon_coeff_c") - kMuonCoeffC) > 1.0e-12 ||
        std::abs(require_double(archive, "muon_eps") - kMuonNormEps) > 1.0e-12 ||
        std::abs(require_double(archive, "aux_adam_beta1") - kAuxAdamBeta1) > 1.0e-12 ||
        std::abs(require_double(archive, "aux_adam_beta2") - kAuxAdamBeta2) > 1.0e-12 ||
        std::abs(require_double(archive, "aux_adam_eps") - kAuxAdamEps) > 1.0e-12) {
        throw std::runtime_error("native Muon archive hyperparameters do not match the compiled optimizer");
    }

    const auto group_count = require_int(archive, "group_count");
    const auto param_count = require_int(archive, "param_count");
    if (group_count != static_cast<int64_t>(groups_.size()) || param_count != static_cast<int64_t>(params_.size())) {
        throw std::runtime_error("native Muon archive layout does not match the current optimizer");
    }

    for (size_t index = 0; index < groups_.size(); ++index) {
        torch::serialize::InputArchive group_archive;
        archive.read(archive_entry_name("group", index), group_archive);
        groups_[index].config.lr = require_double(group_archive, "lr");
        groups_[index].config.weight_decay = require_double(group_archive, "weight_decay");
        groups_[index].config.momentum = require_double(group_archive, "momentum");
        groups_[index].config.use_muon = require_int(group_archive, "use_muon") != 0;
        groups_[index].config.nesterov = require_int(group_archive, "nesterov") != 0;
        const auto param_index_count = require_int(group_archive, "param_index_count");
        if (param_index_count != static_cast<int64_t>(groups_[index].param_indices.size())) {
            throw std::runtime_error("native Muon archive parameter group size does not match the current optimizer");
        }
        for (size_t param_index = 0; param_index < groups_[index].param_indices.size(); ++param_index) {
            const auto stored_param_index =
                require_int(group_archive, archive_entry_name("param_index", param_index).c_str());
            if (stored_param_index != static_cast<int64_t>(groups_[index].param_indices[param_index])) {
                throw std::runtime_error("native Muon archive parameter group order does not match the current optimizer");
            }
        }
    }

    std::vector<bool> use_muon(params_.size(), false);
    for (const auto& group : groups_) {
        for (const auto index : group.param_indices) {
            use_muon[index] = group.config.use_muon;
        }
    }

    for (size_t index = 0; index < params_.size(); ++index) {
        torch::serialize::InputArchive param_archive;
        archive.read(archive_entry_name("param", index), param_archive);
        if (require_string(param_archive, "name") != params_[index].name) {
            throw std::runtime_error("native Muon archive parameter order does not match the current model");
        }
        const bool stored_use_muon = require_int(param_archive, "use_muon") != 0;
        if (stored_use_muon != use_muon[index]) {
            throw std::runtime_error("native Muon archive parameter routing does not match the current optimizer");
        }

        const auto& param = params_[index].tensor;
        auto& state = state_[index];
        if (stored_use_muon) {
            auto momentum_buffer = require_tensor(param_archive, "momentum_buffer");
            if (momentum_buffer.sizes() != param.sizes()) {
                throw std::runtime_error("native Muon archive momentum tensor shape does not match the current model");
            }
            state.step = 0;
            state.exp_avg = torch::Tensor();
            state.exp_avg_sq = torch::Tensor();
            state.momentum_buffer = momentum_buffer.to(param.device(), param.scalar_type()).contiguous();
            continue;
        }

        const auto step = require_int(param_archive, "step");
        auto exp_avg = require_tensor(param_archive, "exp_avg");
        auto exp_avg_sq = require_tensor(param_archive, "exp_avg_sq");
        if (exp_avg.sizes() != param.sizes() || exp_avg_sq.sizes() != param.sizes()) {
            throw std::runtime_error("native Muon archive AuxAdam tensor shape does not match the current model");
        }
        state.step = step;
        state.momentum_buffer = torch::Tensor();
        state.exp_avg = exp_avg.to(param.device(), param.scalar_type()).contiguous();
        state.exp_avg_sq = exp_avg_sq.to(param.device(), param.scalar_type()).contiguous();
    }
}

NativeOptimizer::NativeOptimizer(NativeAdamW optimizer)
    : storage_(std::move(optimizer)) {}

NativeOptimizer::NativeOptimizer(NativeMuonWithAuxAdam optimizer)
    : storage_(std::move(optimizer)) {}

TrainOptimizerKind NativeOptimizer::kind() const {
    return std::holds_alternative<NativeAdamW>(storage_) ? TrainOptimizerKind::AdamW : TrainOptimizerKind::Muon;
}

const char* NativeOptimizer::kind_name() const {
    return train_optimizer_kind_name(kind());
}

const char* NativeOptimizer::backend_name() const {
    return std::visit([](const auto& optimizer) { return optimizer.backend_name(); }, storage_);
}

std::vector<torch::Tensor>& NativeOptimizer::parameters() {
    return std::visit([](auto& optimizer) -> std::vector<torch::Tensor>& { return optimizer.parameters(); }, storage_);
}

const std::vector<torch::Tensor>& NativeOptimizer::parameters() const {
    return std::visit(
        [](const auto& optimizer) -> const std::vector<torch::Tensor>& { return optimizer.parameters(); },
        storage_);
}

const std::vector<std::string>& NativeOptimizer::parameter_names() const {
    return std::visit(
        [](const auto& optimizer) -> const std::vector<std::string>& { return optimizer.parameter_names(); },
        storage_);
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

void NativeOptimizer::save(torch::serialize::OutputArchive& archive) const {
    std::visit([&](const auto& optimizer) { optimizer.save(archive); }, storage_);
}

void NativeOptimizer::load(torch::serialize::InputArchive& archive) {
    std::visit([&](auto& optimizer) { optimizer.load(archive); }, storage_);
}

} // namespace mmltk::rfdetr
