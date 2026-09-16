#include "src/backend/models/rfdetr/core/detection_ops.h"

#include <torch/torch.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda_error.h"
#include <torch/types.h>
#include <torch/serialize.h>
#include "src/backend/ml/cuda/torch_cuda_utils.h"
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
    mmltk::frameworks::gpu::ensure_cuda_ok(scope ? scope.FinalizeStatus(cudaEventCreateWithFlags(&ready_, cudaEventDisableTiming)) : scope.Finalize(),
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
    if (lane >= host_counts_.size() || published_[lane] || target_count < 0) { throw std::runtime_error("invalid RF-DETR target normalizer lane publication"); }
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
GradScaler::GradScaler(const bool enabled, const float init_scale, const float growth_factor, const float backoff_factor, const int growth_interval)
    : enabled_(enabled), scale_(init_scale), growth_factor_(growth_factor), backoff_factor_(backoff_factor), growth_interval_(growth_interval) {}
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
torch::Tensor loss_value_or_zero(const TensorMap& loss_dict, const torch::Device& device, std::string_view key) {
    const auto found = loss_dict.find(std::string(key));
    if (found != loss_dict.end()) { return found->second; }
    return torch::zeros({}, torch::TensorOptions().dtype(torch::kFloat32).device(device));
}
scalar_packet::Tensors ordinary_scalar_tensors(const TensorMap& losses, const torch::Tensor& total, const torch::Tensor& auxiliary) {
    scalar_packet::Tensors values;
    scalar_packet::set<^^TrainingScalars::total>(values, total);
    scalar_packet::set<^^TrainingScalars::auxiliary_weighted>(values, auxiliary);
    const auto assign = [&]<std::meta::info Member>(std::string_view name) {
        const auto found = losses.find(std::string(name));
        if (found != losses.end()) scalar_packet::set<Member>(values, found->second);
    };
    assign.operator()<^^TrainingScalars::classification>("loss_ce");
    assign.operator()<^^TrainingScalars::l1>("loss_bbox");
    assign.operator()<^^TrainingScalars::giou>("loss_giou");
    assign.operator()<^^TrainingScalars::mask_ce>("loss_mask_ce");
    assign.operator()<^^TrainingScalars::mask_dice>("loss_mask_dice");
    assign.operator()<^^TrainingScalars::class_error>("class_error");
    assign.operator()<^^TrainingScalars::cardinality_error>("cardinality_error");
    return values;
}
RoutedTrainingLoss compute_routed_training_loss(NativeRfDetrModel& model, const TrainingSupervisionRoute route, const ModelOutputs& outputs,
                                                const PreparedTargets& targets, const DeviceLossNormalizer& normalizer,
                                                const DetectionConfig& detection_config) {
    if (route_uses_match_free(route)) {
        const auto loss = model.supervision_loss(outputs, targets, normalizer, true);
        scalar_packet::Tensors scalars;
        scalar_packet::set<^^TrainingScalars::total>(scalars, loss.total);
        scalar_packet::set<^^TrainingScalars::classification>(scalars, loss.main.classification);
        scalar_packet::set<^^TrainingScalars::l1>(scalars, loss.main.box);
        scalar_packet::set<^^TrainingScalars::giou>(scalars, loss.main.giou);
        scalar_packet::set<^^TrainingScalars::correspondence_weighted>(scalars, loss.correspondence);
        scalar_packet::set<^^TrainingScalars::auxiliary_weighted>(scalars, loss.auxiliary);
        if (route_uses_denoising(route)) scalar_packet::set<^^TrainingScalars::denoising_weighted>(scalars, loss.denoising);
        return {loss.total, loss.classification, loss.box + loss.giou, {}, std::move(scalars)};
    }
    const double group_divisor = detection_config.sum_group_losses ? 1.0 : static_cast<double>(detection_config.group_detr);
    const double num_boxes = torch::clamp_min(normalizer.target_count * group_divisor, 1.0).item<double>();
    auto ordinary_terms = detection_loss_dict(outputs, targets, detection_config, true, num_boxes);
    torch::Tensor auxiliary;
    auto total = weighted_detection_loss(ordinary_terms, detection_config, outputs.main.pred_logits.device(), &auxiliary);
    auto scalars = ordinary_scalar_tensors(ordinary_terms, total, auxiliary);
    auto classification = loss_value_or_zero(ordinary_terms, outputs.main.pred_logits.device(), "loss_ce");
    auto box = loss_value_or_zero(ordinary_terms, outputs.main.pred_logits.device(), "loss_bbox");
    if (route_uses_denoising(route)) {
        const auto denoising = model.supervision_loss(outputs, targets, normalizer, true);
        total = total + denoising.total;
        scalar_packet::set<^^TrainingScalars::denoising_weighted>(scalars, denoising.total);
        classification = classification + denoising.classification;
        box = box + denoising.box + denoising.giou;
    }
    scalar_packet::set<^^TrainingScalars::total>(scalars, total);
    return {std::move(total), std::move(classification), std::move(box), std::move(ordinary_terms), std::move(scalars)};
}
}  // namespace mmltk::backend::models::rfdetr
