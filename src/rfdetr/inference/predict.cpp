#include "mmltk/rfdetr/predict.h"

#include "dataset_loader.h"
#include "mmltk/rfdetr/checkpoint.h"
#include "mmltk/rfdetr/model.h"
#include "mmltk/rfdetr/target_builder.h"
#include "mmltk_logging.h"
#include "rfdetr/inference/backend_factory.h"
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
using predict_internal::run_compiled_prediction_batch;
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
        TensorInfo{"pred_logits", {reported_batch, artifacts.config.num_queries, artifacts.config.num_classes}, "float32"},
        TensorInfo{"pred_boxes", {reported_batch, artifacts.config.num_queries, 4}, "float32"},
    };
    if (artifacts.config.segmentation) {
        info.outputs.push_back(
            TensorInfo{"pred_masks", {reported_batch, artifacts.config.num_queries, artifacts.config.resolution, artifacts.config.resolution}, "float32"});
    }
    info.num_queries = artifacts.config.num_queries;
    info.num_classes = artifacts.config.num_classes;
    info.has_masks = artifacts.config.segmentation;
    return info;
}

size_t lane_slot_count(const RuntimeSplit& split) {
    return predict_internal::prediction_lane_slot_count(split);
}

/// RAII guard that owns the per-prediction timing, grad suppression, device
/// guard, and progress bar.  The destructor closes the bar and finalizes the
/// result so every return path gets consistent cleanup.
struct PredictionScope {
    PredictionRunResult& result;
    std::unique_ptr<spdmon::ProgressBar> bar;
    bool sort_records;

    PredictionScope(PredictionRunResult& result_ref,
                    std::unique_ptr<spdmon::ProgressBar> bar_ptr,
                    int device_id,
                    bool sort = false)
        : result(result_ref),
          bar(std::move(bar_ptr)),
          sort_records(sort),
          started_at_(std::chrono::steady_clock::now()),
          no_grad_(),
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
    torch::NoGradGuard no_grad_;
    c10::cuda::CUDAGuard device_guard_;
};

template <typename Request>
void apply_compiled_evaluation_common(const EvaluateOptions& options,
                                      Request& request) {
    request.compiled_path = options.compiled_path;
    request.device_id = options.device_id;
    request.workers = options.workers;
    request.cpu_affinity = options.cpu_affinity;
    request.allow_fp16 = options.allow_fp16;
}

struct EvaluationSetup {
    ResolvedModelArtifacts artifacts;
    std::string backend_name;
    CocoDataset dataset;

    explicit EvaluationSetup(const EvaluateOptions& options)
        : artifacts(resolve_model_artifacts(options)),
          backend_name(choose_backend_name(options.backend, artifacts)),
          dataset(CocoDataset::load_from_binary(options.compiled_path)) {}
};

PredictOptions make_weights_evaluation_predict_options(const EvaluateOptions& options) {
    PredictOptions predict_options;
    apply_compiled_evaluation_common(options, predict_options);
    predict_options.weights_path = options.weights_path;
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
    validation_options.log_mode = ValidationLogMode::Interactive;
    return validation_options;
}

