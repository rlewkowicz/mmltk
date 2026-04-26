#pragma once

#include "mmltk/live/live_capture_region.h"
#include "mmltk/live/live_types.h"
#include "mmltk/rfdetr/checkpoint.h"
#include "mmltk/rfdetr/draw_cuda.h"
#include "mmltk/rfdetr/model.h"
#include "mmltk/rfdetr/predict.h"
#include "mmltk/rfdetr/target_builder.h"

#include "mmltk_logging.h"
#include "rfdetr/backends.h"
#include "rfdetr/common/dataset_utils.h"
#include "rfdetr/common/tensor_utils.h"
#include "rfdetr/torch_cuda_utils.h"
#include "rfdetr/evaluator.h"
#include "rfdetr/inference/backend_factory.h"
#include "rfdetr/postprocess.h"
#include "rfdetr/predict_runtime_shared.h"
#include "rfdetr/validate_internal.h"
#include "worker_pool.h"

#include "spdmon/spdmon.hpp"

#include <c10/cuda/CUDAStream.h>

#include <format>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mmltk::rfdetr::predict_internal {

struct LiveSplitRegion {
    std::uint32_t x = 0;
    std::uint32_t width = 0;
};

struct LiveOverlaySelection {
    torch::Tensor boxes_xyxy;
    torch::Tensor labels_zero_based;
    torch::Tensor scores;
    torch::Tensor colors_rgb;
    torch::Tensor masks;
};

struct LiveFrameInputs {
    std::uint64_t frame_id = 0;
    std::uint64_t capture_ns = 0;
    std::uint64_t ready_ns = 0;
    cudaEvent_t ready_event = nullptr;
    bool short_frame = false;
    const std::uint8_t* data = nullptr;
    std::size_t pitch_bytes = 0;
    mmltk::live::LiveCaptureRegion region{};
};

struct LiveSplitRenderData {
    mmltk::live::LiveCaptureRegion source_region{};
    std::vector<Prediction> detections;
    torch::Tensor boxes_xyxy;
    torch::Tensor labels_zero_based;
    torch::Tensor scores;
    torch::Tensor colors_rgb;
    torch::Tensor masks;
};

struct LiveFrameRenderData {
    std::vector<LiveSplitRenderData> splits;
    cudaEvent_t ready_event = nullptr;
    cudaStream_t producer_stream = nullptr;
};

struct NativePredictionLane {
    c10::cuda::CUDAStream stream = c10::cuda::getDefaultCUDAStream();
    torch::Tensor nested_mask;
    std::shared_ptr<PredictionBufferSlotPool> slot_pool;
};

struct StagedPredictionRecord {
    int64_t dataset_index = 0;
    int64_t image_id = 0;
    std::string source_name;
    StagedPredictionBatch staged;
};

struct RawImageBatchItem {
    int64_t dataset_index = 0;
    int64_t image_id = 0;
    std::string source_name;
    int64_t original_height = 0;
    int64_t original_width = 0;
};

inline StagedPredictionRecord make_staged_prediction_record(int64_t dataset_index, int64_t image_id,
                                                            std::string source_name, const TensorMap& result,
                                                            std::size_t category_count, std::size_t max_dets_per_image,
                                                            PredictionBufferLease lease, int device_id,
                                                            void* stream_handle) {
    StagedPredictionRecord staged;
    staged.dataset_index = dataset_index;
    staged.image_id = image_id;
    staged.source_name = std::move(source_name);
    staged.staged = stage_result_to_predictions(static_cast<int>(image_id), result, category_count, max_dets_per_image,
                                                std::move(lease), device_id, stream_handle);
    return staged;
}

inline std::string choose_backend_name(const std::string& requested_backend, const ResolvedModelArtifacts& artifacts) {
    return mmltk::rfdetr::choose_backend_name(requested_backend, artifacts);
}

inline std::unique_ptr<InferenceBackend> make_backend(const ResolvedModelArtifacts& artifacts,
                                                      const std::string& backend_name, int device_id, bool allow_fp16) {
    return mmltk::rfdetr::make_backend(artifacts, backend_name, device_id, allow_fp16);
}

inline std::vector<std::unique_ptr<InferenceBackend>> make_backend_lanes(const ResolvedModelArtifacts& artifacts,
                                                                         const std::string& backend_name, int device_id,
                                                                         bool allow_fp16, int lane_count) {
    return mmltk::rfdetr::make_backend_lanes(artifacts, backend_name, device_id, allow_fp16, lane_count);
}

inline std::shared_ptr<NativeRfDetrModel> load_native_model(const ResolvedModelArtifacts& artifacts, int device_id) {
    auto model = std::make_shared<NativeRfDetrModel>(artifacts.config);
    const StateDictLoadSummary load_summary = model->load_weights(artifacts.weights_path, false);
    mmltk::logging::logger("rfdetr.predict")
        ->info("rfdetr weights: loaded={} missing={} unexpected={} incompatible={} input={}",
               load_summary.loaded_names.size(), load_summary.missing_names.size(),
               load_summary.unexpected_names.size(), load_summary.incompatible_names.size(),
               artifacts.weights_path.string());
    model->eval();
    model->to(cuda_device(device_id));
    return model;
}

