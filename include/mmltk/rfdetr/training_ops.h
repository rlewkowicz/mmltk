#pragma once

#include <ATen/ops/_amp_foreach_non_finite_check_and_unscale.h>
#include <ATen/ops/_foreach_add.h>
#include <ATen/ops/_foreach_mul.h>
#include <ATen/autocast_mode.h>
#include <ATen/cuda/CUDAContext.h>
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
    return (props->major >= 8) ? torch::kBFloat16 : torch::kFloat16;
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
    void unscale_(OptimizerLike& optimizer) {
        if (!enabled_) {
            return;
        }
        if (unscaled_) {
            throw std::runtime_error("unscale_() has already been called since the last step()");
        }
        std::vector<torch::Tensor> grads;
        grads.reserve(optimizer.parameters().size());
        for (auto& param : optimizer.parameters()) {
            if (!param.grad().defined()) {
                continue;
            }
            grads.push_back(param.mutable_grad());
        }
        if (grads.empty()) {
            found_inf_ = false;
            unscaled_ = true;
            return;
        }
        const auto tensor_options = torch::TensorOptions().dtype(torch::kFloat32).device(grads.front().device());
        auto found_inf = torch::zeros({}, tensor_options);
        auto inv_scale = torch::full({}, 1.0f / scale_, tensor_options);
        at::_amp_foreach_non_finite_check_and_unscale_(grads, found_inf, inv_scale);
        found_inf_ = found_inf.item<float>() != 0.0f;
        unscaled_ = true;
    }

    template <typename OptimizerLike>
    void step(OptimizerLike& optimizer) {
        if (!enabled_) {
            optimizer.step();
            return;
        }
        if (!unscaled_) {
            unscale_(optimizer);
        }
        if (!found_inf_) {
            optimizer.step();
        }
    }

    void update() {
        if (!enabled_) {
            return;
        }
        if (found_inf_) {
            scale_ *= backoff_factor_;
            growth_tracker_ = 0;
        } else {
            ++growth_tracker_;
            if (growth_tracker_ >= growth_interval_) {
                scale_ *= growth_factor_;
                growth_tracker_ = 0;
            }
        }
        found_inf_ = false;
        unscaled_ = false;
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
        found_inf_ = false;
        unscaled_ = false;
    }

   private:
    bool enabled_;
    float scale_;
    float growth_factor_;
    float backoff_factor_;
    int growth_interval_;
    int growth_tracker_ = 0;
    bool found_inf_ = false;
    bool unscaled_ = false;
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

}  // namespace mmltk::rfdetr
