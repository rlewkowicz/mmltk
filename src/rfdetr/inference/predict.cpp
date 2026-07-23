#include "mmltk/rfdetr/predict.h"

#include "common_utils.h"
#include "dataset_loader.h"
#include "mmltk/rfdetr/checkpoint.h"
#include "mmltk/rfdetr/model.h"
#include "mmltk/rfdetr/target_builder.h"
#include "mmltk/rfdetr/training_ops.h"
#include "mmltk_logging.h"
#include "rfdetr/inference/backend_factory.h"
#include "rfdetr/weights_validation.h"
#include "rfdetr/compiled_input_resolution.h"
#include "rfdetr/backends.h"
#include "rfdetr/torch_cuda_utils.h"
#include "rfdetr/postprocess.h"
#include "rfdetr/predict_runtime_internal.h"
#include "rfdetr/inference/predict_workflow_helpers.h"
#include "spdmon/spdmon.hpp"
#include "rfdetr/runtime.h"
#include "rfdetr/validate_internal.h"
#include "worker_pool.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <c10/core/InferenceMode.h>
#include <cuda_runtime.h>
#include <torch/torch.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

using json = nlohmann::json;
using namespace validate_detail;

namespace {

using predict_internal::drain_prediction_records;
using predict_internal::decode_raw_image_into_tensor;
using predict_internal::effective_lane_count;
using predict_internal::flush_prediction_work;
using predict_internal::enqueue_prediction_record;
using predict_internal::make_compiled_prediction_setup;
using predict_internal::make_compiled_prediction_record;
using predict_internal::make_raw_prediction_lanes;
using predict_internal::make_raw_prediction_runtime_setup;
using predict_internal::make_staged_prediction_batch;
using predict_internal::make_staged_prediction_record;
using predict_internal::maybe_drain_prediction_work;
using predict_internal::finalize_prediction_result;
using predict_internal::initialize_normalization_tensors;
using predict_internal::load_native_model;
using predict_internal::make_backend_prediction_lanes;
using predict_internal::make_raw_image_batch_workspace;
using predict_internal::make_native_prediction_lanes;
using predict_internal::make_prediction_result;
using predict_internal::make_prediction_bar;
using predict_internal::prediction_record_to_json;
using predict_internal::prediction_max_queries;
using predict_internal::run_raw_prediction_batch;
using predict_internal::run_raw_prediction_batches;
using predict_internal::to_output_tensors;
using predict_internal::RawImageBatchWorkspace;
using predict_internal::RawImageBatchItem;
using predict_internal::RawPredictionRuntimeSetup;
using predict_internal::StagedPredictionRecord;

const char* predict_source_kind_name(PredictSourceKind kind) {
    switch (kind) {
        case PredictSourceKind::CompiledDataset:
            return "compiled_dataset";
        case PredictSourceKind::ImageFiles:
            return "image_files";
    }
    return "unknown";
}

ModelInfo native_model_info(const ResolvedModelArtifacts& artifacts, size_t batch_size = 1) {
    const auto reported_batch = static_cast<int64_t>(std::max<size_t>(1, batch_size));
    ModelInfo info;
    info.backend = "weights";
    info.model_path = artifacts.input_path.string();
    info.input = TensorInfo{
        "images",
        {reported_batch, 3, artifacts.config.resolution, artifacts.config.resolution},
        "float32",
    };
    info.outputs = {
        TensorInfo{
            "pred_logits", {reported_batch, artifacts.config.num_queries, artifacts.config.num_classes}, "float32"},
        TensorInfo{"pred_boxes", {reported_batch, artifacts.config.num_queries, 4}, "float32"},
    };
    if (artifacts.config.segmentation) {
        info.outputs.push_back(TensorInfo{
            "pred_masks",
            {reported_batch, artifacts.config.num_queries, artifacts.config.resolution, artifacts.config.resolution},
            "float32"});
    }
    info.num_queries = artifacts.config.num_queries;
    info.num_classes = artifacts.config.num_classes;
    info.has_masks = artifacts.config.segmentation;
    return info;
}

size_t lane_slot_count(const RuntimeSplit& split, size_t batch_size = 1) {
    return predict_internal::prediction_lane_slot_count(split, batch_size);
}

struct PredictionScope {
    PredictionRunResult& result;
    std::unique_ptr<spdmon::ProgressBar> bar;
    bool sort_records;

    PredictionScope(PredictionRunResult& result_ref, std::unique_ptr<spdmon::ProgressBar> bar_ptr, int device_id,
                    bool sort = false)
        : result(result_ref),
          bar(std::move(bar_ptr)),
          sort_records(sort),
          started_at_(std::chrono::steady_clock::now()),
          inference_mode_(),
          device_guard_(checked_device_index(device_id)) {}

    ~PredictionScope() {
        if (bar) {
            bar->close();
        }
        finalize_prediction_result(result, started_at_, sort_records);
    }

    PredictionScope(const PredictionScope&) = delete;
    PredictionScope& operator=(const PredictionScope&) = delete;