inline OutputTensors to_output_tensors(const ModelOutputs& outputs) {
    return OutputTensors{
        outputs.main.pred_logits,
        outputs.main.pred_boxes,
        outputs.main.pred_masks,
    };
}

inline std::vector<int> load_image_ids(const std::filesystem::path& compiled_path) {
    return CocoDataset::load_from_binary(compiled_path).image_ids();
}

inline PredictionRunResult make_prediction_result(const ResolvedModelArtifacts& artifacts, std::string backend_name,
                                                  std::vector<std::string> class_names = {}) {
    PredictionRunResult result;
    result.artifacts = artifacts;
    result.backend_name = std::move(backend_name);
    result.class_names = std::move(class_names);
    return result;
}

inline PredictionRunResult make_prediction_result(const ResolvedModelArtifacts& artifacts, std::string backend_name,
                                                  const DatasetLoader& loader) {
    return make_prediction_result(artifacts, std::move(backend_name), loader_class_names(loader));
}

inline std::vector<Prediction> filter_threshold(std::vector<Prediction>&& predictions, const float threshold);

std::vector<RawImageBatchItem> load_raw_image_batch(WorkerPool& cpu_pool, const std::vector<PredictImageInput>& inputs,
                                                    size_t start_index, size_t batch_count, int resolution,
                                                    const torch::Tensor& batch_cpu,
                                                    std::vector<std::vector<std::uint8_t>>& resize_scratch_slots);

void append_raw_prediction_records(std::vector<PredictionRecord>& records, std::vector<RawImageBatchItem>& items,
                                   const std::vector<TensorMap>& outputs, std::size_t category_count,
                                   std::size_t max_dets_per_image, float threshold);

inline std::size_t prediction_max_queries(const ResolvedModelArtifacts& artifacts) {
    return artifacts.config.num_select > 0 ? static_cast<std::size_t>(artifacts.config.num_select)
                                           : static_cast<std::size_t>(artifacts.config.num_queries);
}

constexpr int kDefaultCompiledPredictionPrefetchFactor = 2;

struct CompiledPredictionSetup {
    RuntimeContext runtime;
    std::unique_ptr<DatasetLoader> loader;
    PredictionRunResult result;
    std::vector<int> image_ids;
};

inline CompiledPredictionSetup make_compiled_prediction_setup(const PredictOptions& options,
                                                              const ResolvedModelArtifacts& artifacts,
                                                              std::string backend_name, const int runtime_lanes,
                                                              const std::size_t loader_batch_size) {
    RuntimeContext runtime{resolve_runtime_config(options.workers, runtime_lanes, options.cpu_affinity)};
    auto loader = std::make_unique<DatasetLoader>(validate_detail::make_inference_loader_config(
        options.compiled_path.string(), std::max<std::size_t>(1, loader_batch_size), runtime.loader_affinity_string(),
        options.device_id, kDefaultCompiledPredictionPrefetchFactor));
    PredictionRunResult result = make_prediction_result(artifacts, std::move(backend_name), *loader);
    return CompiledPredictionSetup{
        std::move(runtime),
        std::move(loader),
        std::move(result),
        load_image_ids(options.compiled_path),
    };
}

template <typename RunModelFn>
inline OutputTensors run_compiled_prediction_batch(const Batch& batch, const int device_id,
                                                   const std::int64_t image_height, const std::int64_t image_width,
                                                   const torch::Tensor& mean, const torch::Tensor& std,
                                                   RunModelFn&& run_model) {
    const torch::Tensor normalized =
        validate_detail::normalize_batch(batch, device_id, image_height, image_width, mean, std).contiguous();
    return std::forward<RunModelFn>(run_model)(normalized);
}

inline std::size_t prediction_total_images(const DatasetLoader& loader, const std::size_t limit_images) {
    return limit_images > 0 ? std::min(limit_images, loader.num_images()) : loader.num_images();
}

inline std::unique_ptr<spdmon::ProgressBar> make_prediction_bar(bool enabled, const std::string& label,
                                                                std::size_t total_images) {
    if (!enabled) {
        return nullptr;
    }
    return std::make_unique<spdmon::ProgressBar>(label, total_images, "img");
}

inline std::unique_ptr<spdmon::ProgressBar> make_prediction_bar(bool enabled, const std::string& label,
                                                                const DatasetLoader& loader,
                                                                std::size_t limit_images = 0) {
    return make_prediction_bar(enabled, label, prediction_total_images(loader, limit_images));
}

inline std::vector<Prediction> filter_threshold(std::vector<Prediction>&& predictions, const float threshold) {
    std::erase_if(predictions, [threshold](const Prediction& prediction) { return prediction.score < threshold; });
    return std::move(predictions);
}

inline PredictionRecord make_prediction_record(int64_t dataset_index, int64_t image_id, std::string source_name,
                                               std::vector<Prediction>&& detections) {
    PredictionRecord record;
    record.dataset_index = dataset_index;
    record.image_id = image_id;
    record.source_name = std::move(source_name);
    record.detections = std::move(detections);
    return record;
}

