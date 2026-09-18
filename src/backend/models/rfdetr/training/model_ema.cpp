#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "detail/model_ema.h"
#include <ATen/ops/_foreach_add.h>
#include <ATen/ops/_foreach_mul.h>
#include <cmath>
#include <exception>
#include <iterator>
#include "src/backend/ml/cuda/tensor_readback.h"
namespace mmltk::backend::models::rfdetr {
namespace torch_cuda = mmltk::backend::ml::cuda;
ModelEma::ModelEma(const std::vector<torch::Tensor>& parameters, const double decay, const double tau) : decay_(decay), tau_(tau), source_(parameters) {
    shadow_.reserve(parameters.size());
    for (const auto& parameter : parameters) shadow_.push_back(parameter.detach().clone());
}
void ModelEma::update() {
    if (selected_) throw std::logic_error("cannot update selected EMA weights");
    if (shadow_.empty()) return;
    if (completed_updates_ == std::numeric_limits<int64_t>::max()) throw std::overflow_error("EMA update count is exhausted");
    const double updates = static_cast<double>(completed_updates_ + 1);
    const double decay = tau_ > 0.0 ? decay_ * (1.0 - std::exp(-updates / tau_)) : decay_;
    torch::NoGradGuard guard;
    at::_foreach_mul_(shadow_, decay);
    at::_foreach_add_(shadow_, source_, 1.0 - decay);
    ++completed_updates_;
}
ModelEma::ModelEma(const std::vector<torch::Tensor>& parameters, ShadowCandidate candidate, double decay, double tau, int64_t completed_updates)
    : decay_(decay), tau_(tau), completed_updates_(completed_updates), source_(parameters), shadow_(std::move(candidate.tensors)) {}
void ModelEma::validate_cpu_shadow(const std::vector<torch::Tensor>& parameters, const std::vector<torch::Tensor>& cpu_shadow, std::stop_token stop) {
    if (parameters.size() != cpu_shadow.size()) throw std::invalid_argument("EMA CPU inventory differs from active parameters");
    // Validate the entire CPU inventory before any device allocation or copy.
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
        const auto& source = cpu_shadow[index];
        const auto& destination = parameters[index];
        if (!source.defined() || !source.device().is_cpu() || !source.is_floating_point() || source.sizes() != destination.sizes() ||
            source.scalar_type() != destination.scalar_type() || source.layout() != destination.layout() || !torch::isfinite(source).all().item<bool>())
            throw std::invalid_argument("EMA CPU tensor differs from its active parameter");
    }
}
ModelEma ModelEma::from_cpu_shadow(const std::vector<torch::Tensor>& parameters, const std::vector<torch::Tensor>& cpu_shadow, double decay, double tau,
                                   int64_t completed_updates) {
    if (completed_updates < 0 || completed_updates == std::numeric_limits<int64_t>::max()) throw std::invalid_argument("invalid EMA completed update count");
    validate_cpu_shadow(parameters, cpu_shadow);
    ShadowCandidate candidate;
    candidate.tensors.reserve(parameters.size());
    for (std::size_t index = 0; index < parameters.size(); ++index)
        candidate.tensors.push_back(cpu_shadow[index].to(parameters[index].device(), cpu_shadow[index].scalar_type(), false, true));
    return ModelEma(parameters, std::move(candidate), decay, tau, completed_updates);
}
ModelEma::Selection::Selection(ModelEma& owner, NativeRfDetrModel& module) : owner_(&owner), module_(&module), training_(module.is_training()) {
    if (owner.selected_) throw std::logic_error("EMA weights already selected");
    torch::NoGradGuard guard;
    if (owner.backup_.empty()) {
        std::vector<torch::Tensor> candidate;
        candidate.reserve(owner.source_.size());
        for (const auto& parameter : owner.source_) candidate.push_back(torch::empty_like(parameter));
        owner.backup_.swap(candidate);
    }
    for (std::size_t index = 0; index < owner.source_.size(); ++index) owner.backup_[index].copy_(owner.source_[index]);
    owner.selected_ = true;
    try {
        owner.copy_to(owner.source_);
    } catch (...) {
        const auto failure = std::current_exception();
        try {
            restore();
        } catch (...) {}
        std::rethrow_exception(failure);
    }
}
void ModelEma::Selection::restore() {
    if (owner_ == nullptr) return;
    torch::NoGradGuard guard;
    for (std::size_t index = 0; index < owner_->source_.size(); ++index) owner_->source_[index].copy_(owner_->backup_[index]);
    module_->train(training_);
    owner_->selected_ = false;
    owner_ = nullptr;
}
ModelEma::Selection::~Selection() noexcept {
    // Explicit normal-path restoration propagates failure. During unwinding,
    // preserve the original error and leave admission sealed by selected_.
    try {
        restore();
    } catch (...) {}
}
const std::vector<torch::Tensor>& ModelEma::shadow_params() const noexcept { return shadow_; }
ModelEma::ShadowCandidate ModelEma::stage_shadow_params(const std::vector<torch::Tensor>& parameters) const {
    if (parameters.size() != shadow_.size()) { throw std::runtime_error("RF-DETR EMA state does not match the active parameter inventory"); }
    struct DeviceChecks {
        torch::Device device;
        std::vector<torch::Tensor> finite;
    };
    std::vector<DeviceChecks> checks;
    // Validate the complete structural inventory before allocating candidates.
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const auto& parameter = parameters[index];
        const auto& destination = shadow_[index];
        if (!parameter.defined() || !parameter.is_floating_point() || parameter.sizes() != destination.sizes() || parameter.device() != destination.device() ||
            parameter.scalar_type() != destination.scalar_type() || parameter.layout() != destination.layout())
            throw std::runtime_error("RF-DETR EMA tensor does not match its active parameter");
        auto group = std::ranges::find(checks, parameter.device(), &DeviceChecks::device);
        if (group == checks.end()) {
            checks.push_back({parameter.device(), {}});
            group = std::prev(checks.end());
        }
        group->finite.push_back(torch::isfinite(parameter).all());
    }
    std::vector<torch::Tensor> packets;
    packets.reserve(checks.size());
    for (const auto& group : checks) packets.push_back(torch::stack(group.finite));
    torch_cuda::TensorReadbackBuffers readback;
    readback.Begin();
    readback.Reserve(packets);
    for (std::size_t index = 0; index < packets.size(); ++index) (void)readback.Stage(index);
    readback.Complete();
    for (std::size_t index = 0; index < packets.size(); ++index)
        if (!readback.Stage(index).all().item<bool>()) throw std::runtime_error("RF-DETR EMA tensor does not match its active parameter");
    readback.Release();
    ShadowCandidate candidate;
    candidate.tensors.reserve(parameters.size());
    for (const auto& parameter : parameters) candidate.tensors.push_back(parameter.detach().clone());
    return candidate;
}
void ModelEma::commit_shadow_params(ShadowCandidate candidate) noexcept { shadow_.swap(candidate.tensors); }
void ModelEma::load_shadow_params(const std::vector<torch::Tensor>& parameters) { commit_shadow_params(stage_shadow_params(parameters)); }
void ModelEma::copy_to(std::vector<torch::Tensor>& parameters) const {
    torch::NoGradGuard guard;
    for (size_t index = 0; index < std::min(shadow_.size(), parameters.size()); ++index) parameters[index].copy_(shadow_[index]);
}
}  // namespace mmltk::backend::models::rfdetr