PredictionRunResult run_prediction_native(const PredictOptions& options,
                                          const ResolvedModelArtifacts& artifacts,
                                          size_t limit_images = 0) {
    auto setup = make_compiled_prediction_setup(options,
                                                artifacts,
                                                "weights",
                                                1,
                                                std::max<size_t>(1, options.batch_size));
    RuntimeContext& runtime = setup.runtime;
    DatasetLoader& loader = *setup.loader;
    PredictionRunResult& result = setup.result;
    const std::vector<int>& image_ids = setup.image_ids;
    auto model = load_native_model(artifacts, options.device_id);
    const size_t effective_batch_size = std::max<size_t>(1, options.batch_size);
    if (effective_batch_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("batch_size exceeds supported inference compilation range");
    }
    model->optimize_for_inference(
        static_cast<int>(effective_batch_size),
        false,
        options.compilation_mode);

    PredictionScope scope(result,
        make_prediction_bar(options.progress_bar, "weights.predict", loader, limit_images),
        options.device_id);

    const auto inference_stream =
        get_high_priority_cuda_stream(options.device_id);
    torch::Tensor mean;
    torch::Tensor std;
    WorkerPool& cpu_pool = runtime.cpu_pool();
    const RuntimeSplit& split = runtime.split();
    const auto slot_pool = std::make_shared<PredictionBufferSlotPool>(lane_slot_count(split));
    const auto image_height = static_cast<int64_t>(loader.image_height());
    const auto image_width = static_cast<int64_t>(loader.image_width());
    const std::size_t max_queries = prediction_max_queries(artifacts);
    torch::Tensor nested_mask;
    {
        auto normalization = initialize_normalization_tensors(options.device_id, inference_stream);
        mean = std::move(normalization.first);
        std = std::move(normalization.second);
        nested_mask = torch::zeros(
            {static_cast<int64_t>(std::max<size_t>(1, options.batch_size)), image_height, image_width},
            torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, options.device_id));
    }

    loader.begin_epoch();
    Batch batch{};
    size_t submitted_records = 0;
    auto generator = [&](size_t) {
        if (limit_images > 0 && submitted_records >= limit_images) {
            return false;
        }
        return loader.next_batch(batch);
    };
    auto process_fn = [&](size_t) {
        loader.wait_batch(batch);
        void* consumer_stream = reinterpret_cast<void*>(inference_stream.stream());
        loader.handoff_batch(batch, consumer_stream);
        std::vector<TensorMap> processed;
        {
            c10::cuda::CUDAStreamGuard stream_guard(inference_stream);
            auto outputs = run_compiled_prediction_batch(
                batch,
                options.device_id,
                image_height,
                image_width,
                mean,
                std,
                [&](const torch::Tensor& normalized) {
                    return to_output_tensors(model->forward(NestedTensor{
                        normalized,
                        nested_mask.narrow(0, 0, normalized.size(0)),
                    }));
                });
            loader.release_batch(batch, consumer_stream);
            processed = postprocess_outputs_fixed_size(outputs, image_height, image_width, static_cast<int64_t>(max_queries));
        }

        const size_t processed_count = std::min(processed.size(), batch.num_images);
        const size_t remaining =
            limit_images > 0 ? (limit_images - std::min(limit_images, submitted_records)) : processed_count;
        const size_t stage_count = std::min(processed_count, remaining);

        std::vector<StagedPredictionRecord> staged_records;
        staged_records.reserve(stage_count);
        for (size_t image_pos = 0; image_pos < stage_count; ++image_pos) {
            const auto dataset_index = static_cast<int64_t>(batch.image_indices[image_pos]);
            const int64_t image_id = image_id_for_dataset_index(image_ids, dataset_index);
            staged_records.push_back(make_staged_prediction_record(
                dataset_index,
                image_id,
                {},
                processed[image_pos],
                loader.num_classes(),
                options.max_dets_per_image,
                slot_pool->acquire(),
                options.device_id,
                consumer_stream));
        }
        submitted_records += stage_count;
        return staged_records;
    };
    predict_internal::run_sequential_prediction_pipeline(result, scope.bar, cpu_pool, split, options.threshold, generator, process_fn);
    return result;
}