inline PredictionRecord make_compiled_prediction_record(int64_t dataset_index, int64_t image_id,
                                                        const TensorMap& output, std::size_t category_count,
                                                        std::size_t max_dets_per_image, float threshold) {
    return make_prediction_record(
        dataset_index, image_id, {},
        filter_threshold(result_to_predictions(static_cast<int>(image_id), output, category_count, max_dets_per_image),
                         threshold));
}

inline std::future<PredictionRecord> enqueue_prediction_record(WorkerPool& cpu_pool, StagedPredictionRecord&& staged,
                                                               const float threshold) {
    return cpu_pool.enqueue([staged = std::move(staged), threshold]() mutable {
        return make_prediction_record(staged.dataset_index, staged.image_id, std::move(staged.source_name),
                                      filter_threshold(encode_staged_predictions(std::move(staged.staged)), threshold));
    });
}

inline void drain_staged_prediction_records(std::deque<std::future<StagedPredictionRecord>>& lane_futures,
                                            std::deque<std::future<PredictionRecord>>& cpu_futures,
                                            WorkerPool& cpu_pool, const float threshold) {
    StagedPredictionRecord staged = lane_futures.front().get();
    lane_futures.pop_front();
    cpu_futures.push_back(enqueue_prediction_record(cpu_pool, std::move(staged), threshold));
}

inline void drain_prediction_records(std::deque<std::future<PredictionRecord>>& cpu_futures,
                                     PredictionRunResult& result, std::unique_ptr<spdmon::ProgressBar>& bar) {
    result.records.push_back(cpu_futures.front().get());
    cpu_futures.pop_front();
    if (bar) {
        bar->add(1);
    }
}

inline void maybe_drain_prediction_work(std::deque<std::future<StagedPredictionRecord>>& lane_futures,
                                        std::deque<std::future<PredictionRecord>>& cpu_futures, WorkerPool& cpu_pool,
                                        PredictionRunResult& result, std::unique_ptr<spdmon::ProgressBar>& bar,
                                        float threshold, const RuntimeSplit& split) {
    if (static_cast<int>(lane_futures.size()) >= split.lane_threads) {
        drain_staged_prediction_records(lane_futures, cpu_futures, cpu_pool, threshold);
    }
    if (static_cast<int>(cpu_futures.size()) >= split.cpu_threads) {
        drain_prediction_records(cpu_futures, result, bar);
    }
}

inline void flush_prediction_work(std::deque<std::future<StagedPredictionRecord>>& lane_futures,
                                  std::deque<std::future<PredictionRecord>>& cpu_futures, WorkerPool& cpu_pool,
                                  PredictionRunResult& result, std::unique_ptr<spdmon::ProgressBar>& bar,
                                  float threshold, const RuntimeSplit& split) {
    while (!lane_futures.empty()) {
        drain_staged_prediction_records(lane_futures, cpu_futures, cpu_pool, threshold);
        if (static_cast<int>(cpu_futures.size()) >= split.cpu_threads) {
            drain_prediction_records(cpu_futures, result, bar);
        }
    }
    while (!cpu_futures.empty()) {
        drain_prediction_records(cpu_futures, result, bar);
    }
}

template <typename ItemGeneratorFn, typename EnqueueFn>
inline void run_parallel_prediction_pipeline(PredictionRunResult& result, std::unique_ptr<spdmon::ProgressBar>& bar,
                                             WorkerPool& cpu_pool, const RuntimeSplit& split, float threshold,
                                             ItemGeneratorFn&& generator, EnqueueFn&& enqueue_fn) {
    std::deque<std::future<StagedPredictionRecord>> lane_futures;
    std::deque<std::future<PredictionRecord>> cpu_futures;
    size_t item_index = 0;
    while (std::forward<ItemGeneratorFn>(generator)(item_index)) {
        lane_futures.push_back(std::forward<EnqueueFn>(enqueue_fn)(item_index));
        maybe_drain_prediction_work(lane_futures, cpu_futures, cpu_pool, result, bar, threshold, split);
        ++item_index;
    }
    flush_prediction_work(lane_futures, cpu_futures, cpu_pool, result, bar, threshold, split);
}

template <typename ItemGeneratorFn, typename ProcessFn>
inline void run_sequential_prediction_pipeline(PredictionRunResult& result, std::unique_ptr<spdmon::ProgressBar>& bar,
                                               WorkerPool& cpu_pool, const RuntimeSplit& split, float threshold,
                                               ItemGeneratorFn&& generator, ProcessFn&& process_fn) {
    std::deque<std::future<PredictionRecord>> cpu_futures;
    size_t item_index = 0;
    while (std::forward<ItemGeneratorFn>(generator)(item_index)) {
        std::vector<StagedPredictionRecord> staged_records = std::forward<ProcessFn>(process_fn)(item_index);
        for (StagedPredictionRecord& staged : staged_records) {
            cpu_futures.push_back(enqueue_prediction_record(cpu_pool, std::move(staged), threshold));
            if (static_cast<int>(cpu_futures.size()) >= split.cpu_threads) {
                drain_prediction_records(cpu_futures, result, bar);
            }
        }
        ++item_index;
    }
    while (!cpu_futures.empty()) {
        drain_prediction_records(cpu_futures, result, bar);
    }
}