   private:
    std::chrono::steady_clock::time_point started_at_;
    c10::InferenceMode inference_mode_;
    c10::cuda::CUDAGuard device_guard_;
};

auto make_compiled_prediction_setup_for_backend_path(const PredictOptions& options,
                                                     const ResolvedModelArtifacts& artifacts,
                                                     const std::string& backend_name) {
    return make_compiled_prediction_setup(options, artifacts, backend_name,
                                          effective_lane_count(options.batch_size, options.lanes), 1);
}

struct CompiledPredictionSetupRefs {
    RuntimeContext& runtime;
    DatasetLoader& loader;
    PredictionRunResult& result;
    const std::vector<int>& image_ids;
};

template <typename SetupT>
CompiledPredictionSetupRefs bind_compiled_prediction_setup(SetupT& setup) {
    return {setup.runtime, *setup.loader, setup.result, setup.image_ids};
}

template <typename LoaderT>
auto make_prediction_batch_generator(LoaderT& loader, Batch& batch) {
    return [&loader, &batch](size_t) { return loader.next_batch(batch); };
}

template <typename LoaderT>
auto make_limited_prediction_batch_generator(LoaderT& loader, Batch& batch, const size_t limit_images,
                                             const size_t& submitted_records) {
    return [&loader, &batch, limit_images, &submitted_records](size_t) {
        if (limit_images > 0 && submitted_records >= limit_images) {
            return false;
        }
        return loader.next_batch(batch);
    };
}

template <typename ModelT>
void optimize_native_prediction_model(ModelT& model, const PredictOptions& options) {
    const size_t effective_batch_size = std::max<size_t>(1, options.batch_size);
    if (effective_batch_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("batch_size exceeds supported inference compilation range");
    }
    model.optimize_for_inference(static_cast<int>(effective_batch_size), false, options.compilation_mode);
}

struct CompiledPredictionBatch {
    OutputTensors outputs;
    std::vector<PredictionBatchMetadata> metadata;
    void* consumer_stream = nullptr;
};

OutputTensors narrow_prediction_outputs(OutputTensors outputs, const size_t active_count) {
    const int64_t count = checked_cast<int64_t>(active_count, "active prediction batch size overflow");
    if (!outputs.pred_logits.defined() || outputs.pred_logits.dim() == 0 || !outputs.pred_boxes.defined() ||
        outputs.pred_boxes.dim() == 0 || outputs.pred_logits.size(0) < count || outputs.pred_boxes.size(0) < count ||
        (outputs.pred_masks &&
         (!outputs.pred_masks->defined() || outputs.pred_masks->dim() == 0 || outputs.pred_masks->size(0) < count))) {
        throw std::runtime_error("prediction output batch is smaller than its active image count");
    }
    if (outputs.pred_logits.size(0) == count && outputs.pred_boxes.size(0) == count &&
        (!outputs.pred_masks || outputs.pred_masks->size(0) == count)) {
        return outputs;
    }
    outputs.pred_logits = outputs.pred_logits.narrow(0, 0, count);
    outputs.pred_boxes = outputs.pred_boxes.narrow(0, 0, count);
    if (outputs.pred_masks) {
        *outputs.pred_masks = outputs.pred_masks->narrow(0, 0, count);
    }
    return outputs;
}

template <typename StreamT, typename RunNormalizedBatchFn>
auto run_compiled_prediction_on_stream(Batch& batch, DatasetLoader& loader, const StreamT& stream, int device_id,
                                       const std::vector<int>& image_ids, GpuBatchPreprocessor& preprocessor,
                                       RunNormalizedBatchFn&& run_normalized_batch,
                                       const int64_t output_batch_size = 0) {
    loader.wait_batch(batch);
    void* consumer_stream = reinterpret_cast<void*>(stream.stream());
    c10::cuda::CUDAStreamGuard stream_guard(stream);
    LoaderBatchGuard batch_guard(loader, batch, device_id);
    const torch::Tensor normalized = preprocessor.run(batch, output_batch_size);
    std::vector<PredictionBatchMetadata> metadata;
    metadata.reserve(batch.num_images);
    for (size_t image_index = 0; image_index < batch.num_images; ++image_index) {
        const auto dataset_index = static_cast<int64_t>(batch.image_indices[image_index]);
        metadata.push_back(PredictionBatchMetadata{
            dataset_index,
            image_id_for_dataset_index(image_ids, dataset_index),
            {},
        });
    }
    batch_guard.release();
    auto outputs = run_normalized_batch(normalized);
    preprocessor.record_consumer(stream.stream());
    return CompiledPredictionBatch{std::move(outputs), std::move(metadata), consumer_stream};
}

template <typename LaneT, typename RunNormalizedBatchFn>
StagedPredictionRecord run_compiled_prediction_lane_batch(LaneT& lane, Batch& batch, DatasetLoader& loader,
                                                          const std::vector<int>& image_ids, int64_t image_height,
                                                          int64_t image_width, std::size_t max_queries,
                                                          std::size_t category_count, std::size_t max_dets,
                                                          int device_id, RunNormalizedBatchFn&& run_normalized_batch) {
    PredictionBufferLease lease = lane.slot_pool->acquire();

    c10::InferenceMode lane_inference_mode;
    c10::cuda::CUDAGuard lane_device_guard(checked_device_index(device_id));
    TORCH_CHECK(lane.preprocessor != nullptr, "compiled prediction lane preprocessing is not initialized");
    CompiledPredictionBatch compiled =
        run_compiled_prediction_on_stream(batch, loader, lane.stream, device_id, image_ids, *lane.preprocessor,
                                          [&](const torch::Tensor& normalized_batch) {
                                              auto lane_outputs = run_normalized_batch(normalized_batch);
                                              normalized_batch.record_stream(lane.stream);
                                              return lane_outputs;
                                          });

    c10::cuda::CUDAStreamGuard stream_guard(lane.stream);
    auto outputs_postprocessed = postprocess_output_batch_fixed_size(
        narrow_prediction_outputs(std::move(compiled.outputs), compiled.metadata.size()), image_height, image_width,
        static_cast<int64_t>(max_queries));
    return make_staged_prediction_batch(std::move(compiled.metadata), std::move(outputs_postprocessed), category_count,
                                        max_dets, std::move(lease), device_id, compiled.consumer_stream);
}

template <typename LaneCollection, typename MaxQueriesFn, typename CategoryCountFn, typename RunNormalizedBatchFn>
auto enqueue_compiled_prediction_lane_batch(WorkerPool& lane_pool, LaneCollection& lanes, DatasetLoader& loader,
                                            const std::vector<int>& image_ids, Batch batch, size_t item_index,
                                            size_t max_dets, int64_t image_height, int64_t image_width, int device_id,
                                            MaxQueriesFn&& max_queries_for_lane,
                                            CategoryCountFn&& category_count_for_lane,
                                            RunNormalizedBatchFn&& run_normalized_batch) {
    const size_t lane_index = item_index % lanes.size();
    return lane_pool.enqueue(
        [&lanes, &loader, &image_ids, batch, lane_index, max_dets, image_height, image_width, device_id,
         max_queries_for_lane = std::forward<MaxQueriesFn>(max_queries_for_lane),
         category_count_for_lane = std::forward<CategoryCountFn>(category_count_for_lane),
         run_normalized_batch = std::forward<RunNormalizedBatchFn>(run_normalized_batch)]() mutable {
            auto& lane = lanes[lane_index];
            return run_compiled_prediction_lane_batch(
                lane, batch, loader, image_ids, image_height, image_width, max_queries_for_lane(lane),
                category_count_for_lane(lane), max_dets, device_id,
                [&](const torch::Tensor& normalized_batch) { return run_normalized_batch(lane, normalized_batch); });
        });
}

template <typename LaneCollection, typename MaxQueriesFn, typename CategoryCountFn, typename RunNormalizedBatchFn>
void run_compiled_prediction_parallel_with_lanes(PredictionRunResult& result, std::unique_ptr<spdmon::ProgressBar>& bar,
                                                 WorkerPool& cpu_pool, const RuntimeSplit& split, WorkerPool& lane_pool,
                                                 LaneCollection& lanes, DatasetLoader& loader,
                                                 const std::vector<int>& image_ids, const size_t max_dets,
                                                 const int64_t image_height, const int64_t image_width,
                                                 const int device_id, const float threshold, const size_t limit_images,
                                                 CocoDataset* evaluation_dataset, MaxQueriesFn&& max_queries_for_lane,
                                                 CategoryCountFn&& category_count_for_lane,
                                                 RunNormalizedBatchFn&& run_normalized_batch) {
    loader.begin_epoch();
    Batch batch{};
    size_t submitted_records = 0;
    auto max_queries_for_lane_fn = std::forward<MaxQueriesFn>(max_queries_for_lane);
    auto category_count_for_lane_fn = std::forward<CategoryCountFn>(category_count_for_lane);
    auto run_normalized_batch_fn = std::forward<RunNormalizedBatchFn>(run_normalized_batch);
    auto generator = make_limited_prediction_batch_generator(loader, batch, limit_images, submitted_records);
    auto enqueue_fn = [&](size_t item_index) {
        if (limit_images > 0) {
            ++submitted_records;
        }
        return enqueue_compiled_prediction_lane_batch(lane_pool, lanes, loader, image_ids, batch, item_index, max_dets,
                                                      image_height, image_width, device_id, max_queries_for_lane_fn,
                                                      category_count_for_lane_fn, run_normalized_batch_fn);
    };
    predict_internal::run_parallel_prediction_pipeline(result, bar, cpu_pool, split, threshold, generator, enqueue_fn,
                                                       evaluation_dataset);
}

template <typename MakeLanesFn, typename MaxQueriesFn, typename CategoryCountFn, typename RunNormalizedBatchFn>
void run_compiled_prediction_parallel(const PredictOptions& options, RuntimeContext& runtime, DatasetLoader& loader,
                                      PredictionRunResult& result, std::unique_ptr<spdmon::ProgressBar>& bar,
                                      const std::vector<int>& image_ids, const int64_t image_height,
                                      const int64_t image_width, const at::ScalarType preprocessing_dtype,
                                      const size_t limit_images, CocoDataset* evaluation_dataset,
                                      MakeLanesFn&& make_lanes, MaxQueriesFn&& max_queries_for_lane,
                                      CategoryCountFn&& category_count_for_lane,
                                      RunNormalizedBatchFn&& run_normalized_batch) {
    const RuntimeSplit& split = runtime.split();
    const size_t slot_count = lane_slot_count(split, options.batch_size);
    auto lanes = make_lanes(split, slot_count);
    for (auto& lane : lanes) {
        c10::cuda::CUDAStreamGuard stream_guard(lane.stream);
        lane.preprocessor = std::make_unique<GpuBatchPreprocessor>(
            1, static_cast<int>(image_height), static_cast<int>(image_width), options.device_id, preprocessing_dtype);
    }
    WorkerPool lane_pool(static_cast<size_t>(lanes.size()), runtime.lane_cpus(), "rfdpredlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();

    run_compiled_prediction_parallel_with_lanes(
        result, bar, cpu_pool, split, lane_pool, lanes, loader, image_ids, options.max_dets_per_image, image_height,
        image_width, options.device_id, options.threshold, limit_images, evaluation_dataset,
        std::forward<MaxQueriesFn>(max_queries_for_lane), std::forward<CategoryCountFn>(category_count_for_lane),
        std::forward<RunNormalizedBatchFn>(run_normalized_batch));
}

template <typename MakeLanesFn, typename MaxQueriesFn, typename CategoryCountFn, typename RunNormalizedBatchFn>
void run_compiled_prediction_parallel_for_loader(
    const PredictOptions& options, RuntimeContext& runtime, DatasetLoader& loader, PredictionRunResult& result,
    std::unique_ptr<spdmon::ProgressBar>& bar, const std::vector<int>& image_ids,
    const at::ScalarType preprocessing_dtype, const size_t limit_images, CocoDataset* evaluation_dataset,
    MakeLanesFn&& make_lanes, MaxQueriesFn&& max_queries_for_lane, CategoryCountFn&& category_count_for_lane,
    RunNormalizedBatchFn&& run_normalized_batch) {
    run_compiled_prediction_parallel(
        options, runtime, loader, result, bar, image_ids, static_cast<int64_t>(loader.image_height()),
        static_cast<int64_t>(loader.image_width()), preprocessing_dtype, limit_images, evaluation_dataset,
        std::forward<MakeLanesFn>(make_lanes), std::forward<MaxQueriesFn>(max_queries_for_lane),
        std::forward<CategoryCountFn>(category_count_for_lane),
        std::forward<RunNormalizedBatchFn>(run_normalized_batch));
}

template <typename StreamT, typename ItemsT, typename WorkspaceT, typename RunModelFn>
std::vector<TensorMap> run_raw_prediction_batch_on_stream(const StreamT& stream, const torch::Tensor& batch_cpu_view,
                                                          const torch::Tensor& batch_gpu_view, const ItemsT& items,
                                                          WorkspaceT& workspace, const torch::Tensor& mean,
                                                          const torch::Tensor& std_t, const std::size_t max_queries,
                                                          RunModelFn&& run_model) {
    c10::cuda::CUDAStreamGuard stream_guard(stream);
    return run_raw_prediction_batch(batch_cpu_view, batch_gpu_view, items, workspace, mean, std_t, max_queries,
                                    std::forward<RunModelFn>(run_model));
}

template <typename InferBatchFn>
void run_raw_prediction_batches_with_workspace(const PredictOptions& options, const ResolvedModelArtifacts& artifacts,
                                               WorkerPool& cpu_pool, const std::size_t category_count,
                                               RawImageBatchWorkspace& workspace, InferBatchFn&& infer_batch,
                                               PredictionRunResult& result, std::unique_ptr<spdmon::ProgressBar>& bar) {
    run_raw_prediction_batches(options, artifacts, cpu_pool, options.image_inputs, category_count, workspace.batch_cpu,
                               workspace.batch_gpu, workspace.resize_scratch_slots,
                               std::forward<InferBatchFn>(infer_batch), result.records, bar);
}

template <typename Request>
void apply_compiled_evaluation_common(const EvaluateOptions& options, Request& request) {
    request.compiled_path = options.compiled_path;
    request.device_id = options.device_id;
    request.workers = options.workers;
    request.cpu_affinity = options.cpu_affinity;
    request.allow_fp16 = options.allow_fp16;
}

struct EvaluationSetup {
    ResolvedModelArtifacts artifacts;
    std::string backend_name;

    explicit EvaluationSetup(const EvaluateOptions& options)
        : artifacts(resolve_model_artifacts(options)), backend_name(choose_backend_name(options.backend, artifacts)) {}
};

PredictOptions make_weights_evaluation_predict_options(const EvaluateOptions& options) {
    PredictOptions predict_options;
    apply_compiled_evaluation_common(options, predict_options);
    predict_options.weights_path = options.weights_path;
    predict_options.preset_name = options.preset_name;
    predict_options.resolution = options.resolution;
    predict_options.output_path.clear();
    predict_options.batch_size = options.batch_size;
    predict_options.max_dets_per_image = options.eval_max_dets;
    predict_options.lanes = options.lanes;
    predict_options.threshold = 0.0f;
    predict_options.progress_bar = options.progress_bar;
    predict_options.compilation_mode = options.compilation_mode;
    return predict_options;
}

ValidationOptions make_backend_evaluation_options(const EvaluateOptions& options) {
    ValidationOptions validation_options;
    apply_compiled_evaluation_common(options, validation_options);
    validation_options.batch_size = static_cast<size_t>(effective_lane_count(options.batch_size, options.lanes));
    validation_options.eval_max_dets = options.eval_max_dets;
    validation_options.onnx_path = options.onnx_path;
    validation_options.tensorrt_path = options.tensorrt_path;
    validation_options.resolution = static_cast<std::uint32_t>(options.resolution);
    validation_options.log_mode = ValidationLogMode::Interactive;
    return validation_options;
}

PredictionRunResult run_prediction_native(const PredictOptions& options, const ResolvedModelArtifacts& artifacts,
                                          size_t limit_images = 0,
                                          EvaluationProfileRecord* evaluation_profile = nullptr,
                                          CocoDataset* evaluation_dataset = nullptr,
                                          EvalSummary* evaluation_summary = nullptr) {
    if ((evaluation_dataset == nullptr) != (evaluation_summary == nullptr)) {
        throw std::invalid_argument("weights prediction evaluation requires both dataset and summary outputs");
    }
    auto setup =
        make_compiled_prediction_setup(options, artifacts, "weights", 1, std::max<size_t>(1, options.batch_size));
    auto [runtime, loader, result, image_ids] = bind_compiled_prediction_setup(setup);
    auto model = load_native_model(artifacts, options.device_id);
    optimize_native_prediction_model(*model, options);
    model->eval();

    PredictionScope scope(result, make_prediction_bar(options.progress_bar, "weights.predict", loader, limit_images),
                          options.device_id);

    const auto inference_stream = get_high_priority_cuda_stream(options.device_id);
    const bool amp_enabled = options.allow_fp16;
    const at::ScalarType autocast_dtype = amp_enabled ? resolve_cuda_autocast_dtype(options.device_id) : at::kFloat;
    WorkerPool& cpu_pool = runtime.cpu_pool();
    const RuntimeSplit& split = runtime.split();
    const size_t max_cpu_futures = predict_internal::prediction_cpu_batch_limit(split, options.batch_size);
    std::unique_ptr<EvaluationCudaTimingPool> cuda_timing_pool;
    if (evaluation_profile != nullptr) {
        cuda_timing_pool = std::make_unique<EvaluationCudaTimingPool>(max_cpu_futures);
    }
    const auto image_height = static_cast<int64_t>(loader.image_height());
    const auto image_width = static_cast<int64_t>(loader.image_width());
    const std::size_t max_queries = prediction_max_queries(artifacts);
    const auto slot_pool = std::make_shared<PredictionBufferSlotPool>(
        lane_slot_count(split, options.batch_size),
        PredictionBufferConfig{
            static_cast<int64_t>(std::max<size_t>(1, options.batch_size)),
            static_cast<int64_t>(max_queries),
            artifacts.config.segmentation
                ? std::make_optional(std::make_pair(loader.image_height(), loader.image_width()))
                : std::nullopt,
            options.device_id,
        });
    GpuBatchPreprocessor preprocessor(static_cast<int64_t>(std::max<size_t>(1, options.batch_size)),
                                      static_cast<int>(image_height), static_cast<int>(image_width), options.device_id,
                                      autocast_dtype);
    torch::Tensor nested_mask;
    {
        c10::cuda::CUDAStreamGuard stream_guard(inference_stream);
        nested_mask =
            torch::zeros({static_cast<int64_t>(std::max<size_t>(1, options.batch_size)), image_height, image_width},
                         torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, options.device_id));
    }
    if (evaluation_profile != nullptr) {
        evaluation_profile->expected_precision = evaluation_precision_name(autocast_dtype);
        capture_sdp_backend_flags(*evaluation_profile);
    }

    loader.begin_epoch();
    Batch batch{};
    size_t submitted_records = 0;
    auto generator = make_limited_prediction_batch_generator(loader, batch, limit_images, submitted_records);
    auto process_fn = [&](size_t) {
        void* consumer_stream = nullptr;
        PostprocessedBatch processed;
        std::vector<PredictionBatchMetadata> metadata;
        EvaluationCudaTimingLease batch_timing;
        const auto trim_metadata_to_limit = [&] {
            if (limit_images == 0) {
                return;
            }
            const size_t remaining = limit_images - std::min(limit_images, submitted_records);
            metadata.resize(std::min(metadata.size(), remaining));
        };
        if (evaluation_profile == nullptr) {
            auto compiled = run_compiled_prediction_on_stream(
                batch, loader, inference_stream, options.device_id, image_ids, preprocessor,
                [&](const torch::Tensor& normalized) {
                    OutputTensors outputs;
                    {
                        AutocastGuard autocast_guard(amp_enabled, autocast_dtype);
                        outputs = to_output_tensors(model->forward(NestedTensor{
                            normalized,
                            nested_mask.narrow(0, 0, normalized.size(0)),
                        }));
                    }
                    assert_inference_output_dtype(outputs.pred_logits, outputs.pred_boxes, autocast_dtype,
                                                  "RF-DETR standalone weights validation");
                    return outputs;
                },
                static_cast<int64_t>(std::max<size_t>(1, options.batch_size)));
            consumer_stream = compiled.consumer_stream;
            metadata = std::move(compiled.metadata);
            trim_metadata_to_limit();
            c10::cuda::CUDAStreamGuard stream_guard(inference_stream);
            processed = postprocess_output_batch_fixed_size(
                narrow_prediction_outputs(std::move(compiled.outputs), metadata.size()), image_height, image_width,
                static_cast<int64_t>(max_queries));
        } else {
            const auto wait_started = std::chrono::steady_clock::now();
            loader.wait_batch(batch);
            evaluation_profile->loader_wait_seconds +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - wait_started).count();
            consumer_stream = reinterpret_cast<void*>(inference_stream.stream());
            c10::cuda::CUDAStreamGuard stream_guard(inference_stream);
            LoaderBatchGuard batch_guard(loader, batch, options.device_id);
            const auto cuda_stream = reinterpret_cast<cudaStream_t>(consumer_stream);
            batch_timing = cuda_timing_pool->acquire();
            batch_timing.timing->record_start(EvaluationCudaBatchTiming::Phase::Preprocessing, cuda_stream);
            const torch::Tensor normalized =
                preprocessor.run(batch, static_cast<int64_t>(std::max<size_t>(1, options.batch_size)));
            batch_timing.timing->record_stop(EvaluationCudaBatchTiming::Phase::Preprocessing, cuda_stream);
            metadata.reserve(batch.num_images);
            for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
                const auto dataset_index = static_cast<int64_t>(batch.image_indices[image_pos]);
                metadata.push_back(PredictionBatchMetadata{
                    dataset_index,
                    image_id_for_dataset_index(image_ids, dataset_index),
                    {},
                });
            }
            trim_metadata_to_limit();
            batch_guard.release();
            batch_timing.timing->record_start(EvaluationCudaBatchTiming::Phase::ModelForward, cuda_stream);
            OutputTensors outputs;
            {
                AutocastGuard autocast_guard(amp_enabled, autocast_dtype);
                outputs = to_output_tensors(model->forward(NestedTensor{
                    normalized,
                    nested_mask.narrow(0, 0, normalized.size(0)),
                }));
            }
            assert_inference_output_dtype(outputs.pred_logits, outputs.pred_boxes, autocast_dtype,
                                          "RF-DETR standalone weights validation");
            preprocessor.record_consumer(inference_stream.stream());
            batch_timing.timing->record_stop(EvaluationCudaBatchTiming::Phase::ModelForward, cuda_stream);
            evaluation_profile->precision = evaluation_precision_name(outputs.pred_logits.scalar_type());
            evaluation_profile->box_precision = evaluation_precision_name(outputs.pred_boxes.scalar_type());
            evaluation_profile->query_count = static_cast<size_t>(outputs.pred_logits.size(1));
            evaluation_profile->class_count = static_cast<size_t>(outputs.pred_logits.size(2));
            batch_timing.timing->record_start(EvaluationCudaBatchTiming::Phase::Postprocess, cuda_stream);
            processed =
                postprocess_output_batch_fixed_size(narrow_prediction_outputs(std::move(outputs), metadata.size()),
                                                    image_height, image_width, static_cast<int64_t>(max_queries));
            batch_timing.timing->record_stop(EvaluationCudaBatchTiming::Phase::Postprocess, cuda_stream);
        }

