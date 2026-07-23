#include "mmltk/rfdetr/train.h"

#include "dataset_loader.h"
#include "mmltk/rfdetr/checkpoint.h"
#include "mmltk/rfdetr/detection_ops.h"
#include "mmltk/rfdetr/modules.h"
#include "mmltk/rfdetr/target_builder.h"
#include "mmltk/rfdetr/training_ops.h"
#include "rfdetr/gpu_augment.h"
#include "rfdetr/shared_cuda_event.h"
#include "mmltk_logging.h"
#include "profile_utils.h"
#include "rfdetr/archive_utils.h"
#include "rfdetr/checkpoint_internal.h"
#include "rfdetr/torch_cuda_utils.h"
#include "rfdetr/native_optimizer.h"
#include "rfdetr/predict_runtime_shared.h"
#include "spdmon/spdmon.hpp"
#include "rfdetr/runtime.h"
#include "rfdetr/validate_internal.h"
#include "mmltk/rfdetr/draw_cuda.h"

#include "rfdetr/common/dataset_utils.h"
#include "rfdetr/common/tensor_utils.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/core/InferenceMode.h>
#if defined(USE_C10D_NCCL)
#include <torch/csrc/distributed/c10d/FileStore.hpp>
#include <torch/csrc/distributed/c10d/ProcessGroupNCCL.hpp>
#endif
#include <torch/csrc/autograd/autograd.h>
#include <nlohmann/json.hpp>
#include <torch/nn/utils/clip_grad.h>
#include <torch/serialize.h>
#include <torch/version.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <fstream>
#include <future>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

using json = nlohmann::json;
using namespace validate_detail;

namespace {

struct OptimizerBuildResult {
    NativeOptimizer optimizer;
    std::vector<double> base_lrs;
};

struct EvalPassResult {
    std::optional<double> loss;
    EvalSummary summary;
    PhaseTiming timing;
};

struct CapturedEvalSample {
    torch::Tensor image;
    torch::Tensor boxes;
    torch::Tensor labels;
    torch::Tensor masks;
};

struct ResumeState {
    int start_epoch = 0;
    double best_regular = -std::numeric_limits<double>::infinity();
    double best_ema = -std::numeric_limits<double>::infinity();
    std::vector<StateDictEntry> ema_state;
};

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

int rfdetr_output_class_count(uint32_t dataset_class_count) {
    return static_cast<int>(dataset_class_count) + 1;
}

int checked_inference_batch_size(size_t batch_size) {
    const size_t effective_batch_size = std::max<size_t>(1, batch_size);
    if (effective_batch_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("batch_size exceeds supported inference compilation range");
    }
    return static_cast<int>(effective_batch_size);
}

std::string phase_progress_label(const char* phase, int epoch, int total_epochs) {
    return std::format("{} {}/{}", phase, epoch + 1, total_epochs);
}

struct EvalPredictionLane {
    c10::cuda::CUDAStream stream;
    std::shared_ptr<PredictionBufferSlotPool> slot_pool;
};

OutputLayer narrow_output_layer_batch(const OutputLayer& layer, int64_t count) {
    OutputLayer narrowed;
    narrowed.pred_logits = layer.pred_logits.narrow(0, 0, count);
    narrowed.pred_boxes = layer.pred_boxes.narrow(0, 0, count);
    if (layer.pred_masks.has_value()) {
        narrowed.pred_masks = layer.pred_masks->narrow(0, 0, count);
    }
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
    for (const auto& layer : outputs.aux_outputs) {
        narrowed.aux_outputs.push_back(narrow_output_layer_batch(layer, count));
    }
    if (outputs.enc_outputs.has_value()) {
        narrowed.enc_outputs = narrow_output_layer_batch(*outputs.enc_outputs, count);
    }
    return narrowed;
}

class TrainingValidationRuntime {
   public:
    TrainingValidationRuntime(const TrainOptions& options, RuntimeContext& runtime,
                              std::unique_ptr<DatasetLoader> loader, size_t batch_size, bool enable_loss,
                              EvaluationMetricSet metric_set, const int64_t prediction_capacity)
        : runtime_(runtime),
          loader_(std::move(loader)),
          batch_size_(std::max<size_t>(1, batch_size)),
          dataset_(metric_set),
          target_scratch_(enable_loss ? std::make_unique<TargetScratch>() : nullptr),
          lane_pool_(static_cast<size_t>(runtime.split().lane_threads), runtime.lane_cpus(), "rfdtrnlane") {
        if (!loader_) {
            throw std::invalid_argument("training validation runtime requires a dataset loader");
        }
        dataset_ = CocoDataset::load_from_loader(*loader_, metric_set);
        image_ids_ = dataset_.image_ids();

        c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
        amp_enabled_ = options.amp;
        inference_dtype_ = amp_enabled_ ? resolve_cuda_autocast_dtype() : at::kFloat;
        const auto batch_capacity = static_cast<int64_t>(batch_size_);
        batch_tensors_.ensure(batch_capacity, static_cast<int>(loader_->image_height()),
                              static_cast<int>(loader_->image_width()), options.device_id);
        preprocessor_ = std::make_unique<GpuBatchPreprocessor>(
            batch_capacity, static_cast<int>(loader_->image_height()), static_cast<int>(loader_->image_width()),
            options.device_id, inference_dtype_);

        const size_t slot_count = predict_internal::prediction_lane_slot_count(runtime.split(), batch_size_);
        const PredictionBufferConfig prediction_buffer_config{
            static_cast<int64_t>(batch_size_),
            prediction_capacity,
            metric_set == EvaluationMetricSet::BBoxAndMask
                ? std::make_optional(std::make_pair(loader_->image_height(), loader_->image_width()))
                : std::nullopt,
            options.device_id,
        };
        lanes_.reserve(static_cast<size_t>(runtime.split().lane_threads));
        for (int lane_index = 0; lane_index < runtime.split().lane_threads; ++lane_index) {
            lanes_.push_back(EvalPredictionLane{
                get_high_priority_cuda_stream(options.device_id),
                std::make_shared<PredictionBufferSlotPool>(slot_count, prediction_buffer_config),
            });
        }
    }

    void begin_pass() {
        dataset_.clear_predictions();
        loader_->begin_epoch();
    }

    torch::Tensor preprocess(const Batch& batch) {
        return preprocessor_->run(batch, static_cast<int64_t>(batch_size_));
    }
    void record_preprocess_consumer(cudaStream_t stream) {
        preprocessor_->record_consumer(stream);
    }

    RuntimeContext& runtime() {
        return runtime_;
    }
    size_t batch_size() const {
        return batch_size_;
    }
    DatasetLoader& loader() {
        return *loader_;
    }
    CocoDataset& dataset() {
        return dataset_;
    }
    const std::vector<int>& image_ids() const {
        return image_ids_;
    }
    bool amp_enabled() const noexcept {
        return amp_enabled_;
    }
    at::ScalarType inference_dtype() const noexcept {
        return inference_dtype_;
    }
    torch::Tensor nested_mask() const {
        return batch_tensors_.nested_mask_view(static_cast<int64_t>(batch_size_));
    }
    TargetScratch& target_scratch() {
        if (!target_scratch_) {
            throw std::logic_error("validation target scratch requested while validation loss is disabled");
        }
        return *target_scratch_;
    }
    WorkerPool& lane_pool() {
        return lane_pool_;
    }
    std::vector<EvalPredictionLane>& lanes() {
        return lanes_;
    }

   private:
    RuntimeContext& runtime_;
    std::unique_ptr<DatasetLoader> loader_;
    size_t batch_size_ = 1;
    CocoDataset dataset_;
    std::vector<int> image_ids_;
    BatchStaticTensors batch_tensors_;
    std::unique_ptr<GpuBatchPreprocessor> preprocessor_;
    std::unique_ptr<TargetScratch> target_scratch_;
    WorkerPool lane_pool_;
    std::vector<EvalPredictionLane> lanes_;
    at::ScalarType inference_dtype_ = at::kFloat;
    bool amp_enabled_ = false;
};

struct TrainLaneContext {
    c10::cuda::CUDAStream stream;
    TargetScratch target_scratch;
    std::unique_ptr<GpuBatchAugmenter> augmenter;
    std::shared_ptr<NativeRfDetrModel> model;
    std::vector<torch::Tensor> grad_params;
    size_t synced_parameter_version = std::numeric_limits<size_t>::max();
};

struct TrainLaneResult {
    torch::Tensor loss;
    torch::Tensor class_loss;
    torch::Tensor box_loss;
    std::vector<torch::Tensor> gradients;
    std::shared_ptr<SharedCudaEvent> ready_event;
};

struct TrainingMetricSnapshot {
    double loss_sum = 0.0;
    double class_loss_sum = 0.0;
    double box_loss_sum = 0.0;
    double step_loss = 0.0;
    double step_class_loss = 0.0;
    double step_box_loss = 0.0;
    bool loss_finite = true;
    bool gradients_finite = true;
};

class TrainingMetricHandoff {
   public:
    explicit TrainingMetricHandoff(int device_id) : device_id_(device_id) {
        const auto device_options = torch::TensorOptions().dtype(torch::kFloat32).device(cuda_device(device_id_));
        device_values_ = torch::zeros({8}, device_options);
        device_values_.select(0, 6).fill_(1.0f);
        host_values_ =
            torch::empty({8}, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU).pinned_memory(true));
        c10::cuda::CUDAGuard device_guard(checked_device_index(device_id_));
        ensure_cuda_ok(cudaEventCreateWithFlags(&copy_complete_, cudaEventDisableTiming),
                       "cudaEventCreateWithFlags for training metric handoff");
    }

    ~TrainingMetricHandoff() {
        if (copy_complete_ != nullptr) {
            cudaEventDestroy(copy_complete_);
        }
    }

    TrainingMetricHandoff(const TrainingMetricHandoff&) = delete;
    TrainingMetricHandoff& operator=(const TrainingMetricHandoff&) = delete;

    void reset_epoch() {
        device_values_.zero_();
        device_values_.select(0, 6).fill_(1.0f);
    }

    void begin_wave() {
        device_values_.narrow(0, 3, 3).zero_();
    }

    void accumulate(const torch::Tensor& loss, const torch::Tensor& class_loss, const torch::Tensor& box_loss) {
        const auto detached_loss = loss.detach();
        const auto detached_class_loss = class_loss.detach();
        const auto detached_box_loss = box_loss.detach();
        device_values_.select(0, 0).add_(detached_loss);
        device_values_.select(0, 1).add_(detached_class_loss);
        device_values_.select(0, 2).add_(detached_box_loss);
        device_values_.select(0, 3).add_(detached_loss);
        device_values_.select(0, 4).add_(detached_class_loss);
        device_values_.select(0, 5).add_(detached_box_loss);
        device_values_.select(0, 6).mul_(torch::isfinite(detached_loss).to(torch::kFloat32));
    }

    TrainingMetricSnapshot complete_step(const torch::Tensor& found_inf, int64_t wave_micro_batches) {
        device_values_.select(0, 7).copy_(found_inf);
        host_values_.copy_(device_values_, true);
        const auto device_index = checked_device_index(device_id_);
        const auto stream = c10::cuda::getCurrentCUDAStream(device_index);
        ensure_cuda_ok(cudaEventRecord(copy_complete_, stream.stream()), "cudaEventRecord for training metric handoff");
        ensure_cuda_ok(cudaEventSynchronize(copy_complete_), "cudaEventSynchronize for training metric handoff");

        const auto* values = host_values_.data_ptr<float>();
        const double wave_divisor = static_cast<double>(std::max<int64_t>(1, wave_micro_batches));
        TrainingMetricSnapshot snapshot;
        snapshot.loss_sum = static_cast<double>(values[0]);
        snapshot.class_loss_sum = static_cast<double>(values[1]);
        snapshot.box_loss_sum = static_cast<double>(values[2]);
        snapshot.step_loss = static_cast<double>(values[3]) / wave_divisor;
        snapshot.step_class_loss = static_cast<double>(values[4]) / wave_divisor;
        snapshot.step_box_loss = static_cast<double>(values[5]) / wave_divisor;
        snapshot.loss_finite = values[6] != 0.0f;
        snapshot.gradients_finite = values[7] == 0.0f;
        device_values_.select(0, 6).fill_(1.0f);
        device_values_.select(0, 7).zero_();
        return snapshot;
    }

    [[nodiscard]] torch::Tensor loss_sum() const {
        return device_values_.select(0, 0);
    }

   private:
    int device_id_ = 0;
    torch::Tensor device_values_;
    torch::Tensor host_values_;
    cudaEvent_t copy_complete_ = nullptr;
};

int effective_train_lanes(const TrainOptions& options) {
    if (options.lanes < 0) {
        throw std::runtime_error("RF-DETR train --lanes must be non-negative");
    }
    return std::max(1, options.lanes);
}

size_t micro_batches_per_optimizer_step(const TrainOptions& options, int train_lane_count) {
    return static_cast<size_t>(std::max(1, options.grad_accum_steps)) *
           static_cast<size_t>(std::max(1, train_lane_count));
}

size_t effective_batch_per_rank(const TrainOptions& options, int train_lane_count) {
    return static_cast<size_t>(options.batch_size) * micro_batches_per_optimizer_step(options, train_lane_count);
}

size_t effective_batch_global(const TrainOptions& options, const DistributedContext& distributed,
                              int train_lane_count) {
    return effective_batch_per_rank(options, train_lane_count) *
           static_cast<size_t>(std::max(1, distributed.world_size));
}

std::shared_ptr<SharedCudaEvent> record_current_stream_event(int device_id, const char* context) {
    return record_shared_cuda_event(c10::cuda::getCurrentCUDAStream(checked_device_index(device_id)).stream(), context);
}

int64_t batch_target_instance_count(const Batch& batch) {
    int64_t total = 0;
    for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
        const uint32_t dataset_index = batch.image_indices[image_pos];
        total += static_cast<int64_t>(batch.label_index[dataset_index].num_instances);
    }
    return total;
}

