#include "mmltk/rfdetr/predict.h"

#include "dataset_loader.h"
#include "mmltk/rfdetr/checkpoint.h"
#include "mmltk/rfdetr/model.h"
#include "mmltk/rfdetr/target_builder.h"
#include "mmltk_logging.h"
#include "image_resize.h"
#include "rfdetr/inference/backend_factory.h"
#include "rfdetr/backends.h"
#include "rfdetr/cuda_utils.h"
#include "rfdetr/postprocess.h"
#include "rfdetr/predict_runtime_internal.h"
#include "spdmon/spdmon.hpp"
#include "rfdetr/runtime.h"
#include "rfdetr/validate_internal.h"
#include "worker_pool.h"

#include "stb_image.h"

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
#include <tuple>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

using json = nlohmann::json;
using namespace validate_detail;

namespace {

constexpr size_t kDefaultPrefetchFactor = 2;

using predict_internal::drain_prediction_records;
using predict_internal::drain_staged_prediction_records;
using predict_internal::enqueue_prediction_record;
using predict_internal::filter_threshold;
using predict_internal::finalize_prediction_result;
using predict_internal::initialize_normalization_tensors;
using predict_internal::load_image_ids;
using predict_internal::load_native_model;
using predict_internal::make_backend_prediction_lanes;
using predict_internal::make_native_prediction_lanes;
using predict_internal::make_prediction_record;
using predict_internal::make_prediction_bar;
using predict_internal::make_prediction_result;
using predict_internal::StagedPredictionRecord;
using predict_internal::to_output_tensors;

struct RawImageBatchItem {
    int64_t dataset_index = 0;
    int64_t image_id = 0;
    std::string source_name;
    int64_t original_height = 0;
    int64_t original_width = 0;
};

std::vector<RawImageBatchItem> load_raw_image_batch(WorkerPool& cpu_pool,
                                                    const std::vector<PredictImageInput>& inputs,
                                                    size_t start_index,
                                                    size_t batch_count,
                                                    int resolution,
                                                    const torch::Tensor& batch_cpu);

StagedPredictionRecord make_staged_prediction_record(int64_t dataset_index,
                                                     int64_t image_id,
                                                     std::string source_name,
                                                     const TensorMap& result,
                                                     std::size_t category_count,
                                                     std::size_t max_dets_per_image,
                                                     PredictionBufferLease lease,
                                                     int device_id,
                                                     void* stream_handle) {
    StagedPredictionRecord staged;
    staged.dataset_index = dataset_index;
    staged.image_id = image_id;
    staged.source_name = std::move(source_name);
    staged.staged = stage_result_to_predictions(static_cast<int>(image_id),
                                                result,
                                                category_count,
                                                max_dets_per_image,
                                                std::move(lease),
                                                device_id,
                                                stream_handle);
    return staged;
}

void append_raw_prediction_records(std::vector<PredictionRecord>& records,
                                   std::vector<RawImageBatchItem>& items,
                                   const std::vector<TensorMap>& outputs,
                                   std::size_t category_count,
                                   std::size_t max_dets_per_image,
                                   float threshold) {
    for (size_t item_index = 0; item_index < items.size(); ++item_index) {
        auto detections = filter_threshold(
            result_to_predictions(static_cast<int>(items[item_index].image_id),
                                  outputs[item_index],
                                  category_count,
                                  max_dets_per_image),
            threshold);
        records.push_back(make_prediction_record(items[item_index].dataset_index,
                                                 items[item_index].image_id,
                                                 std::move(items[item_index].source_name),
                                                 std::move(detections)));
    }
}

template <typename InferenceFn>
void run_raw_prediction_batches(const PredictOptions& options,
                                const ResolvedModelArtifacts& artifacts,
                                WorkerPool& cpu_pool,
                                const std::vector<PredictImageInput>& image_inputs,
                                std::size_t category_count,
                                torch::Tensor& batch_cpu,
                                torch::Tensor& batch_gpu,
                                InferenceFn&& infer_batch,
                                std::vector<PredictionRecord>& records,
                                std::unique_ptr<spdmon::ProgressBar>& bar) {
    const auto batch_capacity = static_cast<size_t>(batch_cpu.size(0));
    for (size_t start_index = 0; start_index < image_inputs.size(); start_index += batch_capacity) {
        const size_t current_batch = std::min(batch_capacity, image_inputs.size() - start_index);
        torch::Tensor batch_cpu_view = batch_cpu.narrow(0, 0, static_cast<int64_t>(current_batch));
        std::vector<RawImageBatchItem> items = load_raw_image_batch(cpu_pool,
                                                                    image_inputs,
                                                                    start_index,
                                                                    current_batch,
                                                                    artifacts.config.resolution,
                                                                    batch_cpu_view);
        torch::Tensor batch_gpu_view = batch_gpu.narrow(0, 0, static_cast<int64_t>(current_batch));
        std::vector<TensorMap> outputs =
            infer_batch(batch_cpu_view, batch_gpu_view, items);
        append_raw_prediction_records(records,
                                      items,
                                      outputs,
                                      category_count,
                                      options.max_dets_per_image,
                                      options.threshold);
        if (bar) {
            bar->add(current_batch);
        }
    }
}

int effective_lane_count(size_t batch_size, int lanes) {
    return std::max(1, lanes > 0 ? lanes : static_cast<int>(std::max<size_t>(1, batch_size)));
}

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