        const size_t processed_count =
            std::min(static_cast<size_t>(processed.size()), static_cast<size_t>(batch.num_images));
        const size_t stage_count = std::min(processed_count, metadata.size());
        metadata.resize(stage_count);
        submitted_records += stage_count;
        if (evaluation_profile != nullptr) {
            const size_t prediction_count = stage_count * static_cast<size_t>(processed.scores.size(1));
            const size_t bbox_bytes = prediction_count * (static_cast<size_t>(processed.scores.element_size()) +
                                                          static_cast<size_t>(processed.labels.element_size()) +
                                                          4U * static_cast<size_t>(processed.boxes.element_size()));
            size_t mask_bytes = 0;
            if (processed.masks.has_value()) {
                const size_t pixels_per_mask =
                    static_cast<size_t>(processed.masks->size(2)) * static_cast<size_t>(processed.masks->size(3));
                mask_bytes = prediction_count * ((pixels_per_mask + 7U) / 8U);
            }
            evaluation_profile->transferred_bytes += bbox_bytes + mask_bytes;
            evaluation_profile->mask_transferred_bytes += mask_bytes;
        }
        std::vector<StagedPredictionRecord> staged_records;
        staged_records.reserve(1);
        staged_records.push_back(make_staged_prediction_batch(
            std::move(metadata), std::move(processed), loader.num_classes(), options.max_dets_per_image,
            slot_pool->acquire(), options.device_id, consumer_stream));
        if (evaluation_profile != nullptr) {
            staged_records.back().evaluation_profile = evaluation_profile;
            staged_records.back().timing_pool = cuda_timing_pool.get();
            staged_records.back().timing = batch_timing;
        }
        return staged_records;
    };
    predict_internal::run_sequential_prediction_pipeline(result, scope.bar, cpu_pool, max_cpu_futures,
                                                         options.threshold, generator, process_fn, evaluation_dataset);
    if (evaluation_dataset != nullptr) {
        *evaluation_summary = evaluation_dataset->evaluate(options.max_dets_per_image, evaluation_profile, &cpu_pool);
    }
    return result;
}

