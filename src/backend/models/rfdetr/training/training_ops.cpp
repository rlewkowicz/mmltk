#include <ATen/ops/_foreach_add.h>
#include <ATen/ops/_foreach_mul.h>
#include <torch/torch.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "torch_api.h"
#include "torch_cuda_utils.h"

#include "detail/training_ops_private.h"

namespace mmltk::backend::models::rfdetr {

namespace torch_cuda = mmltk::backend::ml::cuda;

void distributed_all_reduce_tensor(const DistributedContext& distributed, torch::Tensor& tensor) {
    if (!distributed.enabled) { return; }
#if defined(USE_C10D_NCCL)
    std::vector<torch::Tensor> tensors{tensor};
    distributed.process_group->allreduce(tensors)->wait();
#else
    (void)distributed;
    (void)tensor;
    throw std::runtime_error("distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#endif
}

WaveTargetNormalizer::WaveTargetNormalizer(const std::size_t lanes, const int device_id, const DistributedContext& distributed)
    : host_counts_(lanes, 0), published_(lanes, false), device_id_(device_id), distributed_(&distributed) {
    if (lanes == 0) { throw std::invalid_argument("RF-DETR target normalizer wave requires at least one lane"); }
    mmltk::frameworks::gpu::CudaDeviceScope scope(device_id_);
    mmltk::frameworks::gpu::ensure_cuda_ok(
        scope ? scope.FinalizeStatus(cudaEventCreateWithFlags(&ready_, cudaEventDisableTiming)) : scope.Finalize(),
        "create RF-DETR target normalizer event");
}

WaveTargetNormalizer::~WaveTargetNormalizer() noexcept {
    if (ready_ != nullptr) {
        mmltk::frameworks::gpu::CudaDeviceScope scope(device_id_);
        const cudaError_t status = scope ? cudaEventDestroy(ready_) : scope.status();
        static_cast<void>(scope.FinalizeStatus(status));
    }
}

void WaveTargetNormalizer::publish(const std::size_t lane, const std::int64_t target_count) {
    std::lock_guard lock(mutex_);
    rethrow_failure_locked();
    if (lane >= host_counts_.size() || published_[lane] || target_count < 0) {
        throw std::runtime_error("invalid RF-DETR target normalizer lane publication");
    }
    host_counts_[lane] = target_count;
    published_[lane] = true;
    ++published_count_;
    condition_.notify_all();
}

void WaveTargetNormalizer::resolve(const DistributedContext& distributed, const torch::Device& device) {
    try {
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return failure_ || published_count_ == host_counts_.size(); });
            rethrow_failure_locked();
        }
        auto counts = torch::tensor(host_counts_, torch::TensorOptions().dtype(torch::kFloat32).device(device));
        distributed_all_reduce_tensor(distributed, counts);
        {
            std::lock_guard lock(mutex_);
            rethrow_failure_locked();
        }
        counts.div_(static_cast<double>(std::max(1, distributed.world_size)));
        mmltk::frameworks::gpu::ensure_cuda_ok(
            cudaEventRecord(ready_, torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(device_id_)).stream()),
            "record RF-DETR target normalizer readiness");
        {
            std::lock_guard lock(mutex_);
            device_counts_ = std::move(counts);
            resolved_ = true;
        }
        condition_.notify_all();
    } catch (...) {
        fail(std::current_exception());
        throw;
    }
}

DeviceLossNormalizer WaveTargetNormalizer::consume(const std::size_t lane, const cudaStream_t stream) {
    {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] { return failure_ || resolved_; });
        rethrow_failure_locked();
        if (lane >= host_counts_.size()) { throw std::runtime_error("RF-DETR target normalizer lane is out of range"); }
    }
    mmltk::frameworks::gpu::ensure_cuda_ok(cudaStreamWaitEvent(stream, ready_), "wait for RF-DETR target normalizer readiness");
    device_counts_.record_stream(torch_cuda::getStreamFromExternal(stream, torch_cuda::checked_device_index(device_id_)));
    return {device_counts_.select(0, static_cast<std::int64_t>(lane))};
}

void WaveTargetNormalizer::fail(std::exception_ptr failure) noexcept {
    bool abort_distributed = false;
    {
        std::lock_guard lock(mutex_);
        if (!failure_) {
            failure_ = std::move(failure);
            abort_distributed = distributed_ != nullptr && distributed_->enabled && !distributed_abort_requested_;
            distributed_abort_requested_ = abort_distributed;
        }
    }
    condition_.notify_all();
    if (abort_distributed) {
#if defined(USE_C10D_NCCL)
        try {
            distributed_->process_group->abort();
        } catch (...) {}
#endif
    }
}

void WaveTargetNormalizer::rethrow_failure_locked() const {
    if (failure_) { std::rethrow_exception(failure_); }
}

void validate_resume_continuation_manifest(const ResumeContinuationManifest& manifest) {
    if (manifest.ema_requested != manifest.ema_present) {
        throw std::runtime_error(manifest.ema_requested ? "native RF-DETR resume checkpoint is missing required EMA state"
                                                        : "native RF-DETR resume checkpoint contains EMA state while EMA is disabled");
    }
    if (manifest.scaler_scale.has_value() != manifest.scaler_growth_tracker.has_value()) {
        throw std::runtime_error("native RF-DETR resume checkpoint gradient scaler state is incomplete");
    }
    if ((manifest.scaler_scale.has_value() && (!std::isfinite(*manifest.scaler_scale) || *manifest.scaler_scale <= 0.0 ||
                                               *manifest.scaler_scale > std::numeric_limits<float>::max())) ||
        (manifest.scaler_growth_tracker.has_value() &&
         (*manifest.scaler_growth_tracker < 0 ||
          *manifest.scaler_growth_tracker > static_cast<int64_t>(std::numeric_limits<int>::max())))) {
        throw std::runtime_error("native RF-DETR resume checkpoint gradient scaler state is invalid");
    }
}

