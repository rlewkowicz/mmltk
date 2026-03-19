#include "rfdetr/predict.h"

#include "dataset_loader.h"
#include "fastloader/rfdetr/checkpoint.h"
#include "fastloader/rfdetr/target_builder.h"
#include "rfdetr/backends.h"
#include "rfdetr/cuda_utils.h"
#include "rfdetr/postprocess.h"
#include "rfdetr/progress_bar.h"
#include "rfdetr/runtime.h"
#include "rfdetr/validate_internal.h"
#include "worker_pool.h"

#include "rfdetr/common/dataset_utils.h"
#include "rfdetr/common/tensor_utils.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>
#include <nlohmann/json.hpp>
#include <torch/torch.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fastloader::rfdetr {

using json = nlohmann::json;
using namespace validate_detail;

namespace {

constexpr size_t kDefaultPrefetchFactor = 2;

struct StagedPredictionRecord {
    int64_t dataset_index = 0;
    int64_t image_id = 0;
    StagedPredictionBatch staged;
};

int effective_lane_count(size_t batch_size, int lanes) {
    return std::max(1, lanes > 0 ? lanes : static_cast<int>(std::max<size_t>(1, batch_size)));
}

const std::filesystem::path& model_input_path(const ResolvedModelArtifacts& artifacts) {
    if (!artifacts.input_path.empty()) {
        return artifacts.input_path;
    }
    return artifacts.weights_path;
}

std::string choose_backend_name(const std::string& requested_backend,
                                const ResolvedModelArtifacts& artifacts) {
    if (!artifacts.weights_path.empty()) {
        if (requested_backend != "auto") {
            throw std::runtime_error("--backend is only valid with explicit --onnx or --tensorrt inputs");
        }
        return "weights";
    }
    if (requested_backend == "auto") {
        if (!artifacts.tensorrt_path.empty()) {
            return "tensorrt";
        }
        if (!artifacts.onnx_path.empty()) {
            return "onnx";
        }
    } else if (requested_backend == "tensorrt") {
        if (!artifacts.tensorrt_path.empty() || !artifacts.onnx_path.empty()) {
            return requested_backend;
        }
    } else if (requested_backend == "onnx") {
        if (!artifacts.onnx_path.empty()) {
            return requested_backend;
        }
    } else {
        throw std::runtime_error("unsupported RF-DETR backend: " + requested_backend);
    }

    throw std::runtime_error(
        "RF-DETR backend " + requested_backend + " is unavailable for " + model_input_path(artifacts).string());
}

std::filesystem::path backend_model_path(const ResolvedModelArtifacts& artifacts,
                                         const std::string& backend_name) {
    if (backend_name == "onnx") {
        if (artifacts.onnx_path.empty()) {
            throw std::runtime_error("RF-DETR ONNX artifact is unavailable for " + model_input_path(artifacts).string());
        }
        return artifacts.onnx_path;
    }
    if (backend_name == "tensorrt") {
        if (!artifacts.tensorrt_path.empty()) {
            return artifacts.tensorrt_path;
        }
        if (!artifacts.onnx_path.empty()) {
            return artifacts.onnx_path;
        }
        throw std::runtime_error(
            "RF-DETR TensorRT requires an engine or ONNX artifact for " + model_input_path(artifacts).string());
    }
    throw std::runtime_error("unsupported RF-DETR backend: " + backend_name);
}

std::unique_ptr<InferenceBackend> make_backend(const ResolvedModelArtifacts& artifacts,
                                               const std::string& backend_name,
                                               int device_id,
                                               bool allow_fp16) {
    const auto model_path = backend_model_path(artifacts, backend_name);
    if (backend_name == "onnx") {
        return make_onnx_backend(model_path, device_id);
    }
    if (backend_name == "tensorrt") {
        return make_tensorrt_backend(model_path, device_id, allow_fp16);
    }
    throw std::runtime_error("unsupported RF-DETR backend: " + backend_name);
}

std::vector<std::unique_ptr<InferenceBackend>> make_backend_lanes(
    const ResolvedModelArtifacts& artifacts,
    const std::string& backend_name,
    int device_id,
    bool allow_fp16,
    int lane_count) {
    const auto model_path = backend_model_path(artifacts, backend_name);
    if (backend_name == "onnx") {
        return make_onnx_backend_lanes(model_path, device_id, lane_count);
    }
    if (backend_name == "tensorrt") {
        return make_tensorrt_backend_lanes(model_path, device_id, allow_fp16, lane_count);
    }
    throw std::runtime_error("unsupported RF-DETR backend: " + backend_name);
}

std::vector<int> load_image_ids(const std::filesystem::path& compiled_path) {
    return CocoDataset::load_from_binary(compiled_path).image_ids();
}

std::shared_ptr<NativeRfDetrModel> load_native_model(const ResolvedModelArtifacts& artifacts, int device_id) {
    auto model = std::make_shared<NativeRfDetrModel>(artifacts.config);
    const StateDictLoadSummary load_summary = model->load_weights(artifacts.weights_path, false);
    std::fprintf(stderr,
                 "rfdetr weights: loaded=%zu missing=%zu unexpected=%zu incompatible=%zu input=%s\n",
                 load_summary.loaded_names.size(),
                 load_summary.missing_names.size(),
                 load_summary.unexpected_names.size(),
                 load_summary.incompatible_names.size(),
                 artifacts.weights_path.c_str());
    model->eval();
    model->to(cuda_device(device_id));
    return model;
}

ModelInfo native_model_info(const ResolvedModelArtifacts& artifacts, size_t batch_size = 1) {
    const int64_t reported_batch = static_cast<int64_t>(std::max<size_t>(1, batch_size));
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

OutputTensors to_output_tensors(const ModelOutputs& outputs) {
    return OutputTensors{
        outputs.main.pred_logits,
        outputs.main.pred_boxes,
        outputs.main.pred_masks,
    };
}

size_t lane_slot_count(const RuntimeSplit& split) {
    const size_t lane_threads = static_cast<size_t>(std::max(1, split.lane_threads));
    const size_t cpu_threads = static_cast<size_t>(std::max(1, split.cpu_threads));
    return std::max<size_t>(2, 1 + ((cpu_threads + lane_threads - 1) / lane_threads));
}

std::vector<Prediction> filter_threshold(std::vector<Prediction>&& predictions, float threshold) {
    predictions.erase(
        std::remove_if(
            predictions.begin(),
            predictions.end(),
            [threshold](const Prediction& prediction) { return prediction.score < threshold; }),
        predictions.end());
    return std::move(predictions);
}

std::string encode_mask_rle(const EncodedMask& mask) {
    std::string encoded;
    for (size_t index = 0; index < mask.runs.size(); ++index) {
        if (index > 0) {
            encoded.push_back(' ');
        }
        encoded += std::to_string(mask.runs[index].first);
        encoded.push_back(':');
        encoded += std::to_string(mask.runs[index].second);
    }
    return encoded;
}

json record_to_json(const PredictionRecord& record, const std::vector<std::string>& class_names) {
    json detections = json::array();
    for (const Prediction& prediction : record.detections) {
        const int class_index = prediction.category_id - 1;
        std::string label = std::to_string(prediction.category_id);
        if (class_index >= 0 && static_cast<size_t>(class_index) < class_names.size()) {
            label = class_names[static_cast<size_t>(class_index)];
        }
        json detection = {
            {"label", std::move(label)},
            {"score", prediction.score},
            {"xyxy", prediction.bbox_xyxy},
        };
        if (prediction.has_mask) {
            detection["mask_rle"] = encode_mask_rle(prediction.mask);
        }
        detections.push_back(std::move(detection));
    }

    return json{
        {"dataset_index", record.dataset_index},
        {"image_id", record.image_id},
        {"detections", std::move(detections)},
    };
}

PredictionRunResult run_prediction_native(const PredictOptions& options,
                                          const ResolvedModelArtifacts& artifacts,
                                          size_t limit_images = 0) {
    const RuntimeContext runtime(resolve_runtime_config(
        options.workers,
        1,
        options.cpu_affinity));
    DatasetLoader loader(make_inference_loader_config(
        options.compiled_path.string(),
        std::max<size_t>(1, options.batch_size),
        runtime.loader_affinity_string(),
        options.device_id,
        static_cast<int>(kDefaultPrefetchFactor)));

    PredictionRunResult result;
    result.artifacts = artifacts;
    result.backend_name = "weights";
    result.class_names = loader_class_names(loader);
    const std::vector<int> image_ids = load_image_ids(options.compiled_path);
    auto model = load_native_model(artifacts, options.device_id);
    const size_t effective_batch_size = std::max<size_t>(1, options.batch_size);
    if (effective_batch_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("batch_size exceeds supported inference compilation range");
    }
    model->optimize_for_inference(
        static_cast<int>(effective_batch_size),
        false,
        options.compilation_mode);

    std::optional<ProgressBar> bar;
    if (options.progress_bar) {
        const size_t total_images =
            limit_images > 0 ? std::min(limit_images, loader.num_images()) : loader.num_images();
        bar.emplace("weights.predict", total_images, "img");
    }

    torch::NoGradGuard no_grad;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    const auto started_at = std::chrono::steady_clock::now();
    const auto inference_stream =
        c10::cuda::getStreamFromPool(false, checked_device_index(options.device_id));
    torch::Tensor mean;
    torch::Tensor std;
    WorkerPool& cpu_pool = runtime.cpu_pool();
    const auto slot_pool = std::make_shared<PredictionBufferSlotPool>(lane_slot_count(runtime.split()));
    const int64_t image_height = static_cast<int64_t>(loader.image_height());
    const int64_t image_width = static_cast<int64_t>(loader.image_width());
    torch::Tensor nested_mask;
    {
        c10::cuda::CUDAStreamGuard stream_guard(inference_stream);
        auto normalization = make_normalization_tensors(options.device_id);
        mean = std::move(normalization.first);
        std = std::move(normalization.second);
        nested_mask = torch::zeros(
            {static_cast<int64_t>(std::max<size_t>(1, options.batch_size)), image_height, image_width},
            torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, options.device_id));
    }

    loader.begin_epoch();
    std::deque<std::future<PredictionRecord>> cpu_futures;
    const auto make_record = [&](StagedPredictionRecord&& staged) {
        return cpu_pool.enqueue([staged = std::move(staged), threshold = options.threshold]() mutable {
            PredictionRecord record;
            record.dataset_index = staged.dataset_index;
            record.image_id = staged.image_id;
            record.detections = filter_threshold(
                encode_staged_predictions(std::move(staged.staged)),
                threshold);
            return record;
        });
    };
    const auto drain_cpu = [&]() {
        result.records.push_back(cpu_futures.front().get());
        cpu_futures.pop_front();
        if (bar.has_value()) {
            bar->add(1);
        }
    };
    Batch batch{};
    size_t submitted_records = 0;
    while (loader.next_batch(batch)) {
        loader.wait_batch(batch);
        void* consumer_stream = reinterpret_cast<void*>(inference_stream.stream());
        loader.handoff_batch(batch, consumer_stream);
        std::vector<TensorMap> processed;
        {
            c10::cuda::CUDAStreamGuard stream_guard(inference_stream);
            auto normalized =
                normalize_batch(batch, options.device_id, loader.image_height(), loader.image_width(), mean, std)
                    .contiguous();
            auto outputs = to_output_tensors(model->forward(NestedTensor{
                normalized,
                nested_mask.narrow(0, 0, normalized.size(0)),
            }));
            processed = postprocess_outputs_fixed_size(
                outputs,
                image_height,
                image_width,
                artifacts.config.num_select > 0 ? artifacts.config.num_select : artifacts.config.num_queries);
        }
        loader.release_batch(batch, consumer_stream);

        const size_t processed_count = std::min(processed.size(), batch.num_images);
        const size_t remaining =
            limit_images > 0 ? (limit_images - std::min(limit_images, submitted_records)) : processed_count;
        const size_t stage_count = std::min(processed_count, remaining);
        for (size_t image_pos = 0; image_pos < stage_count; ++image_pos) {
            const int64_t dataset_index = static_cast<int64_t>(batch.image_indices[image_pos]);
            const int64_t image_id = image_id_for_dataset_index(image_ids, dataset_index);
            StagedPredictionRecord staged;
            staged.dataset_index = dataset_index;
            staged.image_id = image_id;
            staged.staged = stage_result_to_predictions(
                static_cast<int>(image_id),
                processed[image_pos],
                loader.num_classes(),
                options.max_dets_per_image,
                slot_pool->acquire(),
                options.device_id,
                consumer_stream);
            cpu_futures.push_back(make_record(std::move(staged)));
            ++submitted_records;
            if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
                drain_cpu();
            }
        }
        if (limit_images > 0 && submitted_records >= limit_images) {
            break;
        }
    }
    while (!cpu_futures.empty()) {
        drain_cpu();
    }
    if (bar.has_value()) {
        bar->close();
    }

