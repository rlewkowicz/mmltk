#include "rfdetr/validate_internal.h"

#include "rfdetr/inference/backend_factory.h"
#include "rfdetr/cuda_utils.h"
#include "rfdetr/postprocess.h"
#include "rfdetr/predict_runtime_shared.h"
#include "spdmon/spdmon.hpp"
#include "rfdetr/runtime.h"
#include "profile_utils.h"
#include "worker_pool.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
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

struct ParallelInferenceLane {
    std::unique_ptr<InferenceBackend> backend;
    std::shared_ptr<PredictionBufferSlotPool> slot_pool;
};

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

    [[nodiscard]] cudaEvent_t get() const { return event_; }

private:
    cudaEvent_t event_ = nullptr;
};

} // namespace

ValidationBackendResult run_backend_eval_parallel_impl(const ValidationOptions& options,
                                                       const CocoDataset& source_dataset,
                                                       const std::string& backend_name,
                                                       const ModelInfo& model_info,
                                                       const torch::Tensor& mean,
                                                       const torch::Tensor& std) {
    const RuntimeContext runtime(resolve_runtime_config(
        options.workers,
        static_cast<int>(options.batch_size),
        options.cpu_affinity));
    DatasetLoader loader(make_loader_config(options, runtime));
    CocoDataset dataset = source_dataset;

    std::unique_ptr<spdmon::ProgressBar> bar;
    if (interactive_logs(options)) {
        bar = std::make_unique<spdmon::ProgressBar>(model_info.backend, dataset.num_images(), "img");
    }

    const auto started = std::chrono::steady_clock::now();
    torch::NoGradGuard no_grad;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    const auto preprocess_stream =
        get_high_priority_cuda_stream(options.device_id);

    InferenceBackendFactory backend_factory(
        options.onnx_path,
        options.tensorrt_path,
        options.device_id,
        options.allow_fp16,
        options.save_engine_path);
    auto lane_backends = backend_factory.make_backend_lanes(backend_name, runtime.split().lane_threads);
    std::vector<ParallelInferenceLane> lanes;
    lanes.reserve(lane_backends.size());
    const size_t slot_count = predict_internal::prediction_lane_slot_count(runtime.split());
    for (auto& backend : lane_backends) {
        ParallelInferenceLane lane{
            std::move(backend),
            std::make_shared<PredictionBufferSlotPool>(slot_count),
        };
        lanes.push_back(std::move(lane));
    }

    WorkerPool lane_pool(static_cast<size_t>(lanes.size()),
                         runtime.lane_cpus(),
                         "rfdlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();

    auto drain_lane = [&](std::deque<std::future<StagedPredictionBatch>>& lane_futures,
                          std::deque<std::future<std::vector<Prediction>>>& cpu_futures) {
        MMLTK_NVTX_RANGE("drain_lane", NVTX_COLOR_PURPLE);
        MMLTK_PROFILE_SCOPE("rfdetr.native.eval.drain_lane");
        StagedPredictionBatch staged = lane_futures.front().get();
        lane_futures.pop_front();
        cpu_futures.push_back(cpu_pool.enqueue([staged = std::move(staged)]() mutable {
            return encode_staged_predictions(std::move(staged));
        }));
    };
    auto drain_cpu = [&](std::deque<std::future<std::vector<Prediction>>>& cpu_futures, size_t& processed) {
        MMLTK_NVTX_RANGE("drain_cpu", NVTX_COLOR_GREEN);
        MMLTK_PROFILE_SCOPE("rfdetr.native.eval.drain_cpu");
        std::vector<Prediction> predictions = cpu_futures.front().get();
        cpu_futures.pop_front();
        dataset.add_predictions(std::move(predictions));
        ++processed;
        if (bar) {
            bar->add(1);
        }
    };

    loader.begin_epoch();
    std::deque<std::future<StagedPredictionBatch>> lane_futures;
    std::deque<std::future<std::vector<Prediction>>> cpu_futures;
    Batch batch{};
    size_t processed = 0;
    size_t submitted = 0;
    size_t submitted_images = 0;
    const auto image_height = static_cast<int64_t>(loader.image_height());
    const auto image_width = static_cast<int64_t>(loader.image_width());
    while (loader.next_batch(batch)) {
        if (submitted_images >= dataset.num_images()) {
            loader.release_batch(batch);
            break;
        }
        loader.wait_batch(batch);
        const int image_id = static_cast<int>(batch.image_indices[0]) + 1;
        if (!dataset.has_image(image_id)) {
            loader.release_batch(batch);
            continue;
        }
        void* producer_stream = reinterpret_cast<void*>(preprocess_stream.stream());
        loader.handoff_batch(batch, producer_stream);
        torch::Tensor normalized;
        {
            c10::cuda::CUDAStreamGuard stream_guard(preprocess_stream);
            normalized =
                normalize_batch(batch, options.device_id, loader.image_height(), loader.image_width(), mean, std)
                    .contiguous();
        }
        auto normalized_ready = std::make_shared<CudaEvent>();
        ensure_cuda_ok(
            cudaEventRecord(
                normalized_ready->get(),
                reinterpret_cast<cudaStream_t>(producer_stream)),
            "cudaEventRecord for normalized validation batch");
        loader.release_batch(batch, producer_stream);
        const size_t lane_index = submitted % lanes.size();
        ++submitted;
        ++submitted_images;
        lane_futures.push_back(lane_pool.enqueue([&lanes,
                                                  lane_index,
                                                  normalized = std::move(normalized),
                                                  normalized_ready = std::move(normalized_ready),
                                                  image_id,
                                                  category_count = dataset.num_categories(),
                                                  max_dets = options.eval_max_dets,
                                                  image_height,
                                                  image_width,
                                                  device_id = options.device_id]() mutable {
            ParallelInferenceLane& lane = lanes[lane_index];
            PredictionBufferLease lease = lane.slot_pool->acquire();
            const auto lane_stream = backend_cuda_stream(*lane.backend, device_id);
            ensure_cuda_ok(
                cudaStreamWaitEvent(reinterpret_cast<cudaStream_t>(lane_stream.stream()),
                                    normalized_ready->get(),
                                    0),
                "cudaStreamWaitEvent for normalized validation batch");
            torch::NoGradGuard lane_no_grad;
            c10::cuda::CUDAGuard lane_device_guard(checked_device_index(device_id));
            c10::cuda::CUDAStreamGuard stream_guard(lane_stream);
            normalized.record_stream(lane_stream);
            const auto results =
                postprocess_outputs_fixed_size(lane.backend->run(normalized),
                                               image_height,
                                               image_width,
                                               lane.backend->info().num_queries > 0 ? lane.backend->info().num_queries : 300);
            return stage_result_to_predictions(
                image_id,
                results.front(),
                category_count,
                max_dets,
                std::move(lease),
                device_id,
                lane.backend->stream());
        }));

        if (static_cast<int>(lane_futures.size()) >= runtime.split().lane_threads) {
            drain_lane(lane_futures, cpu_futures);
        }
        if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
            drain_cpu(cpu_futures, processed);
        }
    }

    while (!lane_futures.empty()) {
        drain_lane(lane_futures, cpu_futures);
        if (static_cast<int>(cpu_futures.size()) >= runtime.split().cpu_threads) {
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
        dataset.evaluate(options.eval_max_dets),
        elapsed_timing(started, dataset.num_images()),
    };
}

} // namespace mmltk::rfdetr