inline void finalize_prediction_result(PredictionRunResult& result,
                                       const std::chrono::steady_clock::time_point& started_at,
                                       const bool sort_records = false) {
    if (sort_records) {
        std::ranges::sort(result.records, [](const PredictionRecord& lhs, const PredictionRecord& rhs) {
            return lhs.dataset_index < rhs.dataset_index;
        });
    }
    result.timing = validate_detail::elapsed_timing(started_at, result.records.size());
}

struct RawImagePredictionLane {
    std::unique_ptr<InferenceBackend> backend;
    std::shared_ptr<PredictionBufferSlotPool> slot_pool;
    c10::cuda::CUDAStream stream = c10::cuda::getDefaultCUDAStream();
    torch::Tensor batch_cpu;
    torch::Tensor batch_gpu;
    torch::Tensor nested_mask;
    struct RawImageTargetSizeWorkspace {
        torch::Tensor target_sizes_host;
        torch::Tensor target_sizes_device;

        void ensure_capacity(std::size_t batch_capacity, int device_id) {
            if (batch_capacity == 0U) {
                return;
            }
            if (target_sizes_host.defined() && target_sizes_host.size(0) >= static_cast<int64_t>(batch_capacity)) {
                return;
            }

            const auto cpu_options =
                torch::TensorOptions().dtype(torch::kInt64).device(torch::kCPU).pinned_memory(true);
            const auto cuda_options = torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA, device_id);
            target_sizes_host = torch::empty({static_cast<int64_t>(batch_capacity), 2}, cpu_options);
            target_sizes_device = torch::empty({static_cast<int64_t>(batch_capacity), 2}, cuda_options);
        }

        void fill_from_items(const std::vector<RawImageBatchItem>& items, std::size_t count,
                             const std::optional<c10::cuda::CUDAStream>& stream = std::nullopt) {
            if (count == 0U) {
                return;
            }
            if (count > items.size()) {
                throw std::runtime_error("raw image target size workspace received too few items");
            }

            auto* host_values = target_sizes_host.data_ptr<int64_t>();
            for (std::size_t index = 0; index < count; ++index) {
                host_values[index * 2U] = items[index].original_height;
                host_values[index * 2U + 1U] = items[index].original_width;
            }

            const auto host_view = target_sizes_host.narrow(0, 0, static_cast<int64_t>(count));
            const auto device_view = target_sizes_device.narrow(0, 0, static_cast<int64_t>(count));
            if (stream.has_value()) {
                c10::cuda::CUDAStreamGuard stream_guard(*stream);
                device_view.copy_(host_view, true);
            } else {
                device_view.copy_(host_view, true);
            }
        }

        void fill_from_item(const RawImageBatchItem& item,
                            const std::optional<c10::cuda::CUDAStream>& stream = std::nullopt) {
            if (!target_sizes_host.defined() || target_sizes_host.size(0) == 0) {
                throw std::runtime_error("raw image target size workspace is not initialized");
            }
            auto* host_values = target_sizes_host.data_ptr<int64_t>();
            host_values[0] = item.original_height;
            host_values[1] = item.original_width;

            const auto host_view = target_sizes_host.narrow(0, 0, 1);
            const auto device_view = target_sizes_device.narrow(0, 0, 1);
            if (stream.has_value()) {
                c10::cuda::CUDAStreamGuard stream_guard(*stream);
                device_view.copy_(host_view, true);
            } else {
                device_view.copy_(host_view, true);
            }
        }

        [[nodiscard]] torch::Tensor target_sizes(std::size_t count) const {
            return target_sizes_device.narrow(0, 0, static_cast<int64_t>(count));
        }
    } target_sizes_workspace;
    std::vector<std::uint8_t> resize_scratch;
    std::int64_t num_queries = 300;
    std::size_t category_count = 0;
};

using RawImageTargetSizeWorkspace = RawImagePredictionLane::RawImageTargetSizeWorkspace;

struct RawImageBatchWorkspace {
    torch::Tensor batch_cpu;
    torch::Tensor batch_gpu;
    torch::Tensor nested_mask;
    RawImageTargetSizeWorkspace target_sizes_workspace;
    std::vector<std::vector<std::uint8_t>> resize_scratch_slots;
};

template <typename WorkspaceT, typename RunModelFn>
inline std::vector<TensorMap> run_raw_prediction_batch(const torch::Tensor& batch_cpu_view,
                                                       const torch::Tensor& batch_gpu_view,
                                                       const std::vector<RawImageBatchItem>& items,
                                                       WorkspaceT& workspace, const torch::Tensor& mean,
                                                       const torch::Tensor& std, const std::size_t max_queries,
                                                       RunModelFn&& run_model) {
    batch_gpu_view.copy_(batch_cpu_view, true);
    workspace.target_sizes_workspace.fill_from_items(items, items.size());
    const torch::Tensor target_sizes = workspace.target_sizes_workspace.target_sizes(items.size());
    const torch::Tensor normalized = batch_gpu_view.sub(mean).div(std).contiguous();
    return postprocess_outputs(std::forward<RunModelFn>(run_model)(normalized), target_sizes, max_queries);
}