    result.timing = elapsed_timing(started_at, result.records.size());
    return result;
}

PredictionRunResult run_prediction_native_parallel(const PredictOptions& options,
                                                   const ResolvedModelArtifacts& artifacts,
                                                   size_t limit_images = 0) {
    const RuntimeContext runtime(resolve_runtime_config(
        options.workers,
        options.lanes,
        options.cpu_affinity));
    DatasetLoader loader(make_inference_loader_config(
        options.compiled_path.string(),
        1,
        runtime.loader_affinity_string(),
        options.device_id,
        static_cast<int>(kDefaultPrefetchFactor)));

    PredictionRunResult result;
    result.artifacts = artifacts;
    result.backend_name = "weights";
    result.class_names = loader_class_names(loader);
    const std::vector<int> image_ids = load_image_ids(options.compiled_path);

    auto model = load_native_model(artifacts, options.device_id);
    model->optimize_for_inference(1, false, options.compilation_mode);
    std::optional<ProgressBar> bar;
    if (options.progress_bar) {
        const size_t total_images =
            limit_images > 0 ? std::min(limit_images, loader.num_images()) : loader.num_images();
        bar.emplace("weights.predict", total_images, "img");
    }
    const auto started_at = std::chrono::steady_clock::now();

    torch::NoGradGuard no_grad;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    torch::Tensor mean;
    torch::Tensor std;
    {
        auto normalization = make_normalization_tensors(options.device_id);
        mean = std::move(normalization.first);
        std = std::move(normalization.second);
    }

    const int64_t image_height = static_cast<int64_t>(loader.image_height());
    const int64_t image_width = static_cast<int64_t>(loader.image_width());
    const int64_t max_queries =
        artifacts.config.num_select > 0 ? artifacts.config.num_select : artifacts.config.num_queries;
    struct ParallelNativeLane {
        c10::cuda::CUDAStream stream;
        torch::Tensor nested_mask;
        std::shared_ptr<PredictionBufferSlotPool> slot_pool;
    };
    std::vector<ParallelNativeLane> lanes;
    lanes.reserve(static_cast<size_t>(runtime.split().lane_threads));
    const size_t slot_count = lane_slot_count(runtime.split());
    for (int lane_index = 0; lane_index < runtime.split().lane_threads; ++lane_index) {
        const auto lane_stream =
            c10::cuda::getStreamFromPool(false, checked_device_index(options.device_id));
        torch::Tensor nested_mask;
        {
            c10::cuda::CUDAStreamGuard stream_guard(lane_stream);
            nested_mask = torch::zeros(
                {1, image_height, image_width},
                torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, options.device_id));
        }
        lanes.push_back(ParallelNativeLane{
            lane_stream,
            std::move(nested_mask),
            std::make_shared<PredictionBufferSlotPool>(slot_count),
        });
    }

    WorkerPool lane_pool(static_cast<size_t>(lanes.size()), runtime.lane_cpus(), "rfdpredlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();

    auto drain_lane = [&](std::deque<std::future<StagedPredictionRecord>>& lane_futures,
                          std::deque<std::future<PredictionRecord>>& cpu_futures) {
        StagedPredictionRecord staged = lane_futures.front().get();
        lane_futures.pop_front();
        cpu_futures.push_back(cpu_pool.enqueue([staged = std::move(staged), threshold = options.threshold]() mutable {
            PredictionRecord record;
            record.dataset_index = staged.dataset_index;
            record.image_id = staged.image_id;
            record.detections = filter_threshold(
                encode_staged_predictions(std::move(staged.staged)),
                threshold);
            return record;
        }));
    };

    auto drain_cpu = [&](std::deque<std::future<PredictionRecord>>& cpu_futures) {
        result.records.push_back(cpu_futures.front().get());
        cpu_futures.pop_front();
        if (bar.has_value()) {
            bar->add(1);
        }
    };

    loader.begin_epoch();
    std::deque<std::future<StagedPredictionRecord>> lane_futures;
    std::deque<std::future<PredictionRecord>> cpu_futures;
    Batch batch{};
    size_t submitted = 0;
    size_t submitted_records = 0;
    while (loader.next_batch(batch)) {
        if (limit_images > 0 && submitted_records >= limit_images) {
            loader.release_batch(batch);
            break;
        }

        const int64_t dataset_index = static_cast<int64_t>(batch.image_indices[0]);
        const int64_t image_id = image_id_for_dataset_index(image_ids, dataset_index);

        const size_t lane_index = submitted % lanes.size();
        ++submitted;
        ++submitted_records;
        lane_futures.push_back(lane_pool.enqueue([&lanes,
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

            torch::Tensor normalized =
                normalize_batch(batch, device_id, image_height, image_width, mean, std_t)
                    .contiguous();
            loader.release_batch(batch, consumer_stream);

            normalized.record_stream(lane.stream);
            auto outputs = postprocess_outputs_fixed_size(
                to_output_tensors(model->forward(NestedTensor{
                    normalized,
                    lane.nested_mask.narrow(0, 0, normalized.size(0)),
                })),
                image_height,
                image_width,
                max_queries);
            return StagedPredictionRecord{
                dataset_index,
                image_id,
                stage_result_to_predictions(
                    static_cast<int>(image_id),
                    outputs.front(),
                    category_count,
                    max_dets,
                    std::move(lease),
                    device_id,
                    reinterpret_cast<void*>(lane.stream.stream())),
            };
        }));

        if (static_cast<int>(lane_futures.size()) >= runtime.split().lane_threads) {
            drain_lane(lane_futures, cpu_futures);
        }
        if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
            drain_cpu(cpu_futures);
        }
    }

    while (!lane_futures.empty()) {
        drain_lane(lane_futures, cpu_futures);
        if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
            drain_cpu(cpu_futures);
        }
    }
    while (!cpu_futures.empty()) {
        drain_cpu(cpu_futures);
    }
    if (bar.has_value()) {
        bar->close();
    }
    result.timing = elapsed_timing(started_at, result.records.size());

    std::sort(result.records.begin(),
              result.records.end(),
              [](const PredictionRecord& lhs, const PredictionRecord& rhs) {
                  return lhs.dataset_index < rhs.dataset_index;
              });
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
    const RuntimeContext runtime(resolve_runtime_config(
        options.workers,
        effective_lane_count(options.batch_size, options.lanes),
        options.cpu_affinity));
    DatasetLoader loader(make_inference_loader_config(
        options.compiled_path.string(),
        1,
        runtime.loader_affinity_string(),
        options.device_id,
        static_cast<int>(kDefaultPrefetchFactor)));

    PredictionRunResult result;
    result.artifacts = artifacts;
    result.backend_name = backend_name;
    result.class_names = loader_class_names(loader);
    const std::vector<int> image_ids = load_image_ids(options.compiled_path);

    auto backend = make_backend(artifacts, backend_name, options.device_id, options.allow_fp16);
    std::optional<ProgressBar> bar;
    if (options.progress_bar) {
        bar.emplace(backend_name + ".predict", loader.num_images(), "img");
    }
    const auto started_at = std::chrono::steady_clock::now();

    torch::NoGradGuard no_grad;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    const auto backend_stream = backend_cuda_stream(*backend, options.device_id);
    torch::Tensor mean;
    torch::Tensor std;
    {
        c10::cuda::CUDAStreamGuard stream_guard(backend_stream);
        auto normalization = make_normalization_tensors(options.device_id);
        mean = std::move(normalization.first);
        std = std::move(normalization.second);
    }

    loader.begin_epoch();
    Batch batch{};
    while (loader.next_batch(batch)) {
        loader.wait_batch(batch);
        const int64_t dataset_index = static_cast<int64_t>(batch.image_indices[0]);
        const int64_t image_id = image_id_for_dataset_index(image_ids, dataset_index);
        void* consumer_stream = reinterpret_cast<void*>(backend_stream.stream());
        loader.handoff_batch(batch, consumer_stream);
        c10::cuda::CUDAStreamGuard stream_guard(backend_stream);
        auto normalized =
            normalize_batch(batch, options.device_id, loader.image_height(), loader.image_width(), mean, std)
                .contiguous();
        auto outputs = postprocess_outputs_fixed_size(
            backend->run(normalized),
            static_cast<int64_t>(loader.image_height()),
            static_cast<int64_t>(loader.image_width()),
            backend->info().num_queries > 0 ? backend->info().num_queries : 300);
        PredictionRecord record;
        record.dataset_index = dataset_index;
        record.image_id = image_id;
        record.detections = filter_threshold(
            result_to_predictions(
                static_cast<int>(image_id),
                outputs.front(),
                loader.num_classes(),
                options.max_dets_per_image),
            options.threshold);
        result.records.push_back(std::move(record));
        loader.release_batch(batch, consumer_stream);
        if (bar.has_value()) {
            bar->add(1);
        }
    }
    if (bar.has_value()) {
        bar->close();
    }
    result.timing = elapsed_timing(started_at, result.records.size());
    return result;
}