GradScaler::GradScaler(const bool enabled, const float init_scale, const float growth_factor, const float backoff_factor,
                       const int growth_interval)
    : enabled_(enabled),
      scale_(init_scale),
      growth_factor_(growth_factor),
      backoff_factor_(backoff_factor),
      growth_interval_(growth_interval) {}
torch::Tensor GradScaler::scale(const torch::Tensor& loss) { return enabled_ ? loss * scale_ : loss; }
void GradScaler::update(const bool found_inf) {
    if (!enabled_) return;
    if (found_inf) {
        scale_ *= backoff_factor_;
        growth_tracker_ = 0;
    } else if (++growth_tracker_ >= growth_interval_) {
        scale_ *= growth_factor_;
        growth_tracker_ = 0;
    }
}
bool GradScaler::enabled() const noexcept { return enabled_; }
float GradScaler::current_scale() const noexcept { return scale_; }
int GradScaler::growth_tracker() const noexcept { return growth_tracker_; }
void GradScaler::load_state(const float scale, const int growth_tracker) noexcept {
    if (enabled_) {
        scale_ = scale;
        growth_tracker_ = std::max(0, growth_tracker);
    }
}
void GradScaler::ensure_device_state(const torch::Device& device) {
    if (found_inf_device_.defined() && found_inf_device_.device() == device) return;
    const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
    found_inf_device_ = torch::zeros({}, options);
    inverse_scale_device_ = torch::ones({}, options);
}
ModelEma::ModelEma(const std::vector<torch::Tensor>& parameters, const double decay, const double tau) : decay_(decay), tau_(tau) {
    shadow_.reserve(parameters.size());
    for (const auto& parameter : parameters)
        shadow_.push_back(parameter.detach().clone());
}
void ModelEma::update(const std::vector<torch::Tensor>& parameters, const int64_t step) {
    const size_t limit = std::min(shadow_.size(), parameters.size());
    if (limit == 0U) return;
    const double updates = static_cast<double>(std::max<int64_t>(1, step + 1));
    const double decay = tau_ > 0.0 ? decay_ * (1.0 - std::exp(-updates / tau_)) : decay_;
    torch::NoGradGuard guard;
    std::vector<torch::Tensor> shadow(shadow_.begin(), shadow_.begin() + static_cast<std::ptrdiff_t>(limit));
    std::vector<torch::Tensor> source;
    source.reserve(limit);
    for (size_t index = 0; index < limit; ++index)
        source.push_back(parameters[index].detach());
    at::_foreach_mul_(shadow, decay);
    at::_foreach_add_(shadow, source, 1.0 - decay);
}
const std::vector<torch::Tensor>& ModelEma::shadow_params() const noexcept { return shadow_; }
ModelEma::ShadowCandidate ModelEma::stage_shadow_params(const std::vector<torch::Tensor>& parameters) const {
    if (parameters.size() != shadow_.size()) {
        throw std::runtime_error("RF-DETR EMA state does not match the active parameter inventory");
    }
    ShadowCandidate candidate;
    candidate.tensors.reserve(parameters.size());
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const auto& parameter = parameters[index];
        const auto& destination = shadow_[index];
        if (!parameter.defined() || !parameter.is_floating_point() || parameter.sizes() != destination.sizes() ||
            parameter.device() != destination.device() || parameter.scalar_type() != destination.scalar_type() ||
            parameter.layout() != destination.layout() || !torch::isfinite(parameter).all().item<bool>()) {
            throw std::runtime_error("RF-DETR EMA tensor does not match its active parameter");
        }
        candidate.tensors.push_back(parameter.detach().clone());
    }
    return candidate;
}
void ModelEma::commit_shadow_params(ShadowCandidate candidate) noexcept { shadow_.swap(candidate.tensors); }
void ModelEma::load_shadow_params(const std::vector<torch::Tensor>& parameters) { commit_shadow_params(stage_shadow_params(parameters)); }
void ModelEma::copy_to(std::vector<torch::Tensor>& parameters) const {
    torch::NoGradGuard guard;
    for (size_t index = 0; index < std::min(shadow_.size(), parameters.size()); ++index)
        parameters[index].copy_(shadow_[index]);
}
double compute_lr_scale(const LrScheduleConfig& config, const int64_t step, const int64_t steps_per_epoch, const int64_t total_steps) {
    const double warmup = static_cast<double>(steps_per_epoch) * config.warmup_epochs;
    if (warmup > 0.0 && static_cast<double>(step) < warmup) return static_cast<double>(step) / std::max(1.0, warmup);
    if (config.lr_scheduler == TrainLrSchedulerKind::Cosine) {
        const double progress = static_cast<double>(step) - warmup;
        const double denominator = std::max(1.0, static_cast<double>(total_steps) - warmup);
        return config.lr_min_factor + (1.0 - config.lr_min_factor) * 0.5 * (1.0 + std::cos(std::numbers::pi * progress / denominator));
    }
    return step < config.lr_drop * steps_per_epoch ? 1.0 : 0.1;
}
double compute_warmup_momentum(const LrScheduleConfig& config, const int64_t step, const int64_t steps_per_epoch, const double target) {
    const double warmup = static_cast<double>(steps_per_epoch) * config.warmup_epochs;
    if (warmup <= 0.0 || config.warmup_momentum <= 0.0 || static_cast<double>(step) >= warmup) return target;
    const double alpha = static_cast<double>(step) / std::max(1.0, warmup);
    return config.warmup_momentum + (target - config.warmup_momentum) * alpha;
}
}  // namespace mmltk::backend::models::rfdetr