    json payload = {
        {"dataset_index", record.dataset_index},
        {"image_id", record.image_id},
        {"detections", std::move(detections)},
    };
    if (!record.source_name.empty()) {
        payload["source_name"] = record.source_name;
    }
    return payload;
}

void hwc_uint8_to_nchw_float(const uint8_t* src, float* dst, int height, int width) {
    const int hw = height * width;
    const float scale = 1.0f / 255.0f;
    float* dst_r = dst;
    float* dst_g = dst + hw;
    float* dst_b = dst + static_cast<ptrdiff_t>(hw) * 2;
    for (int index = 0; index < hw; ++index) {
        const std::size_t pixel_offset = static_cast<std::size_t>(index) * 3U;
        dst_r[index] = static_cast<float>(src[pixel_offset]) * scale;
        dst_g[index] = static_cast<float>(src[pixel_offset + 1U]) * scale;
        dst_b[index] = static_cast<float>(src[pixel_offset + 2U]) * scale;
    }
}

RawImageBatchItem decode_raw_image_into_tensor(const PredictImageInput& input,
                                               int64_t dataset_index,
                                               int64_t default_image_id,
                                               int resolution,
                                               float* destination) {
    int raw_width = 0;
    int raw_height = 0;
    int raw_channels = 0;
    stbi_uc* raw_pixels = stbi_load(input.image_path.c_str(), &raw_width, &raw_height, &raw_channels, 3);
    if (raw_pixels == nullptr) {
        throw std::runtime_error("failed to load prediction image: " + input.image_path.string());
    }
    if (raw_width <= 0 || raw_height <= 0) {
        stbi_image_free(raw_pixels);
        throw std::runtime_error("invalid prediction image dimensions: " + input.image_path.string());
    }

    const uint8_t* resized_pixels = raw_pixels;
    std::vector<uint8_t> resize_scratch;
    if (raw_width != resolution || raw_height != resolution) {
        resize_scratch.resize(static_cast<size_t>(resolution) * static_cast<size_t>(resolution) * 3);
        RgbImageResizer image_resizer(1);
        image_resizer.resize(raw_pixels,
                             raw_width,
                             raw_height,
                             resize_scratch.data(),
                             resolution,
                             resolution);
        resized_pixels = resize_scratch.data();
    }

    hwc_uint8_to_nchw_float(resized_pixels, destination, resolution, resolution);
    stbi_image_free(raw_pixels);

    RawImageBatchItem item;
    item.dataset_index = dataset_index;
    item.image_id = input.image_id != 0 ? input.image_id : default_image_id;
    item.source_name = input.source_name.empty() ? input.image_path.string() : input.source_name;
    item.original_height = raw_height;
    item.original_width = raw_width;
    return item;
}