template <typename WorkspaceT, typename RunModelFn>
inline std::vector<TensorMap> run_raw_prediction_batch(const torch::Tensor& batch_cpu_view,
                                                       const torch::Tensor& batch_gpu_view,
                                                       const RawImageBatchItem& item, WorkspaceT& workspace,
                                                       const torch::Tensor& mean, const torch::Tensor& std,
                                                       const std::size_t max_queries, RunModelFn&& run_model) {
    batch_gpu_view.copy_(batch_cpu_view, true);
    workspace.target_sizes_workspace.fill_from_item(item);
    const torch::Tensor target_sizes = workspace.target_sizes_workspace.target_sizes(1U);
    const torch::Tensor normalized = batch_gpu_view.sub(mean).div(std).contiguous();
    return postprocess_outputs(std::forward<RunModelFn>(run_model)(normalized), target_sizes, max_queries);
}

template <typename InferenceFn>
inline void run_raw_prediction_batches(const PredictOptions& options, const ResolvedModelArtifacts& artifacts,
                                       WorkerPool& cpu_pool, const std::vector<PredictImageInput>& image_inputs,
                                       const std::size_t category_count, torch::Tensor& batch_cpu,
                                       torch::Tensor& batch_gpu,
                                       std::vector<std::vector<std::uint8_t>>& resize_scratch_slots,
                                       InferenceFn&& infer_batch, std::vector<PredictionRecord>& records,
                                       std::unique_ptr<spdmon::ProgressBar>& bar) {
    const auto batch_capacity = static_cast<size_t>(batch_cpu.size(0));
    if (resize_scratch_slots.size() < batch_capacity) {
        resize_scratch_slots.resize(batch_capacity);
    }
    const std::size_t resize_capacity = static_cast<std::size_t>(artifacts.config.resolution) *
                                        static_cast<std::size_t>(artifacts.config.resolution) * 3U;
    for (auto& scratch : resize_scratch_slots) {
        if (scratch.capacity() < resize_capacity) {
            scratch.reserve(resize_capacity);
        }
    }
    for (size_t start_index = 0; start_index < image_inputs.size(); start_index += batch_capacity) {
        const size_t current_batch = std::min(batch_capacity, image_inputs.size() - start_index);
        torch::Tensor batch_cpu_view = batch_cpu.narrow(0, 0, static_cast<int64_t>(current_batch));
        std::vector<RawImageBatchItem> items =
            load_raw_image_batch(cpu_pool, image_inputs, start_index, current_batch, artifacts.config.resolution,
                                 batch_cpu_view, resize_scratch_slots);
        torch::Tensor batch_gpu_view = batch_gpu.narrow(0, 0, static_cast<int64_t>(current_batch));
        std::vector<TensorMap> outputs = std::forward<InferenceFn>(infer_batch)(batch_cpu_view, batch_gpu_view, items);
        append_raw_prediction_records(records, items, outputs, category_count, options.max_dets_per_image,
                                      options.threshold);
        if (bar) {
            bar->add(current_batch);
        }
    }
}

inline RawImageBatchWorkspace make_raw_image_batch_workspace(const std::size_t batch_capacity,
                                                             const std::int64_t resolution, const int device_id) {
    const std::size_t effective_batch_capacity = std::max<std::size_t>(1, batch_capacity);
    const auto batch_capacity_i64 = static_cast<int64_t>(effective_batch_capacity);
    RawImageBatchWorkspace workspace;
    workspace.batch_cpu =
        torch::empty({batch_capacity_i64, 3, resolution, resolution},
                     torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU).pinned_memory(true));
    workspace.batch_gpu = torch::empty({batch_capacity_i64, 3, resolution, resolution},
                                       torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device_id));
    workspace.nested_mask = torch::zeros({batch_capacity_i64, resolution, resolution},
                                         torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, device_id));
    workspace.target_sizes_workspace.ensure_capacity(effective_batch_capacity, device_id);
    workspace.resize_scratch_slots.resize(effective_batch_capacity);
    return workspace;
}

inline std::pair<torch::Tensor, torch::Tensor> initialize_normalization_tensors(
    int device_id, const std::optional<c10::cuda::CUDAStream>& stream = std::nullopt) {
    if (!stream.has_value()) {
        return make_normalization_tensors(device_id);
    }
    c10::cuda::CUDAStreamGuard stream_guard(*stream);
    return make_normalization_tensors(device_id);
}

struct RawPredictionRuntimeSetup {
    RuntimeContext runtime;
    torch::Tensor mean;
    torch::Tensor std;
};

inline RawPredictionRuntimeSetup make_raw_prediction_runtime_setup(
    const PredictOptions& options, const int runtime_lanes, const int device_id,
    const std::optional<c10::cuda::CUDAStream>& stream = std::nullopt) {
    RawPredictionRuntimeSetup setup{
        RuntimeContext{resolve_runtime_config(options.workers, runtime_lanes, options.cpu_affinity)},
        {},
        {},
    };
    auto normalization = initialize_normalization_tensors(device_id, stream);
    setup.mean = std::move(normalization.first);
    setup.std = std::move(normalization.second);
    return setup;
}

inline BackendExecutionSpec describe_backend_execution(InferenceBackend& backend,
                                                       const ResolvedModelArtifacts& artifacts, const int device_id) {
    return predict_internal::describe_backend_execution(backend, device_id,
                                                        static_cast<std::size_t>(artifacts.config.num_classes));
}

