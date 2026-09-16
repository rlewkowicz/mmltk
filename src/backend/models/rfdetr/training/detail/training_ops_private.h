#pragma once

#include <cuda_runtime_api.h>
#include <torch/torch.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(USE_C10D_NCCL)
#include <torch/csrc/distributed/c10d/FileStore.hpp>
#include <torch/csrc/distributed/c10d/ProcessGroupNCCL.hpp>
#endif

#include "detection_types.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"

namespace mmltk::backend::models::rfdetr {

struct DistributedContext {
    bool enabled = false;
    int rank = 0;
    int world_size = 1;
#if defined(USE_C10D_NCCL)
    c10::intrusive_ptr<c10d::Store> store;
    c10::intrusive_ptr<c10d::Backend> process_group;
#endif
};

void distributed_all_reduce_tensor(const DistributedContext& distributed, torch::Tensor& tensor);

class WaveTargetNormalizer final {
   public:
    WaveTargetNormalizer(std::size_t lanes, int device_id, const DistributedContext& distributed);
    ~WaveTargetNormalizer() noexcept;

    WaveTargetNormalizer(const WaveTargetNormalizer&) = delete;
    WaveTargetNormalizer& operator=(const WaveTargetNormalizer&) = delete;

    void publish(std::size_t lane, std::int64_t target_count);
    void resolve(const DistributedContext& distributed, const torch::Device& device);
    [[nodiscard]] DeviceLossNormalizer consume(std::size_t lane, cudaStream_t stream);
    void fail(std::exception_ptr failure) noexcept;

   private:
    void rethrow_failure_locked() const;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::int64_t> host_counts_;
    std::vector<bool> published_;
    torch::Tensor device_counts_;
    std::exception_ptr failure_;
    std::size_t published_count_ = 0;
    int device_id_ = -1;
    const DistributedContext* distributed_ = nullptr;
    cudaEvent_t ready_ = nullptr;
    bool resolved_ = false;
    bool distributed_abort_requested_ = false;
};

template <class Result>
class ParallelTrainingWave final {
   public:
    ParallelTrainingWave(const std::size_t lane_count, const bool active, const int device_id, const DistributedContext& distributed)
        : distributed_(&distributed) {
        futures_.reserve(lane_count);
        results_.reserve(lane_count);
        if (active) { normalizer_ = std::make_shared<WaveTargetNormalizer>(lane_count, device_id, distributed); }
    }

    ~ParallelTrainingWave() noexcept {
        if (!settled_) {
            fail(std::make_exception_ptr(std::runtime_error("RF-DETR parallel training wave was cancelled")));
            drain();
        }
    }

    ParallelTrainingWave(const ParallelTrainingWave&) = delete;
    ParallelTrainingWave& operator=(const ParallelTrainingWave&) = delete;

    [[nodiscard]] const std::shared_ptr<WaveTargetNormalizer>& normalizer() const noexcept { return normalizer_; }

    void add(std::future<Result> future) { futures_.push_back(std::move(future)); }

    template <class Consumer>
    void settle(const torch::Device& device, Consumer&& consume) {
        try {
            if (normalizer_) { normalizer_->resolve(*distributed_, device); }
        } catch (...) { fail(std::current_exception()); }
        drain();
        settled_ = true;
        if (failure_) { std::rethrow_exception(failure_); }
        try {
            for (auto& result : results_) {
                consume(result);
            }
        } catch (...) {
            fail(std::current_exception());
            std::rethrow_exception(failure_);
        }
    }

   private:
    void fail(std::exception_ptr failure) noexcept {
        if (!failure_) { failure_ = std::move(failure); }
        if (normalizer_) { normalizer_->fail(failure_); }
    }

    void drain() noexcept {
        for (auto& future : futures_) {
            if (!future.valid()) { continue; }
            try {
                results_.push_back(future.get());
            } catch (...) { fail(std::current_exception()); }
        }
        futures_.clear();
    }

    const DistributedContext* distributed_;
    std::shared_ptr<WaveTargetNormalizer> normalizer_;
    std::vector<std::future<Result>> futures_;
    std::vector<Result> results_;
    std::exception_ptr failure_;
    bool settled_ = false;
};

class GradScaler {
   public:
    explicit GradScaler(bool enabled, float init_scale = 65536.0f, float growth_factor = 2.0f, float backoff_factor = 0.5f,
                        int growth_interval = 2000);

    torch::Tensor scale(const torch::Tensor& loss);

    template <typename OptimizerLike>
    torch::Tensor check_and_unscale_(OptimizerLike& optimizer) {
        gradient_scratch_.clear();
        gradient_scratch_.reserve(optimizer.parameters().size());
        for (auto& param : optimizer.parameters()) {
            if (!param.grad().defined()) { continue; }
            gradient_scratch_.push_back(param.mutable_grad());
        }
        const auto& parameters = optimizer.parameters();
        if (parameters.empty()) { throw std::runtime_error("gradient finite check requires optimizer parameters"); }
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
        if (!found_inf) { optimizer.step(); }
    }

    void update(bool found_inf);

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] float current_scale() const noexcept;
    [[nodiscard]] int growth_tracker() const noexcept;

    void load_state(float scale, int growth_tracker) noexcept;

   private:
    void ensure_device_state(const torch::Device& device);

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

struct LrScheduleConfig {
    double warmup_epochs = 0.0;
    double warmup_momentum = 0.0;
    TrainLrSchedulerKind lr_scheduler = TrainLrSchedulerKind::Cosine;
    int64_t lr_drop = 1;
    double lr_min_factor = 0.0;
};

double compute_lr_scale(const LrScheduleConfig& config, int64_t current_step, int64_t steps_per_epoch, int64_t total_training_steps);

double compute_warmup_momentum(const LrScheduleConfig& config, int64_t current_step, int64_t steps_per_epoch, double target_momentum);

template <typename OptimizerLike>
inline void set_optimizer_lrs(OptimizerLike& optimizer, const std::vector<double>& base_lrs, const double scale) {
    optimizer.set_lrs(base_lrs, scale);
}

}  // namespace mmltk::backend::models::rfdetr