PredictionRunResult run_prediction_native_parallel(const PredictOptions& options,
                                                   const ResolvedModelArtifacts& artifacts, size_t limit_images = 0,
                                                   CocoDataset* evaluation_dataset = nullptr,
                                                   EvalSummary* evaluation_summary = nullptr) {
    if ((evaluation_dataset == nullptr) != (evaluation_summary == nullptr)) {
        throw std::invalid_argument("parallel weights prediction evaluation requires both dataset and summary outputs");
    }
    auto setup = make_compiled_prediction_setup(options, artifacts, "weights", options.lanes, 1);
    auto [runtime, loader, result, image_ids] = bind_compiled_prediction_setup(setup);

    auto model = load_native_model(artifacts, options.device_id);
    optimize_native_prediction_model(*model, options);
    model->eval();
    PredictionScope scope(result, make_prediction_bar(options.progress_bar, "weights.predict", loader, limit_images),
                          options.device_id, true);

    const bool amp_enabled = options.allow_fp16;
    const at::ScalarType autocast_dtype = amp_enabled ? resolve_cuda_autocast_dtype(options.device_id) : at::kFloat;

    const std::size_t max_queries = prediction_max_queries(artifacts);
    run_compiled_prediction_parallel_for_loader(
        options, runtime, loader, result, scope.bar, image_ids, autocast_dtype, limit_images, evaluation_dataset,
        [&](const RuntimeSplit& split, const size_t slot_count) {
            return make_native_prediction_lanes(options.device_id, static_cast<int64_t>(loader.image_height()),
                                                static_cast<int64_t>(loader.image_width()), split.lane_threads,
                                                slot_count, static_cast<int64_t>(max_queries),
                                                artifacts.config.segmentation);
        },
        [max_queries](const auto&) { return max_queries; },
        [&loader](const auto&) { return static_cast<std::size_t>(loader.num_classes()); },
        [model, amp_enabled, autocast_dtype](auto& lane, const torch::Tensor& normalized_batch) {
            OutputTensors outputs;
            {
                AutocastGuard autocast_guard(amp_enabled, autocast_dtype);
                outputs = to_output_tensors(model->forward(NestedTensor{
                    normalized_batch,
                    lane.nested_mask.narrow(0, 0, normalized_batch.size(0)),
                }));
            }
            assert_inference_output_dtype(outputs.pred_logits, outputs.pred_boxes, autocast_dtype,
                                          "RF-DETR parallel weights inference");
            return outputs;
        });
    if (evaluation_dataset != nullptr) {
        *evaluation_summary = evaluation_dataset->evaluate(options.max_dets_per_image, nullptr, &runtime.cpu_pool());
    }
    return result;
}