PredictionRunResult run_prediction_parallel(const PredictOptions& options,
                                            const ResolvedModelArtifacts& artifacts,
                                            const std::string& backend_name) {
    const RuntimeContext runtime(resolve_runtime_config(
        options.workers,
        effective_lane_count(options.batch_size, options.lanes),
        options.cpu_affinity));
    DatasetLoader loader(make_inference_loader_config(
        options.compiled_path.string(),
        1,
        runtime.loader_affinity_string(),
        options.device_id,
        static_cast<int>(kDefaultPrefetchFactor)));

    PredictionRunResult result;
    result.artifacts = artifacts;
    result.backend_name = backend_name;
    result.class_names = loader_class_names(loader);
    const std::vector<int> image_ids = load_image_ids(options.compiled_path);

    std::optional<ProgressBar> bar;
    if (options.progress_bar) {
        bar.emplace(backend_name + ".predict", loader.num_images(), "img");
    }
    const auto started_at = std::chrono::steady_clock::now();

    torch::NoGradGuard no_grad;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    torch::Tensor mean;
    torch::Tensor std;
    {
        auto normalization = make_normalization_tensors(options.device_id);
        mean = std::move(normalization.first);
        std = std::move(normalization.second);
    }

    auto lane_backends = make_backend_lanes(
        artifacts,
        backend_name,
        options.device_id,
        options.allow_fp16,
        runtime.split().lane_threads);
    struct ParallelLane {
        std::unique_ptr<InferenceBackend> backend;
        std::shared_ptr<PredictionBufferSlotPool> slot_pool;
    };
    std::vector<ParallelLane> lanes;
    lanes.reserve(lane_backends.size());
    const size_t slot_count = lane_slot_count(runtime.split());
    for (auto& backend : lane_backends) {
        lanes.push_back(ParallelLane{
            std::move(backend),
            std::make_shared<PredictionBufferSlotPool>(slot_count),
        });
    }

    WorkerPool lane_pool(static_cast<size_t>(lanes.size()), runtime.lane_cpus(), "rfdpredlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();

    auto drain_lane = [&](std::deque<std::future<StagedPredictionRecord>>& lane_futures,
                          std::deque<std::future<PredictionRecord>>& cpu_futures) {
        StagedPredictionRecord staged = lane_futures.front().get();
        lane_futures.pop_front();
        cpu_futures.push_back(cpu_pool.enqueue([staged = std::move(staged), threshold = options.threshold]() mutable {
            PredictionRecord record;
            record.dataset_index = staged.dataset_index;
            record.image_id = staged.image_id;
            record.detections = filter_threshold(
                encode_staged_predictions(std::move(staged.staged)),
                threshold);
            return record;
        }));
    };

    auto drain_cpu = [&](std::deque<std::future<PredictionRecord>>& cpu_futures) {
        result.records.push_back(cpu_futures.front().get());
        cpu_futures.pop_front();
        if (bar.has_value()) {
            bar->add(1);
        }
    };

    loader.begin_epoch();
    std::deque<std::future<StagedPredictionRecord>> lane_futures;
    std::deque<std::future<PredictionRecord>> cpu_futures;
    Batch batch{};
    size_t submitted = 0;
    const int64_t image_height = static_cast<int64_t>(loader.image_height());
    const int64_t image_width = static_cast<int64_t>(loader.image_width());
    while (loader.next_batch(batch)) {
        const int64_t dataset_index = static_cast<int64_t>(batch.image_indices[0]);
        const int64_t image_id = image_id_for_dataset_index(image_ids, dataset_index);

        const size_t lane_index = submitted % lanes.size();
        ++submitted;
        lane_futures.push_back(lane_pool.enqueue([&lanes,
                                                  &loader,
                                                  batch,
                                                  lane_index,
                                                  dataset_index,
                                                  image_id,
                                                  category_count = loader.num_classes(),
                                                  max_dets = options.max_dets_per_image,
                                                  image_height,
                                                  image_width,
                                                  mean,
                                                  std_t = std,
                                                  device_id = options.device_id]() mutable {
            auto& lane = lanes[lane_index];
            PredictionBufferLease lease = lane.slot_pool->acquire();

            loader.wait_batch(batch);
            auto backend_stream = backend_cuda_stream(*lane.backend, device_id);
            void* consumer_stream = reinterpret_cast<void*>(backend_stream.stream());
            loader.handoff_batch(batch, consumer_stream);

            torch::NoGradGuard lane_no_grad;
            c10::cuda::CUDAGuard lane_device_guard(checked_device_index(device_id));
            c10::cuda::CUDAStreamGuard stream_guard(backend_stream);

            torch::Tensor normalized =
                normalize_batch(batch, device_id, image_height, image_width, mean, std_t)
                    .contiguous();
            loader.release_batch(batch, consumer_stream);

            auto outputs = postprocess_outputs_fixed_size(
                lane.backend->run(normalized),
                image_height,
                image_width,
                lane.backend->info().num_queries > 0 ? lane.backend->info().num_queries : 300);
            return StagedPredictionRecord{
                dataset_index,
                image_id,
                stage_result_to_predictions(
                    static_cast<int>(image_id),
                    outputs.front(),
                    category_count,
                    max_dets,
                    std::move(lease),
                    device_id,
                    reinterpret_cast<void*>(backend_stream.stream())),
            };
        }));

        if (static_cast<int>(lane_futures.size()) >= runtime.split().lane_threads) {
            drain_lane(lane_futures, cpu_futures);
        }
        if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
            drain_cpu(cpu_futures);
        }
    }

    while (!lane_futures.empty()) {
        drain_lane(lane_futures, cpu_futures);
        if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
            drain_cpu(cpu_futures);
        }
    }
    while (!cpu_futures.empty()) {
        drain_cpu(cpu_futures);
    }
    if (bar.has_value()) {
        bar->close();
    }
    result.timing = elapsed_timing(started_at, result.records.size());

    std::sort(result.records.begin(),
              result.records.end(),
              [](const PredictionRecord& lhs, const PredictionRecord& rhs) {
                  return lhs.dataset_index < rhs.dataset_index;
              });
    return result;
}

} // namespace