double resolve_num_boxes_value(int64_t num_boxes_int, const DetectionConfig& config, bool training_mode,
                               const DistributedContext& distributed, const torch::Device& device) {
    const int64_t group_detr = training_mode ? config.group_detr : 1;
    if (!config.sum_group_losses) {
        num_boxes_int *= group_detr;
    }

    double num_boxes_value = std::max(
        static_cast<double>(num_boxes_int) / static_cast<double>(std::max<int64_t>(1, config.world_size)), 1.0);
    if (!distributed.enabled) {
        return num_boxes_value;
    }

    auto num_boxes = torch::tensor({static_cast<float>(num_boxes_int)},
                                   torch::TensorOptions().dtype(torch::kFloat32).device(device));
    distributed_all_reduce_tensor(distributed, num_boxes);
    return torch::clamp_min(num_boxes / std::max<int64_t>(1, config.world_size), 1.0).item<double>();
}

void wait_for_lane_result(const TrainLaneResult& lane_result, int device_id) {
    if (!lane_result.ready_event) {
        throw std::runtime_error("parallel RF-DETR train result is missing a completion event");
    }
    ensure_cuda_ok(cudaStreamWaitEvent(c10::cuda::getCurrentCUDAStream(checked_device_index(device_id)).stream(),
                                       lane_result.ready_event->get(), 0),
                   "cudaStreamWaitEvent for parallel train lane result");
}

void record_lane_result_on_current_stream(const TrainLaneResult& lane_result, int device_id) {
    const auto stream = c10::cuda::getCurrentCUDAStream(checked_device_index(device_id));
    const auto record_tensor = [&stream](const torch::Tensor& value) {
        if (value.defined() && value.device().is_cuda()) {
            value.record_stream(stream);
        }
    };
    record_tensor(lane_result.loss);
    record_tensor(lane_result.class_loss);
    record_tensor(lane_result.box_loss);
    for (const auto& gradient : lane_result.gradients) {
        record_tensor(gradient);
    }
}

void merge_lane_gradients(const TrainLaneResult& lane_result, std::vector<torch::Tensor>& parameters, int device_id) {
    wait_for_lane_result(lane_result, device_id);
    record_lane_result_on_current_stream(lane_result, device_id);

    torch::NoGradGuard no_grad;
    const size_t limit = std::min(parameters.size(), lane_result.gradients.size());
    for (size_t index = 0; index < limit; ++index) {
        const auto& gradient = lane_result.gradients[index];
        if (!gradient.defined()) {
            continue;
        }
        if (!parameters[index].grad().defined()) {
            parameters[index].mutable_grad() = gradient.detach().clone();
        } else {
            parameters[index].mutable_grad().add_(gradient);
        }
    }
}

void copy_module_state(torch::nn::Module& destination, const torch::nn::Module& source) {
    torch::NoGradGuard no_grad;

    auto destination_parameters = destination.named_parameters(true);
    for (const auto& item : source.named_parameters(true)) {
        auto* target = destination_parameters.find(item.key());
        if (target == nullptr) {
            throw std::runtime_error("parallel RF-DETR lane is missing parameter: " + item.key());
        }
        target->copy_(item.value());
        target->requires_grad_(item.value().requires_grad());
    }

    auto destination_buffers = destination.named_buffers(true);
    for (const auto& item : source.named_buffers(true)) {
        auto* target = destination_buffers.find(item.key());
        if (target == nullptr) {
            throw std::runtime_error("parallel RF-DETR lane is missing buffer: " + item.key());
        }
        target->copy_(item.value());
    }
}

std::shared_ptr<NativeRfDetrModel> make_train_lane_model(NativeRfDetrModel& model, int device_id) {
    auto lane_model = std::make_shared<NativeRfDetrModel>(model.config());
    lane_model->to(cuda_device(device_id));
    lane_model->train();
    copy_module_state(*lane_model, model);
    return lane_model;
}

std::vector<torch::Tensor> lane_grad_parameters(const NativeRfDetrModel& lane_model,
                                                const std::vector<std::string>& parameter_names) {
    const auto named_parameters = lane_model.named_parameters(true);
    std::vector<torch::Tensor> parameters;
    parameters.reserve(parameter_names.size());
    for (const auto& name : parameter_names) {
        auto* tensor = named_parameters.find(name);
        if (tensor == nullptr) {
            throw std::runtime_error("parallel RF-DETR lane is missing trainable parameter: " + name);
        }
        parameters.push_back(*tensor);
    }
    return parameters;
}

std::optional<std::string> first_running_stats_buffer_name(NativeRfDetrModel& model) {
    for (const auto& item : model.named_buffers(true)) {
        if (item.key().find("running_mean") != std::string::npos ||
            item.key().find("running_var") != std::string::npos ||
            item.key().find("num_batches_tracked") != std::string::npos) {
            return item.key();
        }
    }
    return std::nullopt;
}

void ensure_train_lane_model_supported(NativeRfDetrModel& model, int train_lane_count) {
    if (train_lane_count <= 1) {
        return;
    }
    const auto running_stats = first_running_stats_buffer_name(model);
    if (running_stats.has_value()) {
        throw std::runtime_error(
            "parallel RF-DETR train --lanes requires a model without running-stat buffers; found " + *running_stats);
    }
}

bool is_rank_zero(const DistributedContext& distributed) {
    return distributed.rank == 0;
}

std::string train_progress_postfix(double average_class_loss, double average_box_loss, double average_loss,
                                   double step_class_loss, double step_box_loss, double step_loss,
                                   double images_per_second, int64_t optimizer_steps, int64_t steps_per_epoch) {
    return std::format("cl={:.4f}, bl={:.4f}, l={:.4f}, scl={:.4f}, sbl={:.4f}, sl={:.4f}, img/s={:.2f}, step={}/{}",
                       average_class_loss, average_box_loss, average_loss, step_class_loss, step_box_loss, step_loss,
                       images_per_second, optimizer_steps, steps_per_epoch);
}

torch::Tensor loss_value_or_zero(const TensorMap& loss_dict, const torch::Device& device, std::string_view key) {
    const auto found = loss_dict.find(std::string(key));
    if (found != loss_dict.end()) {
        return found->second;
    }
    return torch::zeros({}, torch::TensorOptions().dtype(torch::kFloat32).device(device));
}

std::string format_nonfinite_loss_report(const TensorMap& loss_dict, const std::vector<torch::Tensor>& parameters,
                                         const std::vector<std::string>& parameter_names) {
    std::ostringstream report;
    bool wrote_loss = false;
    for (const auto& [name, value] : loss_dict) {
        if (!torch::isfinite(value).item<bool>()) {
            if (!wrote_loss) {
                report << " nonfinite_losses=[";
                wrote_loss = true;
            } else {
                report << ", ";
            }
            report << name << "=" << value.item<double>();
        }
    }
    if (wrote_loss) {
        report << "]";
    }

    for (size_t index = 0; index < parameters.size(); ++index) {
        const auto& param = parameters[index];
        if (param.defined() && !torch::isfinite(param).all().item<bool>()) {
            report << " nonfinite_param=" << parameter_names[index];
            break;
        }
        if (param.grad().defined() && !torch::isfinite(param.grad()).all().item<bool>()) {
            report << " nonfinite_grad=" << parameter_names[index];
            break;
        }
    }

    return report.str();
}

const char* scalar_type_name(const at::ScalarType scalar_type) {
    switch (scalar_type) {
        case torch::kFloat:
            return "fp32";
        case torch::kFloat16:
            return "fp16";
        case torch::kBFloat16:
            return "bf16";
        default:
            return "other";
    }
}

void append_json_line(const std::filesystem::path& path, const json& payload) {
    std::ofstream stream(path, std::ios::app);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to append RF-DETR training log: " + path.string());
    }
    stream << payload.dump() << '\n';
}

void write_json_file(const std::filesystem::path& path, const json& payload) {
    const std::filesystem::path temporary_path = path.string() + ".tmp";
    std::ofstream stream(temporary_path);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to write RF-DETR JSON file: " + temporary_path.string());
    }
    stream << payload.dump(2) << '\n';
    stream.close();
    if (!stream) {
        throw std::runtime_error("failed to flush RF-DETR JSON file: " + temporary_path.string());
    }
    std::filesystem::rename(temporary_path, path);
}

class LatestJsonWriter {
   public:
    explicit LatestJsonWriter(std::filesystem::path path,
                              std::chrono::milliseconds minimum_interval = std::chrono::milliseconds(250))
        : path_(std::move(path)),
          minimum_interval_(minimum_interval),
          uncaught_on_entry_(std::uncaught_exceptions()),
          worker_([this] { run(); }) {}

    ~LatestJsonWriter() {
        close_noexcept();
    }

    LatestJsonWriter(const LatestJsonWriter&) = delete;
    LatestJsonWriter& operator=(const LatestJsonWriter&) = delete;

    void submit(json payload, bool force) {
        std::unique_lock<std::mutex> lock(mutex_);
        rethrow_worker_error();
        if (pending_.has_value()) {
            pending_->payload = std::move(payload);
            pending_->force = pending_->force || force;
        } else {
            pending_ = Pending{std::move(payload), force};
        }
        wake_.notify_one();
        if (force) {
            drained_.wait(lock, [this] { return error_ != nullptr || (!pending_.has_value() && !writing_); });
            rethrow_worker_error();
        }
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        rethrow_worker_error();
    }

   private:
    struct Pending {
        json payload;
        bool force = false;
    };

    void rethrow_worker_error() const {
        if (error_ != nullptr) {
            std::rethrow_exception(error_);
        }
    }

    void run() noexcept {
        auto last_write = std::chrono::steady_clock::now() - minimum_interval_;
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            wake_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_ && !pending_.has_value()) {
                return;
            }
            if (!pending_.has_value()) {
                continue;
            }
            if (!pending_->force && !stopping_) {
                const auto deadline = last_write + minimum_interval_;
                wake_.wait_until(lock, deadline,
                                 [this] { return stopping_ || (pending_.has_value() && pending_->force); });
                if (!pending_.has_value()) {
                    continue;
                }
            }
            if (!pending_.has_value()) {
                continue;
            }