inline std::vector<BackendInferenceLane> make_backend_prediction_lanes(const ResolvedModelArtifacts& artifacts,
                                                                       const std::string& backend_name,
                                                                       const int device_id, const bool allow_fp16,
                                                                       const int lane_count,
                                                                       const std::size_t slot_count) {
    auto backend_lanes =
        predict_internal::make_backend_lanes(artifacts, backend_name, device_id, allow_fp16, lane_count);
    return predict_internal::make_backend_inference_lanes(std::move(backend_lanes), device_id, slot_count,
                                                          static_cast<std::size_t>(artifacts.config.num_classes));
}

inline std::vector<RawImagePredictionLane> make_raw_prediction_lanes(const ResolvedModelArtifacts& artifacts,
                                                                     const std::string& backend_name,
                                                                     const int device_id, const bool allow_fp16,
                                                                     const int lane_count, const std::size_t slot_count,
                                                                     const std::int64_t resolution) {
    std::vector<BackendInferenceLane> backend_lanes =
        make_backend_prediction_lanes(artifacts, backend_name, device_id, allow_fp16, lane_count, slot_count);
    std::vector<RawImagePredictionLane> lanes;
    lanes.reserve(backend_lanes.size());

    c10::cuda::CUDAGuard device_guard(checked_device_index(device_id));
    for (auto& backend_lane : backend_lanes) {
        RawImagePredictionLane lane;
        lane.backend = std::move(backend_lane.backend);
        lane.slot_pool = std::move(backend_lane.slot_pool);
        lane.stream = backend_lane.stream;
        lane.num_queries = backend_lane.num_queries;
        lane.category_count = backend_lane.category_count;

        c10::cuda::CUDAStreamGuard stream_guard(lane.stream);
        RawImageBatchWorkspace workspace = make_raw_image_batch_workspace(1, resolution, device_id);
        lane.batch_cpu = std::move(workspace.batch_cpu);
        lane.batch_gpu = std::move(workspace.batch_gpu);
        lane.nested_mask = std::move(workspace.nested_mask);
        lane.target_sizes_workspace = std::move(workspace.target_sizes_workspace);
        lane.resize_scratch = std::move(workspace.resize_scratch_slots.front());
        lanes.push_back(std::move(lane));
    }
    return lanes;
}

inline std::vector<NativePredictionLane> make_native_prediction_lanes(const int device_id,
                                                                      const std::int64_t image_height,
                                                                      const std::int64_t image_width,
                                                                      const int lane_count,
                                                                      const std::size_t slot_count) {
    c10::cuda::CUDAGuard device_guard(checked_device_index(device_id));
    std::vector<NativePredictionLane> lanes;
    lanes.reserve(static_cast<std::size_t>(std::max(1, lane_count)));
    for (int lane_index = 0; lane_index < lane_count; ++lane_index) {
        const auto lane_stream = get_high_priority_cuda_stream(device_id);
        torch::Tensor nested_mask;
        {
            c10::cuda::CUDAStreamGuard stream_guard(lane_stream);
            nested_mask = torch::zeros({1, image_height, image_width},
                                       torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, device_id));
        }
        lanes.push_back(NativePredictionLane{
            lane_stream,
            std::move(nested_mask),
            std::make_shared<PredictionBufferSlotPool>(slot_count),
        });
    }
    return lanes;
}

struct LiveRunnerState {
    c10::cuda::CUDAStream stream = c10::cuda::getDefaultCUDAStream();
    torch::Tensor mean;
    torch::Tensor std;
    torch::Tensor input_gpu;
    cudaEvent_t ready_event = nullptr;
};

inline LiveRunnerState make_live_runner_state(const ResolvedModelArtifacts& artifacts, int device_id,
                                              const c10::cuda::CUDAStream& stream, const char* ready_event_label) {
    const auto resolution = static_cast<std::int64_t>(artifacts.config.resolution);
    c10::cuda::CUDAGuard device_guard(checked_device_index(device_id));
    c10::cuda::CUDAStreamGuard stream_guard(stream);

    LiveRunnerState state{stream, {}, {}, {}, nullptr};
    auto normalization = initialize_normalization_tensors(device_id, stream);
    state.mean = std::move(normalization.first);
    state.std = std::move(normalization.second);
    state.input_gpu = torch::empty({1, 3, resolution, resolution},
                                   torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device_id));
    ensure_cuda_ok(cudaEventCreateWithFlags(&state.ready_event, cudaEventDisableTiming), ready_event_label);
    return state;
}

inline void destroy_cuda_event(cudaEvent_t& event) noexcept {
    if (event != nullptr) {
        cudaEventDestroy(event);
        event = nullptr;
    }
}

inline int live_image_id(const std::uint64_t frame_id) {
    return frame_id > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max()
                                                                                  : static_cast<int>(frame_id);
}

