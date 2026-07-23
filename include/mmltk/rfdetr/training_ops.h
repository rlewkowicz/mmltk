#pragma once

#include <ATen/ops/_amp_foreach_non_finite_check_and_unscale.h>
#include <ATen/ops/_foreach_add.h>
#include <ATen/ops/_foreach_mul.h>
#include <ATen/autocast_mode.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>
#include <torch/torch.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace mmltk::rfdetr {

class AutocastGuard {
   public:
    AutocastGuard(bool enabled, at::ScalarType dtype)
        : prev_enabled_(at::autocast::is_autocast_enabled(at::kCUDA)),
          prev_dtype_(at::autocast::get_autocast_dtype(at::kCUDA)),
          prev_cache_enabled_(at::autocast::is_autocast_cache_enabled()) {
        at::autocast::set_autocast_enabled(at::kCUDA, enabled);
        if (enabled) {
            at::autocast::set_autocast_dtype(at::kCUDA, dtype);
        }
        at::autocast::set_autocast_cache_enabled(enabled);
        at::autocast::increment_nesting();
    }

    ~AutocastGuard() {
        at::autocast::decrement_nesting();
        at::autocast::set_autocast_enabled(at::kCUDA, prev_enabled_);
        at::autocast::set_autocast_dtype(at::kCUDA, prev_dtype_);
        at::autocast::set_autocast_cache_enabled(prev_cache_enabled_);
        if (!prev_enabled_) {
            at::autocast::clear_cache();
        }
    }

    AutocastGuard(const AutocastGuard&) = delete;
    AutocastGuard& operator=(const AutocastGuard&) = delete;

   private:
    bool prev_enabled_;
    at::ScalarType prev_dtype_;
    bool prev_cache_enabled_;
};

inline at::ScalarType resolve_cuda_autocast_dtype() {
    const auto props = at::cuda::getCurrentDeviceProperties();
    TORCH_CHECK(props != nullptr, "CUDA autocast requires a valid CUDA device");
    TORCH_CHECK(props->major >= 7, "RF-DETR CUDA autocast requires compute capability 7.0 or newer");
    return props->major >= 8 ? torch::kBFloat16 : torch::kFloat16;
}

inline at::ScalarType resolve_cuda_autocast_dtype(const int device_id) {
    c10::cuda::CUDAGuard device_guard(static_cast<c10::DeviceIndex>(device_id));
    return resolve_cuda_autocast_dtype();
}

inline void assert_inference_output_dtype(const torch::Tensor& pred_logits, const torch::Tensor& pred_boxes,
                                          const at::ScalarType expected_dtype, const char* context) {
    TORCH_CHECK(pred_logits.defined() && pred_boxes.defined(), context, " returned undefined detection outputs");
    TORCH_CHECK(pred_logits.scalar_type() == expected_dtype, context, " pred_logits dtype is ",
                pred_logits.scalar_type(), " but expected ", expected_dtype);
    const bool valid_box_dtype = pred_boxes.scalar_type() == expected_dtype ||
                                 (expected_dtype != at::kFloat && pred_boxes.scalar_type() == at::kFloat);
    TORCH_CHECK(valid_box_dtype, context, " pred_boxes dtype is ", pred_boxes.scalar_type(), " but expected ",
                expected_dtype, " or FP32 reference-box promotion");
}

class GradScaler {
   public:
    explicit GradScaler(bool enabled, float init_scale = 65536.0f, float growth_factor = 2.0f,
                        float backoff_factor = 0.5f, int growth_interval = 2000)
        : enabled_(enabled),
          scale_(init_scale),
          growth_factor_(growth_factor),
          backoff_factor_(backoff_factor),
          growth_interval_(growth_interval) {}

    torch::Tensor scale(const torch::Tensor& loss) {
        if (!enabled_) {
            return loss;
        }
        return loss * scale_;
    }

    template <typename OptimizerLike>
    torch::Tensor check_and_unscale_(OptimizerLike& optimizer) {
        gradient_scratch_.clear();
        gradient_scratch_.reserve(optimizer.parameters().size());
        for (auto& param : optimizer.parameters()) {
            if (!param.grad().defined()) {
                continue;
            }
            gradient_scratch_.push_back(param.mutable_grad());
        }
        const auto& parameters = optimizer.parameters();
        if (parameters.empty()) {
            throw std::runtime_error("gradient finite check requires optimizer parameters");
        }
        ensure_device_state(parameters.front().device());
        found_inf_device_.zero_();
        inverse_scale_device_.fill_(enabled_ ? 1.0f / scale_ : 1.0f);
        if (!gradient_scratch_.empty()) {
            at::_amp_foreach_non_finite_check_and_unscale_(gradient_scratch_, found_inf_device_, inverse_scale_device_);
        }
        gradient_scratch_.clear();
        return found_inf_device_;
    }

    template <typename OptimizerLike>
    void step(OptimizerLike& optimizer, bool found_inf) {
        if (!found_inf) {
            optimizer.step();
        }
    }

    void update(bool found_inf) {
        if (!enabled_) {
            return;
        }
        if (found_inf) {
            scale_ *= backoff_factor_;
            growth_tracker_ = 0;
        } else {
            ++growth_tracker_;
            if (growth_tracker_ >= growth_interval_) {
                scale_ *= growth_factor_;
                growth_tracker_ = 0;
            }
        }
    }