std::vector<RawImageBatchItem> load_raw_image_batch(WorkerPool& cpu_pool,
                                                    const std::vector<PredictImageInput>& inputs,
                                                    size_t start_index,
                                                    size_t batch_count,
                                                    int resolution,
                                                    const torch::Tensor& batch_cpu) {
    std::vector<std::future<RawImageBatchItem>> futures;
    futures.reserve(batch_count);
    for (size_t offset = 0; offset < batch_count; ++offset) {
        PredictImageInput input = inputs[start_index + offset];
        torch::Tensor batch_slot = batch_cpu[static_cast<int64_t>(offset)];
        futures.push_back(cpu_pool.enqueue([input = std::move(input),
                                            dataset_index = static_cast<int64_t>(start_index + offset),
                                            image_id = static_cast<int64_t>(start_index + offset + 1),
                                            resolution,
                                            batch_slot = std::move(batch_slot)]() mutable {
            return decode_raw_image_into_tensor(
                input,
                dataset_index,
                image_id,
                resolution,
                batch_slot.data_ptr<float>());
        }));
    }

    std::vector<RawImageBatchItem> items;
    items.reserve(batch_count);
    for (auto& future : futures) {
        items.push_back(future.get());
    }
    return items;
}

torch::Tensor raw_image_target_sizes(const std::vector<RawImageBatchItem>& items,
                                     int device_id,
                                     const std::optional<c10::cuda::CUDAStream>& stream = std::nullopt) {
    std::vector<int64_t> target_sizes;
    target_sizes.reserve(items.size() * 2);
    for (const RawImageBatchItem& item : items) {
        target_sizes.push_back(item.original_height);
        target_sizes.push_back(item.original_width);
    }

    const auto options = torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA, device_id);
    if (!stream.has_value()) {
        return torch::tensor(target_sizes, options)
            .view({static_cast<int64_t>(items.size()), 2});
    }
    c10::cuda::CUDAStreamGuard stream_guard(*stream);
    return torch::tensor(target_sizes, options)
        .view({static_cast<int64_t>(items.size()), 2});
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

    PredictionRunResult result = make_prediction_result(artifacts, "weights", loader);
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

    std::unique_ptr<spdmon::ProgressBar> bar =
        make_prediction_bar(options.progress_bar, "weights.predict", loader, limit_images);

    torch::NoGradGuard no_grad;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    const auto started_at = std::chrono::steady_clock::now();
    const auto inference_stream =
        get_high_priority_cuda_stream(options.device_id);
    torch::Tensor mean;
    torch::Tensor std;
    WorkerPool& cpu_pool = runtime.cpu_pool();
    const auto slot_pool = std::make_shared<PredictionBufferSlotPool>(lane_slot_count(runtime.split()));
    const auto image_height = static_cast<int64_t>(loader.image_height());
    const auto image_width = static_cast<int64_t>(loader.image_width());
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
    std::deque<std::future<PredictionRecord>> cpu_futures;
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
            const auto dataset_index = static_cast<int64_t>(batch.image_indices[image_pos]);
            const int64_t image_id = image_id_for_dataset_index(image_ids, dataset_index);
            StagedPredictionRecord staged = make_staged_prediction_record(
                dataset_index,
                image_id,
                {},
                processed[image_pos],
                loader.num_classes(),
                options.max_dets_per_image,
                slot_pool->acquire(),
                options.device_id,
                consumer_stream);
            cpu_futures.push_back(enqueue_prediction_record(cpu_pool, std::move(staged), options.threshold));
            ++submitted_records;
            if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
                drain_prediction_records(cpu_futures, result, bar);
            }
        }
        if (limit_images > 0 && submitted_records >= limit_images) {
            break;
        }
    }
    while (!cpu_futures.empty()) {
        drain_prediction_records(cpu_futures, result, bar);
    }
    if (bar) {
        bar->close();
    }
    finalize_prediction_result(result, started_at);
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

    PredictionRunResult result = make_prediction_result(artifacts, "weights", loader);
    const std::vector<int> image_ids = load_image_ids(options.compiled_path);

    auto model = load_native_model(artifacts, options.device_id);
    model->optimize_for_inference(1, false, options.compilation_mode);
    std::unique_ptr<spdmon::ProgressBar> bar =
        make_prediction_bar(options.progress_bar, "weights.predict", loader, limit_images);
    const auto started_at = std::chrono::steady_clock::now();

    torch::NoGradGuard no_grad;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    torch::Tensor mean;
    torch::Tensor std;
    std::tie(mean, std) = initialize_normalization_tensors(options.device_id);

    const auto image_height = static_cast<int64_t>(loader.image_height());
    const auto image_width = static_cast<int64_t>(loader.image_width());
    const int64_t max_queries =
        artifacts.config.num_select > 0 ? artifacts.config.num_select : artifacts.config.num_queries;
    const size_t slot_count = lane_slot_count(runtime.split());
    std::vector<predict_internal::NativePredictionLane> lanes =
        make_native_prediction_lanes(options.device_id,
                                     image_height,
                                     image_width,
                                     runtime.split().lane_threads,
                                     slot_count);

    WorkerPool lane_pool(static_cast<size_t>(lanes.size()), runtime.lane_cpus(), "rfdpredlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();

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

        const auto dataset_index = static_cast<int64_t>(batch.image_indices[0]);
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
            return make_staged_prediction_record(dataset_index,
                                                 image_id,
                                                 {},
                                                 outputs.front(),
                                                 category_count,
                                                 max_dets,
                                                 std::move(lease),
                                                 device_id,
                                                 reinterpret_cast<void*>(lane.stream.stream()));
        }));

        if (static_cast<int>(lane_futures.size()) >= runtime.split().lane_threads) {
            drain_staged_prediction_records(lane_futures, cpu_futures, cpu_pool, options.threshold);
        }
        if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
            drain_prediction_records(cpu_futures, result, bar);
        }
    }

    while (!lane_futures.empty()) {
        drain_staged_prediction_records(lane_futures, cpu_futures, cpu_pool, options.threshold);
        if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
            drain_prediction_records(cpu_futures, result, bar);
        }
    }
    while (!cpu_futures.empty()) {
        drain_prediction_records(cpu_futures, result, bar);
    }
    if (bar) {
        bar->close();
    }
    finalize_prediction_result(result, started_at, true);
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

    PredictionRunResult result = make_prediction_result(artifacts, backend_name, loader);
    const std::vector<int> image_ids = load_image_ids(options.compiled_path);

    auto backend = make_backend(artifacts, backend_name, options.device_id, options.allow_fp16);
    std::unique_ptr<spdmon::ProgressBar> bar =
        make_prediction_bar(options.progress_bar, backend_name + ".predict", loader);
    const auto started_at = std::chrono::steady_clock::now();

    torch::NoGradGuard no_grad;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
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
    while (loader.next_batch(batch)) {
        loader.wait_batch(batch);
        const auto dataset_index = static_cast<int64_t>(batch.image_indices[0]);
        const int64_t image_id = image_id_for_dataset_index(image_ids, dataset_index);
        void* consumer_stream = reinterpret_cast<void*>(backend_execution.stream.stream());
        loader.handoff_batch(batch, consumer_stream);
        c10::cuda::CUDAStreamGuard stream_guard(backend_execution.stream);
        auto normalized =
            normalize_batch(batch, options.device_id, loader.image_height(), loader.image_width(), mean, std)
                .contiguous();
        auto outputs = postprocess_outputs_fixed_size(
            backend->run(normalized),
            static_cast<int64_t>(loader.image_height()),
            static_cast<int64_t>(loader.image_width()),
            backend_execution.num_queries);
        result.records.push_back(make_prediction_record(
            dataset_index,
            image_id,
            {},
            filter_threshold(result_to_predictions(static_cast<int>(image_id),
                                                   outputs.front(),
                                                   backend_execution.category_count,
                                                   options.max_dets_per_image),
                              options.threshold)));
        loader.release_batch(batch, consumer_stream);
        if (bar) {
            bar->add(1);
        }
    }
    if (bar) {
        bar->close();
    }
    finalize_prediction_result(result, started_at);
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

    PredictionRunResult result = make_prediction_result(artifacts, backend_name, loader);
    const std::vector<int> image_ids = load_image_ids(options.compiled_path);

    std::unique_ptr<spdmon::ProgressBar> bar =
        make_prediction_bar(options.progress_bar, backend_name + ".predict", loader);
    const auto started_at = std::chrono::steady_clock::now();

    torch::NoGradGuard no_grad;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    torch::Tensor mean;
    torch::Tensor std;
    std::tie(mean, std) = initialize_normalization_tensors(options.device_id);

    const size_t slot_count = lane_slot_count(runtime.split());
    std::vector<predict_internal::BackendPredictionLane> lanes =
        make_backend_prediction_lanes(artifacts,
                                      backend_name,
                                      options.device_id,
                                      options.allow_fp16,
                                      runtime.split().lane_threads,
                                      slot_count);

    WorkerPool lane_pool(static_cast<size_t>(lanes.size()), runtime.lane_cpus(), "rfdpredlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();

    loader.begin_epoch();
    std::deque<std::future<StagedPredictionRecord>> lane_futures;
    std::deque<std::future<PredictionRecord>> cpu_futures;
    Batch batch{};
    size_t submitted = 0;
    const auto image_height = static_cast<int64_t>(loader.image_height());
    const auto image_width = static_cast<int64_t>(loader.image_width());
    while (loader.next_batch(batch)) {
        const auto dataset_index = static_cast<int64_t>(batch.image_indices[0]);
        const int64_t image_id = image_id_for_dataset_index(image_ids, dataset_index);

        const size_t lane_index = submitted % lanes.size();
        ++submitted;
        lane_futures.push_back(lane_pool.enqueue([&lanes,
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

            torch::Tensor normalized =
                normalize_batch(batch, device_id, image_height, image_width, mean, std_t)
                    .contiguous();
            loader.release_batch(batch, consumer_stream);

            auto outputs = postprocess_outputs_fixed_size(
                lane.backend->run(normalized),
                image_height,
                image_width,
                lane.num_queries);
            return make_staged_prediction_record(dataset_index,
                                                 image_id,
                                                 {},
                                                 outputs.front(),
                                                 lane.category_count,
                                                 max_dets,
                                                 std::move(lease),
                                                 device_id,
                                                 reinterpret_cast<void*>(lane.stream.stream()));
        }));

        if (static_cast<int>(lane_futures.size()) >= runtime.split().lane_threads) {
            drain_staged_prediction_records(lane_futures, cpu_futures, cpu_pool, options.threshold);
        }
        if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
            drain_prediction_records(cpu_futures, result, bar);
        }
    }

    while (!lane_futures.empty()) {
        drain_staged_prediction_records(lane_futures, cpu_futures, cpu_pool, options.threshold);
        if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
            drain_prediction_records(cpu_futures, result, bar);
        }
    }
    while (!cpu_futures.empty()) {
        drain_prediction_records(cpu_futures, result, bar);
    }
    if (bar) {
        bar->close();
    }
    finalize_prediction_result(result, started_at, true);
    return result;
}