inline std::vector<LiveSplitRegion> build_horizontal_splits(const std::uint32_t total_width,
                                                            const std::uint32_t requested_splits) {
    std::vector<LiveSplitRegion> splits;
    if (total_width == 0U) {
        return splits;
    }

    const std::uint32_t split_count = std::max<std::uint32_t>(1U, std::min(requested_splits, total_width));
    splits.reserve(split_count);

    const std::uint32_t base_width = total_width / split_count;
    const std::uint32_t remainder = total_width % split_count;
    std::uint32_t x = 0;
    for (std::uint32_t split_index = 0; split_index < split_count; ++split_index) {
        const std::uint32_t width = base_width + (split_index < remainder ? 1U : 0U);
        if (width == 0U) {
            continue;
        }
        splits.push_back(LiveSplitRegion{x, width});
        x += width;
    }
    return splits;
}

inline std::string format_live_split_context(const std::uint64_t frame_id, const std::uint32_t split_index,
                                             const std::size_t src_pitch_bytes, const std::uint32_t src_width,
                                             const std::uint32_t src_height, const std::uint32_t dst_width,
                                             const std::uint32_t dst_height) {
    return std::format("frame_id={} split_index={} src={}x{} src_pitch_bytes={} dst={}x{}", frame_id, split_index,
                       src_width, src_height, src_pitch_bytes, dst_width, dst_height);
}

inline mmltk::live::LiveCaptureRegion make_split_region(const mmltk::live::LiveCaptureRegion& frame_region,
                                                        const LiveSplitRegion& split) {
    return mmltk::live::LiveCaptureRegion{
        frame_region.x + split.x,
        frame_region.y,
        split.width,
        frame_region.height,
    };
}

inline OutputTensors live_output_tensors(const ModelOutputs& outputs, const bool include_masks) {
    OutputTensors tensors = to_output_tensors(outputs);
    if (!include_masks) {
        tensors.pred_masks.reset();
    }
    return tensors;
}

inline LiveOverlaySelection select_live_overlay_for_preview(const TensorMap& result, const std::size_t category_count,
                                                            const std::size_t max_dets_per_image, const float threshold,
                                                            const int num_classes, const std::uint32_t split_width,
                                                            const std::uint32_t split_height,
                                                            const cudaStream_t stream) {
    const auto score_it = result.find("scores");
    const auto label_it = result.find("labels");
    const auto box_it = result.find("boxes");
    if (score_it == result.end() || label_it == result.end() || box_it == result.end()) {
        throw std::runtime_error("live overlay selection requires scores, labels, and boxes");
    }

    torch::Tensor scores = score_it->second.reshape({-1});
    torch::Tensor labels = label_it->second.reshape({-1}).to(torch::kInt64);
    torch::Tensor boxes = box_it->second;
    if (boxes.dim() != 2 || boxes.size(1) != 4) {
        throw std::runtime_error("live overlay selection requires boxes shaped [num_predictions,4]");
    }

    torch::Tensor valid = torch::logical_and(labels.ge(0), labels.lt(static_cast<std::int64_t>(category_count)));
    if (threshold > 0.0f) {
        valid = torch::logical_and(valid, scores.ge(threshold));
    }

    torch::Tensor indices = torch::nonzero(valid).flatten();
    const std::int64_t selected_count =
        std::min<std::int64_t>(indices.size(0), static_cast<std::int64_t>(max_dets_per_image));
    if (selected_count <= 0) {
        return {};
    }
    if (indices.size(0) > selected_count) {
        indices = indices.narrow(0, 0, selected_count).contiguous();
    }

    LiveOverlaySelection selection;
    selection.boxes_xyxy = boxes.index_select(0, indices).to(torch::kFloat32).contiguous();
    selection.boxes_xyxy.select(1, 0).clamp_(0.0, static_cast<double>(split_width));
    selection.boxes_xyxy.select(1, 1).clamp_(0.0, static_cast<double>(split_height));
    selection.boxes_xyxy.select(1, 2).clamp_(0.0, static_cast<double>(split_width));
    selection.boxes_xyxy.select(1, 3).clamp_(0.0, static_cast<double>(split_height));
    selection.labels_zero_based = labels.index_select(0, indices).to(torch::kInt32).contiguous();
    selection.scores = scores.index_select(0, indices).to(torch::kFloat32).contiguous();

    torch::Tensor valid_boxes =
        torch::logical_and(selection.boxes_xyxy.select(1, 2).gt(selection.boxes_xyxy.select(1, 0)),
                           selection.boxes_xyxy.select(1, 3).gt(selection.boxes_xyxy.select(1, 1)));
    torch::Tensor valid_box_indices = torch::nonzero(valid_boxes).flatten();
    if (valid_box_indices.numel() == 0) {
        return {};
    }
    if (valid_box_indices.size(0) != selection.labels_zero_based.size(0)) {
        selection.boxes_xyxy = selection.boxes_xyxy.index_select(0, valid_box_indices).contiguous();
        selection.labels_zero_based = selection.labels_zero_based.index_select(0, valid_box_indices).contiguous();
        selection.scores = selection.scores.index_select(0, valid_box_indices).contiguous();
        indices = indices.index_select(0, valid_box_indices).contiguous();
    }

    selection.colors_rgb =
        torch::empty({selection.labels_zero_based.size(0), 3},
                     torch::TensorOptions().dtype(torch::kUInt8).device(selection.labels_zero_based.device()));
    launch_build_instance_colors_from_zero_based_labels(
        selection.labels_zero_based.data_ptr<int>(), static_cast<std::size_t>(selection.labels_zero_based.size(0)),
        num_classes, selection.colors_rgb.data_ptr<std::uint8_t>(), stream);
    ensure_cuda_ok(cudaPeekAtLastError(), "live overlay color generation");

    const auto mask_it = result.find("masks");
    if (mask_it != result.end()) {
        torch::Tensor masks = mask_it->second;
        if (masks.dim() == 4 && masks.size(1) == 1) {
            masks = masks.squeeze(1);
        }
        if (masks.dim() != 3) {
            throw std::runtime_error("live overlay masks must be [num_predictions,height,width]");
        }
        selection.masks = masks.index_select(0, indices).to(torch::kBool).contiguous();
    }

    return selection;
}

