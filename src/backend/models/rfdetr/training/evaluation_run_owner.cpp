#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/backend/models/rfdetr/core/detection_ops.h"
#include "src/backend/models/rfdetr/core/execution_precision.h"
#include "src/backend/models/rfdetr/core/sample_output.h"
#include "src/backend/models/rfdetr/core/detail/matcher_workspace.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/common/math/checked_arithmetic.h"
#include <c10/core/InferenceMode.h>
#include <random>
#include <spdlog/spdlog.h>
#include "spdmon/spdmon.hpp"
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
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/core/postprocess.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/frameworks/gpu/cuda_priority.h"
#include <torch/types.h>
#include <torch/serialize.h>
#include "detail/evaluation_runtime.h"
import mmltk.backend.ml.cuda.gpu_quiescence;
import mmltk.backend.models.rfdetr.core.dataset_limit_resolution;
import mmltk.common.logging.profile_utils;
import mmltk.common.logging.mmltk_logging;
namespace mmltk::backend::models::rfdetr {
using mmltk::common::math::checked_cast;
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
PredictionBufferLease TrainingEvaluationRunOwner::acquire_next_prediction_slot() {
    // The existing lane/encoding drain bounds leave a free slot for the next
    // round-robin submission. Its metadata and pinned tensors share one lease.
    const std::size_t lane_index = impl_->progress_.submitted_batches % impl_->lanes_.size();
    return impl_->lanes_[lane_index].slot_pool->acquire();
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
std::size_t prediction_lane_slot_count(const RuntimeSplit& split, const std::size_t batch_size) {
    const auto lanes = static_cast<std::size_t>(std::max(1, split.lane_threads));
    const auto cpus = static_cast<std::size_t>(std::max(1, split.cpu_threads));
    const auto wave = lanes * std::max<std::size_t>(1U, batch_size);
    return std::max<std::size_t>(2U, 1U + (cpus + wave - 1U) / wave);
}
std::size_t prediction_cpu_batch_limit(const RuntimeSplit& split, const std::size_t batch_size) {
    const auto cpus = static_cast<std::size_t>(std::max(1, split.cpu_threads));
    const auto images = std::max<std::size_t>(1U, batch_size);
    return std::max<std::size_t>(2U, (cpus + images - 1U) / images);
}
PhaseTiming elapsed_timing(const std::chrono::steady_clock::time_point start, const std::size_t images) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return {
        seconds,
        seconds > 0.0 ? static_cast<double>(images) / seconds : 0.0,
        images,
    };
}
struct CapturedEvalSample {
    torch::Tensor image;
    torch::Tensor boxes;
    torch::Tensor labels;
    torch::Tensor masks;
};
OutputLayer narrow_output_layer_batch(const OutputLayer& layer, int64_t count) {
    OutputLayer narrowed;
    narrowed.pred_logits = layer.pred_logits.narrow(0, 0, count);
    narrowed.pred_boxes = layer.pred_boxes.narrow(0, 0, count);
    if (layer.query_features.has_value()) { narrowed.query_features = layer.query_features->narrow(0, 0, count); }
    narrowed.query_layout = layer.query_layout;
    if (layer.pred_masks.has_value()) { narrowed.pred_masks = layer.pred_masks->narrow(0, 0, count); }
    if (layer.sparse_pred_masks.has_value()) {
        narrowed.sparse_pred_masks = OutputLayer::SparsePredMasks{
            layer.sparse_pred_masks->spatial_features.narrow(0, 0, count),
            layer.sparse_pred_masks->query_features.narrow(0, 0, count),
            layer.sparse_pred_masks->bias,
        };
    }
    return narrowed;
}
ModelOutputs narrow_model_outputs_batch(const ModelOutputs& outputs, int64_t count) {
    ModelOutputs narrowed;
    narrowed.main = narrow_output_layer_batch(outputs.main, count);
    narrowed.aux_outputs.reserve(outputs.aux_outputs.size());
    for (const auto& layer : outputs.aux_outputs) { narrowed.aux_outputs.push_back(narrow_output_layer_batch(layer, count)); }
    if (outputs.enc_outputs.has_value()) { narrowed.enc_outputs = narrow_output_layer_batch(*outputs.enc_outputs, count); }
    return narrowed;
}
struct TrainingValidationRuntime::Impl {
   public:
    Impl(const TrainRequest& options, RuntimeContext& runtime, std::unique_ptr<mmltk::backend::data::DatasetLoader> loader, size_t batch_size, bool enable_loss,
         EvaluationMetricSet metric_set, const int64_t prediction_capacity, std::string split_name, const bool query_count_automatic)
        : runtime_(runtime),
          loader_(std::move(loader)),
          batch_size_(std::max<size_t>(1, batch_size)),
          target_scratch_(enable_loss ? std::make_unique<TargetScratch>() : nullptr),
          lane_pool_(static_cast<size_t>(runtime.split().lane_threads), runtime.lane_cpus(), "rfdtrnlane", 0U, &runtime.execution().placement, false),
          split_name_(std::move(split_name)),
          query_count_automatic_(query_count_automatic) {
        if (!loader_) { throw std::invalid_argument("training validation runtime requires a dataset loader"); }
        detection_limit_ = resolve_dataset_limit(loader_->max_instances_per_image(), options.eval_max_dets);
        torch_cuda::TorchCudaDeviceGuard device_guard(torch_cuda::checked_device_index(options.device_id));
        amp_enabled_ = options.amp;
        inference_dtype_ = amp_enabled_ ? resolve_cuda_autocast_dtype() : at::kFloat;
        const auto batch_capacity = static_cast<int64_t>(batch_size_);
        batch_tensors_.ensure(batch_capacity, static_cast<int>(loader_->image_height()), static_cast<int>(loader_->image_width()), options.device_id);
        preprocessor_ = std::make_unique<GpuBatchPreprocessor>(batch_capacity, static_cast<int>(loader_->image_height()),
                                                               static_cast<int>(loader_->image_width()), options.device_id, inference_dtype_);
        const size_t slot_count = prediction_lane_slot_count(runtime.split(), batch_size_);
        const size_t encoding_capacity = prediction_cpu_batch_limit(runtime.split(), batch_size_);
        evaluation_run_ = std::make_unique<TrainingEvaluationRunOwner>(EvaluationRunConfig{
            metric_set,
            batch_size_,
            static_cast<size_t>(prediction_capacity),
            static_cast<size_t>(runtime.split().lane_threads),
            slot_count,
            encoding_capacity,
            checked_cast<std::uint32_t>(loader_->image_height(), "RF-DETR image height exceeds uint32_t"),
            checked_cast<std::uint32_t>(loader_->image_width(), "RF-DETR image width exceeds uint32_t"),
            options.device_id,
            options.validation_profile,
        });
        evaluation_run_->load_dataset(*loader_);
        image_ids_ = evaluation_run_->image_ids();
    }
    void begin_pass() {
        evaluation_run_->begin();
        loader_->begin_epoch();
    }
    torch::Tensor preprocess(const mmltk::backend::data::Batch& batch) { return preprocessor_->run(batch, static_cast<int64_t>(batch_size_)); }
    void record_preprocess_consumer(cudaStream_t stream) { preprocessor_->record_consumer(stream); }
    RuntimeContext& runtime() { return runtime_; }
    size_t batch_size() const { return batch_size_; }
    mmltk::backend::data::DatasetLoader& loader() { return *loader_; }
    const std::vector<int>& image_ids() const { return image_ids_; }
    bool amp_enabled() const noexcept { return amp_enabled_; }
    at::ScalarType inference_dtype() const noexcept { return inference_dtype_; }
    torch::Tensor nested_mask() const { return batch_tensors_.nested_mask_view(static_cast<int64_t>(batch_size_)); }
    TargetScratch& target_scratch() {
        if (!target_scratch_) { throw std::logic_error("validation target scratch requested while validation loss is disabled"); }
        return *target_scratch_;
    }
    mmltk::common::concurrency::WorkerPool& lane_pool() { return lane_pool_; }
    TrainingEvaluationRunOwner& evaluation_run() noexcept { return *evaluation_run_; }
    EvaluationSampleWriter& sample_writer() noexcept { return sample_writer_; }
    std::string_view split_name() const noexcept { return split_name_; }
    const ResolvedDatasetLimit& detection_limit() const noexcept { return detection_limit_; }
    bool query_count_automatic() const noexcept { return query_count_automatic_; }