PredictionRunResult run_prediction_raw_weights(const PredictOptions& options,
                                               const ResolvedModelArtifacts& artifacts) {
    if (options.lanes > 1) {
        throw std::runtime_error(
            "RF-DETR raw-image weights predict does not support --lanes > 1");
    }

    const RuntimeContext runtime(resolve_runtime_config(options.workers, 1, options.cpu_affinity));
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

    std::unique_ptr<spdmon::ProgressBar> bar =
        make_prediction_bar(options.progress_bar, "weights.predict", options.image_inputs.size());

    torch::NoGradGuard no_grad;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    const auto started_at = std::chrono::steady_clock::now();
    const auto inference_stream =
        get_high_priority_cuda_stream(options.device_id);
    torch::Tensor mean;
    torch::Tensor std;
    {
        auto normalization = initialize_normalization_tensors(options.device_id, inference_stream);
        mean = std::move(normalization.first);
        std = std::move(normalization.second);
    }

    const int64_t resolution = artifacts.config.resolution;
    torch::Tensor batch_cpu = torch::empty(
        {static_cast<int64_t>(effective_batch_size), 3, resolution, resolution},
        torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU).pinned_memory(true));
    torch::Tensor batch_gpu = torch::empty(
        {static_cast<int64_t>(effective_batch_size), 3, resolution, resolution},
        torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, options.device_id));
    torch::Tensor nested_mask = torch::zeros(
        {static_cast<int64_t>(effective_batch_size), resolution, resolution},
        torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, options.device_id));

    WorkerPool& cpu_pool = runtime.cpu_pool();
    auto infer_batch = [&](const torch::Tensor& batch_cpu_view,
                           const torch::Tensor& batch_gpu_view,
                           const std::vector<RawImageBatchItem>& items) {
        c10::cuda::CUDAStreamGuard stream_guard(inference_stream);
        batch_gpu_view.copy_(batch_cpu_view, true);
        torch::Tensor target_sizes = raw_image_target_sizes(items, options.device_id, inference_stream);
        torch::Tensor normalized = batch_gpu_view.sub(mean).div(std).contiguous();
        return postprocess_outputs(
            to_output_tensors(model->forward(NestedTensor{
                normalized,
                nested_mask.narrow(0, 0, normalized.size(0)),
            })),
            target_sizes,
            artifacts.config.num_select > 0 ? artifacts.config.num_select : artifacts.config.num_queries);
    };
    run_raw_prediction_batches(options,
                               artifacts,
                               cpu_pool,
                               options.image_inputs,
                               static_cast<size_t>(artifacts.config.num_classes),
                               batch_cpu,
                               batch_gpu,
                               infer_batch,
                               result.records,
                               bar);

    if (bar) {
        bar->close();
    }
    finalize_prediction_result(result, started_at);
    return result;
}