PredictionRunResult run_prediction_weights(const PredictOptions& options, const ResolvedModelArtifacts& artifacts,
                                           size_t limit_images = 0,
                                           EvaluationProfileRecord* evaluation_profile = nullptr,
                                           CocoDataset* evaluation_dataset = nullptr,
                                           EvalSummary* evaluation_summary = nullptr) {
    if (options.lanes > 1) {
        if (options.batch_size != 1) {
            throw std::runtime_error("RF-DETR weights overlap requires --batch-size 1 when --lanes > 1");
        }
        return run_prediction_native_parallel(options, artifacts, limit_images, evaluation_dataset, evaluation_summary);
    }
    return run_prediction_native(options, artifacts, limit_images, evaluation_profile, evaluation_dataset,
                                 evaluation_summary);
}

ValidationBackendResult run_weights_validation_backend_impl(const ValidationOptions& options,
                                                            const CocoDataset& source_dataset) {
    EvaluateOptions evaluate;
    evaluate.weights_path = options.weights_path;
    evaluate.preset_name = options.preset_name;
    evaluate.resolution = static_cast<int>(options.resolution);
    evaluate.compiled_path = options.compiled_path;
    evaluate.batch_size = options.batch_size;
    evaluate.limit_images = options.limit_images;
    evaluate.eval_max_dets = options.eval_max_dets;
    evaluate.device_id = options.device_id;
    evaluate.workers = options.workers;
    evaluate.cpu_affinity = options.cpu_affinity;
    evaluate.allow_fp16 = options.allow_fp16;
    evaluate.progress_bar = interactive_logs(options);

    const ResolvedModelArtifacts artifacts = resolve_model_artifacts(evaluate);
    const bool expects_masks = source_dataset.metric_set() == EvaluationMetricSet::BBoxAndMask;
    if (artifacts.config.segmentation != expects_masks) {
        throw std::runtime_error("weights backend mask outputs do not match the dataset evaluation mode");
    }
    const ModelInfo model_info = native_model_info(artifacts, options.batch_size);
    auto evaluation_profile = make_standalone_evaluation_profile(options, model_info, source_dataset);
    if (evaluation_profile) {
        evaluation_profile->started_at = std::chrono::steady_clock::now();
    }
    PredictOptions predict_options = make_weights_evaluation_predict_options(evaluate);
    CocoDataset dataset = source_dataset;
    EvalSummary evaluation_summary;
    PredictionRunResult predictions = run_prediction_weights(predict_options, artifacts, options.limit_images,
                                                             evaluation_profile.get(), &dataset, &evaluation_summary);

    ValidationBackendResult result;
    result.model_info = model_info;
    result.summary = evaluation_summary;
    result.timing = predictions.timing;
    return result;
}

