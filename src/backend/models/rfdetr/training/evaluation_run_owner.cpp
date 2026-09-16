#include "src/backend/models/rfdetr/core/evaluator.h"
#include <ATen/Context.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>
#include <torch/torch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "detection_types.h"
#include "postprocess.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/frameworks/gpu/cuda_priority.h"
#include "torch_api.h"
#include "torch_cuda_utils.h"
import mmltk.backend.ml.cuda.gpu_quiescence;
#include "detail/evaluation_runtime.h"
namespace mmltk::backend::models::rfdetr {
struct TrainingEvaluationRunOwner::Impl final {
    explicit Impl(EvaluationRunConfig value) : config(std::move(value)) {
        if (config.batch_capacity == 0U || config.prediction_capacity == 0U || config.lane_count == 0U || config.slots_per_lane == 0U ||
            config.encoding_capacity == 0U || config.device_id < 0) {
            throw std::invalid_argument("evaluation run requires positive fixed capacities and a CUDA device");
        }
        const PredictionBufferConfig buffers{
            static_cast<std::int64_t>(config.batch_capacity),
            static_cast<std::int64_t>(config.prediction_capacity),
            config.metric_set == EvaluationMetricSet::BBoxAndMask ? std::make_optional(std::make_pair(config.image_height, config.image_width)) : std::nullopt,
            config.device_id,
        };
        lanes_.reserve(config.lane_count);
        for (std::size_t lane = 0; lane < config.lane_count; ++lane) {
            lanes_.push_back(EvaluationPredictionLane{
                mmltk::backend::ml::cuda::get_priority_cuda_stream(config.device_id, mmltk::frameworks::gpu::current_cuda_highest_stream_priority()),
                std::make_shared<PredictionBufferSlotPool>(config.slots_per_lane, buffers),
            });
        }
        if (config.profile) {
            profile_ = std::make_unique<EvaluationProfileRecord>();
            timing_pool_ = std::make_unique<EvaluationCudaTimingPool>(config.lane_count + config.encoding_capacity);
        }
    }
    ~Impl() { cancel_and_drain(); }
    void begin() {
        if (!lane_futures_.empty() || !encoding_queue_.empty() || !unsubmitted_timing_.empty() || !lane_timing_.empty() || !encoding_timing_.empty())
            throw std::logic_error("evaluation run cannot restart with outstanding work");
        if (!dataset_) throw std::logic_error("evaluation run requires a loaded dataset");
        dataset_->clear_predictions();
        progress_ = {};
        terminal_.reset();
        lane_timing_.clear();
        encoding_timing_.clear();
        if (profile_) {
            profile_->transferred_bytes = 0U;
            profile_->mask_transferred_bytes = 0U;
            profile_->peak_in_flight_tasks = 0U;
            profile_->peak_in_flight_slots = 0U;
            profile_->loader_wait_seconds = 0.0;
            profile_->preprocessing_seconds = 0.0;
            profile_->model_forward_seconds = 0.0;
            profile_->postprocess_seconds = 0.0;
            profile_->d2h_wait_nanoseconds.store(0U, std::memory_order_relaxed);
            profile_->cpu_encode_nanoseconds.store(0U, std::memory_order_relaxed);
            profile_->cpu_match_nanoseconds.store(0U, std::memory_order_relaxed);
            profile_->final_sort_ap_nanoseconds.store(0U, std::memory_order_relaxed);
            profile_->iou_candidate_count.store(0U, std::memory_order_relaxed);
            profile_->mask_iou_candidate_count.store(0U, std::memory_order_relaxed);
            profile_->mask_task_count.store(0U, std::memory_order_relaxed);
        }
    }
    void cancel_and_drain() noexcept {
        if (progress_.terminal) return;
        progress_.cancelled = true;
        while (!lane_futures_.empty()) {
            std::future<StagedPredictionBatch> future = std::move(lane_futures_.front());
            lane_futures_.pop_front();
            try {
                static_cast<void>(future.get());
            } catch (...) {}
        }
        while (!encoding_queue_.empty()) {
            PendingPredictionBatchEncoding pending = std::move(encoding_queue_.front());
            encoding_queue_.pop_front();
            try {
                static_cast<void>(collect_prediction_batch_encoding(std::move(pending)));
            } catch (...) {}
        }
        if (timing_pool_) {
            for (auto& lease : unsubmitted_timing_) timing_pool_->release(lease);
            for (auto& lease : lane_timing_) timing_pool_->release(lease);
            for (auto& lease : encoding_timing_) timing_pool_->release(lease);
        }
        unsubmitted_timing_.clear();
        lane_timing_.clear();
        encoding_timing_.clear();
        progress_.in_flight_batches = 0U;
        if (!terminal_) { terminal_ = EvaluationRunTerminal{{}, progress_.completed_images, dataset_ ? dataset_->category_count() : 0U, true}; }
        progress_.terminal = true;
    }
    void update_profile_peaks() noexcept {
        if (!profile_) return;
        std::size_t task_count = lane_futures_.size();
        for (const PendingPredictionBatchEncoding& pending : encoding_queue_) task_count += pending.images.size();
        profile_->peak_in_flight_tasks = std::max(profile_->peak_in_flight_tasks, task_count);
        profile_->peak_in_flight_slots = std::max(profile_->peak_in_flight_slots, lane_futures_.size() + encoding_queue_.size());
    }
    void write_profile() const {
        const EvaluationDatasetOwner::Facts facts = dataset_->facts();
        const auto seconds = [](const std::uint64_t nanoseconds) { return static_cast<double>(nanoseconds) / 1.0e9; };
        const double total_wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - profile_->started_at).count();
        const nlohmann::json payload{
            {"event", "rfdetr.validation.pass"},
            {"event_source", profile_->event_source},
            {"validation_pass", profile_->validation_pass ? nlohmann::json(*profile_->validation_pass) : nlohmann::json(nullptr)},
            {"epoch", profile_->validation_pass ? nlohmann::json(*profile_->validation_pass) : nlohmann::json(nullptr)},
            {"model_source", profile_->model_source},
            {"batch_size", profile_->batch_size},
            {"image_height", profile_->image_height},
            {"image_width", profile_->image_width},
            {"query_count", profile_->query_count},
            {"persisted_max_instances_per_image", profile_->persisted_max_instances_per_image},
            {"resolved_query_count", profile_->resolved_query_count},
            {"backend_query_count", profile_->backend_query_count},
            {"resolved_detection_cap", profile_->resolved_detection_cap},
            {"query_count_automatic", profile_->query_count_automatic},
            {"detection_cap_automatic", profile_->detection_cap_automatic},
            {"class_count", profile_->class_count},
            {"precision", profile_->precision},
            {"box_precision", profile_->box_precision},
            {"expected_precision", profile_->expected_precision},
            {"sdp_backends",
             {{"flash", profile_->sdp_flash_enabled},
              {"memory_efficient", profile_->sdp_mem_efficient_enabled},
              {"math", profile_->sdp_math_enabled},
              {"cudnn", profile_->sdp_cudnn_enabled}}},
            {"metric_mode", evaluation_metric_set_name(facts.metric_set)},
            {"image_count", dataset_->image_count()},
            {"prediction_count", facts.prediction_count},
            {"ground_truth_count", facts.ground_truth_count},
            {"iou_candidate_count", profile_->iou_candidate_count.load(std::memory_order_relaxed)},
            {"mask_iou_candidate_count", profile_->mask_iou_candidate_count.load(std::memory_order_relaxed)},
            {"mask_task_count", profile_->mask_task_count.load(std::memory_order_relaxed)},
            {"transferred_bytes", profile_->transferred_bytes},
            {"mask_transferred_bytes", profile_->mask_transferred_bytes},
            {"mask_rle_pairs_loaded", facts.mask_rle_pair_count},
            {"peak_in_flight_tasks", profile_->peak_in_flight_tasks},
            {"peak_in_flight_slots", profile_->peak_in_flight_slots},
            {"phases",
             {{"loader_wait_seconds", profile_->loader_wait_seconds},
              {"preprocessing_seconds", profile_->preprocessing_seconds},
              {"model_forward_seconds", profile_->model_forward_seconds},
              {"postprocess_seconds", profile_->postprocess_seconds},
              {"d2h_wait_seconds", seconds(profile_->d2h_wait_nanoseconds.load(std::memory_order_relaxed))},
              {"cpu_encode_seconds", seconds(profile_->cpu_encode_nanoseconds.load(std::memory_order_relaxed))},
              {"cpu_match_seconds", seconds(profile_->cpu_match_nanoseconds.load(std::memory_order_relaxed))},
              {"final_sort_ap_seconds", seconds(profile_->final_sort_ap_nanoseconds.load(std::memory_order_relaxed))},
              {"total_wall_seconds", total_wall_seconds}}},
        };
        std::ofstream stream(profile_->jsonl_path, std::ios::app);
        if (!stream.is_open()) throw std::runtime_error("failed to append RF-DETR validation profile: " + profile_->jsonl_path.string());
        stream << payload.dump() << '\n';
    }
    EvaluationRunConfig config;
    std::unique_ptr<EvaluationDatasetOwner> dataset_;
    std::vector<EvaluationPredictionLane> lanes_;
    std::deque<std::future<StagedPredictionBatch>> lane_futures_;
    std::deque<PendingPredictionBatchEncoding> encoding_queue_;
    std::unique_ptr<EvaluationCudaTimingPool> timing_pool_;
    std::deque<EvaluationCudaTimingLease> unsubmitted_timing_;
    std::deque<EvaluationCudaTimingLease> lane_timing_;
    std::deque<EvaluationCudaTimingLease> encoding_timing_;
    std::unique_ptr<EvaluationProfileRecord> profile_;
    EvaluationRunProgress progress_{};
    std::optional<EvaluationRunTerminal> terminal_;
};
TrainingEvaluationRunOwner::TrainingEvaluationRunOwner(EvaluationRunConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
TrainingEvaluationRunOwner::~TrainingEvaluationRunOwner() = default;
TrainingEvaluationRunOwner::TrainingEvaluationRunOwner(TrainingEvaluationRunOwner&&) noexcept = default;
TrainingEvaluationRunOwner& TrainingEvaluationRunOwner::operator=(TrainingEvaluationRunOwner&&) noexcept = default;
void TrainingEvaluationRunOwner::begin() { impl_->begin(); }
void TrainingEvaluationRunOwner::cancel() noexcept { impl_->cancel_and_drain(); }
EvaluationRunProgress TrainingEvaluationRunOwner::progress() const noexcept { return impl_->progress_; }
std::optional<EvaluationRunTerminal> TrainingEvaluationRunOwner::terminal() const { return impl_->terminal_; }
std::vector<int> TrainingEvaluationRunOwner::image_ids() const { return impl_->dataset_ ? impl_->dataset_->image_ids() : std::vector<int>{}; }
void TrainingEvaluationRunOwner::load_dataset(mmltk::backend::data::DatasetLoader& loader) {
    if (!impl_->lane_futures_.empty() || !impl_->encoding_queue_.empty()) {
        throw std::logic_error("evaluation dataset cannot change while work is in flight");
    }
    impl_->dataset_ = std::make_unique<EvaluationDatasetOwner>(loader, impl_->config.metric_set);
}
EvaluationDatasetOwner& TrainingEvaluationRunOwner::dataset() noexcept { return *impl_->dataset_; }
const EvaluationDatasetOwner& TrainingEvaluationRunOwner::dataset() const noexcept { return *impl_->dataset_; }
std::size_t TrainingEvaluationRunOwner::pending_lane_count() const noexcept { return impl_->lane_futures_.size(); }
std::size_t TrainingEvaluationRunOwner::pending_encoding_count() const noexcept { return impl_->encoding_queue_.size(); }
bool TrainingEvaluationRunOwner::profiling() const noexcept { return impl_->profile_ != nullptr; }
void TrainingEvaluationRunOwner::configure_profile(EvaluationProfileSetup setup) {
    if (!impl_->profile_) throw std::logic_error("evaluation profiling is disabled");
    static_cast<EvaluationProfileSetup&>(*impl_->profile_) = std::move(setup);
    impl_->profile_->precision = "unknown";
    impl_->profile_->box_precision = "unknown";
    capture_sdp_backend_flags(*impl_->profile_);
    impl_->profile_->resolved_query_count = impl_->profile_->query_count;
    impl_->profile_->backend_query_count = impl_->profile_->query_count;
}
void TrainingEvaluationRunOwner::record_loader_wait(const double seconds) noexcept {
    if (impl_->profile_) impl_->profile_->loader_wait_seconds += seconds;
}
void TrainingEvaluationRunOwner::record_timing_start(const EvaluationCudaTimingLease lease, const EvaluationCudaBatchTiming::Phase phase, void* stream) {
    if (lease) lease.timing->record_start(phase, static_cast<cudaStream_t>(stream));
}
void TrainingEvaluationRunOwner::record_timing_stop(const EvaluationCudaTimingLease lease, const EvaluationCudaBatchTiming::Phase phase, void* stream) {
    if (lease) lease.timing->record_stop(phase, static_cast<cudaStream_t>(stream));
}
void TrainingEvaluationRunOwner::record_model_output(std::string precision, std::string box_precision, const std::size_t query_count,
                                                     const std::size_t class_count) {
    if (!impl_->profile_) return;
    impl_->profile_->precision = std::move(precision);
    impl_->profile_->box_precision = std::move(box_precision);
    impl_->profile_->query_count = query_count;
    impl_->profile_->backend_query_count = query_count;
    impl_->profile_->class_count = class_count;
}
void TrainingEvaluationRunOwner::record_prediction_transfer(const PostprocessedBatch& processed, const std::size_t image_count) {
    if (impl_->profile_) accumulate_prediction_transfer_bytes(*impl_->profile_, processed, image_count);
}
EvaluationCudaTimingLease TrainingEvaluationRunOwner::acquire_timing() {
    if (!impl_->timing_pool_) return {};
    EvaluationCudaTimingLease lease = impl_->timing_pool_->acquire();
    try {
        impl_->unsubmitted_timing_.push_back(lease);
    } catch (...) {
        impl_->timing_pool_->release(lease);
        throw;
    }
    return lease;
}
void TrainingEvaluationRunOwner::submit(mmltk::common::concurrency::WorkerPool& lane_pool, EvaluationLaneWork work, EvaluationCudaTimingLease timing) {
    const std::size_t lane_index = impl_->progress_.submitted_batches % impl_->lanes_.size();
    EvaluationPredictionLane& lane = impl_->lanes_[lane_index];
    if (static_cast<bool>(timing) != profiling() ||
        (timing && (impl_->unsubmitted_timing_.empty() || impl_->unsubmitted_timing_.front().slot_index != timing.slot_index ||
                    impl_->unsubmitted_timing_.front().timing != timing.timing))) {
        throw std::logic_error("evaluation timing custody was not acquired by this run");
    }
    // Reserve both owning records before admitting a task. Assignment of its
    // future is noexcept, so no submitted work can escape queue custody.
    impl_->lane_futures_.emplace_back();
    try {
        if (timing) impl_->lane_timing_.push_back(timing);
    } catch (...) {
        impl_->lane_futures_.pop_back();
        throw;
    }
    try {
        impl_->lane_futures_.back() = lane_pool.enqueue([&lane, work = std::move(work)]() mutable {
            auto owned_work = std::move(work);
            return owned_work(lane);
        });
    } catch (...) {
        if (timing) impl_->lane_timing_.pop_back();
        impl_->lane_futures_.pop_back();
        throw;
    }
    if (timing) impl_->unsubmitted_timing_.pop_front();
    ++impl_->progress_.submitted_batches;
    ++impl_->progress_.in_flight_batches;
    impl_->update_profile_peaks();
}
void TrainingEvaluationRunOwner::drain_lane(mmltk::common::concurrency::WorkerPool& cpu_pool) {
    impl_->encoding_queue_.emplace_back();
    try {
        if (impl_->profile_) impl_->encoding_timing_.push_back(impl_->lane_timing_.front());
    } catch (...) {
        impl_->encoding_queue_.pop_back();
        throw;
    }
    try {
        StagedPredictionBatch staged = impl_->lane_futures_.front().get();
        impl_->encoding_queue_.back() = enqueue_prediction_batch_encoding(cpu_pool, std::move(staged), impl_->profile_.get(), &dataset());
    } catch (...) {
        // Admission settles any partial image batch before it throws. The lane
        // timing remains with the original lane receipt for cancellation.
        if (impl_->profile_) impl_->encoding_timing_.pop_back();
        impl_->encoding_queue_.pop_back();
        throw;
    }
    impl_->lane_futures_.pop_front();
    if (impl_->profile_) impl_->lane_timing_.pop_front();
    impl_->update_profile_peaks();
}
std::size_t TrainingEvaluationRunOwner::drain_encoding() {
    std::vector<PredictionBatchItem> completed = collect_prediction_batch_encoding(std::move(impl_->encoding_queue_.front()));
    if (impl_->profile_) {
        EvaluationCudaTimingLease& timing = impl_->encoding_timing_.front();
        timing.timing->accumulate(*impl_->profile_);
        impl_->timing_pool_->release(timing);
        impl_->encoding_timing_.pop_front();
    }
    impl_->encoding_queue_.pop_front();
    for (auto& image : completed) {
        if (!image.evaluation_matches) throw std::logic_error("validation image task omitted compact evaluation matches");
        dataset().merge_matches(std::move(*image.evaluation_matches));
    }
    impl_->progress_.completed_images += completed.size();
    if (impl_->progress_.in_flight_batches != 0U) --impl_->progress_.in_flight_batches;
    return completed.size();
}
EvalSummary TrainingEvaluationRunOwner::evaluate(const std::size_t max_dets_per_image, mmltk::common::concurrency::WorkerPool& cpu_pool) {
    const auto started = impl_->profile_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    EvalSummary summary = dataset().evaluate(max_dets_per_image, cpu_pool, EvaluationDetailRetention::CompactOnly);
    if (impl_->profile_) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
        impl_->profile_->final_sort_ap_nanoseconds.fetch_add(static_cast<std::uint64_t>(elapsed.count()), std::memory_order_relaxed);
        impl_->write_profile();
    }
    return summary;
}
void TrainingEvaluationRunOwner::settle(EvalSummary summary) {
    if (!impl_->lane_futures_.empty() || !impl_->encoding_queue_.empty() || !impl_->unsubmitted_timing_.empty() || !impl_->lane_timing_.empty() ||
        !impl_->encoding_timing_.empty())
        throw std::logic_error("evaluation run cannot settle with outstanding work");
    if (impl_->terminal_) throw std::logic_error("evaluation run already reached a terminal outcome");
    impl_->terminal_ =
        EvaluationRunTerminal{std::move(summary), impl_->progress_.completed_images, impl_->dataset_->category_count(), impl_->progress_.cancelled};
    impl_->progress_.terminal = true;
}
}  // namespace mmltk::backend::models::rfdetr