    [[nodiscard]] bool enabled() const {
        return enabled_;
    }
    [[nodiscard]] float current_scale() const {
        return scale_;
    }
    [[nodiscard]] int growth_tracker() const {
        return growth_tracker_;
    }

    void load_state(float scale, int growth_tracker) {
        if (!enabled_) {
            return;
        }
        scale_ = scale;
        growth_tracker_ = std::max(0, growth_tracker);
    }

   private:
    void ensure_device_state(const torch::Device& device) {
        if (found_inf_device_.defined() && found_inf_device_.device() == device) {
            return;
        }
        const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
        found_inf_device_ = torch::zeros({}, options);
        inverse_scale_device_ = torch::ones({}, options);
    }

    bool enabled_;
    float scale_;
    float growth_factor_;
    float backoff_factor_;
    int growth_interval_;
    int growth_tracker_ = 0;
    torch::Tensor found_inf_device_;
    torch::Tensor inverse_scale_device_;
    std::vector<torch::Tensor> gradient_scratch_;
};

class ModelEma {
   public:
    ModelEma(const std::vector<torch::Tensor>& model_params, double decay, double tau) : decay_(decay), tau_(tau) {
        shadow_.reserve(model_params.size());
        for (const auto& p : model_params) {
            shadow_.push_back(p.detach().clone());
        }
    }

    void update(const std::vector<torch::Tensor>& model_params, int64_t step) {
        const size_t limit = std::min(shadow_.size(), model_params.size());
        if (limit == 0) {
            return;
        }
        const double updates = static_cast<double>(std::max<int64_t>(1, step + 1));
        const double d = tau_ > 0.0 ? decay_ * (1.0 - std::exp(-updates / tau_)) : decay_;
        torch::NoGradGuard no_grad;
        std::vector<torch::Tensor> shadow_slice;
        std::vector<torch::Tensor> model_slice;
        shadow_slice.reserve(limit);
        model_slice.reserve(limit);
        for (size_t i = 0; i < limit; ++i) {
            shadow_slice.push_back(shadow_[i]);
            model_slice.push_back(model_params[i].detach());
        }
        at::_foreach_mul_(shadow_slice, d);
        at::_foreach_add_(shadow_slice, model_slice, 1.0 - d);
    }

    [[nodiscard]] const std::vector<torch::Tensor>& shadow_params() const {
        return shadow_;
    }

    void load_shadow_params(const std::vector<torch::Tensor>& shadow_params) {
        shadow_.clear();
        shadow_.reserve(shadow_params.size());
        for (const auto& value : shadow_params) {
            shadow_.push_back(value.detach().clone());
        }
    }

    void copy_to(std::vector<torch::Tensor>& model_params) const {
        torch::NoGradGuard no_grad;
        for (size_t i = 0; i < shadow_.size() && i < model_params.size(); ++i) {
            model_params[i].copy_(shadow_[i]);
        }
    }

   private:
    double decay_;
    double tau_;
    std::vector<torch::Tensor> shadow_;
};

struct LrScheduleConfig {
    double warmup_epochs = 0.0;
    double warmup_momentum = 0.0;
    std::string lr_scheduler = "cosine";
    int64_t lr_drop = 1;
    double lr_min_factor = 0.0;
};

inline double compute_lr_scale(const LrScheduleConfig& config, int64_t current_step, int64_t steps_per_epoch,
                               int64_t total_training_steps) {
    const double warmup_steps = static_cast<double>(steps_per_epoch) * config.warmup_epochs;
    if (warmup_steps > 0.0 && static_cast<double>(current_step) < warmup_steps) {
        return static_cast<double>(current_step) / std::max(1.0, warmup_steps);
    }
    if (config.lr_scheduler == "cosine") {
        const double progress = static_cast<double>(current_step) - warmup_steps;
        const double denom = std::max(1.0, static_cast<double>(total_training_steps) - warmup_steps);
        return config.lr_min_factor +
               (1.0 - config.lr_min_factor) * 0.5 * (1.0 + std::cos(std::numbers::pi * progress / denom));
    }
    if (current_step < config.lr_drop * steps_per_epoch) {
        return 1.0;
    }
    return 0.1;
}

inline double compute_warmup_momentum(const LrScheduleConfig& config, int64_t current_step, int64_t steps_per_epoch,
                                      double target_momentum) {
    const double warmup_steps = static_cast<double>(steps_per_epoch) * config.warmup_epochs;
    if (warmup_steps <= 0.0 || config.warmup_momentum <= 0.0 || static_cast<double>(current_step) >= warmup_steps) {
        return target_momentum;
    }
    const double alpha = static_cast<double>(current_step) / std::max(1.0, warmup_steps);
    return config.warmup_momentum + (target_momentum - config.warmup_momentum) * alpha;
}

template <typename OptimizerLike>
inline void set_optimizer_lrs(OptimizerLike& optimizer, const std::vector<double>& base_lrs, const double scale) {
    optimizer.set_lrs(base_lrs, scale);
}

}  