PredictionRunResult run_prediction_sequential(const PredictOptions& options, const ResolvedModelArtifacts& artifacts,
                                              const std::string& backend_name) {
    auto setup = make_compiled_prediction_setup_for_backend_path(options, artifacts, backend_name);
    auto [runtime, loader, result, image_ids] = bind_compiled_prediction_setup(setup);

    auto backend = make_backend(artifacts, backend_name, options.device_id, options.allow_fp16);
    PredictionScope scope(result, make_prediction_bar(options.progress_bar, backend_name + ".predict", loader),
                          options.device_id);

    const predict_internal::BackendExecutionSpec backend_execution =
        predict_internal::describe_backend_execution(*backend, artifacts, options.device_id);
    constexpr size_t kSequentialBatchSize = 1;
    GpuBatchPreprocessor preprocessor(static_cast<int64_t>(kSequentialBatchSize),
                                      static_cast<int>(loader.image_height()), static_cast<int>(loader.image_width()),
                                      options.device_id, at::kFloat);

    loader.begin_epoch();
    Batch batch{};
    const auto slot_pool = std::make_shared<PredictionBufferSlotPool>(
        lane_slot_count(runtime.split(), kSequentialBatchSize),
        PredictionBufferConfig{
            kSequentialBatchSize,
            backend_execution.num_queries,
            artifacts.config.segmentation
                ? std::make_optional(std::make_pair(loader.image_height(), loader.image_width()))
                : std::nullopt,
            options.device_id,
        });
    auto generator = make_prediction_batch_generator(loader, batch);
    auto process_fn = [&](size_t) {
        auto compiled = run_compiled_prediction_on_stream(
            batch, loader, backend_execution.stream, options.device_id, image_ids, preprocessor,
            [&](const torch::Tensor& normalized_batch) { return backend->run(normalized_batch); });
        c10::cuda::CUDAStreamGuard stream_guard(backend_execution.stream);
        auto processed = postprocess_output_batch_fixed_size(
            narrow_prediction_outputs(std::move(compiled.outputs), compiled.metadata.size()),
            static_cast<int64_t>(loader.image_height()), static_cast<int64_t>(loader.image_width()),
            backend_execution.num_queries);
        std::vector<StagedPredictionRecord> staged_records;
        staged_records.reserve(1);
        staged_records.push_back(make_staged_prediction_batch(
            std::move(compiled.metadata), std::move(processed), backend_execution.category_count,
            options.max_dets_per_image, slot_pool->acquire(), options.device_id, compiled.consumer_stream));
        return staged_records;
    };
    WorkerPool& cpu_pool = runtime.cpu_pool();
    const size_t max_cpu_futures = predict_internal::prediction_cpu_batch_limit(runtime.split(), kSequentialBatchSize);
    predict_internal::run_sequential_prediction_pipeline(result, scope.bar, cpu_pool, max_cpu_futures,
                                                         options.threshold, generator, process_fn, nullptr);
    return result;
}

PredictionRunResult run_prediction_parallel(const PredictOptions& options, const ResolvedModelArtifacts& artifacts,
                                            const std::string& backend_name) {
    auto setup = make_compiled_prediction_setup_for_backend_path(options, artifacts, backend_name);
    auto [runtime, loader, result, image_ids] = bind_compiled_prediction_setup(setup);

    PredictionScope scope(result, make_prediction_bar(options.progress_bar, backend_name + ".predict", loader),
                          options.device_id, true);

    run_compiled_prediction_parallel_for_loader(
        options, runtime, loader, result, scope.bar, image_ids, at::kFloat, 0, nullptr,
        [&](const RuntimeSplit& split, const size_t slot_count) {
            return make_backend_prediction_lanes(artifacts, backend_name, options.device_id, options.allow_fp16,
                                                 split.lane_threads, slot_count);
        },
        [](const auto& lane) { return lane.num_queries; }, [](const auto& lane) { return lane.category_count; },
        [](auto& lane, const torch::Tensor& normalized_batch) { return lane.backend->run(normalized_batch); });
    return result;
}

