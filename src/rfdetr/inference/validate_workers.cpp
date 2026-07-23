#include "rfdetr/validate_internal.h"

#include "rfdetr/inference/backend_factory.h"
#include "rfdetr/torch_cuda_utils.h"
#include "rfdetr/postprocess.h"
#include "rfdetr/predict_runtime_shared.h"
#include "spdmon/spdmon.hpp"
#include "rfdetr/runtime.h"
#include "profile_utils.h"
#include "worker_pool.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <c10/core/InferenceMode.h>
#include <cuda_runtime.h>
#include <torch/torch.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

using namespace validate_detail;

namespace {

class CudaEvent final {
   public:
    CudaEvent() {
        ensure_cuda_ok(cudaEventCreateWithFlags(&event_, cudaEventDisableTiming),
                       "cudaEventCreateWithFlags for validation dispatch");
    }

    ~CudaEvent() {
        if (event_ != nullptr) {
            cudaEventDestroy(event_);
        }
    }

    [[nodiscard]] cudaEvent_t get() const {
        return event_;
    }

   private:
    cudaEvent_t event_ = nullptr;
};

}  

ValidationBackendResult run_backend_eval_parallel_impl(const ValidationOptions& options, const CocoDataset& dataset,
                                                       const InferenceBackend& backend) {
    const ModelInfo& model_info = backend.info();
    const RuntimeContext runtime(resolve_runtime_config(options.workers, static_cast<int>(options.batch_size),
                                                        checked_prefetch_factor(options.prefetch_factor),
                                                        options.cpu_affinity));
    DatasetLoader loader(make_loader_config(options, runtime));
    CocoDataset dataset_copy = dataset;

    std::unique_ptr<spdmon::ProgressBar> bar;
    if (interactive_logs(options)) {
        bar = std::make_unique<spdmon::ProgressBar>(model_info.backend, dataset_copy.num_images(), "img");
    }

    const auto started = std::chrono::steady_clock::now();
    c10::InferenceMode inference_mode;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    const auto preprocess_stream = get_high_priority_cuda_stream(options.device_id);
    auto evaluation_profile = make_standalone_evaluation_profile(options, model_info, dataset_copy);
    if (evaluation_profile) {
        evaluation_profile->started_at = started;
    }
    constexpr size_t kLoaderBatchSize = 1;
    const size_t slot_count = predict_internal::prediction_lane_slot_count(runtime.split(), kLoaderBatchSize);
    const size_t max_cpu_futures = predict_internal::prediction_cpu_batch_limit(runtime.split(), kLoaderBatchSize);
    auto lanes = predict_internal::make_backend_inference_lanes(
        backend.make_lanes(runtime.split().lane_threads), options.device_id, slot_count, dataset_copy.num_categories(),
        static_cast<int64_t>(kLoaderBatchSize),
        dataset_copy.metric_set() == EvaluationMetricSet::BBoxAndMask
            ? std::make_optional(std::make_pair(loader.image_height(), loader.image_width()))
            : std::nullopt);
    const auto image_height = static_cast<int64_t>(loader.image_height());
    const auto image_width = static_cast<int64_t>(loader.image_width());
    for (auto& lane : lanes) {
        lane.preprocessor = std::make_unique<GpuBatchPreprocessor>(
            static_cast<int64_t>(kLoaderBatchSize), static_cast<int>(image_height), static_cast<int>(image_width),
            options.device_id, at::kFloat);
    }

    WorkerPool lane_pool(static_cast<size_t>(lanes.size()), runtime.lane_cpus(), "rfdlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();
    std::unique_ptr<EvaluationCudaTimingPool> cuda_timing_pool;
    std::deque<EvaluationCudaTimingLease> lane_timing_leases;
    std::deque<EvaluationCudaTimingLease> cpu_timing_leases;
    if (evaluation_profile) {
        cuda_timing_pool = std::make_unique<EvaluationCudaTimingPool>(
            static_cast<size_t>(runtime.split().lane_threads) + max_cpu_futures);
    }

    auto drain_lane = [&](std::deque<std::future<StagedPredictionBatch>>& lane_futures,
                          std::deque<PendingPredictionBatchEncoding>& cpu_futures) {
        MMLTK_NVTX_RANGE("drain_lane", NVTX_COLOR_PURPLE);
        MMLTK_PROFILE_SCOPE("rfdetr.native.eval.drain_lane");
        StagedPredictionBatch staged = lane_futures.front().get();
        lane_futures.pop_front();
        if (evaluation_profile) {
            cpu_timing_leases.push_back(lane_timing_leases.front());
            lane_timing_leases.pop_front();
        }
        if (evaluation_profile) {
            const auto tensor_bytes = [](const torch::Tensor& tensor) {
                return static_cast<size_t>(tensor.numel()) * static_cast<size_t>(tensor.element_size());
            };
            const size_t mask_bytes = staged.mask ? tensor_bytes(staged.mask->masks_cpu) : 0;
            evaluation_profile->transferred_bytes += tensor_bytes(staged.bbox.scores_cpu) +
                                                     tensor_bytes(staged.bbox.labels_cpu) +
                                                     tensor_bytes(staged.bbox.boxes_cpu) + mask_bytes;
            evaluation_profile->mask_transferred_bytes += mask_bytes;
        }
        cpu_futures.push_back(
            enqueue_prediction_batch_encoding(cpu_pool, std::move(staged), evaluation_profile.get(), &dataset_copy));
        if (evaluation_profile) {
            size_t task_count = lane_futures.size();
            for (const PendingPredictionBatchEncoding& pending : cpu_futures) {
                task_count += pending.images.size();
            }
            evaluation_profile->peak_in_flight_tasks = std::max(evaluation_profile->peak_in_flight_tasks, task_count);
            evaluation_profile->peak_in_flight_slots =
                std::max(evaluation_profile->peak_in_flight_slots, lane_futures.size() + cpu_futures.size());
        }
    };
    auto drain_cpu = [&](std::deque<PendingPredictionBatchEncoding>& cpu_futures, size_t& processed) {
        MMLTK_NVTX_RANGE("drain_cpu", NVTX_COLOR_GREEN);
        MMLTK_PROFILE_SCOPE("rfdetr.native.eval.drain_cpu");
        std::vector<PredictionBatchItem> completed = collect_prediction_batch_encoding(std::move(cpu_futures.front()));
        cpu_futures.pop_front();
        if (evaluation_profile) {
            EvaluationCudaTimingLease& timing = cpu_timing_leases.front();
            timing.timing->accumulate(*evaluation_profile);
            cuda_timing_pool->release(timing);
            cpu_timing_leases.pop_front();
        }
        for (auto& image : completed) {
            if (!image.evaluation_matches) {
                throw std::logic_error("validation image task omitted compact evaluation matches");
            }
            dataset_copy.merge_matches(std::move(*image.evaluation_matches));
        }
        processed += completed.size();
        if (bar) {
            bar->add(completed.size());
        }
    };

    loader.begin_epoch();
    std::deque<std::future<StagedPredictionBatch>> lane_futures;
    std::deque<PendingPredictionBatchEncoding> cpu_futures;
    Batch batch{};
    size_t processed = 0;
    size_t submitted = 0;
    size_t submitted_images = 0;
    while (loader.next_batch(batch)) {
        if (submitted_images >= dataset_copy.num_images()) {
            loader.release_batch(batch);
            break;
        }
        if (evaluation_profile) {
            const auto wait_started = std::chrono::steady_clock::now();
            loader.wait_batch(batch);
            evaluation_profile->loader_wait_seconds +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - wait_started).count();
        } else {
            loader.wait_batch(batch);
        }
        const size_t remaining_images = dataset_copy.num_images() - submitted_images;
        const size_t stage_count = std::min(batch.num_images, remaining_images);
        std::vector<PredictionBatchMetadata> metadata;
        metadata.reserve(stage_count);
        for (size_t image_index = 0; image_index < stage_count; ++image_index) {
            const auto dataset_index = static_cast<int64_t>(batch.image_indices[image_index]);
            const int64_t image_id = dataset_index + 1;
            if (!dataset_copy.has_image(static_cast<int>(image_id))) {
                break;
            }
            metadata.push_back(PredictionBatchMetadata{dataset_index, image_id, {}});
        }
        if (metadata.empty()) {
            loader.release_batch(batch);
            continue;
        }
        const size_t lane_index = submitted % lanes.size();
        void* producer_stream = reinterpret_cast<void*>(preprocess_stream.stream());
        EvaluationCudaTimingLease batch_timing;
        if (evaluation_profile) {
            batch_timing = cuda_timing_pool->acquire();
            batch_timing.timing->record_start(EvaluationCudaBatchTiming::Phase::Preprocessing,
                                              reinterpret_cast<cudaStream_t>(producer_stream));
        }
        torch::Tensor normalized;
        {
            c10::cuda::CUDAStreamGuard stream_guard(preprocess_stream);
            LoaderBatchGuard batch_guard(loader, batch, options.device_id);
            normalized = lanes[lane_index].preprocessor->run(batch);
            if (batch_timing) {
                batch_timing.timing->record_stop(EvaluationCudaBatchTiming::Phase::Preprocessing,
                                                 reinterpret_cast<cudaStream_t>(producer_stream));
            }
            batch_guard.release();
        }
        auto normalized_ready = std::make_shared<CudaEvent>();
        ensure_cuda_ok(cudaEventRecord(normalized_ready->get(), reinterpret_cast<cudaStream_t>(producer_stream)),
                       "cudaEventRecord for normalized validation batch");
        ++submitted;
        submitted_images += metadata.size();
        lane_futures.push_back(lane_pool.enqueue([&lanes, lane_index, normalized = std::move(normalized),
                                                  normalized_ready = std::move(normalized_ready), batch_timing,
                                                  metadata = std::move(metadata), max_dets = options.eval_max_dets,
                                                  image_height, image_width, device_id = options.device_id]() mutable {
            auto& lane = lanes[lane_index];
            PredictionBufferLease lease = lane.slot_pool->acquire();
            ensure_cuda_ok(
                cudaStreamWaitEvent(reinterpret_cast<cudaStream_t>(lane.stream.stream()), normalized_ready->get(), 0),
                "cudaStreamWaitEvent for normalized validation batch");
            c10::InferenceMode lane_inference_mode;
            c10::cuda::CUDAGuard lane_device_guard(checked_device_index(device_id));
            c10::cuda::CUDAStreamGuard stream_guard(lane.stream);
            normalized.record_stream(lane.stream);
            const auto lane_stream = reinterpret_cast<cudaStream_t>(lane.stream.stream());
            if (batch_timing) {
                batch_timing.timing->record_start(EvaluationCudaBatchTiming::Phase::ModelForward, lane_stream);
            }
            auto raw_outputs = lane.backend->run(normalized);
            lane.preprocessor->record_consumer(lane.stream.stream());
            if (batch_timing) {
                batch_timing.timing->record_stop(EvaluationCudaBatchTiming::Phase::ModelForward, lane_stream);
                batch_timing.timing->record_start(EvaluationCudaBatchTiming::Phase::Postprocess, lane_stream);
            }
            auto results =
                postprocess_output_batch_fixed_size(raw_outputs, image_height, image_width, lane.num_queries);
            if (batch_timing) {
                batch_timing.timing->record_stop(EvaluationCudaBatchTiming::Phase::Postprocess, lane_stream);
            }
            return stage_prediction_batch(std::move(metadata), std::move(results), lane.category_count, max_dets,
                                          std::move(lease), device_id, lane.backend->stream());
        }));
        if (batch_timing) {
            lane_timing_leases.push_back(batch_timing);
        }
        if (evaluation_profile) {
            evaluation_profile->peak_in_flight_tasks =
                std::max(evaluation_profile->peak_in_flight_tasks, lane_futures.size() + cpu_futures.size());
            evaluation_profile->peak_in_flight_slots =
                std::max(evaluation_profile->peak_in_flight_slots, lane_futures.size() + cpu_futures.size());
        }

        if (static_cast<int>(lane_futures.size()) >= runtime.split().lane_threads) {
            drain_lane(lane_futures, cpu_futures);
        }
        if (cpu_futures.size() >= max_cpu_futures) {
            drain_cpu(cpu_futures, processed);
        }
    }

    while (!lane_futures.empty()) {
        drain_lane(lane_futures, cpu_futures);
        if (cpu_futures.size() >= max_cpu_futures) {
            drain_cpu(cpu_futures, processed);
        }
    }
    while (!cpu_futures.empty()) {
        drain_cpu(cpu_futures, processed);
    }
    if (bar) {
        bar->close();
    }

    return ValidationBackendResult{
        model_info,
        dataset_copy.evaluate(options.eval_max_dets, evaluation_profile.get(), &cpu_pool),
        elapsed_timing(started, dataset_copy.num_images()),
    };
}

}  