PredictionRunResult run_prediction_raw_sequential(const PredictOptions& options,
                                                  const ResolvedModelArtifacts& artifacts,
                                                  const std::string& backend_name) {
    const RuntimeContext runtime(resolve_runtime_config(options.workers, 1, options.cpu_affinity));
    PredictionRunResult result = make_prediction_result(artifacts, backend_name);

    auto backend = make_backend(artifacts, backend_name, options.device_id, options.allow_fp16);
    std::unique_ptr<spdmon::ProgressBar> bar =
        make_prediction_bar(options.progress_bar, backend_name + ".predict", options.image_inputs.size());
    const auto started_at = std::chrono::steady_clock::now();

    torch::NoGradGuard no_grad;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    const predict_internal::BackendExecutionSpec backend_execution =
        predict_internal::describe_backend_execution(*backend, artifacts, options.device_id);
    torch::Tensor mean;
    torch::Tensor std;
    {
        auto normalization = initialize_normalization_tensors(options.device_id, backend_execution.stream);
        mean = std::move(normalization.first);
        std = std::move(normalization.second);
    }

    const int64_t resolution = artifacts.config.resolution;
    torch::Tensor batch_cpu = torch::empty(
        {1, 3, resolution, resolution},
        torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU).pinned_memory(true));
    torch::Tensor batch_gpu = torch::empty(
        {1, 3, resolution, resolution},
        torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, options.device_id));

    WorkerPool& cpu_pool = runtime.cpu_pool();
    auto infer_batch = [&](const torch::Tensor& batch_cpu_view,
                           const torch::Tensor& batch_gpu_view,
                           const std::vector<RawImageBatchItem>& items) {
        c10::cuda::CUDAStreamGuard stream_guard(backend_execution.stream);
        batch_gpu_view.copy_(batch_cpu_view, true);
        torch::Tensor target_sizes = raw_image_target_sizes(items, options.device_id, backend_execution.stream);
        torch::Tensor normalized = batch_gpu_view.sub(mean).div(std).contiguous();
        return postprocess_outputs(backend->run(normalized), target_sizes, backend_execution.num_queries);
    };
    run_raw_prediction_batches(options,
                               artifacts,
                               cpu_pool,
                               options.image_inputs,
                               backend_execution.category_count,
                               batch_cpu,
                               batch_gpu,
                               infer_batch,
                               result.records,
                               bar);

    if (bar) {
        bar->close();
    }
    finalize_prediction_result(result, started_at);
    return result;
}