PredictionRunResult run_prediction_raw_weights(const PredictOptions& options, const ResolvedModelArtifacts& artifacts) {
    if (options.lanes > 1) {
        throw std::runtime_error("RF-DETR raw-image weights predict does not support --lanes > 1");
    }

    PredictionRunResult result = make_prediction_result(artifacts, "weights");
    auto model = load_native_model(artifacts, options.device_id);
    optimize_native_prediction_model(*model, options);
    model->eval();

    PredictionScope scope(result,
                          make_prediction_bar(options.progress_bar, "weights.predict", options.image_inputs.size()),
                          options.device_id);

    const auto inference_stream = get_high_priority_cuda_stream(options.device_id);
    RawPredictionRuntimeSetup setup =
        make_raw_prediction_runtime_setup(options, 1, options.device_id, inference_stream);
    RuntimeContext& runtime = setup.runtime;
    torch::Tensor mean = std::move(setup.mean);
    torch::Tensor std = std::move(setup.std);
    RawImageBatchWorkspace workspace = make_raw_image_batch_workspace(std::max<size_t>(1, options.batch_size),
                                                                      artifacts.config.resolution, options.device_id);
    const std::size_t max_queries = prediction_max_queries(artifacts);

    WorkerPool& cpu_pool = runtime.cpu_pool();
    auto infer_batch = [&](const torch::Tensor& batch_cpu_view, const torch::Tensor& batch_gpu_view,
                           const std::vector<RawImageBatchItem>& items) {
        return run_raw_prediction_batch_on_stream(inference_stream, batch_cpu_view, batch_gpu_view, items, workspace,
                                                  mean, std, max_queries, [&](const torch::Tensor& normalized) {
                                                      return to_output_tensors(model->forward(NestedTensor{
                                                          normalized,
                                                          workspace.nested_mask.narrow(0, 0, normalized.size(0)),
                                                      }));
                                                  });
    };
    run_raw_prediction_batches_with_workspace(options, artifacts, cpu_pool,
                                              static_cast<size_t>(artifacts.config.num_classes), workspace, infer_batch,
                                              result, scope.bar);
    return result;
}

PredictionRunResult run_prediction_raw_sequential(const PredictOptions& options,
                                                  const ResolvedModelArtifacts& artifacts,
                                                  const std::string& backend_name) {
    PredictionRunResult result = make_prediction_result(artifacts, backend_name);

    auto backend = make_backend(artifacts, backend_name, options.device_id, options.allow_fp16);
    PredictionScope scope(
        result, make_prediction_bar(options.progress_bar, backend_name + ".predict", options.image_inputs.size()),
        options.device_id);

    const predict_internal::BackendExecutionSpec backend_execution =
        predict_internal::describe_backend_execution(*backend, artifacts, options.device_id);
    RawPredictionRuntimeSetup setup =
        make_raw_prediction_runtime_setup(options, 1, options.device_id, backend_execution.stream);
    RuntimeContext& runtime = setup.runtime;
    torch::Tensor mean = std::move(setup.mean);
    torch::Tensor std = std::move(setup.std);
    RawImageBatchWorkspace workspace =
        make_raw_image_batch_workspace(1, artifacts.config.resolution, options.device_id);
    const std::size_t max_queries = backend_execution.num_queries;

    WorkerPool& cpu_pool = runtime.cpu_pool();
    auto infer_batch = [&](const torch::Tensor& batch_cpu_view, const torch::Tensor& batch_gpu_view,
                           const std::vector<RawImageBatchItem>& items) {
        return run_raw_prediction_batch_on_stream(
            backend_execution.stream, batch_cpu_view, batch_gpu_view, items, workspace, mean, std, max_queries,
            [&](const torch::Tensor& normalized) { return backend->run(normalized); });
    };
    run_raw_prediction_batches_with_workspace(options, artifacts, cpu_pool, backend_execution.category_count, workspace,
                                              infer_batch, result, scope.bar);
    return result;
}

PredictionRunResult run_prediction_raw_parallel(const PredictOptions& options, const ResolvedModelArtifacts& artifacts,
                                                const std::string& backend_name) {
    PredictionRunResult result = make_prediction_result(artifacts, backend_name);

    PredictionScope scope(
        result, make_prediction_bar(options.progress_bar, backend_name + ".predict", options.image_inputs.size()),
        options.device_id, true);

    RawPredictionRuntimeSetup setup = make_raw_prediction_runtime_setup(
        options, effective_lane_count(options.batch_size, options.lanes), options.device_id);
    RuntimeContext& runtime = setup.runtime;
    torch::Tensor mean = std::move(setup.mean);
    torch::Tensor std = std::move(setup.std);

    const RuntimeSplit& split = runtime.split();
    const size_t slot_count = lane_slot_count(split);
    std::vector<predict_internal::RawImagePredictionLane> lanes =
        make_raw_prediction_lanes(artifacts, backend_name, options.device_id, options.allow_fp16, split.lane_threads,
                                  slot_count, artifacts.config.resolution);

    WorkerPool lane_pool(static_cast<size_t>(lanes.size()), runtime.lane_cpus(), "rfdpredlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();

    auto generator = [&](size_t item_index) { return item_index < options.image_inputs.size(); };
    auto enqueue_fn = [&](size_t item_index) {
        PredictImageInput input = options.image_inputs[item_index];
        const auto dataset_index = static_cast<int64_t>(item_index);
        const auto image_id = input.image_id != 0 ? input.image_id : static_cast<int64_t>(item_index + 1);

        const size_t lane_index = item_index % lanes.size();
        return lane_pool.enqueue([&lanes, input = std::move(input), lane_index, dataset_index, image_id,
                                  max_dets = options.max_dets_per_image, mean, std_t = std,
                                  device_id = options.device_id]() mutable {
            auto& lane = lanes[lane_index];
            PredictionBufferLease lease = lane.slot_pool->acquire();
            RawImageBatchItem item =
                decode_raw_image_into_tensor(input, dataset_index, image_id, static_cast<int>(lane.batch_cpu.size(2)),
                                             lane.batch_cpu[0].data_ptr<float>(), lane.resize_scratch);

            c10::InferenceMode lane_inference_mode;
            c10::cuda::CUDAGuard lane_device_guard(checked_device_index(device_id));
            c10::cuda::CUDAStreamGuard stream_guard(lane.stream);
            auto outputs = run_raw_prediction_batch(
                lane.batch_cpu.narrow(0, 0, 1), lane.batch_gpu.narrow(0, 0, 1), item, lane, mean, std_t,
                lane.num_queries, [&](const torch::Tensor& normalized) { return lane.backend->run(normalized); });
            return make_staged_prediction_record(item.dataset_index, item.image_id, item.source_name, outputs.front(),
                                                 lane.category_count, max_dets, std::move(lease), device_id,
                                                 reinterpret_cast<void*>(lane.stream.stream()));
        });
    };
    predict_internal::run_parallel_prediction_pipeline(result, scope.bar, cpu_pool, split, options.threshold, generator,
                                                       enqueue_fn);
    return result;
}

}  