            Pending next = std::move(*pending_);
            pending_.reset();
            writing_ = true;
            lock.unlock();
            json completed_payload;
            try {
                write_json_file(path_, next.payload);
                completed_payload = std::move(next.payload);
            } catch (...) {
                lock.lock();
                error_ = std::current_exception();
                writing_ = false;
                stopping_ = true;
                pending_.reset();
                drained_.notify_all();
                return;
            }
            lock.lock();
            last_written_ = std::move(completed_payload);
            last_write = std::chrono::steady_clock::now();
            writing_ = false;
            drained_.notify_all();
        }
    }

    void close_noexcept() noexcept {
        if (std::uncaught_exceptions() > uncaught_on_entry_) {
            try {
                std::lock_guard<std::mutex> lock(mutex_);
                if (error_ == nullptr) {
                    json error_payload =
                        pending_.has_value() ? pending_->payload : last_written_.value_or(json::object());
                    error_payload["phase"] = "error";
                    pending_ = Pending{std::move(error_payload), true};
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = std::current_exception();
            }
        }
        try {
            close();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = std::current_exception();
        }
    }

    std::filesystem::path path_;
    std::chrono::milliseconds minimum_interval_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable drained_;
    std::optional<Pending> pending_;
    std::optional<json> last_written_;
    std::exception_ptr error_;
    int uncaught_on_entry_ = 0;
    bool writing_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

json metric_summary_json(const MetricSummary& summary) {
    return json{
        {"ap", summary.ap},
        {"ap50", summary.ap50},
        {"ap75", summary.ap75},
    };
}

json eval_summary_json(const EvalSummary& summary) {
    return json{
        {"bbox", metric_summary_json(summary.bbox)},
        {"mask", summary.mask.has_value() ? metric_summary_json(*summary.mask) : json(nullptr)},
    };
}

double checkpoint_metric(const EvalSummary& summary, const bool include_masks) {
    if (!include_masks) {
        return summary.bbox.ap;
    }
    if (!summary.mask.has_value()) {
        throw std::logic_error("segmentation validation did not produce a mask metric");
    }
    return summary.mask->ap;
}

std::string formatted_mask_ap(const EvalSummary& summary) {
    return summary.mask.has_value() ? std::format("{:.4f}", summary.mask->ap) : "null";
}

json augmentation_group_json(const AugmentationGroupConfig& group) {
    return json{
        {"probability", group.probability},
        {"min_strength", group.min_strength},
        {"max_strength", group.max_strength},
    };
}

json gpu_augmentation_json(const GpuAugmentationConfig& config) {
    return json{
        {"enabled", config.enabled},
        {"geometry", augmentation_group_json(config.geometry)},
        {"resize", augmentation_group_json(config.resize)},
        {"color", augmentation_group_json(config.color)},
        {"noise", augmentation_group_json(config.noise)},
        {"blur", augmentation_group_json(config.blur)},
        {"occlusion", augmentation_group_json(config.occlusion)},
        {"copy_paste_probability", config.copy_paste_probability},
    };
}

DistributedContext make_distributed_context(const TrainOptions& options) {
    DistributedContext distributed;
    if (!options.distributed_worker || options.distributed_world_size <= 1) {
        return distributed;
    }
    if (options.distributed_store_path.empty()) {
        throw std::runtime_error("distributed RF-DETR worker requires --dist-store-file");
    }
    if (options.distributed_rank < 0 || options.distributed_rank >= options.distributed_world_size) {
        throw std::runtime_error("distributed RF-DETR worker rank is out of range");
    }

#if !defined(USE_C10D_NCCL)
    throw std::runtime_error("distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#else
    distributed.enabled = true;
    distributed.rank = options.distributed_rank;
    distributed.world_size = options.distributed_world_size;
    c10::cuda::CUDAGuard device_guard(cuda_device_index(options.device_id));
    distributed.store =
        c10::make_intrusive<c10d::FileStore>(options.distributed_store_path.string(), options.distributed_world_size);

    auto pg_options = c10::make_intrusive<c10d::ProcessGroupNCCL::Options>();
    pg_options->timeout = c10d::kProcessGroupNCCLDefaultTimeout;
    distributed.process_group = c10::make_intrusive<c10d::ProcessGroupNCCL>(
        distributed.store, distributed.rank, distributed.world_size, std::move(pg_options));
    return distributed;
#endif
}

void distributed_barrier(const DistributedContext& distributed) {
    if (!distributed.enabled) {
        return;
    }
#if defined(USE_C10D_NCCL)
    distributed.process_group->barrier()->wait();
#else
    (void)distributed;
    throw std::runtime_error("distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#endif
}

void distributed_all_reduce_tensor(const DistributedContext& distributed, torch::Tensor& tensor) {
    if (!distributed.enabled) {
        return;
    }
#if defined(USE_C10D_NCCL)
    std::vector<torch::Tensor> tensors = {tensor};
    distributed.process_group->allreduce(tensors)->wait();
#else
    (void)distributed;
    (void)tensor;
    throw std::runtime_error("distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#endif
}

void distributed_all_reduce_coalesced(const DistributedContext& distributed, std::vector<torch::Tensor>& tensors) {
    if (!distributed.enabled || tensors.empty()) {
        return;
    }
#if defined(USE_C10D_NCCL)
    distributed.process_group->allreduce_coalesced(tensors)->wait();
#else
    (void)distributed;
    (void)tensors;
    throw std::runtime_error("distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#endif
}

bool is_encoder_param(std::string_view name) {
    return name.find("backbone.0.encoder") != std::string_view::npos;
}

bool encoder_zero_weight_decay(std::string_view name) {
    return name.find("gamma") != std::string_view::npos || name.find("bias") != std::string_view::npos ||
           name.find("norm") != std::string_view::npos || name.find("embeddings") != std::string_view::npos ||
           name.find("position_embeddings") != std::string_view::npos || name.find("rel_pos") != std::string_view::npos;
}

int encoder_layer_id(std::string_view name) {
    constexpr int kNumVitLayers = 13;
    if (name.find("embeddings") != std::string_view::npos) {
        return 0;
    }
    const auto layer_pos = name.find(".layer.");
    if (layer_pos != std::string_view::npos && name.find(".residual.") == std::string_view::npos) {
        const auto digits_pos = layer_pos + std::string_view(".layer.").size();
        size_t digits_len = 0;
        while (digits_pos + digits_len < name.size() &&
               std::isdigit(static_cast<unsigned char>(name[digits_pos + digits_len]))) {
            ++digits_len;
        }
        if (digits_len > 0) {
            return std::stoi(std::string(name.substr(digits_pos, digits_len))) + 1;
        }
    }
    return kNumVitLayers + 1;
}

double parameter_lr(std::string_view name, const TrainOptions& options) {
    if (is_encoder_param(name)) {
        constexpr int kNumVitLayers = 13;
        const int layer_id = encoder_layer_id(name);
        return options.lr_encoder *
               std::pow(options.encoder_layer_decay, static_cast<double>(kNumVitLayers + 1 - layer_id)) *
               options.lr_component_decay * options.lr_component_decay;
    }
    if (name.find("transformer.decoder") != std::string_view::npos) {
        return options.lr * options.lr_component_decay;
    }
    return options.lr;
}

double parameter_weight_decay(std::string_view name, const TrainOptions& options) {
    if (is_encoder_param(name)) {
        return encoder_zero_weight_decay(name) ? 0.0 : options.weight_decay;
    }
    return options.weight_decay;
}

}  

bool is_muon_hidden_weight(std::string_view name, const torch::Tensor& param) {
    if ((param.dim() != 2 && param.dim() != 4) || name.find(".weight") == std::string_view::npos) {
        return false;
    }
    if (name.find("embeddings") != std::string_view::npos ||
        name.find("position_embeddings") != std::string_view::npos ||
        name.find("cls_token") != std::string_view::npos || name.find("mask_token") != std::string_view::npos ||
        name.find("query_feat") != std::string_view::npos || name.find("refpoint_embed") != std::string_view::npos ||
        name.find("class_embed") != std::string_view::npos || name.find("bbox_embed") != std::string_view::npos ||
        name.find("segmentation_head") != std::string_view::npos) {
        return false;
    }
    return true;
}

namespace {

OptimizerBuildResult build_optimizer(NativeRfDetrModel& model, const TrainOptions& options) {
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
        if (!param.requires_grad()) {
            continue;
        }

        const double lr = parameter_lr(item.key(), options);
        const double weight_decay = parameter_weight_decay(item.key(), options);
        const bool use_muon = options.optimizer == TrainOptimizerKind::Muon && is_muon_hidden_weight(item.key(), param);
        const std::string key = std::format("{}{}:{}", use_muon ? "muon:" : "aux:", lr, weight_decay);
        auto found = group_index.find(key);
        if (found == group_index.end()) {
            found = group_index.emplace(key, groups.size()).first;
            groups.push_back(GroupSpec{lr, weight_decay, use_muon, {}});
        }
        named_params.emplace_back(item.key(), param);
        groups[found->second].param_indices.push_back(named_params.size() - 1);
    }

    if (groups.empty() || named_params.empty()) {
        throw std::runtime_error("RF-DETR runtime found no trainable parameters");
    }

    std::vector<double> base_lrs;
    base_lrs.reserve(groups.size());
    if (options.optimizer == TrainOptimizerKind::Muon) {
        std::vector<NativeMuonWithAuxAdam::Group> optimizer_groups;
        std::vector<NativeMuonWithAuxAdam::NamedParameter> optimizer_params;
        optimizer_groups.reserve(groups.size());
        optimizer_params.reserve(named_params.size());
        for (const auto& named_param : named_params) {
            optimizer_params.push_back(NativeMuonWithAuxAdam::NamedParameter{named_param.first, named_param.second});
        }
        for (auto& group : groups) {
            base_lrs.push_back(group.lr);
            optimizer_groups.push_back(NativeMuonWithAuxAdam::Group{
                NativeMuonGroupConfig{group.lr, group.weight_decay, options.momentum, group.use_muon, true},
                std::move(group.param_indices)});
        }
        return OptimizerBuildResult{
            NativeOptimizer(NativeMuonWithAuxAdam(std::move(optimizer_groups), std::move(optimizer_params))),
            std::move(base_lrs),
        };
    }

    std::vector<NativeAdamW::Group> optimizer_groups;
    std::vector<NativeAdamW::NamedParameter> optimizer_params;
    optimizer_groups.reserve(groups.size());
    optimizer_params.reserve(named_params.size());
    for (const auto& named_param : named_params) {
        optimizer_params.push_back(NativeAdamW::NamedParameter{named_param.first, named_param.second});
    }
    for (auto& group : groups) {
        base_lrs.push_back(group.lr);
        optimizer_groups.push_back(NativeAdamW::Group{NativeAdamWGroupConfig{group.lr, group.weight_decay, false},
                                                      std::move(group.param_indices)});
    }

    std::vector<torch::Tensor> trainable_params;
    trainable_params.reserve(optimizer_params.size());
    for (const auto& named_param : optimizer_params) {
        trainable_params.push_back(named_param.tensor);
    }
    const auto backend = (options.fused_optimizer && native_optimizer_supports_fused(trainable_params))
                             ? NativeOptimizerBackend::fused
                         : native_optimizer_supports_foreach(trainable_params) ? NativeOptimizerBackend::foreach
                                                                               : NativeOptimizerBackend::eager;

    return OptimizerBuildResult{
        NativeOptimizer(NativeAdamW(std::move(optimizer_groups), std::move(optimizer_params), backend)),
        std::move(base_lrs),
    };
}

std::vector<StateDictEntry> collect_module_state(
    const torch::nn::Module& module,
    const std::unordered_map<std::string, torch::Tensor>* parameter_overrides = nullptr) {
    std::vector<StateDictEntry> state;

    const auto parameters = module.named_parameters(true);
    state.reserve(parameters.size() + module.named_buffers(true).size());
    for (const auto& item : parameters) {
        StateDictEntry entry;
        entry.name = item.key();
        if (parameter_overrides != nullptr) {
            const auto found = parameter_overrides->find(entry.name);
            if (found != parameter_overrides->end()) {
                entry.tensor = found->second.detach();
                state.push_back(std::move(entry));
                continue;
            }
        }
        entry.tensor = item.value().detach();
        state.push_back(std::move(entry));
    }

    for (const auto& item : module.named_buffers(true)) {
        StateDictEntry entry;
        entry.name = item.key();
        entry.tensor = item.value().detach();
        state.push_back(std::move(entry));
    }
    return state;
}

std::vector<StateDictEntry> read_state_archive(torch::serialize::InputArchive& archive, const char* key) {
    torch::serialize::InputArchive state_archive;
    archive.read(key, state_archive);
    const int64_t entry_count = require_int(state_archive, "entry_count");
    if (entry_count < 0) {
        throw std::runtime_error("RF-DETR training checkpoint entry_count is negative");
    }

    std::vector<StateDictEntry> state_dict;
    state_dict.reserve(static_cast<size_t>(entry_count));
    for (int64_t index = 0; index < entry_count; ++index) {
        torch::serialize::InputArchive entry_archive;
        state_archive.read(archive_entry_name(static_cast<size_t>(index)), entry_archive);
        c10::IValue name_value;
        entry_archive.read("name", name_value);
        if (!name_value.isString()) {
            throw std::runtime_error("RF-DETR training checkpoint state entry name is not a string");
        }
        StateDictEntry entry;
        entry.name = name_value.toStringRef();
        entry_archive.read("tensor", entry.tensor);
        state_dict.push_back(std::move(entry));
    }
    return state_dict;
}

NativeCheckpointMetadata checkpoint_metadata(const ResolvedModelArtifacts& artifacts, int64_t num_classes) {
    NativeCheckpointMetadata metadata;
    metadata.preset_name = artifacts.config.preset_name;
    metadata.source_kind = artifacts.input_kind;
    metadata.source_path = artifacts.input_path.string();
    metadata.num_classes = num_classes;
    metadata.sum_group_losses = artifacts.config.sum_group_losses;
    metadata.use_varifocal_loss = artifacts.config.use_varifocal_loss;
    metadata.use_position_supervised_loss = artifacts.config.use_position_supervised_loss;
    metadata.ia_bce_loss = artifacts.config.ia_bce_loss;
    metadata.aux_loss = artifacts.config.aux_loss;
    metadata.mask_point_sample_ratio = artifacts.config.mask_point_sample_ratio;
    metadata.focal_alpha = artifacts.config.focal_alpha;
    metadata.cls_loss_coef = artifacts.config.cls_loss_coef;
    metadata.bbox_loss_coef = artifacts.config.bbox_loss_coef;
    metadata.giou_loss_coef = artifacts.config.giou_loss_coef;
    metadata.mask_ce_loss_coef = artifacts.config.mask_ce_loss_coef;
    metadata.mask_dice_loss_coef = artifacts.config.mask_dice_loss_coef;
    metadata.set_cost_class = artifacts.config.set_cost_class;
    metadata.set_cost_bbox = artifacts.config.set_cost_bbox;
    metadata.set_cost_giou = artifacts.config.set_cost_giou;
    return metadata;
}

std::unordered_map<std::string, torch::Tensor> ema_override_map(const std::vector<std::string>& param_names,
                                                                const ModelEma& ema) {
    const auto& shadow_params = ema.shadow_params();
    if (shadow_params.size() != param_names.size()) {
        throw std::runtime_error("RF-DETR EMA parameter count changed unexpectedly");
    }

    std::unordered_map<std::string, torch::Tensor> overrides;
    overrides.reserve(param_names.size());
    for (size_t index = 0; index < param_names.size(); ++index) {
        overrides.emplace(param_names[index], shadow_params[index]);
    }
    return overrides;
}

std::vector<StateDictEntry> ema_state_entries(const std::vector<std::string>& param_names, const ModelEma& ema) {
    const auto overrides = ema_override_map(param_names, ema);
    std::vector<StateDictEntry> state;
    state.reserve(overrides.size());
    for (const auto& item : param_names) {
        StateDictEntry entry;
        entry.name = item;
        entry.tensor = overrides.at(item).detach();
        state.push_back(std::move(entry));
    }
    return state;
}

void save_resume_checkpoint(const std::filesystem::path& checkpoint_path, NativeRfDetrModel& model,
                            const NativeCheckpointMetadata& metadata, const NativeOptimizer& optimizer,
                            const GradScaler& grad_scaler, const TrainOptions& options, int epoch, double best_regular,
                            double best_ema, const std::vector<std::string>& param_names,
                            const std::optional<ModelEma>& ema) {
    MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.total");
    std::filesystem::create_directories(checkpoint_path.parent_path());

    torch::serialize::OutputArchive archive;
    write_string(archive, "format", kNativeCheckpointFormat);
    write_int(archive, "format_version", kNativeCheckpointFormatVersion);
    write_string(archive, "preset_name", metadata.preset_name);
    write_string(archive, "source_kind", metadata.source_kind);
    write_string(archive, "source_path", metadata.source_path);
    write_int(archive, "num_classes", metadata.num_classes);
    if (metadata.sum_group_losses.has_value()) {
        write_bool(archive, "sum_group_losses", *metadata.sum_group_losses);
    }
    if (metadata.use_varifocal_loss.has_value()) {
        write_bool(archive, "use_varifocal_loss", *metadata.use_varifocal_loss);
    }
    if (metadata.use_position_supervised_loss.has_value()) {
        write_bool(archive, "use_position_supervised_loss", *metadata.use_position_supervised_loss);
    }
    if (metadata.ia_bce_loss.has_value()) {
        write_bool(archive, "ia_bce_loss", *metadata.ia_bce_loss);
    }
    if (metadata.aux_loss.has_value()) {
        write_bool(archive, "aux_loss", *metadata.aux_loss);
    }
    if (metadata.mask_point_sample_ratio.has_value()) {
        write_int(archive, "mask_point_sample_ratio", *metadata.mask_point_sample_ratio);
    }
    if (metadata.focal_alpha.has_value()) {
        write_double(archive, "focal_alpha", *metadata.focal_alpha);
    }
    if (metadata.cls_loss_coef.has_value()) {
        write_double(archive, "cls_loss_coef", *metadata.cls_loss_coef);
    }
    if (metadata.bbox_loss_coef.has_value()) {
        write_double(archive, "bbox_loss_coef", *metadata.bbox_loss_coef);
    }
    if (metadata.giou_loss_coef.has_value()) {
        write_double(archive, "giou_loss_coef", *metadata.giou_loss_coef);
    }
    if (metadata.mask_ce_loss_coef.has_value()) {
        write_double(archive, "mask_ce_loss_coef", *metadata.mask_ce_loss_coef);
    }
    if (metadata.mask_dice_loss_coef.has_value()) {
        write_double(archive, "mask_dice_loss_coef", *metadata.mask_dice_loss_coef);
    }
    if (metadata.set_cost_class.has_value()) {
        write_double(archive, "set_cost_class", *metadata.set_cost_class);
    }
    if (metadata.set_cost_bbox.has_value()) {
        write_double(archive, "set_cost_bbox", *metadata.set_cost_bbox);
    }
    if (metadata.set_cost_giou.has_value()) {
        write_double(archive, "set_cost_giou", *metadata.set_cost_giou);
    }
    std::vector<StateDictEntry> model_state;
    {
        MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.collect_state");
        model_state = collect_module_state(model);
    }
    {
        MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.write_state");
        detail::write_state_archive(archive, "state", model_state);
    }

    torch::serialize::OutputArchive optimizer_archive;
    {
        MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.optimizer_state");
        optimizer.save(optimizer_archive);
    }
    write_string(archive, "optimizer_kind", optimizer.kind_name());
    archive.write("optimizer", optimizer_archive);

    if (ema.has_value()) {
        std::vector<StateDictEntry> ema_state;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.ema_collect_state");
            ema_state = ema_state_entries(param_names, *ema);
        }
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.ema_write_state");
            detail::write_state_archive(archive, "ema_state", ema_state);
        }
    }

    write_int(archive, "epoch", epoch);
    write_string(archive, "lr_scheduler", options.lr_scheduler);
    write_int(archive, "lr_drop", options.lr_drop);
    write_double(archive, "warmup_epochs", options.warmup_epochs);
    write_double(archive, "warmup_momentum", options.warmup_momentum);
    write_double(archive, "lr_min_factor", options.lr_min_factor);
    write_bool(archive, "gpu_augment_enabled", options.gpu_augmentation.enabled);
    const auto write_augmentation_group = [&archive](const char* prefix, const AugmentationGroupConfig& group) {
        const std::string probability_key = std::string(prefix) + "_probability";
        const std::string min_strength_key = std::string(prefix) + "_min_strength";
        const std::string max_strength_key = std::string(prefix) + "_max_strength";
        write_double(archive, probability_key.c_str(), group.probability);
        write_double(archive, min_strength_key.c_str(), group.min_strength);
        write_double(archive, max_strength_key.c_str(), group.max_strength);
    };
    write_augmentation_group("gpu_augment_geometry", options.gpu_augmentation.geometry);
    write_augmentation_group("gpu_augment_resize", options.gpu_augmentation.resize);
    write_augmentation_group("gpu_augment_color", options.gpu_augmentation.color);
    write_augmentation_group("gpu_augment_noise", options.gpu_augmentation.noise);
    write_augmentation_group("gpu_augment_blur", options.gpu_augmentation.blur);
    write_augmentation_group("gpu_augment_occlusion", options.gpu_augmentation.occlusion);
    write_double(archive, "gpu_augment_copy_paste_probability", options.gpu_augmentation.copy_paste_probability);
    write_double(archive, "best_regular_metric", best_regular);
    write_double(archive, "best_ema_metric", best_ema);
    write_double(archive, "grad_scaler_scale", static_cast<double>(grad_scaler.current_scale()));
    write_int(archive, "grad_scaler_growth_tracker", grad_scaler.growth_tracker());
    {
        MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.archive_save_to");
        archive.save_to(checkpoint_path.string());
    }
}

ResumeState load_resume_checkpoint_state(const std::filesystem::path& checkpoint_path, NativeOptimizer& optimizer,
                                         GradScaler& grad_scaler, const TrainOptions& options) {
    if (!is_native_checkpoint_file(checkpoint_path)) {
        throw std::runtime_error("--resume requires a native RF-DETR .pt checkpoint: " + checkpoint_path.string());
    }

    torch::serialize::InputArchive archive;
    archive.load_from(checkpoint_path.string());

    if (const auto lr_scheduler = read_optional_string(archive, "lr_scheduler");
        lr_scheduler.has_value() && *lr_scheduler != options.lr_scheduler) {
        throw std::runtime_error(
            "native RF-DETR resume checkpoint lr_scheduler does not match current training options");
    }
    if (const auto lr_drop = read_optional_int(archive, "lr_drop");
        lr_drop.has_value() && *lr_drop != options.lr_drop) {
        throw std::runtime_error("native RF-DETR resume checkpoint lr_drop does not match current training options");
    }
    if (const auto warmup_epochs = read_optional_double(archive, "warmup_epochs");
        warmup_epochs.has_value() && *warmup_epochs != options.warmup_epochs) {
        throw std::runtime_error(
            "native RF-DETR resume checkpoint warmup_epochs does not match current training options");
    }
    if (const auto warmup_momentum = read_optional_double(archive, "warmup_momentum");
        warmup_momentum.has_value() && *warmup_momentum != options.warmup_momentum) {
        throw std::runtime_error(
            "native RF-DETR resume checkpoint warmup_momentum does not match current training options");
    }
    if (const auto lr_min_factor = read_optional_double(archive, "lr_min_factor");
        lr_min_factor.has_value() && *lr_min_factor != options.lr_min_factor) {
        throw std::runtime_error(
            "native RF-DETR resume checkpoint lr_min_factor does not match current training options");
    }
    const auto optimizer_kind = read_optional_string(archive, "optimizer_kind");
    if (!optimizer_kind.has_value()) {
        throw std::runtime_error("native RF-DETR resume checkpoint is missing optimizer_kind: " +
                                 checkpoint_path.string());
    }
    if (*optimizer_kind != optimizer.kind_name()) {
        throw std::runtime_error(
            "native RF-DETR resume checkpoint optimizer_kind does not match current training options");
    }

    torch::serialize::InputArchive optimizer_archive;
    if (!archive.try_read("optimizer", optimizer_archive)) {
        throw std::runtime_error("native RF-DETR resume checkpoint is missing optimizer state: " +
                                 checkpoint_path.string());
    }
    try {
        optimizer.load(optimizer_archive);
    } catch (const std::exception& error) {
        throw std::runtime_error("failed to load native RF-DETR optimizer state from " + checkpoint_path.string() +
                                 ": " + error.what());
    }

    ResumeState state;
    state.start_epoch = static_cast<int>(read_optional_int(archive, "epoch").value_or(-1) + 1);
    state.best_regular =
        read_optional_double(archive, "best_regular_metric").value_or(-std::numeric_limits<double>::infinity());
    state.best_ema =
        read_optional_double(archive, "best_ema_metric").value_or(-std::numeric_limits<double>::infinity());

    if (const auto scale = read_optional_double(archive, "grad_scaler_scale"); scale.has_value()) {
        grad_scaler.load_state(static_cast<float>(*scale),
                               static_cast<int>(read_optional_int(archive, "grad_scaler_growth_tracker").value_or(0)));
    }

    torch::serialize::InputArchive ema_archive;
    if (archive.try_read("ema_state", ema_archive)) {
        state.ema_state = read_state_archive(archive, "ema_state");
    }
    return state;
}

DetectionConfig make_detection_config(const NativeRfDetrConfig& config, int64_t world_size,
                                      CompilationMode compilation_mode) {
    DetectionConfig detection_config;
    detection_config.num_classes = config.num_classes;
    detection_config.group_detr = config.group_detr;
    detection_config.dec_layers = config.dec_layers;
    detection_config.num_select = config.num_select;
    detection_config.world_size = std::max<int64_t>(1, world_size);
    detection_config.sum_group_losses = config.sum_group_losses;
    detection_config.use_varifocal_loss = config.use_varifocal_loss;
    detection_config.use_position_supervised_loss = config.use_position_supervised_loss;
    detection_config.ia_bce_loss = config.ia_bce_loss;
    detection_config.aux_loss = config.aux_loss;
    detection_config.two_stage = config.two_stage;
    detection_config.include_masks = config.segmentation;
    detection_config.use_jit_traced_loss_ops = (compilation_mode == CompilationMode::kSelective);
    detection_config.mask_point_sample_ratio = config.mask_point_sample_ratio;
    detection_config.focal_alpha = config.focal_alpha;
    detection_config.cls_loss_coef = config.cls_loss_coef;
    detection_config.bbox_loss_coef = config.bbox_loss_coef;
    detection_config.giou_loss_coef = config.giou_loss_coef;
    detection_config.mask_ce_loss_coef = config.mask_ce_loss_coef;
    detection_config.mask_dice_loss_coef = config.mask_dice_loss_coef;
    detection_config.set_cost_class = config.set_cost_class;
    detection_config.set_cost_bbox = config.set_cost_bbox;
    detection_config.set_cost_giou = config.set_cost_giou;
    populate_default_detection_weight_dict(detection_config);
    return detection_config;
}

size_t full_batches_per_rank(size_t total_images, size_t batch_size, size_t world_size, int grad_accum_steps,
                             int train_lane_count) {
    const size_t total_full_batches = total_images / batch_size;
    const size_t local_full_batches = total_full_batches / std::max<size_t>(1, world_size);
    const size_t batches_per_step =
        static_cast<size_t>(std::max(1, grad_accum_steps)) * static_cast<size_t>(std::max(1, train_lane_count));
    return (local_full_batches / batches_per_step) * batches_per_step;
}

void average_gradients(const DistributedContext& distributed, const std::vector<torch::Tensor>& parameters) {
    if (!distributed.enabled) {
        return;
    }

    std::vector<torch::Tensor> gradients;
    gradients.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        if (parameter.grad().defined()) {
            gradients.push_back(parameter.grad());
        }
    }
    if (gradients.empty()) {
        return;
    }

    distributed_all_reduce_coalesced(distributed, gradients);
    const double scale = 1.0 / static_cast<double>(distributed.world_size);
    for (auto& gradient : gradients) {
        gradient.mul_(scale);
    }
}

std::future<TrainLaneResult> enqueue_train_lane(WorkerPool& lane_pool, WorkerPool* cpu_pool, DatasetLoader& loader,
                                                TrainLaneContext& lane, const Batch& batch,
                                                std::shared_ptr<SharedCudaEvent> params_ready, double num_boxes_value,
                                                double scaled_loss_factor, size_t parameter_version,
                                                const DetectionConfig& detection_config, const NativeRfDetrModel& model,
                                                int device_id, int image_height, int image_width, std::uint64_t seed,
                                                int epoch, int rank, std::uint64_t augmentation_sequence,
                                                bool amp_enabled, at::ScalarType autocast_dtype) {
    return lane_pool.enqueue([cpu_pool, &loader, &lane, batch, params_ready = std::move(params_ready), num_boxes_value,
                              scaled_loss_factor, parameter_version, &detection_config, &model, device_id, image_height,
                              image_width, seed, epoch, rank, augmentation_sequence, amp_enabled,
                              autocast_dtype]() mutable {
        ScopedWorkerPool worker_scope(cpu_pool);
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.wait_batch");
            loader.wait_batch(batch);
        }

        c10::cuda::CUDAGuard device_guard(checked_device_index(device_id));
        c10::cuda::CUDAStreamGuard stream_guard(lane.stream);
        LoaderBatchGuard batch_guard(loader, batch, device_id);

        torch::Tensor normalized;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.augment");
            TORCH_CHECK(lane.augmenter, "parallel RF-DETR train lane is missing its GPU augmenter");
            normalized = lane.augmenter->run(batch, seed, epoch, rank, augmentation_sequence);
        }

        PreparedTargets prepared;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.targets");
            prepared = build_targets(batch, image_height, image_width, detection_config.include_masks,
                                     detection_config.include_masks, device_id, lane.target_scratch,
                                     &lane.augmenter->batch_plan());
        }
        batch_guard.set_consumer_stream(lane.augmenter->finish_batch(batch, prepared, lane.target_scratch));
        batch_guard.release();

        if (!lane.model) {
            throw std::runtime_error("parallel RF-DETR train lane is missing its model replica");
        }
        if (lane.synced_parameter_version != parameter_version) {
            if (params_ready) {
                ensure_cuda_ok(cudaStreamWaitEvent(lane.stream.stream(), params_ready->get(), 0),
                               "cudaStreamWaitEvent for parallel train parameter readiness");
            }
            copy_module_state(*lane.model, model);
            lane.synced_parameter_version = parameter_version;
        }
        lane.model->train();

        torch::Tensor detached_loss;
        torch::Tensor detached_class_loss;
        torch::Tensor detached_box_loss;
        std::vector<torch::Tensor> gradients;
        {
            TensorMap loss_dict;
            torch::Tensor loss;
            torch::Tensor class_loss;
            torch::Tensor box_loss;
            AutocastGuard autocast_guard(amp_enabled, autocast_dtype);
            ModelOutputs outputs;
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.forward");
                outputs = lane.model->forward(NestedTensor{normalized, prepared.nested_mask});
            }
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.targets_handoff");
                lane.target_scratch.handoff_pending_copy_to_current_stream(device_id);
            }
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.loss_dict");
                loss_dict = detection_loss_dict(outputs, prepared, detection_config, true, num_boxes_value);
            }
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.loss_total");
                loss = weighted_detection_loss(loss_dict, detection_config, normalized.device());
                class_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_ce");
                box_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_bbox");
            }
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.grad");
                gradients =
                    torch::autograd::grad({loss * scaled_loss_factor}, lane.grad_params, {}, std::nullopt, false, true);
            }
            detached_loss = loss.detach();
            detached_class_loss = class_loss.detach();
            detached_box_loss = box_loss.detach();
        }

        return TrainLaneResult{
            std::move(detached_loss),
            std::move(detached_class_loss),
            std::move(detached_box_loss),
            std::move(gradients),
            record_shared_cuda_event(lane.stream.stream(), "parallel train lane completion event"),
        };
    });
}