PredictionRunResult run_prediction(const PredictOptions& options) {
    if (options.compiled_path.empty()) {
        throw std::runtime_error("RF-DETR predict requires --compiled");
    }
    if (options.output_path.empty()) {
        throw std::runtime_error("RF-DETR predict requires --output");
    }
    if (options.batch_size == 0) {
        throw std::runtime_error("RF-DETR predict requires positive --batch-size");
    }

    const ResolvedModelArtifacts artifacts = resolve_model_artifacts(options);
    const std::string backend_name = choose_backend_name(options.backend, artifacts);
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
        records.push_back(record_to_json(record, result.class_names));
    }

    json payload = {
        {"compiled_path", options.compiled_path.string()},
        {"model_kind", result.artifacts.input_kind},
        {"model_path", result.artifacts.input_path.string()},
        {"preset_name", result.artifacts.config.preset_name},
        {"backend", result.backend_name},
        {"mask_rle_encoding", "row_major_start_length"},
        {"records", std::move(records)},
    };
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
    std::fprintf(stderr,
                 "rfdetr predict[%s]: input=%s preset=%s records=%zu threshold=%.4f img_per_s=%.2f output=%s\n",
                 result.backend_name.c_str(),
                 result.artifacts.input_path.c_str(),
                 result.artifacts.config.preset_name.c_str(),
                 result.records.size(),
                 options.threshold,
                 result.timing.img_per_s,
                 options.output_path.c_str());
}