ModelInfo inspect_weights_model_info(const ValidationOptions& options) {
    ModelArtifactRequest request;
    request.weights_path = options.weights_path;
    request.preset_name = options.preset_name;
    request.resolution = static_cast<int>(options.resolution);
    return native_model_info(resolve_model_artifacts(request), options.batch_size);
}

ValidationBackendResult run_weights_validation_backend(const ValidationOptions& options,
                                                       const CocoDataset& source_dataset) {
    return run_weights_validation_backend_impl(options, source_dataset);
}

PredictionRunResult run_prediction(const PredictOptions& options) {
    if (options.output_path.empty()) {
        throw std::runtime_error("RF-DETR predict requires --output");
    }
    if (options.batch_size == 0) {
        throw std::runtime_error("RF-DETR predict requires positive --batch-size");
    }
    if (options.source_kind == PredictSourceKind::CompiledDataset) {
        if (options.compiled_path.empty()) {
            throw std::runtime_error("RF-DETR compiled predict requires --compiled");
        }
        if (!options.image_inputs.empty()) {
            throw std::runtime_error("RF-DETR compiled predict does not accept image_inputs");
        }
    } else if (options.source_kind == PredictSourceKind::ImageFiles) {
        if (!options.compiled_path.empty()) {
            throw std::runtime_error("RF-DETR image-file predict does not accept --compiled");
        }
        if (options.image_inputs.empty()) {
            throw std::runtime_error("RF-DETR image-file predict requires image_inputs");
        }
    } else {
        throw std::runtime_error("unsupported RF-DETR predict source kind");
    }

    PredictOptions effective_options = options;
    if (effective_options.source_kind == PredictSourceKind::CompiledDataset) {
        effective_options.resolution = static_cast<int>(compiled_input_resolution(effective_options.compiled_path));
    }
    const ResolvedModelArtifacts artifacts = resolve_model_artifacts(effective_options);
    const std::string backend_name = choose_backend_name(effective_options.backend, artifacts);
    if (effective_options.source_kind == PredictSourceKind::ImageFiles) {
        if (backend_name == "weights") {
            return run_prediction_raw_weights(effective_options, artifacts);
        }
        const int lane_count = effective_lane_count(effective_options.batch_size, effective_options.lanes);
        if (lane_count > 1) {
            return run_prediction_raw_parallel(effective_options, artifacts, backend_name);
        }
        return run_prediction_raw_sequential(effective_options, artifacts, backend_name);
    }

    if (backend_name == "weights") {
        return run_prediction_weights(effective_options, artifacts);
    }
    const int lane_count = effective_lane_count(effective_options.batch_size, effective_options.lanes);
    if (lane_count > 1) {
        return run_prediction_parallel(effective_options, artifacts, backend_name);
    }
    return run_prediction_sequential(effective_options, artifacts, backend_name);
}

void write_prediction_json(const PredictOptions& options, const PredictionRunResult& result) {
    json records = json::array();
    for (const auto& record : result.records) {
        records.push_back(prediction_record_to_json(record, result.class_names));
    }

    json payload = {
        {"source_kind", predict_source_kind_name(options.source_kind)},
        {"model_kind", result.artifacts.input_kind},
        {"model_path", result.artifacts.input_path.string()},
        {"preset_name", result.artifacts.config.preset_name},
        {"backend", result.backend_name},
        {"mask_rle_encoding", "row_major_start_length"},
        {"records", std::move(records)},
    };
    if (options.source_kind == PredictSourceKind::CompiledDataset) {
        payload["compiled_path"] = options.compiled_path.string();
    } else {
        payload["input_image_count"] = options.image_inputs.size();
    }
    if (!result.artifacts.weights_path.empty()) {
        payload["weights_path"] = result.artifacts.weights_path.string();
    }
    if (!result.artifacts.onnx_path.empty()) {
        payload["onnx_path"] = result.artifacts.onnx_path.string();
    }
    if (!result.artifacts.tensorrt_path.empty()) {
        payload["tensorrt_path"] = result.artifacts.tensorrt_path.string();
    }

    std::ofstream stream(options.output_path);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to open RF-DETR prediction output: " + options.output_path.string());
    }
    stream << payload.dump(2) << '\n';
}

void print_prediction_summary(const PredictOptions& options, const PredictionRunResult& result) {
    mmltk::logging::logger("rfdetr.predict")->info("{}", predict_internal::format_prediction_summary(options, result));
}

EvaluationRunResult run_evaluation(const EvaluateOptions& options) {
    if (options.compiled_path.empty()) {
        throw std::runtime_error("RF-DETR evaluate requires --compiled");
    }
    if (options.batch_size == 0) {
        throw std::runtime_error("RF-DETR evaluate requires positive --batch-size");
    }

    EvaluateOptions effective_options = options;
    effective_options.resolution = static_cast<int>(compiled_input_resolution(effective_options.compiled_path));
    EvaluationSetup setup(effective_options);
    EvaluationRunResult out;
    out.artifacts = setup.artifacts;
    out.backend_name = setup.backend_name;
    std::unique_ptr<InferenceBackend> backend;
    ModelInfo model_info;
    if (out.backend_name == "weights") {
        model_info = native_model_info(out.artifacts, options.batch_size);
    } else {
        backend =
            make_backend(out.artifacts, out.backend_name, effective_options.device_id, effective_options.allow_fp16);
        model_info = backend->info();
    }
    CocoDataset dataset = CocoDataset::load_from_binary(
        options.compiled_path, model_info.has_masks ? EvaluationMetricSet::BBoxAndMask : EvaluationMetricSet::BBox);
    if (options.limit_images > 0) {
        dataset.limit_images(options.limit_images);
    }
    out.image_count = dataset.num_images();
    out.category_count = dataset.num_categories();

    if (out.backend_name == "weights") {
        const PredictOptions predict_options = make_weights_evaluation_predict_options(effective_options);
        print_model_metadata(model_info, dataset.num_images(), dataset.num_categories(),
                             ValidationLogMode::Interactive);
        EvalSummary evaluation_summary;
        auto predictions = run_prediction_weights(predict_options, out.artifacts, effective_options.limit_images,
                                                  nullptr, &dataset, &evaluation_summary);
        out.result.model_info = model_info;
        out.result.summary = evaluation_summary;
        out.result.timing = predictions.timing;
        return out;
    }

    const ValidationOptions validation_options = make_backend_evaluation_options(effective_options);

    print_model_metadata(model_info, dataset.num_images(), dataset.num_categories(), validation_options.log_mode);
    out.result = run_validation_backend(validation_options, dataset, *backend);
    return out;
}

void print_evaluation_summary(const EvaluateOptions& options, const EvaluationRunResult& result) {
    mmltk::logging::logger("rfdetr.evaluate")->info("{}", predict_internal::format_evaluation_summary(options, result));
}

}  