EvalPassResult evaluate_model(const TrainOptions& options, TrainingValidationRuntime& validation,
                              NativeRfDetrModel& model, const DetectionConfig& detection_config, bool calculate_loss,
                              std::optional<int> current_epoch, std::string progress_label) {
    MMLTK_PROFILE_SCOPE("rfdetr.train.eval.total");
    RuntimeContext& runtime = validation.runtime();
    DatasetLoader& loader = validation.loader();
    CocoDataset& dataset = validation.dataset();
    const std::vector<int>& image_ids = validation.image_ids();

    EvalPassResult result;
    c10::InferenceMode inference_mode;
    model.eval();

    const bool amp_enabled = validation.amp_enabled();
    const at::ScalarType autocast_dtype = validation.inference_dtype();

    const auto started_at = std::chrono::steady_clock::now();
    std::unique_ptr<EvaluationProfileRecord> validation_profile;
    if (options.validation_profile) {
        validation_profile = std::make_unique<EvaluationProfileRecord>();
        validation_profile->jsonl_path = options.output_dir / "validation_profile.jsonl";
        validation_profile->event_source = "training_validation";
        validation_profile->model_source =
            progress_label.starts_with("ema") ? "ema" : (progress_label.starts_with("test") ? "test" : "model");
        validation_profile->precision = "unknown";
        validation_profile->box_precision = "unknown";
        validation_profile->expected_precision = evaluation_precision_name(autocast_dtype);
        capture_sdp_backend_flags(*validation_profile);
        validation_profile->validation_pass =
            current_epoch.has_value() ? std::optional<int>(*current_epoch + 1) : std::nullopt;
        validation_profile->metric_set = validation.dataset().metric_set();
        validation_profile->batch_size = validation.batch_size();
        validation_profile->image_height = loader.image_height();
        validation_profile->image_width = loader.image_width();
        validation_profile->query_count = static_cast<size_t>(model.config().num_queries);
        validation_profile->class_count = static_cast<size_t>(model.config().num_classes);
        validation_profile->started_at = started_at;
    }
    size_t image_count = 0;
    size_t batch_count = 0;
    size_t seen_images = 0;
    double loss_sum = 0.0;
    std::optional<CapturedEvalSample> captured_sample;
    const bool capture_eval_sample = current_epoch.has_value() && progress_label.starts_with("val ");
    std::unique_ptr<spdmon::ProgressBar> progress;
    if (options.progress_bar) {
        progress = std::make_unique<spdmon::ProgressBar>(std::move(progress_label), loader.num_images(), "img");
    }
    std::mt19937_64 sample_rng(static_cast<uint64_t>(options.seed) ^
                               (current_epoch.has_value()
                                    ? (0x9e3779b97f4a7c15ULL + static_cast<uint64_t>(*current_epoch + 1))
                                    : 0xd1b54a32d192ed03ULL));

    WorkerPool& lane_pool = validation.lane_pool();
    WorkerPool& cpu_pool = runtime.cpu_pool();
    std::vector<EvalPredictionLane>& lanes = validation.lanes();
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));

    const size_t max_cpu_futures =
        predict_internal::prediction_cpu_batch_limit(runtime.split(), validation.batch_size());
    std::deque<std::future<StagedPredictionBatch>> lane_futures;
    std::deque<PendingPredictionBatchEncoding> cpu_futures;
    std::unique_ptr<EvaluationCudaTimingPool> cuda_timing_pool;
    std::deque<EvaluationCudaTimingLease> lane_timing_leases;
    std::deque<EvaluationCudaTimingLease> cpu_timing_leases;
    if (validation_profile) {
        cuda_timing_pool = std::make_unique<EvaluationCudaTimingPool>(
            static_cast<size_t>(runtime.split().lane_threads) + max_cpu_futures);
    }
    size_t submitted_batches = 0;
    auto update_profile_peaks = [&]() {
        if (!validation_profile) {
            return;
        }
        size_t task_count = lane_futures.size();
        for (const PendingPredictionBatchEncoding& pending : cpu_futures) {
            task_count += pending.images.size();
        }
        const size_t slot_count = lane_futures.size() + cpu_futures.size();
        validation_profile->peak_in_flight_tasks = std::max(validation_profile->peak_in_flight_tasks, task_count);
        validation_profile->peak_in_flight_slots = std::max(validation_profile->peak_in_flight_slots, slot_count);
    };
    auto drain_lane = [&]() {
        StagedPredictionBatch staged = lane_futures.front().get();
        lane_futures.pop_front();
        if (validation_profile) {
            cpu_timing_leases.push_back(lane_timing_leases.front());
            lane_timing_leases.pop_front();
        }
        cpu_futures.push_back(
            enqueue_prediction_batch_encoding(cpu_pool, std::move(staged), validation_profile.get(), &dataset));
        update_profile_peaks();
    };
    auto drain_cpu = [&]() {
        std::vector<PredictionBatchItem> completed = collect_prediction_batch_encoding(std::move(cpu_futures.front()));
        cpu_futures.pop_front();
        if (validation_profile) {
            EvaluationCudaTimingLease& timing = cpu_timing_leases.front();
            timing.timing->accumulate(*validation_profile);
            cuda_timing_pool->release(timing);
            cpu_timing_leases.pop_front();
        }
        for (auto& image : completed) {
            if (!image.evaluation_matches) {
                throw std::logic_error("validation image task omitted compact evaluation matches");
            }
            dataset.merge_matches(std::move(*image.evaluation_matches));
        }
        image_count += completed.size();
        if (progress) {
            progress->add(completed.size());
        }
    };

    validation.begin_pass();
    Batch batch{};
    while (loader.next_batch(batch)) {
        MMLTK_PROFILE_SCOPE("rfdetr.train.eval.batch");
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.wait_batch");
            if (validation_profile) {
                const auto phase_started = std::chrono::steady_clock::now();
                loader.wait_batch(batch);
                validation_profile->loader_wait_seconds +=
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_started).count();
            } else {
                loader.wait_batch(batch);
            }
        }
        LoaderBatchGuard batch_guard(loader, batch, options.device_id);
        MMLTK_PROFILE_ADD("rfdetr.train.eval.images", batch.num_images);
        EvaluationCudaTimingLease batch_timing;
        cudaStream_t evaluation_stream = nullptr;
        if (validation_profile) {
            batch_timing = cuda_timing_pool->acquire();
            evaluation_stream = c10::cuda::getCurrentCUDAStream(checked_device_index(options.device_id)).stream();
            batch_timing.timing->record_start(EvaluationCudaBatchTiming::Phase::Preprocessing, evaluation_stream);
        }

        torch::Tensor inference_input;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.normalize");
            inference_input = validation.preprocess(batch);
        }
        if (batch_timing) {
            batch_timing.timing->record_stop(EvaluationCudaBatchTiming::Phase::Preprocessing, evaluation_stream);
        }

        std::optional<size_t> sampled_image_pos;
        torch::Tensor sampled_image;
        if (capture_eval_sample) {
            for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
                ++seen_images;
                std::uniform_int_distribution<size_t> select_current(0, seen_images - 1);
                if (select_current(sample_rng) == 0) {
                    sampled_image_pos = image_pos;
                    sampled_image =
                        make_device_batch_tensor(batch, options.device_id, loader.image_height(), loader.image_width())
                            .select(0, static_cast<int64_t>(image_pos))
                            .clone();
                }
            }
        }
        std::vector<PredictionBatchMetadata> prediction_metadata;
        prediction_metadata.reserve(batch.num_images);
        for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
            const auto dataset_index = static_cast<int64_t>(batch.image_indices[image_pos]);
            prediction_metadata.push_back(PredictionBatchMetadata{
                dataset_index,
                image_id_for_dataset_index(image_ids, dataset_index),
                {},
            });
        }
        std::optional<PreparedTargets> prepared;
        if (calculate_loss) {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.targets");
            prepared = build_targets(batch, static_cast<int>(loader.image_height()),
                                     static_cast<int>(loader.image_width()), detection_config.include_masks,
                                     detection_config.include_masks, options.device_id, validation.target_scratch());
        }
        batch_guard.release();

        ModelOutputs outputs;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.forward");
            if (batch_timing) {
                batch_timing.timing->record_start(EvaluationCudaBatchTiming::Phase::ModelForward, evaluation_stream);
            }
            {
                AutocastGuard autocast_guard(amp_enabled, autocast_dtype);
                outputs = model.forward(NestedTensor{inference_input, validation.nested_mask()});
            }
            assert_inference_output_dtype(outputs.main.pred_logits, outputs.main.pred_boxes, autocast_dtype,
                                          "RF-DETR training validation");
            validation.record_preprocess_consumer(
                c10::cuda::getCurrentCUDAStream(checked_device_index(options.device_id)).stream());
            if (validation_profile) {
                validation_profile->precision = evaluation_precision_name(outputs.main.pred_logits.scalar_type());
                validation_profile->box_precision = evaluation_precision_name(outputs.main.pred_boxes.scalar_type());
                validation_profile->query_count = static_cast<size_t>(outputs.main.pred_logits.size(1));
                validation_profile->class_count = static_cast<size_t>(outputs.main.pred_logits.size(2));
            }
            if (batch_timing) {
                batch_timing.timing->record_stop(EvaluationCudaBatchTiming::Phase::ModelForward, evaluation_stream);
            }
        }

        const auto real_count = static_cast<int64_t>(batch.num_images);
        if (outputs.main.pred_logits.size(0) < real_count) {
            throw std::runtime_error("RF-DETR validation output batch is smaller than the input batch");
        }
        std::optional<ModelOutputs> real_outputs;
        const ModelOutputs* evaluated_outputs = &outputs;
        if (outputs.main.pred_logits.size(0) != real_count) {
            real_outputs = narrow_model_outputs_batch(outputs, real_count);
            evaluated_outputs = &*real_outputs;
        }

        if (calculate_loss) {
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.eval.targets_handoff");
                validation.target_scratch().handoff_pending_copy_to_current_stream(options.device_id);
            }

            TensorMap loss_dict;
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.eval.loss_dict");
                loss_dict = detection_loss_dict(*evaluated_outputs, *prepared, detection_config, false, false);
            }

            torch::Tensor loss;
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.eval.loss_total");
                loss = weighted_detection_loss(loss_dict, detection_config, inference_input.device());
            }
            loss_sum += loss.item<double>();
            ++batch_count;
        }

        PostprocessedBatch processed = [&]() {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.postprocess");
            if (batch_timing) {
                batch_timing.timing->record_start(EvaluationCudaBatchTiming::Phase::Postprocess, evaluation_stream);
            }
            PostprocessedBatch postprocessed = postprocess_output_batch_fixed_size(
                OutputTensors{evaluated_outputs->main.pred_logits, evaluated_outputs->main.pred_boxes,
                              evaluated_outputs->main.pred_masks},
                static_cast<int64_t>(loader.image_height()), static_cast<int64_t>(loader.image_width()),
                model.config().num_select);
            if (batch_timing) {
                batch_timing.timing->record_stop(EvaluationCudaBatchTiming::Phase::Postprocess, evaluation_stream);
            }
            return postprocessed;
        }();
        auto processed_ready =
            record_shared_cuda_event(c10::cuda::getCurrentCUDAStream(checked_device_index(options.device_id)).stream(),
                                     "cudaEventRecord for train eval predictions");

        const size_t processed_count =
            std::min(static_cast<size_t>(processed.size()), static_cast<size_t>(batch.num_images));
        if (validation_profile) {
            const size_t prediction_count = processed_count * static_cast<size_t>(processed.scores.size(1));
            const size_t bbox_bytes = prediction_count * (static_cast<size_t>(processed.scores.element_size()) +
                                                          static_cast<size_t>(processed.labels.element_size()) +
                                                          4U * static_cast<size_t>(processed.boxes.element_size()));
            size_t mask_bytes = 0;
            if (processed.masks.has_value()) {
                const size_t pixels_per_mask =
                    static_cast<size_t>(processed.masks->size(2)) * static_cast<size_t>(processed.masks->size(3));
                mask_bytes = prediction_count * ((pixels_per_mask + 7U) / 8U);
            }
            validation_profile->transferred_bytes += bbox_bytes + mask_bytes;
            validation_profile->mask_transferred_bytes += mask_bytes;
        }
        prediction_metadata.resize(processed_count);
        if (sampled_image_pos.has_value() && *sampled_image_pos < processed_count) {
            const auto image_index = static_cast<int64_t>(*sampled_image_pos);
            const auto scores = processed.scores[image_index];
            const auto labels = processed.labels[image_index];
            const auto boxes = processed.boxes[image_index];
            const torch::Tensor masks = processed.masks.has_value() ? (*processed.masks)[image_index] : torch::Tensor();
            const auto keep = scores > 0.35f;
            torch::Tensor filtered_masks;
            if (masks.defined()) {
                filtered_masks = masks.index({keep}).clone();
            }
            captured_sample = CapturedEvalSample{
                std::move(sampled_image),
                boxes.index({keep}).clone(),
                labels.index({keep}).clone(),
                std::move(filtered_masks),
            };
        }

        const size_t lane_index = submitted_batches % lanes.size();
        lane_futures.push_back(
            lane_pool.enqueue([&lanes, lane_index, processed = std::move(processed), processed_ready,
                               metadata = std::move(prediction_metadata), category_count = loader.num_classes(),
                               max_dets = options.eval_max_dets, device_id = options.device_id]() mutable {
                auto& lane = lanes[lane_index];
                PredictionBufferLease lease = lane.slot_pool->acquire();
                ensure_cuda_ok(cudaStreamWaitEvent(lane.stream.stream(), processed_ready->get(), 0),
                               "cudaStreamWaitEvent for train eval prediction staging");
                c10::InferenceMode lane_inference_mode;
                c10::cuda::CUDAGuard lane_device_guard(checked_device_index(device_id));
                c10::cuda::CUDAStreamGuard stream_guard(lane.stream);
                return stage_prediction_batch(std::move(metadata), std::move(processed), category_count, max_dets,
                                              std::move(lease), device_id,
                                              reinterpret_cast<void*>(lane.stream.stream()));
            }));
        if (batch_timing) {
            lane_timing_leases.push_back(batch_timing);
        }
        ++submitted_batches;
        update_profile_peaks();
        while (static_cast<int>(lane_futures.size()) >= runtime.split().lane_threads) {
            drain_lane();
        }
        while (cpu_futures.size() >= max_cpu_futures) {
            drain_cpu();
        }
    }

    while (!lane_futures.empty()) {
        drain_lane();
        while (cpu_futures.size() >= max_cpu_futures) {
            drain_cpu();
        }
    }
    while (!cpu_futures.empty()) {
        drain_cpu();
    }
    if (capture_eval_sample && captured_sample.has_value()) {
        RenderSampleOptions render_options;
        render_options.num_classes = model.config().num_classes;
        render_options.output_path =
            options.output_dir / "eval_samples" / std::format("epoch_{}.png", *current_epoch + 1);
        draw_eval_sample_async_gpu(captured_sample->image, captured_sample->boxes, captured_sample->labels,
                                   captured_sample->masks, render_options);
    }
    if (progress) {
        progress->close();
    }
    if (calculate_loss) {
        result.loss = batch_count > 0 ? loss_sum / static_cast<double>(batch_count) : 0.0;
    }
    {
        MMLTK_PROFILE_SCOPE("rfdetr.train.eval.metric");
        result.summary = dataset.evaluate(options.eval_max_dets, validation_profile.get(), &cpu_pool);
    }
    flush_eval_sample_writes();
    result.timing = elapsed_timing(started_at, image_count);
    return result;
}

}  