PredictionRunResult run_prediction_native_parallel(const PredictOptions& options,
                                                   const ResolvedModelArtifacts& artifacts,
                                                   size_t limit_images = 0) {
    auto setup = make_compiled_prediction_setup(options, artifacts, "weights", options.lanes, 1);
    RuntimeContext& runtime = setup.runtime;
    DatasetLoader& loader = *setup.loader;
    PredictionRunResult& result = setup.result;
    const std::vector<int>& image_ids = setup.image_ids;

    auto model = load_native_model(artifacts, options.device_id);
    model->optimize_for_inference(1, false, options.compilation_mode);
    PredictionScope scope(result,
        make_prediction_bar(options.progress_bar, "weights.predict", loader, limit_images),
        options.device_id, /*sort=*/true);

    torch::Tensor mean;
    torch::Tensor std;
    std::tie(mean, std) = initialize_normalization_tensors(options.device_id);

    const auto image_height = static_cast<int64_t>(loader.image_height());
    const auto image_width = static_cast<int64_t>(loader.image_width());
    const RuntimeSplit& split = runtime.split();
    const std::size_t max_queries = prediction_max_queries(artifacts);
    const size_t slot_count = lane_slot_count(split);
    std::vector<predict_internal::NativePredictionLane> lanes =
        make_native_prediction_lanes(options.device_id,
                                     image_height,
                                     image_width,
                                     split.lane_threads,
                                     slot_count);

    WorkerPool lane_pool(static_cast<size_t>(lanes.size()), runtime.lane_cpus(), "rfdpredlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();

    loader.begin_epoch();
    Batch batch{};
    size_t submitted_records = 0;
    auto generator = [&](size_t) {
        if (limit_images > 0 && submitted_records >= limit_images) {
            return false;
        }
        return loader.next_batch(batch);
    };
    auto enqueue_fn = [&](size_t item_index) {
        const auto dataset_index = static_cast<int64_t>(batch.image_indices[0]);
        const int64_t image_id = image_id_for_dataset_index(image_ids, dataset_index);

        const size_t lane_index = item_index % lanes.size();
        ++submitted_records;
        return lane_pool.enqueue([&lanes,
                                  &loader,
                                  batch,
                                  model,
                                  lane_index,
                                  dataset_index,
                                  image_id,
                                  category_count = loader.num_classes(),
                                  max_dets = options.max_dets_per_image,
                                  image_height,
                                  image_width,
                                  max_queries,
                                  mean,
                                  std_t = std,
                                  device_id = options.device_id]() mutable {
            auto& lane = lanes[lane_index];
            PredictionBufferLease lease = lane.slot_pool->acquire();

            loader.wait_batch(batch);
            void* consumer_stream = reinterpret_cast<void*>(lane.stream.stream());
            loader.handoff_batch(batch, consumer_stream);

            torch::NoGradGuard lane_no_grad;
            c10::cuda::CUDAGuard lane_device_guard(checked_device_index(device_id));
            c10::cuda::CUDAStreamGuard stream_guard(lane.stream);

            auto outputs = run_compiled_prediction_batch(
                batch,
                device_id,
                image_height,
                image_width,
                mean,
                std_t,
                [&](const torch::Tensor& normalized_batch) {
                    auto outputs = to_output_tensors(model->forward(NestedTensor{
                        normalized_batch,
                        lane.nested_mask.narrow(0, 0, normalized_batch.size(0)),
                    }));
                    normalized_batch.record_stream(lane.stream);
                    return outputs;
                });
            loader.release_batch(batch, consumer_stream);
            auto outputs_postprocessed =
                postprocess_outputs_fixed_size(outputs, image_height, image_width, static_cast<int64_t>(max_queries));
            return make_staged_prediction_record(dataset_index,
                                                 image_id,
                                                 {},
                                                 outputs_postprocessed.front(),
                                                 category_count,
                                                 max_dets,
                                                 std::move(lease),
                                                 device_id,
                                                 reinterpret_cast<void*>(lane.stream.stream()));
        });
    };
    predict_internal::run_parallel_prediction_pipeline(result, scope.bar, cpu_pool, split, options.threshold, generator, enqueue_fn);
    return result;
}

PredictionRunResult run_prediction_weights(const PredictOptions& options,
                                           const ResolvedModelArtifacts& artifacts,
                                           size_t limit_images = 0) {
    if (options.lanes > 1) {
        if (options.batch_size != 1) {
            throw std::runtime_error(
                "RF-DETR weights overlap requires --batch-size 1 when --lanes > 1");
        }
        return run_prediction_native_parallel(options, artifacts, limit_images);
    }
    return run_prediction_native(options, artifacts, limit_images);
}

PredictionRunResult run_prediction_sequential(const PredictOptions& options,
                                              const ResolvedModelArtifacts& artifacts,
                                              const std::string& backend_name) {
    auto setup = make_compiled_prediction_setup(options,
                                                artifacts,
                                                backend_name,
                                                effective_lane_count(options.batch_size, options.lanes),
                                                1);
    RuntimeContext& runtime = setup.runtime;
    DatasetLoader& loader = *setup.loader;
    PredictionRunResult& result = setup.result;
    const std::vector<int>& image_ids = setup.image_ids;

    auto backend = make_backend(artifacts, backend_name, options.device_id, options.allow_fp16);
    PredictionScope scope(result,
        make_prediction_bar(options.progress_bar, backend_name + ".predict", loader),
        options.device_id);

    const predict_internal::BackendExecutionSpec backend_execution =
        predict_internal::describe_backend_execution(*backend, artifacts, options.device_id);
    torch::Tensor mean;
    torch::Tensor std;
    {
        auto normalization = initialize_normalization_tensors(options.device_id, backend_execution.stream);
        mean = std::move(normalization.first);
        std = std::move(normalization.second);
    }

    loader.begin_epoch();
    Batch batch{};
    auto generator = [&](size_t) {
        return loader.next_batch(batch);
    };
    auto process_fn = [&](size_t) {
        loader.wait_batch(batch);
        const auto dataset_index = static_cast<int64_t>(batch.image_indices[0]);
        const int64_t image_id = image_id_for_dataset_index(image_ids, dataset_index);
        void* consumer_stream = reinterpret_cast<void*>(backend_execution.stream.stream());
        loader.handoff_batch(batch, consumer_stream);
        c10::cuda::CUDAStreamGuard stream_guard(backend_execution.stream);
        auto outputs = run_compiled_prediction_batch(
            batch,
            options.device_id,
            static_cast<int64_t>(loader.image_height()),
            static_cast<int64_t>(loader.image_width()),
            mean,
            std,
            [&](const torch::Tensor& normalized_batch) { return backend->run(normalized_batch); });
        loader.release_batch(batch, consumer_stream);
        auto processed = postprocess_outputs_fixed_size(
            outputs,
            static_cast<int64_t>(loader.image_height()),
            static_cast<int64_t>(loader.image_width()),
            backend_execution.num_queries);
            
        std::vector<StagedPredictionRecord> staged_records;
        staged_records.push_back(make_staged_prediction_record(
            dataset_index,
            image_id,
            {},
            processed.front(),
            backend_execution.category_count,
            options.max_dets_per_image,
            PredictionBufferLease{},
            options.device_id,
            nullptr));
        return staged_records;
    };
    WorkerPool& cpu_pool = runtime.cpu_pool();
    predict_internal::run_sequential_prediction_pipeline(result, scope.bar, cpu_pool, runtime.split(), options.threshold, generator, process_fn);
    return result;
}

PredictionRunResult run_prediction_parallel(const PredictOptions& options,
                                            const ResolvedModelArtifacts& artifacts,
                                            const std::string& backend_name) {
    auto setup = make_compiled_prediction_setup(options,
                                                artifacts,
                                                backend_name,
                                                effective_lane_count(options.batch_size, options.lanes),
                                                1);
    RuntimeContext& runtime = setup.runtime;
    DatasetLoader& loader = *setup.loader;
    PredictionRunResult& result = setup.result;
    const std::vector<int>& image_ids = setup.image_ids;

    PredictionScope scope(result,
        make_prediction_bar(options.progress_bar, backend_name + ".predict", loader),
        options.device_id, /*sort=*/true);

    torch::Tensor mean;
    torch::Tensor std;
    std::tie(mean, std) = initialize_normalization_tensors(options.device_id);

    const RuntimeSplit& split = runtime.split();
    const size_t slot_count = lane_slot_count(split);
    std::vector<predict_internal::BackendInferenceLane> lanes =
        make_backend_prediction_lanes(artifacts,
                                      backend_name,
                                      options.device_id,
                                      options.allow_fp16,
                                      split.lane_threads,
                                      slot_count);

    WorkerPool lane_pool(static_cast<size_t>(lanes.size()), runtime.lane_cpus(), "rfdpredlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();

    loader.begin_epoch();
    Batch batch{};
    const auto image_height = static_cast<int64_t>(loader.image_height());
    const auto image_width = static_cast<int64_t>(loader.image_width());
    auto generator = [&](size_t) {
        return loader.next_batch(batch);
    };
    auto enqueue_fn = [&](size_t item_index) {
        const auto dataset_index = static_cast<int64_t>(batch.image_indices[0]);
        const int64_t image_id = image_id_for_dataset_index(image_ids, dataset_index);

        const size_t lane_index = item_index % lanes.size();
        return lane_pool.enqueue([&lanes,
                                  &loader,
                                  batch,
                                  lane_index,
                                  dataset_index,
                                  image_id,
                                  max_dets = options.max_dets_per_image,
                                  image_height,
                                  image_width,
                                  mean,
                                  std_t = std,
                                  device_id = options.device_id]() mutable {
            auto& lane = lanes[lane_index];
            PredictionBufferLease lease = lane.slot_pool->acquire();

            loader.wait_batch(batch);
            void* consumer_stream = reinterpret_cast<void*>(lane.stream.stream());
            loader.handoff_batch(batch, consumer_stream);

            torch::NoGradGuard lane_no_grad;
            c10::cuda::CUDAGuard lane_device_guard(checked_device_index(device_id));
            c10::cuda::CUDAStreamGuard stream_guard(lane.stream);
            auto outputs = run_compiled_prediction_batch(
                batch,
                device_id,
                image_height,
                image_width,
                mean,
                std_t,
                [&](const torch::Tensor& normalized_batch) {
                    auto outputs = lane.backend->run(normalized_batch);
                    normalized_batch.record_stream(lane.stream);
                    return outputs;
                });
            loader.release_batch(batch, consumer_stream);
            auto outputs_postprocessed =
                postprocess_outputs_fixed_size(outputs, image_height, image_width, lane.num_queries);
            return make_staged_prediction_record(dataset_index,
                                                 image_id,
                                                 {},
                                                 outputs_postprocessed.front(),
                                                 lane.category_count,
                                                 max_dets,
                                                 std::move(lease),
                                                 device_id,
                                                 reinterpret_cast<void*>(lane.stream.stream()));
        });
    };
    predict_internal::run_parallel_prediction_pipeline(result, scope.bar, cpu_pool, split, options.threshold, generator, enqueue_fn);
    return result;
}

PredictionRunResult run_prediction_raw_weights(const PredictOptions& options,
                                               const ResolvedModelArtifacts& artifacts) {
    if (options.lanes > 1) {
        throw std::runtime_error(
            "RF-DETR raw-image weights predict does not support --lanes > 1");
    }

    PredictionRunResult result = make_prediction_result(artifacts, "weights");
    auto model = load_native_model(artifacts, options.device_id);
    const size_t effective_batch_size = std::max<size_t>(1, options.batch_size);
    if (effective_batch_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("batch_size exceeds supported inference compilation range");
    }
    model->optimize_for_inference(
        static_cast<int>(effective_batch_size),
        false,
        options.compilation_mode);

    PredictionScope scope(result,
        make_prediction_bar(options.progress_bar, "weights.predict", options.image_inputs.size()),
        options.device_id);

    const auto inference_stream =
        get_high_priority_cuda_stream(options.device_id);
    RawPredictionRuntimeSetup setup =
        make_raw_prediction_runtime_setup(options, 1, options.device_id, inference_stream);
    RuntimeContext& runtime = setup.runtime;
    torch::Tensor mean = std::move(setup.mean);
    torch::Tensor std = std::move(setup.std);
    RawImageBatchWorkspace workspace = make_raw_image_batch_workspace(
        effective_batch_size, artifacts.config.resolution, options.device_id);
    const std::size_t max_queries = prediction_max_queries(artifacts);

    WorkerPool& cpu_pool = runtime.cpu_pool();
    auto infer_batch = [&](const torch::Tensor& batch_cpu_view,
                           const torch::Tensor& batch_gpu_view,
                           const std::vector<RawImageBatchItem>& items) {
        c10::cuda::CUDAStreamGuard stream_guard(inference_stream);
        return run_raw_prediction_batch(
            batch_cpu_view,
            batch_gpu_view,
            items,
            workspace,
            mean,
            std,
            max_queries,
            [&](const torch::Tensor& normalized) {
                return to_output_tensors(model->forward(NestedTensor{
                    normalized,
                    workspace.nested_mask.narrow(0, 0, normalized.size(0)),
                }));
            });
    };
    run_raw_prediction_batches(options,
                               artifacts,
                               cpu_pool,
                               options.image_inputs,
                               static_cast<size_t>(artifacts.config.num_classes),
                               workspace.batch_cpu,
                               workspace.batch_gpu,
                               workspace.resize_scratch_slots,
                               infer_batch,
                               result.records,
                               scope.bar);
    return result;
}

PredictionRunResult run_prediction_raw_sequential(const PredictOptions& options,
                                                  const ResolvedModelArtifacts& artifacts,
                                                  const std::string& backend_name) {
    PredictionRunResult result = make_prediction_result(artifacts, backend_name);

    auto backend = make_backend(artifacts, backend_name, options.device_id, options.allow_fp16);
    PredictionScope scope(result,
        make_prediction_bar(options.progress_bar, backend_name + ".predict", options.image_inputs.size()),
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
    auto infer_batch = [&](const torch::Tensor& batch_cpu_view,
                           const torch::Tensor& batch_gpu_view,
                           const std::vector<RawImageBatchItem>& items) {
        c10::cuda::CUDAStreamGuard stream_guard(backend_execution.stream);
        return run_raw_prediction_batch(
            batch_cpu_view,
            batch_gpu_view,
            items,
            workspace,
            mean,
            std,
            max_queries,
            [&](const torch::Tensor& normalized) { return backend->run(normalized); });
    };
    run_raw_prediction_batches(options,
                               artifacts,
                               cpu_pool,
                               options.image_inputs,
                               backend_execution.category_count,
                               workspace.batch_cpu,
                               workspace.batch_gpu,
                               workspace.resize_scratch_slots,
                               infer_batch,
                               result.records,
                               scope.bar);
    return result;
}

PredictionRunResult run_prediction_raw_parallel(const PredictOptions& options,
                                                const ResolvedModelArtifacts& artifacts,
                                                const std::string& backend_name) {
    PredictionRunResult result = make_prediction_result(artifacts, backend_name);

    PredictionScope scope(result,
        make_prediction_bar(options.progress_bar, backend_name + ".predict", options.image_inputs.size()),
        options.device_id, /*sort=*/true);

    RawPredictionRuntimeSetup setup = make_raw_prediction_runtime_setup(
        options,
        effective_lane_count(options.batch_size, options.lanes),
        options.device_id);
    RuntimeContext& runtime = setup.runtime;
    torch::Tensor mean = std::move(setup.mean);
    torch::Tensor std = std::move(setup.std);

    const RuntimeSplit& split = runtime.split();
    const size_t slot_count = lane_slot_count(split);
    std::vector<predict_internal::RawImagePredictionLane> lanes =
        make_raw_prediction_lanes(artifacts,
                                  backend_name,
                                  options.device_id,
                                  options.allow_fp16,
                                  split.lane_threads,
                                  slot_count,
                                  artifacts.config.resolution);

    WorkerPool lane_pool(static_cast<size_t>(lanes.size()), runtime.lane_cpus(), "rfdpredlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();

    auto generator = [&](size_t item_index) {
        return item_index < options.image_inputs.size();
    };
    auto enqueue_fn = [&](size_t item_index) {
        PredictImageInput input = options.image_inputs[item_index];
        const auto dataset_index = static_cast<int64_t>(item_index);
        const auto image_id = input.image_id != 0 ? input.image_id : static_cast<int64_t>(item_index + 1);

        const size_t lane_index = item_index % lanes.size();
        return lane_pool.enqueue([&lanes,
                                  input = std::move(input),
                                  lane_index,
                                  dataset_index,
                                  image_id,
                                  max_dets = options.max_dets_per_image,
                                  mean,
                                  std_t = std,
                                  device_id = options.device_id]() mutable {
            auto& lane = lanes[lane_index];
            PredictionBufferLease lease = lane.slot_pool->acquire();
            RawImageBatchItem item = decode_raw_image_into_tensor(
                input,
                dataset_index,
                image_id,
                static_cast<int>(lane.batch_cpu.size(2)),
                lane.batch_cpu[0].data_ptr<float>(),
                lane.resize_scratch);

            torch::NoGradGuard lane_no_grad;
            c10::cuda::CUDAGuard lane_device_guard(checked_device_index(device_id));
            c10::cuda::CUDAStreamGuard stream_guard(lane.stream);
            auto outputs = run_raw_prediction_batch(
                lane.batch_cpu.narrow(0, 0, 1),
                lane.batch_gpu.narrow(0, 0, 1),
                item,
                lane,
                mean,
                std_t,
                lane.num_queries,
                [&](const torch::Tensor& normalized) { return lane.backend->run(normalized); });
            return make_staged_prediction_record(item.dataset_index,
                                                 item.image_id,
                                                 item.source_name,
                                                 outputs.front(),
                                                 lane.category_count,
                                                 max_dets,
                                                 std::move(lease),
                                                 device_id,
                                                 reinterpret_cast<void*>(lane.stream.stream()));
        });
    };
    predict_internal::run_parallel_prediction_pipeline(result, scope.bar, cpu_pool, split, options.threshold, generator, enqueue_fn);
    return result;
}

} // namespace

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

    const ResolvedModelArtifacts artifacts = resolve_model_artifacts(options);
    const std::string backend_name = choose_backend_name(options.backend, artifacts);
    if (options.source_kind == PredictSourceKind::ImageFiles) {
        if (backend_name == "weights") {
            return run_prediction_raw_weights(options, artifacts);
        }
        const int lane_count = effective_lane_count(options.batch_size, options.lanes);
        if (lane_count > 1) {
            return run_prediction_raw_parallel(options, artifacts, backend_name);
        }
        return run_prediction_raw_sequential(options, artifacts, backend_name);
    }

    if (backend_name == "weights") {
        return run_prediction_weights(options, artifacts);
    }
    const int lane_count = effective_lane_count(options.batch_size, options.lanes);
    if (lane_count > 1) {
        return run_prediction_parallel(options, artifacts, backend_name);
    }
    return run_prediction_sequential(options, artifacts, backend_name);
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

    EvaluationSetup setup(options);
    EvaluationRunResult out;
    out.artifacts = setup.artifacts;
    out.backend_name = setup.backend_name;
    CocoDataset& dataset = setup.dataset;
    if (options.limit_images > 0) {
        dataset.limit_images(options.limit_images);
    }
    out.image_count = dataset.num_images();
    out.category_count = dataset.num_categories();

    if (out.backend_name == "weights") {
        const PredictOptions predict_options = make_weights_evaluation_predict_options(options);
        print_model_metadata(
            native_model_info(out.artifacts, options.batch_size),
            dataset.num_images(),
            dataset.num_categories(),
            ValidationLogMode::Interactive);
        auto predictions = run_prediction_weights(predict_options, out.artifacts, options.limit_images);
        for (const auto& record : predictions.records) {
            dataset.add_predictions(record.detections);
        }
        out.result.model_info = native_model_info(out.artifacts, options.batch_size);
        out.result.summary = dataset.evaluate(options.eval_max_dets);
        out.result.timing = predictions.timing;
        return out;
    }

    auto backend = make_backend(out.artifacts, out.backend_name, options.device_id, options.allow_fp16);
    const ValidationOptions validation_options = make_backend_evaluation_options(options);

    print_model_metadata(
        backend->info(),
        dataset.num_images(),
        dataset.num_categories(),
        validation_options.log_mode);
    out.result = run_validation_backend(validation_options, dataset, *backend);
    return out;
}

void print_evaluation_summary(const EvaluateOptions& options, const EvaluationRunResult& result) {
    mmltk::logging::logger("rfdetr.evaluate")->info("{}", predict_internal::format_evaluation_summary(options, result));
}

} // namespace mmltk::rfdetr