template <typename RunModelFn>
inline LiveFrameRenderData run_live_frame_pipeline(
    const LiveFrameInputs& frame, const std::uint32_t split_count, const std::size_t max_dets_per_image,
    const float threshold, const std::size_t category_count, const int num_classes, const std::uint32_t resolution,
    const c10::cuda::CUDAStream& stream, const int device_id, const cudaEvent_t ready_event, torch::Tensor& input_gpu,
    const torch::Tensor& mean, const torch::Tensor& std, const bool include_masks, const bool include_status_detections,
    const std::int64_t max_queries, RunModelFn&& run_model, const char* runtime_label) {
    LiveFrameRenderData out;
    const std::string runtime_label_string = runtime_label != nullptr ? runtime_label : "runtime";
    const std::vector<LiveSplitRegion> splits = build_horizontal_splits(frame.region.width, split_count);
    if (splits.empty()) {
        throw std::runtime_error("RF-DETR live analyzer split list is empty");
    }

    const auto cuda_stream = reinterpret_cast<cudaStream_t>(stream.stream());
    ensure_cuda_ok(cudaStreamWaitEvent(cuda_stream, frame.ready_event, 0),
                   ("cudaStreamWaitEvent for live " + runtime_label_string + " frame").c_str());

    c10::cuda::CUDAGuard device_guard(checked_device_index(device_id));
    torch::NoGradGuard no_grad;
    out.splits.reserve(splits.size());
    for (std::size_t split_index = 0; split_index < splits.size(); ++split_index) {
        const LiveSplitRegion split = splits[split_index];
        const mmltk::live::LiveCaptureRegion split_region = make_split_region(frame.region, split);
        const auto* split_ptr = frame.data + static_cast<std::size_t>(split.x) * 3U;
        LiveSplitRenderData split_render;
        split_render.source_region = split_region;
        {
            c10::cuda::CUDAStreamGuard stream_guard(stream);
            if (const char* arg_error = validate_bgr_split_to_planar_float_args(
                    frame.pitch_bytes, split.width, frame.region.height, resolution, resolution, cuda_stream)) {
                throw std::runtime_error("RF-DETR live " + runtime_label_string +
                                         " preprocess arguments are invalid: " +
                                         format_live_split_context(
                                             frame.frame_id, static_cast<std::uint32_t>(split_index), frame.pitch_bytes,
                                             split.width, frame.region.height, resolution, resolution) +
                                         " reason=" + arg_error);
            }
            ensure_cuda_ok(
                launch_bgr_split_to_planar_float(split_ptr, frame.pitch_bytes, split.width, frame.region.height,
                                                 input_gpu.data_ptr<float>(), resolution, resolution, cuda_stream),
                ("launch_bgr_split_to_planar_float for live " + runtime_label_string + " frame: " +
                 format_live_split_context(frame.frame_id, static_cast<std::uint32_t>(split_index), frame.pitch_bytes,
                                           split.width, frame.region.height, resolution, resolution))
                    .c_str());
            input_gpu.sub_(mean).div_(std);
            OutputTensors model_outputs = run_model();
            if (!include_masks) {
                model_outputs.pred_masks.reset();
            }
            const std::vector<TensorMap> outputs =
                postprocess_outputs_fixed_size(model_outputs, split_region.height, split_region.width, max_queries);
            ensure_cuda_ok(cudaPeekAtLastError(), "live frame postprocess");
            if (include_status_detections) {
                split_render.detections =
                    filter_threshold(result_to_predictions(live_image_id(frame.frame_id), outputs.front(),
                                                           category_count, max_dets_per_image),
                                     threshold);
            }
            LiveOverlaySelection overlay =
                select_live_overlay_for_preview(outputs.front(), category_count, max_dets_per_image, threshold,
                                                num_classes, split_region.width, split_region.height, cuda_stream);
            split_render.boxes_xyxy = std::move(overlay.boxes_xyxy);
            split_render.labels_zero_based = std::move(overlay.labels_zero_based);
            split_render.scores = std::move(overlay.scores);
            split_render.colors_rgb = std::move(overlay.colors_rgb);
            split_render.masks = std::move(overlay.masks);
        }
        out.splits.push_back(std::move(split_render));
    }
    ensure_cuda_ok(cudaEventRecord(ready_event, cuda_stream),
                   ("cudaEventRecord for live " + runtime_label_string + " frame").c_str());
    out.ready_event = ready_event;
    out.producer_stream = cuda_stream;
    return out;
}

}  // namespace mmltk::rfdetr::predict_internal