TrainRunResult run_training(const TrainOptions& options) {
    MMLTK_PROFILE_SCOPE("rfdetr.train.total");
    if (options.train_compiled_path.empty() || options.val_compiled_path.empty() || options.output_dir.empty()) {
        throw std::runtime_error("RF-DETR train requires --train-compiled, --val-compiled, and --output-dir");
    }
    const size_t selected_inputs =
        static_cast<size_t>(!options.weights_path.empty()) + static_cast<size_t>(!options.resume_path.empty());
    if (selected_inputs != 1) {
        throw std::runtime_error("RF-DETR train requires exactly one of --weights or --resume");
    }
    if (options.lr_scheduler != "step" && options.lr_scheduler != "cosine") {
        throw std::runtime_error("RF-DETR train lr_scheduler must be 'step' or 'cosine'");
    }
    if (options.batch_size == 0 || options.epochs <= 0 || options.grad_accum_steps <= 0) {
        throw std::runtime_error("RF-DETR train requires positive batch-size, epochs, and grad-accum-steps");
    }
    if (options.prefetch_factor <= 0) {
        throw std::runtime_error("RF-DETR train requires a positive prefetch-factor");
    }
    if (!valid_gpu_augmentation_config(options.gpu_augmentation)) {
        throw std::runtime_error(
            "RF-DETR GPU augmentation probabilities and strength ranges must satisfy 0 <= min <= max <= 1");
    }
    if (!options.distributed_worker && options.device_ids.size() > 1) {
        throw std::runtime_error("multi-GPU RF-DETR training must be launched through the CLI spawner");
    }

    DistributedContext distributed = make_distributed_context(options);
    const bool main_process = is_rank_zero(distributed);
    const int requested_train_lanes = effective_train_lanes(options);
    RuntimeContext train_runtime(
        resolve_runtime_config(options.workers, requested_train_lanes, options.prefetch_factor, options.cpu_affinity));
    const int train_lane_count = train_runtime.split().lane_threads;
    ScopedWorkerPool worker_scope(&train_runtime.cpu_pool());

    auto make_loader_config_for = [&](const std::filesystem::path& compiled_path, size_t loader_batch_size,
                                      bool shuffle, int prefetch_factor, bool shard_batches, bool drop_last) {
        auto config = make_loader_config(compiled_path.string(), loader_batch_size, shuffle, prefetch_factor,
                                         train_runtime.split().gather_threads, train_runtime.loader_affinity_string(),
                                         options.device_id, static_cast<uint64_t>(options.seed));
        config.drop_last = drop_last;
        if (distributed.enabled && shard_batches) {
            config.batch_shard_rank = static_cast<uint32_t>(distributed.rank);
            config.batch_shard_count = static_cast<uint32_t>(distributed.world_size);
        }
        return config;
    };

    const size_t val_batch_size = options.val_batch_size > 0 ? options.val_batch_size : options.batch_size;
    DatasetLoader train_loader(make_loader_config_for(options.train_compiled_path, options.batch_size, true,
                                                      options.prefetch_factor, true, true));
    std::unique_ptr<DatasetLoader> val_loader;
    if (main_process) {
        val_loader = std::make_unique<DatasetLoader>(make_loader_config_for(
            options.val_compiled_path, val_batch_size, false, options.prefetch_factor, false, false));
    }

    const auto source_checkpoint = !options.resume_path.empty() ? options.resume_path : options.weights_path;
    if (train_loader.image_width() != train_loader.image_height()) {
        throw std::runtime_error("train compiled RF-DETR input must be square");
    }
    ModelArtifactRequest artifact_request;
    artifact_request.weights_path = source_checkpoint;
    artifact_request.preset_name = options.preset_name;
    artifact_request.resolution = static_cast<int>(train_loader.image_width());
    auto artifacts = resolve_model_artifacts(artifact_request);
    const int dataset_output_classes = rfdetr_output_class_count(train_loader.num_classes());
    if (!options.resume_path.empty()) {
        const auto resume_checkpoint = load_checkpoint(options.resume_path);
        if (resume_checkpoint.metadata.num_classes > 0 &&
            resume_checkpoint.metadata.num_classes != static_cast<int64_t>(dataset_output_classes)) {
            throw std::runtime_error("resume checkpoint class count does not match compiled dataset class count");
        }
    }
    artifacts.config.num_classes = dataset_output_classes;

    const auto validate_loader = [&](const DatasetLoader& loader, const char* split) {
        if (loader.image_width() != static_cast<uint32_t>(artifacts.config.resolution) ||
            loader.image_height() != static_cast<uint32_t>(artifacts.config.resolution)) {
            throw std::runtime_error(std::string(split) + " compiled resolution does not match RF-DETR input size");
        }
        if (loader.num_classes() != train_loader.num_classes()) {
            throw std::runtime_error("compiled class count mismatch across train/val/test splits");
        }
    };
    validate_loader(train_loader, "train");
    if (val_loader) {
        validate_loader(*val_loader, "val");
    }

    NativeRfDetrModel model(artifacts.config);
    model.to(cuda_device(options.device_id));
    const StateDictLoadSummary load_summary = model.load_weights(source_checkpoint, false);
    model.optimize_for_inference(checked_inference_batch_size(options.batch_size), true, options.compilation_mode);
    if (!options.val_compiled_path.empty()) {
        model.optimize_for_inference(checked_inference_batch_size(val_batch_size), false, options.compilation_mode);
    }
    if (main_process) {
        mmltk::logging::logger("rfdetr.train")
            ->info("rfdetr weights: loaded={} missing={} unexpected={} incompatible={} input={}",
                   load_summary.loaded_names.size(), load_summary.missing_names.size(),
                   load_summary.unexpected_names.size(), load_summary.incompatible_names.size(),
                   source_checkpoint.string());
        for (const auto& name : load_summary.missing_names) {
            mmltk::logging::logger("rfdetr.train")->warn("  missing: {}", name);
        }
        for (const auto& name : load_summary.unexpected_names) {
            mmltk::logging::logger("rfdetr.train")->warn("  unexpected: {}", name);
        }
    }
    ensure_train_lane_model_supported(model, train_lane_count);
    model.train();

    if (options.freeze_encoder) {
        for (auto& item : model.named_parameters(true)) {
            if (is_encoder_param(item.key())) {
                item.value().set_requires_grad(false);
            }
        }
    }

    auto optimizer_build = build_optimizer(model, options);
    auto& optimizer = optimizer_build.optimizer;
    auto& all_params = optimizer.parameters();
    const auto& all_param_names = optimizer.parameter_names();

    const bool amp_enabled = options.amp;
    const auto autocast_dtype = amp_enabled ? resolve_cuda_autocast_dtype() : at::kFloat;
    const bool scaler_enabled = amp_enabled && autocast_dtype == torch::kFloat16;
    GradScaler grad_scaler(scaler_enabled);
    std::optional<ModelEma> ema;
    if (options.use_ema && main_process) {
        ema.emplace(all_params, options.ema_decay, static_cast<double>(options.ema_tau));
    }

    int start_epoch = 0;
    double best_regular = -std::numeric_limits<double>::infinity();
    double best_ema = -std::numeric_limits<double>::infinity();
    if (!options.resume_path.empty()) {
        const ResumeState resume_state =
            load_resume_checkpoint_state(options.resume_path, optimizer, grad_scaler, options);
        start_epoch = resume_state.start_epoch;
        best_regular = resume_state.best_regular;
        best_ema = resume_state.best_ema;
        if (!resume_state.ema_state.empty() && main_process) {
            std::vector<torch::Tensor> shadow_params;
            shadow_params.reserve(resume_state.ema_state.size());
            for (const auto& entry : resume_state.ema_state) {
                shadow_params.push_back(entry.tensor.to(all_params.front().device()));
            }
            if (!ema.has_value()) {
                ema.emplace(all_params, options.ema_decay, static_cast<double>(options.ema_tau));
            }
            ema->load_shadow_params(shadow_params);
        }
    }

    LrScheduleConfig lr_config;
    lr_config.warmup_epochs = options.warmup_epochs;
    lr_config.warmup_momentum = options.warmup_momentum;
    lr_config.lr_scheduler = options.lr_scheduler;
    lr_config.lr_drop = options.lr_drop;
    lr_config.lr_min_factor = options.lr_min_factor;

    if (main_process) {
        mmltk::logging::logger("rfdetr.train")
            ->info(
                "rfdetr train runtime: torch={} autocast={} optimizer={} optimizer_backend={} scaler={} train_lanes={} "
                "eval_lanes={} loader_threads={} gather_threads={} cpu_threads={} effective_batch_per_rank={} "
                "effective_batch_global={}",
                TORCH_VERSION, scalar_type_name(autocast_dtype), optimizer.kind_name(), optimizer.backend_name(),
                grad_scaler.enabled() ? "on" : "off", train_lane_count, train_runtime.split().lane_threads,
                train_runtime.split().loader_threads, train_runtime.split().gather_threads,
                train_runtime.split().cpu_threads, effective_batch_per_rank(options, train_lane_count),
                effective_batch_global(options, distributed, train_lane_count));
        if (options.optimizer == TrainOptimizerKind::Muon && options.fused_optimizer) {
            mmltk::logging::logger("rfdetr.train")
                ->warn(
                    "rfdetr train runtime: optimizer=muon ignores --fused-optimizer and runs with the eager backend "
                    "only");
        }
    }

    const size_t usable_full_batches =
        full_batches_per_rank(train_loader.num_images(), options.batch_size,
                              static_cast<size_t>(distributed.world_size), options.grad_accum_steps, train_lane_count);
    if (usable_full_batches == 0) {
        throw std::runtime_error(
            "compiled train split is too small for one effective batch per rank; reduce batch_size, "
            "grad_accum_steps, --lanes, or world size");
    }
    const size_t batches_per_step = micro_batches_per_optimizer_step(options, train_lane_count);
    const auto steps_per_epoch = static_cast<int64_t>(usable_full_batches / batches_per_step);
    const int64_t total_training_steps = std::max<int64_t>(1, steps_per_epoch * options.epochs);
    DetectionConfig detection_config =
        make_detection_config(artifacts.config, distributed.world_size, options.compilation_mode);
    TargetScratch target_scratch;

    if (main_process) {
        std::filesystem::create_directories(options.output_dir);
    }
    const auto progress_path = options.output_dir / "progress.json";
    const auto log_path = options.output_dir / "log.txt";
    const auto validation_profile_path = options.output_dir / "validation_profile.jsonl";
    const auto results_path = options.output_dir / "results.json";
    const auto checkpoint_path = options.output_dir / "checkpoint.pt";
    const auto best_regular_checkpoint_path = options.output_dir / "checkpoint_best_regular.pt";
    const auto best_ema_checkpoint_path = options.output_dir / "checkpoint_best_ema.pt";
    const NativeCheckpointMetadata metadata = checkpoint_metadata(artifacts, dataset_output_classes);

    TrainRunResult result;
    result.artifacts = artifacts;
    result.gpu_augmentation = options.gpu_augmentation;
    result.output_dir = options.output_dir;
    result.checkpoint_path = checkpoint_path;
    result.best_regular_checkpoint_path = best_regular_checkpoint_path;
    result.best_ema_checkpoint_path = best_ema_checkpoint_path;
    if (main_process && std::isfinite(best_regular) && std::filesystem::exists(best_regular_checkpoint_path)) {
        result.best_checkpoint_path = best_regular_checkpoint_path;
        result.best_is_ema = false;
    }
    if (main_process && std::isfinite(best_ema) && std::filesystem::exists(best_ema_checkpoint_path) &&
        best_ema >= best_regular) {
        result.best_checkpoint_path = best_ema_checkpoint_path;
        result.best_is_ema = true;
    }
    std::unique_ptr<LatestJsonWriter> progress_writer;
    if (main_process) {
        std::error_code ignored_error;
        std::filesystem::remove(progress_path, ignored_error);
        std::filesystem::remove(log_path, ignored_error);
        std::filesystem::remove(results_path, ignored_error);
        if (options.validation_profile) {
            std::filesystem::remove(validation_profile_path, ignored_error);
        }
        progress_writer = std::make_unique<LatestJsonWriter>(progress_path);
        progress_writer->submit(
            json{
                {"phase", "starting"},
                {"epoch", start_epoch},
                {"total_epochs", options.epochs},
                {"completed_batches", 0},
                {"total_batches", usable_full_batches},
                {"completed_waves", 0},
                {"optimizer_steps", 0},
                {"steps_per_epoch", steps_per_epoch},
                {"train_lanes", train_lane_count},
                {"train_loss", 0.0},
                {"class_loss", 0.0},
                {"box_loss", 0.0},
                {"step_loss", 0.0},
                {"step_class_loss", 0.0},
                {"step_box_loss", 0.0},
                {"batches_per_second", 0.0},
                {"images_per_second", 0.0},
                {"elapsed_seconds", 0.0},
                {"checkpoint_path", std::string{}},
                {"val_loss", nullptr},
                {"val", json()},
            },
            true);
    }

    c10::cuda::CUDAGuard device_guard(cuda_device_index(options.device_id));
    std::unique_ptr<TrainingValidationRuntime> validation_runtime;
    if (main_process) {
        validation_runtime = std::make_unique<TrainingValidationRuntime>(
            options, train_runtime, std::move(val_loader), val_batch_size, options.validation_loss,
            detection_config.include_masks ? EvaluationMetricSet::BBoxAndMask : EvaluationMetricSet::BBox,
            artifacts.config.num_select);
    }
    TrainingMetricHandoff metric_handoff(options.device_id);
    std::unique_ptr<WorkerPool> train_lane_pool;
    std::vector<TrainLaneContext> train_lanes;
    std::unique_ptr<GpuBatchAugmenter> single_lane_augmenter;
    if (train_lane_count <= 1) {
        single_lane_augmenter = std::make_unique<GpuBatchAugmenter>(
            options.gpu_augmentation, static_cast<std::int64_t>(options.batch_size),
            static_cast<int>(train_loader.image_height()), static_cast<int>(train_loader.image_width()),
            options.device_id, detection_config.include_masks);
    }
    if (train_lane_count > 1) {
        train_lane_pool = std::make_unique<WorkerPool>(static_cast<size_t>(train_lane_count), train_runtime.lane_cpus(),
                                                       "rfdtrtlane");
        train_lanes.reserve(static_cast<size_t>(train_lane_count));
        for (int lane_index = 0; lane_index < train_lane_count; ++lane_index) {
            train_lanes.push_back(TrainLaneContext{
                .stream = get_high_priority_cuda_stream(options.device_id),
                .target_scratch = {},
                .augmenter = {},
                .model = {},
                .grad_params = {},
                .synced_parameter_version = std::numeric_limits<size_t>::max(),
            });
        }
        for (auto& lane : train_lanes) {
            lane.augmenter = std::make_unique<GpuBatchAugmenter>(
                options.gpu_augmentation, static_cast<std::int64_t>(options.batch_size),
                static_cast<int>(train_loader.image_height()), static_cast<int>(train_loader.image_width()),
                options.device_id, detection_config.include_masks);
            lane.model = make_train_lane_model(model, options.device_id);
            lane.model->optimize_for_inference(checked_inference_batch_size(options.batch_size), true,
                                               options.compilation_mode);
            lane.grad_params = lane_grad_parameters(*lane.model, all_param_names);
        }
    }
    size_t parameter_version = 0;
    distributed_barrier(distributed);
    for (int epoch = start_epoch; epoch < options.epochs; ++epoch) {
        MMLTK_PROFILE_SCOPE("rfdetr.train.epoch");
        result.last_epoch = epoch;
        train_loader.begin_epoch();
        model.train();
        optimizer.zero_grad(true);
        metric_handoff.reset_epoch();
        const auto epoch_started = std::chrono::steady_clock::now();
        int64_t local_micro_batches = 0;
        int64_t reported_micro_batches = 0;
        int64_t local_waves = 0;
        int64_t optimizer_steps = 0;
        size_t local_full_batches = 0;
        double host_loss_sum = 0.0;
        double host_class_loss_sum = 0.0;
        double host_box_loss_sum = 0.0;
        double current_step_loss = 0.0;
        double current_step_class_loss = 0.0;
        double current_step_box_loss = 0.0;
        auto last_progress_submit = epoch_started - std::chrono::milliseconds(250);

        std::unique_ptr<spdmon::ProgressBar> progress;
        if (main_process && options.progress_bar) {
            progress = std::make_unique<spdmon::ProgressBar>(
                phase_progress_label("train", epoch, options.epochs),
                static_cast<size_t>(usable_full_batches) * static_cast<size_t>(options.batch_size), "img");
            progress->set_postfix("cl=warming, bl=warming, l=warming");
        }

        auto write_progress_snapshot = [&](std::string_view phase, std::optional<double> val_loss,
                                           const json& val_summary, const std::filesystem::path& checkpoint_override,
                                           bool force) {
            if (!main_process) {
                return;
            }
            const double elapsed_seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch_started).count();
            const double average_class_loss =
                reported_micro_batches > 0 ? host_class_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double average_box_loss =
                reported_micro_batches > 0 ? host_box_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double average_loss =
                reported_micro_batches > 0 ? host_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double batches_per_second =
                elapsed_seconds > 0.0 ? static_cast<double>(local_micro_batches) / elapsed_seconds : 0.0;
            const double images_per_second = elapsed_seconds > 0.0
                                                 ? static_cast<double>(local_micro_batches) *
                                                       static_cast<double>(options.batch_size) / elapsed_seconds
                                                 : 0.0;
            progress_writer->submit(
                json{
                    {"phase", std::string(phase)},
                    {"epoch", epoch},
                    {"total_epochs", options.epochs},
                    {"completed_batches", local_micro_batches},
                    {"total_batches", usable_full_batches},
                    {"completed_waves", local_waves},
                    {"optimizer_steps", optimizer_steps},
                    {"steps_per_epoch", steps_per_epoch},
                    {"train_lanes", train_lane_count},
                    {"train_loss", average_loss},
                    {"class_loss", average_class_loss},
                    {"box_loss", average_box_loss},
                    {"step_loss", current_step_loss},
                    {"step_class_loss", current_step_class_loss},
                    {"step_box_loss", current_step_box_loss},
                    {"batches_per_second", batches_per_second},
                    {"images_per_second", images_per_second},
                    {"elapsed_seconds", elapsed_seconds},
                    {"checkpoint_path", checkpoint_override.empty() ? std::string{} : checkpoint_override.string()},
                    {"val_loss", val_loss.has_value() ? json(*val_loss) : json(nullptr)},
                    {"val", val_summary},
                },
                force);
        };

        auto flush_progress = [&](bool force) {
            if (local_micro_batches == 0) {
                return;
            }
            const double average_class_loss =
                reported_micro_batches > 0 ? host_class_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double average_box_loss =
                reported_micro_batches > 0 ? host_box_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double average_loss =
                reported_micro_batches > 0 ? host_loss_sum / static_cast<double>(reported_micro_batches) : 0.0;
            const double elapsed_seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch_started).count();
            const double images_per_second = elapsed_seconds > 0.0
                                                 ? static_cast<double>(local_micro_batches) *
                                                       static_cast<double>(options.batch_size) / elapsed_seconds
                                                 : 0.0;
            const bool should_update_bar =
                static_cast<bool>(progress) && (force || local_micro_batches % std::max(1, options.print_freq) == 0);
            if (should_update_bar) {
                progress->set_postfix(train_progress_postfix(
                    average_class_loss, average_box_loss, average_loss, current_step_class_loss, current_step_box_loss,
                    current_step_loss, images_per_second, optimizer_steps, steps_per_epoch));
            }
            const auto now = std::chrono::steady_clock::now();
            if (force || now - last_progress_submit >= std::chrono::milliseconds(250)) {
                write_progress_snapshot("train", std::nullopt, json{}, std::filesystem::path{}, force);
                last_progress_submit = now;
            }
        };

        write_progress_snapshot("train", std::nullopt, json{}, std::filesystem::path{}, true);
        last_progress_submit = std::chrono::steady_clock::now();

        const auto apply_optimizer_schedule = [&](int64_t current_step) {
            const double lr_scale = compute_lr_scale(lr_config, current_step, steps_per_epoch, total_training_steps);
            set_optimizer_lrs(optimizer, optimizer_build.base_lrs, lr_scale);
            if (options.optimizer == TrainOptimizerKind::Muon) {
                optimizer.set_muon_momentum(
                    compute_warmup_momentum(lr_config, current_step, steps_per_epoch, options.momentum));
            }
        };
        const auto run_optimizer_step = [&](const auto& post_step, const TensorMap* loss_report,
                                            int64_t wave_micro_batches) {
            MMLTK_PROFILE_SCOPE("rfdetr.train.optimizer");
            const int64_t current_step = static_cast<int64_t>(epoch) * steps_per_epoch + optimizer_steps;
            apply_optimizer_schedule(current_step);
            average_gradients(distributed, all_params);
            const auto found_inf = grad_scaler.check_and_unscale_(optimizer);
            if (options.clip_max_norm > 0.0) {
                torch::nn::utils::clip_grad_norm_(all_params, options.clip_max_norm);
            }

            const TrainingMetricSnapshot metrics = metric_handoff.complete_step(found_inf, wave_micro_batches);
            host_loss_sum = metrics.loss_sum;
            host_class_loss_sum = metrics.class_loss_sum;
            host_box_loss_sum = metrics.box_loss_sum;
            current_step_loss = metrics.step_loss;
            current_step_class_loss = metrics.step_class_loss;
            current_step_box_loss = metrics.step_box_loss;
            reported_micro_batches = local_micro_batches;

            const bool gradient_overflow = !metrics.gradients_finite;
            if (!metrics.loss_finite || (gradient_overflow && !grad_scaler.enabled())) {
                const TensorMap empty_loss_report;
                const auto& report = loss_report != nullptr ? *loss_report : empty_loss_report;
                const std::string details = format_nonfinite_loss_report(report, all_params, all_param_names);
                grad_scaler.update(gradient_overflow);
                optimizer.zero_grad(true);
                throw std::runtime_error("non-finite RF-DETR loss or gradients encountered during native training" +
                                         details);
            }

            std::string overflow_details;
            if (gradient_overflow) {
                const TensorMap empty_loss_report;
                const auto& report = loss_report != nullptr ? *loss_report : empty_loss_report;
                overflow_details = format_nonfinite_loss_report(report, all_params, all_param_names);
            }
            grad_scaler.step(optimizer, gradient_overflow);
            grad_scaler.update(gradient_overflow);
            optimizer.zero_grad(true);
            if (ema.has_value() && !gradient_overflow) {
                ema->update(all_params, optimizer_steps);
            }
            if (gradient_overflow) {
                mmltk::logging::logger("rfdetr.train")
                    ->warn("skipped RF-DETR optimizer update after gradient overflow{}", overflow_details);
            }
            ++optimizer_steps;
            post_step();
        };

        auto next_train_full_batch = [&](bool wait_now = true) -> std::optional<Batch> {
            Batch batch{};
            while (train_loader.next_batch(batch)) {
                MMLTK_PROFILE_SCOPE("rfdetr.train.batch");
                if (wait_now) {
                    MMLTK_PROFILE_SCOPE("rfdetr.train.wait_batch");
                    train_loader.wait_batch(batch);
                }
                if (batch.num_images != options.batch_size) {
                    if (!wait_now) {
                        train_loader.wait_batch(batch);
                    }
                    train_loader.release_batch(batch);
                    continue;
                }
                if (local_full_batches >= usable_full_batches) {
                    if (!wait_now) {
                        train_loader.wait_batch(batch);
                    }
                    train_loader.release_batch(batch);
                    return std::nullopt;
                }
                ++local_full_batches;
                MMLTK_PROFILE_ADD("rfdetr.train.images", batch.num_images);
                MMLTK_PROFILE_ADD("rfdetr.train.full_batches", 1);
                return batch;
            }
            return std::nullopt;
        };

        std::shared_ptr<SharedCudaEvent> params_ready;
        if (train_lane_count > 1) {
            params_ready = record_current_stream_event(options.device_id, "parallel train parameter readiness event");
        }

        if (train_lane_count <= 1) {
            while (true) {
                auto batch = next_train_full_batch();
                if (!batch.has_value()) {
                    break;
                }
                metric_handoff.begin_wave();

                LoaderBatchGuard batch_guard(train_loader, *batch, options.device_id);
                torch::Tensor normalized;
                {
                    MMLTK_PROFILE_SCOPE("rfdetr.train.augment");
                    normalized = single_lane_augmenter->run(*batch, static_cast<std::uint64_t>(options.seed), epoch,
                                                            distributed.rank, local_full_batches - 1);
                }

                torch::Tensor loss;
                torch::Tensor class_loss;
                torch::Tensor box_loss;
                TensorMap loss_dict;
                {
                    PreparedTargets prepared;
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.targets");
                        prepared =
                            build_targets(*batch, static_cast<int>(train_loader.image_height()),
                                          static_cast<int>(train_loader.image_width()), detection_config.include_masks,
                                          detection_config.include_masks, options.device_id, target_scratch,
                                          &single_lane_augmenter->batch_plan());
                    }
                    batch_guard.set_consumer_stream(
                        single_lane_augmenter->finish_batch(*batch, prepared, target_scratch));
                    batch_guard.release();

                    AutocastGuard autocast_guard(amp_enabled, autocast_dtype);
                    ModelOutputs outputs;
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.forward");
                        outputs = model.forward(NestedTensor{normalized, prepared.nested_mask});
                    }
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.targets_handoff");
                        target_scratch.handoff_pending_copy_to_current_stream(options.device_id);
                    }
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.loss_dict");
                        loss_dict = detection_loss_dict(outputs, prepared, detection_config, true, distributed.enabled,
                                                        distributed.enabled
                                                            ? AllReduceTensorFn([&distributed](torch::Tensor& value) {
                                                                  distributed_all_reduce_tensor(distributed, value);
                                                              })
                                                            : AllReduceTensorFn{});
                    }
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.loss_total");
                        loss = weighted_detection_loss(loss_dict, detection_config, normalized.device());
                        class_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_ce");
                        box_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_bbox");
                    }
                }
                metric_handoff.accumulate(loss, class_loss, box_loss);
                ++local_micro_batches;
                ++local_waves;
                const auto scaled_loss = grad_scaler.scale(loss / static_cast<double>(options.grad_accum_steps));
                {
                    MMLTK_PROFILE_SCOPE("rfdetr.train.backward");
                    scaled_loss.backward();
                }

                if (local_waves % options.grad_accum_steps == 0) {
                    run_optimizer_step([] {}, &loss_dict, 1);
                }

                if (progress) {
                    progress->add(static_cast<size_t>(options.batch_size));
                }
                flush_progress(false);
            }
        } else {
            while (local_full_batches < usable_full_batches) {
                metric_handoff.begin_wave();
                const double current_scale =
                    grad_scaler.enabled() ? static_cast<double>(grad_scaler.current_scale()) : 1.0;
                const double scaled_loss_factor =
                    current_scale / static_cast<double>(micro_batches_per_optimizer_step(options, train_lane_count));
                std::vector<std::future<TrainLaneResult>> lane_futures;
                lane_futures.reserve(static_cast<size_t>(train_lane_count));

                for (int lane_index = 0; lane_index < train_lane_count; ++lane_index) {
                    auto batch = next_train_full_batch(false);
                    if (!batch.has_value()) {
                        throw std::runtime_error(
                            "native RF-DETR training ended an epoch with an incomplete parallel train wave");
                    }

                    auto& lane = train_lanes[static_cast<size_t>(lane_index)];

                    const double num_boxes_value =
                        resolve_num_boxes_value(batch_target_instance_count(*batch), detection_config, true,
                                                distributed, cuda_device(options.device_id));
                    lane_futures.push_back(enqueue_train_lane(
                        *train_lane_pool, &train_runtime.cpu_pool(), train_loader, lane, *batch, params_ready,
                        num_boxes_value, scaled_loss_factor, parameter_version, detection_config, model,
                        options.device_id, static_cast<int>(train_loader.image_height()),
                        static_cast<int>(train_loader.image_width()), static_cast<std::uint64_t>(options.seed), epoch,
                        distributed.rank, local_full_batches - 1, amp_enabled, autocast_dtype));
                }

                for (auto& lane_future : lane_futures) {
                    TrainLaneResult lane_result = lane_future.get();
                    merge_lane_gradients(lane_result, all_params, options.device_id);
                    metric_handoff.accumulate(lane_result.loss, lane_result.class_loss, lane_result.box_loss);
                    ++local_micro_batches;
                    if (progress) {
                        progress->add(static_cast<size_t>(options.batch_size));
                    }
                }

                ++local_waves;
                if (local_waves % options.grad_accum_steps == 0) {
                    run_optimizer_step(
                        [&] {
                            ++parameter_version;
                            params_ready = record_current_stream_event(options.device_id,
                                                                       "parallel train parameter readiness event");
                        },
                        nullptr, static_cast<int64_t>(lane_futures.size()));
                }
                flush_progress(false);
            }
        }

        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.drain_loader");
            Batch drain_batch{};
            while (train_loader.next_batch(drain_batch)) {
                train_loader.release_batch(drain_batch);
            }
        }

        if (local_micro_batches == 0 || local_waves % options.grad_accum_steps != 0) {
            throw std::runtime_error("native RF-DETR training ended an epoch with incomplete gradient accumulation");
        }
        if (train_lane_count <= 1) {
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.wait_pending_copy");
                target_scratch.wait_for_pending_copy();
            }
        } else {
            MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.wait_pending_copy");
            for (auto& lane : train_lanes) {
                lane.target_scratch.wait_for_pending_copy();
            }
        }
        flush_progress(true);
        if (progress) {
            progress->close();
        }

        auto reduced_loss_sum = metric_handoff.loss_sum().detach().clone();
        auto reduced_micro_batches =
            torch::tensor({static_cast<float>(local_micro_batches)},
                          torch::TensorOptions().dtype(torch::kFloat32).device(cuda_device(options.device_id)));
        if (distributed.enabled) {
            distributed_all_reduce_tensor(distributed, reduced_loss_sum);
            distributed_all_reduce_tensor(distributed, reduced_micro_batches);
        }
        const double train_loss = reduced_loss_sum.item<double>() / std::max(1.0, reduced_micro_batches.item<double>());
        distributed_barrier(distributed);

        if (main_process) {
            write_progress_snapshot("validate", std::nullopt, json{}, std::filesystem::path{}, true);
            auto val_result =
                evaluate_model(options, *validation_runtime, model, detection_config, options.validation_loss, epoch,
                               phase_progress_label("val", epoch, options.epochs));

            if (val_result.loss.has_value()) {
                mmltk::logging::logger("rfdetr.train")
                    ->info("epoch {} stats: optimizer={} train_loss={:.6f} val_loss={:.6f} bbox_ap={:.4f} mask_ap={}",
                           epoch + 1, optimizer.kind_name(), train_loss, *val_result.loss, val_result.summary.bbox.ap,
                           formatted_mask_ap(val_result.summary));
            } else {
                mmltk::logging::logger("rfdetr.train")
                    ->info("epoch {} stats: optimizer={} train_loss={:.6f} bbox_ap={:.4f} mask_ap={}", epoch + 1,
                           optimizer.kind_name(), train_loss, val_result.summary.bbox.ap,
                           formatted_mask_ap(val_result.summary));
            }

            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.save.epoch");
                NativeCheckpoint epoch_checkpoint;
                epoch_checkpoint.metadata = metadata;
                epoch_checkpoint.state_dict = collect_module_state(model);
                auto epoch_path = options.output_dir / std::format("checkpoint_epoch_{}.pt", epoch + 1);
                save_native_checkpoint(epoch_path, epoch_checkpoint);
            }

            TrainEpochSummary epoch_summary;
            epoch_summary.epoch = epoch;
            epoch_summary.train_loss = train_loss;
            epoch_summary.val_loss = val_result.loss;
            epoch_summary.val_summary = val_result.summary;

            const double regular_metric = checkpoint_metric(val_result.summary, detection_config.include_masks);
            if (regular_metric > best_regular) {
                best_regular = regular_metric;
                {
                    MMLTK_PROFILE_SCOPE("rfdetr.train.save.best_regular");
                    NativeCheckpoint checkpoint;
                    checkpoint.metadata = metadata;
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.save.best_regular.collect_state");
                        checkpoint.state_dict = collect_module_state(model);
                    }
                    save_native_checkpoint(best_regular_checkpoint_path, checkpoint);
                }
                result.best_is_ema = false;
                result.best_checkpoint_path = best_regular_checkpoint_path;
            }

            if (ema.has_value()) {
                std::vector<torch::Tensor> saved_params;
                saved_params.reserve(all_params.size());
                {
                    torch::NoGradGuard no_grad;
                    for (const auto& param : all_params) {
                        saved_params.push_back(param.detach().clone());
                    }
                    ema->copy_to(all_params);
                }
                auto ema_result =
                    evaluate_model(options, *validation_runtime, model, detection_config, options.validation_loss,
                                   epoch, phase_progress_label("ema", epoch, options.epochs));
                {
                    torch::NoGradGuard no_grad;
                    for (size_t index = 0; index < all_params.size(); ++index) {
                        all_params[index].copy_(saved_params[index]);
                    }
                }
                epoch_summary.ema_val_loss = ema_result.loss;
                epoch_summary.ema_val_summary = ema_result.summary;

                const double ema_metric = checkpoint_metric(ema_result.summary, detection_config.include_masks);
                if (ema_metric > best_ema) {
                    best_ema = ema_metric;
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.save.best_ema");
                        NativeCheckpoint checkpoint;
                        checkpoint.metadata = metadata;
                        {
                            MMLTK_PROFILE_SCOPE("rfdetr.train.save.best_ema.collect_state");
                            const auto overrides = ema_override_map(all_param_names, *ema);
                            checkpoint.state_dict = collect_module_state(model, &overrides);
                        }
                        save_native_checkpoint(best_ema_checkpoint_path, checkpoint);
                    }
                    result.best_is_ema = true;
                    result.best_checkpoint_path = best_ema_checkpoint_path;
                }
            }

            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume");
                save_resume_checkpoint(checkpoint_path, model, metadata, optimizer, grad_scaler, options, epoch,
                                       best_regular, best_ema, all_param_names, ema);
            }

            result.history.push_back(epoch_summary);
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.save.log_json");
                append_json_line(
                    log_path,
                    json{
                        {"epoch", epoch},
                        {"train_lanes", train_lane_count},
                        {"eval_lanes", train_runtime.split().lane_threads},
                        {"effective_batch_per_rank", effective_batch_per_rank(options, train_lane_count)},
                        {"effective_batch_global", effective_batch_global(options, distributed, train_lane_count)},
                        {"train_loss", train_loss},
                        {"val_loss", val_result.loss.has_value() ? json(*val_result.loss) : json(nullptr)},
                        {"val", eval_summary_json(val_result.summary)},
                        {"ema_val_loss",
                         epoch_summary.ema_val_loss.has_value() ? json(*epoch_summary.ema_val_loss) : json(nullptr)},
                        {"ema_val", epoch_summary.ema_val_summary.has_value()
                                        ? eval_summary_json(*epoch_summary.ema_val_summary)
                                        : json()},
                    });
            }
            write_progress_snapshot(
                "epoch_complete", val_result.loss, eval_summary_json(val_result.summary),
                result.best_checkpoint_path.has_value() ? *result.best_checkpoint_path : checkpoint_path, true);
        }
        distributed_barrier(distributed);
    }

    if (main_process && !result.best_checkpoint_path.has_value()) {
        result.best_checkpoint_path = checkpoint_path;
    }

    if (main_process && !options.test_compiled_path.empty()) {
        auto test_config = make_loader_config_for(options.test_compiled_path, val_batch_size, false,
                                                  options.prefetch_factor, false, false);
        auto test_loader = std::make_unique<DatasetLoader>(test_config);
        validate_loader(*test_loader, "test");
        TrainingValidationRuntime test_runtime(
            options, train_runtime, std::move(test_loader), val_batch_size, false,
            detection_config.include_masks ? EvaluationMetricSet::BBoxAndMask : EvaluationMetricSet::BBox,
            artifacts.config.num_select);
        NativeRfDetrModel best_model(artifacts.config);
        best_model.to(cuda_device(options.device_id));
        best_model.load_weights(*result.best_checkpoint_path, false);
        best_model.optimize_for_inference(checked_inference_batch_size(val_batch_size), false,
                                          options.compilation_mode);
        result.test_summary =
            evaluate_model(options, test_runtime, best_model, detection_config, false, std::nullopt, "test").summary;
    }

    if (main_process) {
        MMLTK_PROFILE_SCOPE("rfdetr.train.save.results_json");
        write_json_file(
            results_path,
            json{
                {"preset_name", artifacts.config.preset_name},
                {"output_dir", options.output_dir.string()},
                {"checkpoint", checkpoint_path.string()},
                {"best_checkpoint", result.best_checkpoint_path->string()},
                {"best_is_ema", result.best_is_ema},
                {"best_regular_metric", best_regular},
                {"best_ema_metric", best_ema},
                {"last_epoch", result.last_epoch},
                {"history_size", result.history.size()},
                {"train_lanes", train_lane_count},
                {"eval_lanes", train_runtime.split().lane_threads},
                {"effective_batch_per_rank", effective_batch_per_rank(options, train_lane_count)},
                {"effective_batch_global", effective_batch_global(options, distributed, train_lane_count)},
                {"gpu_augmentation", gpu_augmentation_json(options.gpu_augmentation)},
                {"test", result.test_summary.has_value() ? eval_summary_json(*result.test_summary) : json()},
            });
    }
    distributed_barrier(distributed);
    if (distributed.enabled) {
#if defined(USE_C10D_NCCL)
        distributed.process_group.reset();
        distributed.store.reset();
#endif
    }
    if (main_process) {
        progress_writer->close();
    }

    return result;
}