   private:
    RuntimeContext& runtime_;
    std::unique_ptr<mmltk::backend::data::DatasetLoader> loader_;
    size_t batch_size_ = 1;
    std::unique_ptr<TrainingEvaluationRunOwner> evaluation_run_;
    EvaluationSampleWriter sample_writer_;
    std::vector<int> image_ids_;
    BatchStaticTensors batch_tensors_;
    std::unique_ptr<GpuBatchPreprocessor> preprocessor_;
    std::unique_ptr<TargetScratch> target_scratch_;
    mmltk::common::concurrency::WorkerPool lane_pool_;
    std::string split_name_;
    ResolvedDatasetLimit detection_limit_;
    at::ScalarType inference_dtype_ = at::kFloat;
    bool query_count_automatic_ = false;
    bool amp_enabled_ = false;
};
TrainingValidationRuntime::TrainingValidationRuntime(const TrainRequest& options, RuntimeContext& runtime,
                                                     std::unique_ptr<mmltk::backend::data::DatasetLoader> loader, size_t batch_size, bool enable_loss,
                                                     EvaluationMetricSet metric_set, const int64_t prediction_capacity, std::string split_name,
                                                     const bool query_count_automatic)
    : impl_(std::make_unique<Impl>(options, runtime, std::move(loader), batch_size, enable_loss, metric_set, prediction_capacity, std::move(split_name),
                                   query_count_automatic)) {}
TrainingValidationRuntime::~TrainingValidationRuntime() = default;
void TrainingValidationRuntime::begin_pass() { impl_->begin_pass(); }
torch::Tensor TrainingValidationRuntime::preprocess(const mmltk::backend::data::Batch& batch) { return impl_->preprocess(batch); }
void TrainingValidationRuntime::record_preprocess_consumer(cudaStream_t stream) { impl_->record_preprocess_consumer(stream); }
RuntimeContext& TrainingValidationRuntime::runtime() { return impl_->runtime(); }
size_t TrainingValidationRuntime::batch_size() const { return impl_->batch_size(); }
mmltk::backend::data::DatasetLoader& TrainingValidationRuntime::loader() { return impl_->loader(); }
const std::vector<int>& TrainingValidationRuntime::image_ids() const { return impl_->image_ids(); }
bool TrainingValidationRuntime::amp_enabled() const noexcept { return impl_->amp_enabled(); }
at::ScalarType TrainingValidationRuntime::inference_dtype() const noexcept { return impl_->inference_dtype(); }
torch::Tensor TrainingValidationRuntime::nested_mask() const { return impl_->nested_mask(); }
TargetScratch& TrainingValidationRuntime::target_scratch() { return impl_->target_scratch(); }
mmltk::common::concurrency::WorkerPool& TrainingValidationRuntime::lane_pool() { return impl_->lane_pool(); }
TrainingEvaluationRunOwner& TrainingValidationRuntime::evaluation_run() noexcept { return impl_->evaluation_run(); }
EvaluationSampleWriter& TrainingValidationRuntime::sample_writer() noexcept { return impl_->sample_writer(); }
std::string_view TrainingValidationRuntime::split_name() const noexcept { return impl_->split_name(); }
std::size_t TrainingValidationRuntime::detection_limit() const noexcept { return impl_->detection_limit().as_size; }
bool TrainingValidationRuntime::automatic_detection_limit() const noexcept { return impl_->detection_limit().automatic; }
bool TrainingValidationRuntime::query_count_automatic() const noexcept { return impl_->query_count_automatic(); }
EvalPassResult evaluate_model(const TrainRequest& options, TrainingValidationRuntime& validation, NativeRfDetrModel& model, TrainingEventOwner& event_owner,
                              const DetectionConfig& detection_config, bool calculate_loss, EvaluationPurpose purpose, EvaluatedWeights evaluated_weights,
                              std::optional<int> current_epoch, TrainingMetricHandoff* metrics) {
    const bool capture_eval_sample = purpose == EvaluationPurpose::ScheduledValidation;
    if (capture_eval_sample && !current_epoch) throw std::logic_error("scheduled validation requires its epoch");
    mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_total{"rfdetr.train.eval.total"};
    RuntimeContext& runtime = validation.runtime();
    mmltk::backend::data::DatasetLoader& loader = validation.loader();
    const std::vector<int>& image_ids = validation.image_ids();
    EvalPassResult result;
    c10::InferenceMode inference_mode;
    struct RestoreMode final {
        NativeRfDetrModel& module;
        bool training;
        ~RestoreMode() { module.train(training); }
    } restore_mode{(model), (model).is_training()};
    model.eval();
    validation.begin_pass();
    TrainingEvaluationRunOwner& evaluation_run = validation.evaluation_run();
    auto cancel_unsettled_run = std::unique_ptr<TrainingEvaluationRunOwner, void (*)(TrainingEvaluationRunOwner*)>{
        &validation.evaluation_run(), [](TrainingEvaluationRunOwner* owner) { owner->cancel(); }};
    const bool amp_enabled = validation.amp_enabled();
    const at::ScalarType autocast_dtype = validation.inference_dtype();
    const bool match_free_loss = calculate_loss && model.config().training_supervision.assignment == TrainAssignmentKind::MatchFree;
    const auto started_at = std::chrono::steady_clock::now();
    const bool profiling = evaluation_run.profiling();
    if (profiling) {
        evaluation_run.configure_profile(EvaluationProfileSetup{
            options.output_dir / "validation_profile.jsonl",
            "training_validation",
            purpose == EvaluationPurpose::FinalTest ? "test" : (evaluated_weights == EvaluatedWeights::Ema ? "ema" : "model"),
            evaluation_precision_name(autocast_dtype),
            current_epoch.has_value() ? std::optional<int>(*current_epoch + 1) : std::nullopt,
            evaluation_run.dataset().facts().metric_set,
            validation.batch_size(),
            loader.image_height(),
            loader.image_width(),
            static_cast<size_t>(model.config().num_queries),
            loader.max_instances_per_image(),
            validation.detection_limit(),
            static_cast<size_t>(model.config().num_classes),
            validation.query_count_automatic(),
            validation.automatic_detection_limit(),
            started_at,
        });
    }
    size_t image_count = 0;
    size_t batch_count = 0;
    size_t seen_images = 0;
    if (calculate_loss && !metrics) throw std::logic_error("validation loss requires the retained scalar handoff");
    if (calculate_loss) metrics->begin_validation();
    std::optional<CapturedEvalSample> captured_sample;
    std::unique_ptr<spdmon::ProgressBar> progress;
    if (options.progress_bar) {
        auto label = purpose == EvaluationPurpose::FinalTest
                         ? std::string("test")
                         : std::format("{} {}/{}", evaluated_weights == EvaluatedWeights::Ema ? "ema" : "val", *current_epoch + 1, options.epochs);
        progress = std::make_unique<spdmon::ProgressBar>(std::move(label), loader.num_images(), "img");
    }
    std::mt19937_64 sample_rng(static_cast<uint64_t>(options.seed) ^
                               (current_epoch.has_value() ? (0x9e3779b97f4a7c15ULL + static_cast<uint64_t>(*current_epoch + 1)) : 0xd1b54a32d192ed03ULL));
    mmltk::common::concurrency::WorkerPool& lane_pool = validation.lane_pool();
    mmltk::common::concurrency::WorkerPool& cpu_pool = runtime.cpu_pool();
    torch_cuda::TorchCudaDeviceGuard device_guard(torch_cuda::checked_device_index(options.device_id));
    const size_t max_cpu_futures = prediction_cpu_batch_limit(runtime.split(), validation.batch_size());
    auto drain_cpu = [&]() {
        const size_t completed_count = evaluation_run.drain_encoding();
        image_count += completed_count;
        if (progress) { progress->add(completed_count); }
    };
    mmltk::backend::data::Batch batch{};
    ClassPostprocessLane evaluation_classes(model.class_layout());
    evaluation_classes.Prepare(mmltk::backend::ml::cuda::cuda_device(options.device_id));
    while (loader.next_batch(batch)) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_batch{"rfdetr.train.eval.batch"};
        {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_wait_batch{"rfdetr.train.eval.wait_batch"};
            if (profiling) {
                const auto phase_started = std::chrono::steady_clock::now();
                loader.wait_batch(batch);
                evaluation_run.record_loader_wait(std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_started).count());
            }
        }
        LoaderBatchGuard batch_guard(loader, batch, options.device_id);
        mmltk::common::logging::profile_add_value("rfdetr.train.eval.images", batch.num_images);
        EvaluationCudaTimingLease batch_timing;
        cudaStream_t evaluation_stream = nullptr;
        if (profiling) {
            batch_timing = evaluation_run.acquire_timing();
            evaluation_stream = torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(options.device_id)).stream();
            evaluation_run.record_timing_start(batch_timing, EvaluationCudaBatchTiming::Phase::Preprocessing, evaluation_stream);
        }
        torch::Tensor inference_input;
        {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_normalize{"rfdetr.train.eval.normalize"};
            inference_input = validation.preprocess(batch);
        }
        if (batch_timing) { evaluation_run.record_timing_stop(batch_timing, EvaluationCudaBatchTiming::Phase::Preprocessing, evaluation_stream); }
        std::optional<size_t> sampled_image_pos;
        torch::Tensor sampled_image;
        if (capture_eval_sample) {
            for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
                ++seen_images;
                std::uniform_int_distribution<size_t> select_current(0, seen_images - 1);
                if (select_current(sample_rng) == 0) {
                    sampled_image_pos = image_pos;
                    sampled_image = make_device_batch_tensor(batch, options.device_id, loader.image_height(), loader.image_width())
                                        .select(0, static_cast<int64_t>(image_pos))
                                        .clone();
                }
            }
        }
        auto prediction_slot = evaluation_run.acquire_next_prediction_slot();
        auto& prediction_metadata = prediction_slot.buffers->images;
        prepare_prediction_batch_metadata(prediction_metadata, batch, image_ids);
        std::optional<PreparedTargets> prepared;
        if (calculate_loss) {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_targets{"rfdetr.train.eval.targets"};
            prepared =
                build_targets(batch, static_cast<int>(loader.image_height()), static_cast<int>(loader.image_width()), detection_config.include_masks,
                              detection_config.include_masks, options.device_id, validation.target_scratch(), validation.split_name(),
                              model.config().num_queries, model.config().training_supervision, static_cast<int>(model.class_layout()->catalog()->size()));
        }
        batch_guard.release();
        std::optional<TargetConsumerLease> target_consumer;
        if (prepared) { target_consumer.emplace(validation.target_scratch(), *prepared, options.device_id); }
        ModelOutputs outputs;
        {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_forward{"rfdetr.train.eval.forward"};
            if (batch_timing) { evaluation_run.record_timing_start(batch_timing, EvaluationCudaBatchTiming::Phase::ModelForward, evaluation_stream); }
            {
                mmltk::backend::ml::cuda::TorchAutocastScope autocast_guard(amp_enabled, autocast_dtype);
                outputs = match_free_loss ? model.forward_for_match_free(NestedTensor{inference_input, validation.nested_mask()})
                                          : model.forward(NestedTensor{inference_input, validation.nested_mask()}, true);
            }
            assert_inference_output_dtype(outputs.main.pred_logits, outputs.main.pred_boxes, autocast_dtype, "RF-DETR training validation");
            validation.record_preprocess_consumer(torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(options.device_id)).stream());
            if (profiling) {
                evaluation_run.record_model_output(
                    evaluation_precision_name(outputs.main.pred_logits.scalar_type()), evaluation_precision_name(outputs.main.pred_boxes.scalar_type()),
                    static_cast<size_t>(outputs.main.pred_logits.size(1)), static_cast<size_t>(outputs.main.pred_logits.size(2)));
            }
            if (batch_timing) { evaluation_run.record_timing_stop(batch_timing, EvaluationCudaBatchTiming::Phase::ModelForward, evaluation_stream); }
        }
        const auto real_count = static_cast<int64_t>(batch.num_images);
        if (outputs.main.pred_logits.size(0) < real_count) { throw std::runtime_error("RF-DETR validation output batch is smaller than the input batch"); }
        std::optional<ModelOutputs> real_outputs;
        const ModelOutputs* evaluated_outputs = &outputs;
        if (outputs.main.pred_logits.size(0) != real_count) {
            real_outputs = narrow_model_outputs_batch(outputs, real_count);
            evaluated_outputs = &*real_outputs;
        }
        if (calculate_loss) {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_targets_handoff{"rfdetr.train.eval.targets_handoff"};
            target_consumer->handoff();
            torch::Tensor loss;
            auto& model_owner = (model);
            SupervisionTimingLease criterion_timing(model_owner, training_supervision_enabled(model.config().training_supervision),
                                                    SupervisionTimingLease::Kind::Criterion);
            if (match_free_loss) {
                const auto target_count = torch::tensor({static_cast<float>(prepared_target_count(*prepared))},
                                                        torch::TensorOptions().dtype(torch::kFloat32).device(inference_input.device()));
                loss = (model).supervision_loss(*evaluated_outputs, *prepared, DeviceLossNormalizer{target_count.select(0, 0)}, false).total;
            } else {
                TensorMap loss_dict;
                {
                    mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_loss_dict{"rfdetr.train.eval.loss_dict"};
                    loss_dict = detection_loss_dict(*evaluated_outputs, *prepared, detection_config, false, false);
                    runtime.matcher_workspace().complete_assignments(
                        torch_cuda::getCurrentCUDAStream(torch_cuda::checked_device_index(options.device_id)).stream());
                }
                mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_loss_total{"rfdetr.train.eval.loss_total"};
                loss = weighted_detection_loss(loss_dict, detection_config, inference_input.device());
            }
            criterion_timing.finish();
            target_consumer->retire();
            metrics->accumulate_validation(loss);
            static_cast<void>(model.harvest_supervision_timing());
            ++batch_count;
        }
        PostprocessedBatch processed = [&]() {
            mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_postprocess{"rfdetr.train.eval.postprocess"};
            if (batch_timing) { evaluation_run.record_timing_start(batch_timing, EvaluationCudaBatchTiming::Phase::Postprocess, evaluation_stream); }
            PostprocessedBatch postprocessed = postprocess_output_batch_fixed_size(
                OutputTensors{evaluated_outputs->main.pred_logits, evaluated_outputs->main.pred_boxes, evaluated_outputs->main.pred_masks},
                static_cast<int64_t>(loader.image_height()), static_cast<int64_t>(loader.image_width()), model.config().num_select, &evaluation_classes);
            if (batch_timing) { evaluation_run.record_timing_stop(batch_timing, EvaluationCudaBatchTiming::Phase::Postprocess, evaluation_stream); }
            return postprocessed;
        }();
        auto processed_ready = record_current_stream_event(event_owner.pool(), options.device_id, "record train eval prediction readiness");
        if (!processed_ready.has_value()) { throw std::runtime_error("training event pool is unavailable for evaluation"); }
        const size_t processed_count = std::min(static_cast<size_t>(processed.size()), static_cast<size_t>(batch.num_images));
        evaluation_run.record_prediction_transfer(processed, processed_count);
        prediction_metadata.resize(processed_count);
        if (sampled_image_pos.has_value() && *sampled_image_pos < processed_count) {
            const auto image_index = static_cast<int64_t>(*sampled_image_pos);
            const auto scores = processed.scores[image_index];
            const auto labels = processed.labels[image_index];
            const auto boxes = processed.boxes[image_index];
            const torch::Tensor masks = processed.masks.has_value() ? (*processed.masks)[image_index] : torch::Tensor();
            const auto keep = scores.gt(0.35f);
            const auto keep_indices = keep.nonzero().squeeze(1);
            torch::Tensor filtered_masks;
            if (masks.defined()) { filtered_masks = masks.index_select(0, keep_indices).clone(); }
            captured_sample = CapturedEvalSample{
                std::move(sampled_image),
                boxes.index_select(0, keep_indices).clone(),
                labels.index_select(0, keep_indices).clone(),
                std::move(filtered_masks),
            };
        }
        evaluation_run.submit(
            lane_pool,
            [processed = std::move(processed), processed_ready = std::move(*processed_ready), lease = std::move(prediction_slot),
             category_count = loader.num_classes(), max_dets = validation.detection_limit(), device_id = options.device_id](auto& lane) mutable {
                processed_ready.wait(reinterpret_cast<std::uintptr_t>(lane.stream.stream()), "wait for train eval prediction readiness");
                processed_ready.retire();
                c10::InferenceMode lane_inference_mode;
                torch_cuda::TorchCudaDeviceGuard lane_device_guard(torch_cuda::checked_device_index(device_id));
                torch_cuda::TorchCudaStreamGuard stream_guard(lane.stream);
                return stage_prediction_batch(std::move(processed), category_count, max_dets, std::move(lease), device_id,
                                              reinterpret_cast<void*>(lane.stream.stream()));
            },
            batch_timing);
        while (static_cast<int>(evaluation_run.pending_lane_count()) >= runtime.split().lane_threads) { evaluation_run.drain_lane(cpu_pool); }
        while (evaluation_run.pending_encoding_count() >= max_cpu_futures) { drain_cpu(); }
    }
    while (evaluation_run.pending_lane_count() != 0U) {
        evaluation_run.drain_lane(cpu_pool);
        while (evaluation_run.pending_encoding_count() >= max_cpu_futures) { drain_cpu(); }
    }
    while (evaluation_run.pending_encoding_count() != 0U) { drain_cpu(); }
    if (capture_eval_sample && captured_sample.has_value()) {
        RenderSampleOptions render_options;
        render_options.num_classes = static_cast<int>(model.class_layout()->catalog()->size());
        render_options.output_path = options.output_dir / "eval_samples" / std::format("epoch_{}.png", *current_epoch + 1);
        validation.sample_writer().Draw(captured_sample->image, captured_sample->boxes, captured_sample->labels, captured_sample->masks, render_options);
    }
    if (progress) { progress->close(); }
    if (calculate_loss) { result.loss = metrics->validation_average(batch_count); }
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_train_eval_metric{"rfdetr.train.eval.metric"};
        result.summary = evaluation_run.evaluate(validation.detection_limit(), cpu_pool);
        result.summary.model_detection_budget = checked_cast<std::uint32_t>(model.config().num_select, "training model detection budget exceeds uint32_t");
    }
    evaluation_run.settle(result.summary);
    static_cast<void>(cancel_unsettled_run.release());
    validation.sample_writer().Flush();
    result.timing = elapsed_timing(started_at, image_count);
    return result;
}
}  // namespace mmltk::backend::models::rfdetr