PredictionRunResult run_prediction_raw_parallel(const PredictOptions& options,
                                                const ResolvedModelArtifacts& artifacts,
                                                const std::string& backend_name) {
    const RuntimeContext runtime(resolve_runtime_config(
        options.workers,
        effective_lane_count(options.batch_size, options.lanes),
        options.cpu_affinity));
    PredictionRunResult result = make_prediction_result(artifacts, backend_name);

    std::unique_ptr<spdmon::ProgressBar> bar =
        make_prediction_bar(options.progress_bar, backend_name + ".predict", options.image_inputs.size());
    const auto started_at = std::chrono::steady_clock::now();

    torch::NoGradGuard no_grad;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    torch::Tensor mean;
    torch::Tensor std;
    std::tie(mean, std) = initialize_normalization_tensors(options.device_id);

    const size_t slot_count = lane_slot_count(runtime.split());
    std::vector<predict_internal::BackendPredictionLane> lanes =
        make_backend_prediction_lanes(artifacts,
                                      backend_name,
                                      options.device_id,
                                      options.allow_fp16,
                                      runtime.split().lane_threads,
                                      slot_count);

    WorkerPool lane_pool(static_cast<size_t>(lanes.size()), runtime.lane_cpus(), "rfdpredlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();

    std::deque<std::future<StagedPredictionRecord>> lane_futures;
    std::deque<std::future<PredictionRecord>> cpu_futures;
    size_t submitted = 0;
    const int64_t resolution = artifacts.config.resolution;
    for (size_t input_index = 0; input_index < options.image_inputs.size(); ++input_index) {
        PredictImageInput input = options.image_inputs[input_index];
        const auto dataset_index = static_cast<int64_t>(input_index);
        const auto image_id = input.image_id != 0 ? input.image_id : static_cast<int64_t>(input_index + 1);

        const size_t lane_index = submitted % lanes.size();
        ++submitted;
        lane_futures.push_back(lane_pool.enqueue([&lanes,
                                                  input = std::move(input),
                                                  lane_index,
                                                  dataset_index,
                                                  image_id,
                                                  max_dets = options.max_dets_per_image,
                                                  resolution,
                                                  mean,
                                                  std_t = std,
                                                  device_id = options.device_id]() mutable {
            auto& lane = lanes[lane_index];
            PredictionBufferLease lease = lane.slot_pool->acquire();

            torch::Tensor batch_cpu = torch::empty(
                {1, 3, resolution, resolution},
                torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU).pinned_memory(true));
            RawImageBatchItem item = decode_raw_image_into_tensor(
                input,
                dataset_index,
                image_id,
                static_cast<int>(resolution),
                batch_cpu[0].data_ptr<float>());
            torch::Tensor batch_gpu = torch::empty(
                {1, 3, resolution, resolution},
                torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device_id));

            torch::NoGradGuard lane_no_grad;
            c10::cuda::CUDAGuard lane_device_guard(checked_device_index(device_id));
            c10::cuda::CUDAStreamGuard stream_guard(lane.stream);

            batch_gpu.copy_(batch_cpu, true);
            torch::Tensor target_sizes =
                raw_image_target_sizes(std::vector<RawImageBatchItem>{item}, device_id, lane.stream);
            torch::Tensor normalized = batch_gpu.sub(mean).div(std_t).contiguous();
            auto outputs = postprocess_outputs(
                lane.backend->run(normalized),
                target_sizes,
                lane.num_queries);
            return make_staged_prediction_record(item.dataset_index,
                                                 item.image_id,
                                                 item.source_name,
                                                 outputs.front(),
                                                 lane.category_count,
                                                 max_dets,
                                                 std::move(lease),
                                                 device_id,
                                                 reinterpret_cast<void*>(lane.stream.stream()));
        }));

        if (static_cast<int>(lane_futures.size()) >= runtime.split().lane_threads) {
            drain_staged_prediction_records(lane_futures, cpu_futures, cpu_pool, options.threshold);
        }
        if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
            drain_prediction_records(cpu_futures, result, bar);
        }
    }

    while (!lane_futures.empty()) {
        drain_staged_prediction_records(lane_futures, cpu_futures, cpu_pool, options.threshold);
        if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
            drain_prediction_records(cpu_futures, result, bar);
        }
    }
    while (!cpu_futures.empty()) {
        drain_prediction_records(cpu_futures, result, bar);
    }
    if (bar) {
        bar->close();
    }
    finalize_prediction_result(result, started_at, true);
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
        records.push_back(record_to_json(record, result.class_names));
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
    mmltk::logging::logger("rfdetr.predict")->info(
        "rfdetr predict[{}]: source={} input={} preset={} records={} threshold={:.4f} img_per_s={:.2f} output={}",
        result.backend_name,
        predict_source_kind_name(options.source_kind),
        result.artifacts.input_path.string(),
        result.artifacts.config.preset_name,
        result.records.size(),
        options.threshold,
        result.timing.img_per_s,
        options.output_path.string());
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
    mmltk::logging::logger("rfdetr.evaluate")->info(
        "rfdetr evaluate[{}]: input={} preset={} images={} bbox_ap={:.6f} bbox_ap50={:.6f} bbox_ap75={:.6f} mask_ap={:.6f} mask_ap50={:.6f} mask_ap75={:.6f} img_per_s={:.2f}",
        result.backend_name,
        result.artifacts.input_path.string(),
        result.artifacts.config.preset_name,
        result.image_count,
        result.result.summary.bbox.ap,
        result.result.summary.bbox.ap50,
        result.result.summary.bbox.ap75,
        result.result.summary.mask.ap,
        result.result.summary.mask.ap50,
        result.result.summary.mask.ap75,
        result.result.timing.img_per_s);
}

} // namespace mmltk::rfdetr