void print_training_summary(const TrainOptions& options, const TrainRunResult& result) {
    if (options.distributed_worker && options.distributed_rank != 0) {
        return;
    }
    const char* source_label = options.resume_path.empty() ? "weights" : "resume";
    const auto best_path = result.best_checkpoint_path.has_value() ? result.best_checkpoint_path->string() : "";
    const auto checkpoint_path = result.checkpoint_path.string();
    const double train_loss = result.history.empty() ? 0.0 : result.history.back().train_loss;
    const auto val_loss = result.history.empty() ? std::optional<double>{} : result.history.back().val_loss;
    const double val_bbox_ap = result.history.empty() ? 0.0 : result.history.back().val_summary.bbox.ap;
    const std::string val_mask_ap =
        result.history.empty() ? "null" : formatted_mask_ap(result.history.back().val_summary);
    if (val_loss.has_value()) {
        mmltk::logging::logger("rfdetr.train")
            ->info(
                "rfdetr train[{}]: preset={} optimizer={} epochs={} train_loss={:.6f} val_loss={:.6f} "
                "val_bbox_ap={:.4f} val_mask_ap={} best={} checkpoint={}",
                source_label, result.artifacts.config.preset_name, train_optimizer_kind_name(options.optimizer),
                result.last_epoch + 1, train_loss, *val_loss, val_bbox_ap, val_mask_ap, best_path, checkpoint_path);
    } else {
        mmltk::logging::logger("rfdetr.train")
            ->info(
                "rfdetr train[{}]: preset={} optimizer={} epochs={} train_loss={:.6f} val_bbox_ap={:.4f} "
                "val_mask_ap={} best={} checkpoint={}",
                source_label, result.artifacts.config.preset_name, train_optimizer_kind_name(options.optimizer),
                result.last_epoch + 1, train_loss, val_bbox_ap, val_mask_ap, best_path, checkpoint_path);
    }
}

}  