EvaluationRunResult run_evaluation(const EvaluateOptions& options) {
    if (options.compiled_path.empty()) {
        throw std::runtime_error("RF-DETR evaluate requires --compiled");
    }
    if (options.batch_size == 0) {
        throw std::runtime_error("RF-DETR evaluate requires positive --batch-size");
    }

    EvaluationRunResult out;
    out.artifacts = resolve_model_artifacts(options);
    out.backend_name = choose_backend_name(options.backend, out.artifacts);

    CocoDataset dataset = CocoDataset::load_from_binary(options.compiled_path);
    if (options.limit_images > 0) {
        dataset.limit_images(options.limit_images);
    }
    out.image_count = dataset.num_images();
    out.category_count = dataset.num_categories();

    if (out.backend_name == "weights") {
        PredictOptions predict_options;
        predict_options.compiled_path = options.compiled_path;
        predict_options.weights_path = options.weights_path;
        predict_options.output_path.clear();
        predict_options.batch_size = options.batch_size;
        predict_options.max_dets_per_image = options.eval_max_dets;
        predict_options.device_id = options.device_id;
        predict_options.workers = options.workers;
        predict_options.lanes = options.lanes;
        predict_options.threshold = 0.0f;
        predict_options.cpu_affinity = options.cpu_affinity;
        predict_options.allow_fp16 = options.allow_fp16;
        predict_options.progress_bar = options.progress_bar;
        predict_options.compilation_mode = options.compilation_mode;

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
    ValidationOptions validation_options;
    validation_options.compiled_path = options.compiled_path;
    validation_options.batch_size = static_cast<size_t>(effective_lane_count(options.batch_size, options.lanes));
    validation_options.device_id = options.device_id;
    validation_options.workers = options.workers;
    validation_options.cpu_affinity = options.cpu_affinity;
    validation_options.eval_max_dets = options.eval_max_dets;
    validation_options.allow_fp16 = options.allow_fp16;
    validation_options.onnx_path = options.onnx_path;
    validation_options.tensorrt_path = options.tensorrt_path;
    validation_options.log_mode = ValidationLogMode::Interactive;

    print_model_metadata(
        backend->info(),
        dataset.num_images(),
        dataset.num_categories(),
        validation_options.log_mode);
    out.result = run_validation_backend(validation_options, dataset, *backend);
    return out;
}

void print_evaluation_summary(const EvaluateOptions&, const EvaluationRunResult& result) {
    std::fprintf(stderr,
                 "rfdetr evaluate[%s]: input=%s preset=%s images=%zu bbox_ap=%.6f bbox_ap50=%.6f bbox_ap75=%.6f mask_ap=%.6f mask_ap50=%.6f mask_ap75=%.6f img_per_s=%.2f\n",
                 result.backend_name.c_str(),
                 result.artifacts.input_path.c_str(),
                 result.artifacts.config.preset_name.c_str(),
                 result.image_count,
                 result.result.summary.bbox.ap,
                 result.result.summary.bbox.ap50,
                 result.result.summary.bbox.ap75,
                 result.result.summary.mask.ap,
                 result.result.summary.mask.ap50,
                 result.result.summary.mask.ap75,
                 result.result.timing.img_per_s);
}

} // namespace fastloader::rfdetr
