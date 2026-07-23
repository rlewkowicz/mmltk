#include "mmltk/rfdetr/validate.h"
#include "rfdetr/validate_internal.h"

#include "dataset_compiler.h"
#include "compile_progress_monitor.h"
#include "dataset_loader.h"
#include "rfdetr/onnx_tool_shared.h"
#include "rfdetr/torch_cuda_utils.h"
#include "rfdetr/postprocess.h"
#include "spdmon/spdmon.hpp"
#include "rfdetr/inference/backend_factory.h"
#include "rfdetr/runtime.h"
#include "rfdetr/weights_validation.h"
#include "rfdetr/compiled_input_resolution.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <c10/core/InferenceMode.h>
#include <cuda_runtime.h>
#include <nlohmann/json.hpp>
#include <torch/torch.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

using namespace validate_detail;
using json = nlohmann::json;

namespace {

class CudaEvent final {
   public:
    CudaEvent() {
        ensure_cuda_ok(cudaEventCreateWithFlags(&event_, cudaEventDisableTiming),
                       "cudaEventCreateWithFlags for validation alignment");
    }

    ~CudaEvent() {
        if (event_ != nullptr) {
            cudaEventDestroy(event_);
        }
    }

    [[nodiscard]] cudaEvent_t get() const noexcept {
        return event_;
    }

   private:
    cudaEvent_t event_ = nullptr;
};

void log_line(const ValidationOptions& options, const std::string& line) {
    if (interactive_logs(options)) {
        spdmon::ProgressBar::log(line);
    }
}

std::string trim_copy(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> split_csv(const std::string& value) {
    std::vector<std::string> items;
    std::string current;
    for (char ch : value) {
        if (ch == ',') {
            const std::string item = trim_copy(current);
            if (!item.empty()) {
                items.push_back(item);
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    const std::string item = trim_copy(current);
    if (!item.empty()) {
        items.push_back(item);
    }
    return items;
}

void compile_dataset(const ValidationOptions& options) {
    log_line(options, "dataset: compiling " + options.source_dir.string() + "/" + options.split + " -> " +
                          options.compiled_path.parent_path().string());

    size_t last_done = 0;
    size_t total = 0;
    std::unique_ptr<spdmon::ProgressBar> bar;
    if (interactive_logs(options)) {
        bar = std::make_unique<spdmon::ProgressBar>("compile", 0, "img");
    }

    CompilerConfig config;
    config.source_dir = options.source_dir.string();
    config.output_dir = options.compiled_path.parent_path().string();
    config.split = options.split;
    config.target_width = options.resolution;
    config.target_height = options.resolution;
    config.num_workers = options.compile_workers;
    config.cuda_mask_batch_size = options.compile_cuda_mask_batch_size;
    config.cuda_device_id = options.compile_cuda_device_id;
    const DatasetCompilePlan plan = DatasetCompiler::prepare(config, {config.split});
    if (bar) {
        size_t progress_pulse = 0;
        compile_with_progress(plan, 0U, [&bar, &last_done, &total, &progress_pulse](const CompileProgress& progress) {
            if (progress.total != total) {
                total = progress.total;
                bar->set_total(total);
            }
            if (progress.done > last_done) {
                bar->add(progress.done - last_done);
                last_done = progress.done;
            }
            bar->set_postfix(spdmon::format_compile_postfix(progress, progress_pulse++));
        });
        bar->close();
        return;
    }

    DatasetCompiler::compile(plan, 0U);
}

void ensure_compiled_dataset(const ValidationOptions& options) {
    if (options.recompile) {
        compile_dataset(options);
    }
    if (!std::filesystem::exists(options.compiled_path)) {
        throw std::runtime_error("missing compiled dataset: " + options.compiled_path.string());
    }
}

void validate_loader_shape(const ValidationOptions& options) {
    if (options.batch_size == 0) {
        throw std::runtime_error("native RF-DETR validation requires batch_size >= 1");
    }
    if (options.prefetch_factor == 0) {
        throw std::runtime_error("native RF-DETR validation requires prefetch_factor >= 1");
    }
    if (options.workers < 0) {
        throw std::runtime_error("native RF-DETR validation requires workers >= 0");
    }
}

void print_model_metadata_impl(const ModelInfo& info, size_t images, size_t categories, ValidationLogMode log_mode) {
    if (log_mode != ValidationLogMode::Interactive) {
        return;
    }
    spdmon::ProgressBar::log(
        "model[" + info.backend + "]: path=" + info.model_path + " input=" + info.input.name + " " +
        format_shape(info.input.shape) + " outputs=" + std::to_string(info.outputs.size()) +
        " queries=" + std::to_string(info.num_queries) + " classes=" + std::to_string(info.num_classes) +
        " dataset_images=" + std::to_string(images) + " dataset_classes=" + std::to_string(categories));
    for (const TensorInfo& output : info.outputs) {
        spdmon::ProgressBar::log("  output: " + output.name + " " + format_shape(output.shape) + " " + output.dtype);
    }
}

AlignmentStats merge_alignment(const AlignmentStats& lhs, const AlignmentStats& rhs) {
    AlignmentStats out;
    out.images_compared = lhs.images_compared + rhs.images_compared;
    if (out.images_compared == 0) {
        return out;
    }
    const auto weighted_mean = [total = out.images_compared](double lhs_value, size_t lhs_count, double rhs_value,
                                                             size_t rhs_count) {
        return ((lhs_value * static_cast<double>(lhs_count)) + (rhs_value * static_cast<double>(rhs_count))) /
               static_cast<double>(total);
    };
    out.top1_score_abs_diff_mean = weighted_mean(lhs.top1_score_abs_diff_mean, lhs.images_compared,
                                                 rhs.top1_score_abs_diff_mean, rhs.images_compared);
    out.top1_box_abs_diff_px_mean = weighted_mean(lhs.top1_box_abs_diff_px_mean, lhs.images_compared,
                                                  rhs.top1_box_abs_diff_px_mean, rhs.images_compared);
    out.top1_mask_xor_pixels_mean = weighted_mean(lhs.top1_mask_xor_pixels_mean, lhs.images_compared,
                                                  rhs.top1_mask_xor_pixels_mean, rhs.images_compared);
    out.top1_score_abs_diff_max = std::max(lhs.top1_score_abs_diff_max, rhs.top1_score_abs_diff_max);
    out.top1_box_abs_diff_px_max = std::max(lhs.top1_box_abs_diff_px_max, rhs.top1_box_abs_diff_px_max);
    out.top1_mask_xor_pixels_max = std::max(lhs.top1_mask_xor_pixels_max, rhs.top1_mask_xor_pixels_max);
    return out;
}

RuntimeContext make_validation_runtime_context(const ValidationOptions& options) {
    return RuntimeContext(resolve_runtime_config(options.workers, static_cast<int>(options.batch_size),
                                                 checked_prefetch_factor(options.prefetch_factor),
                                                 options.cpu_affinity));
}

AlignmentStats run_alignment(const ValidationOptions& options, CocoDataset& dataset, InferenceBackend& lhs_backend,
                             InferenceBackend& rhs_backend) {
    const RuntimeContext runtime = make_validation_runtime_context(options);
    DatasetLoader loader(make_loader_config(options, runtime));

    const size_t target_images = std::min(options.alignment_images, dataset.num_images());
    std::unique_ptr<spdmon::ProgressBar> bar;
    if (interactive_logs(options)) {
        bar = std::make_unique<spdmon::ProgressBar>("align", target_images, "img");
    }

    AlignmentStats total_stats;
    c10::InferenceMode inference_mode;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    const auto lhs_stream = backend_cuda_stream(lhs_backend, options.device_id);
    const auto rhs_stream = backend_cuda_stream(rhs_backend, options.device_id);
    GpuBatchPreprocessor preprocessor(1, static_cast<int>(loader.image_height()),
                                      static_cast<int>(loader.image_width()), options.device_id, at::kFloat);
    CudaEvent normalized_ready;
    loader.begin_epoch();
    Batch batch{};
    size_t compared = 0;
    while (compared < target_images && loader.next_batch(batch)) {
        loader.wait_batch(batch);
        std::vector<TensorMap> lhs_results;
        torch::Tensor normalized;
        {
            c10::cuda::CUDAStreamGuard lhs_stream_guard(lhs_stream);
            LoaderBatchGuard batch_guard(loader, batch, options.device_id);
            normalized = preprocessor.run(batch);
            ensure_cuda_ok(cudaEventRecord(normalized_ready.get(), lhs_stream.stream()),
                           "cudaEventRecord for alignment preprocessing");
            batch_guard.release();
            lhs_results = postprocess_outputs_fixed_size(
                lhs_backend.run(normalized), static_cast<int64_t>(loader.image_height()),
                static_cast<int64_t>(loader.image_width()),
                lhs_backend.info().num_queries > 0 ? lhs_backend.info().num_queries : 300);
        }
        std::vector<TensorMap> rhs_results;
        {
            c10::cuda::CUDAStreamGuard rhs_stream_guard(rhs_stream);
            ensure_cuda_ok(cudaStreamWaitEvent(rhs_stream.stream(), normalized_ready.get(), 0),
                           "cudaStreamWaitEvent for alignment preprocessing");
            normalized.record_stream(rhs_stream);
            rhs_results = postprocess_outputs_fixed_size(
                rhs_backend.run(normalized), static_cast<int64_t>(loader.image_height()),
                static_cast<int64_t>(loader.image_width()),
                rhs_backend.info().num_queries > 0 ? rhs_backend.info().num_queries : 300);
        }
        ensure_cuda_ok(cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(lhs_backend.stream())),
                       "cudaStreamSynchronize for alignment lhs backend");
        ensure_cuda_ok(cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(rhs_backend.stream())),
                       "cudaStreamSynchronize for alignment rhs backend");
        const AlignmentStats sample = compare_top1(lhs_results.front(), rhs_results.front(), dataset.num_categories());
        total_stats = merge_alignment(total_stats, sample);
        if (bar && total_stats.images_compared > 0) {
            std::ostringstream postfix;
            postfix.setf(std::ios::fixed);
            postfix.precision(5);
            postfix << "score=" << total_stats.top1_score_abs_diff_mean;
            postfix.precision(3);
            postfix << ", box=" << total_stats.top1_box_abs_diff_px_mean;
            postfix.precision(1);
            postfix << ", mask=" << total_stats.top1_mask_xor_pixels_mean;
            bar->set_postfix(postfix.str());
            bar->add(1);
        }
        ++compared;
    }
    if (bar) {
        bar->close();
    }
    return total_stats;
}

ValidationBackendResult run_backend_eval_impl(const ValidationOptions& options, const CocoDataset& source_dataset,
                                              InferenceBackend& backend) {
    const RuntimeContext runtime = make_validation_runtime_context(options);
    DatasetLoader loader(make_loader_config(options, runtime));
    CocoDataset dataset = source_dataset;

    std::unique_ptr<spdmon::ProgressBar> bar;
    if (interactive_logs(options)) {
        bar = std::make_unique<spdmon::ProgressBar>(backend.info().backend, dataset.num_images(), "img");
    }

    c10::InferenceMode inference_mode;
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    const auto inference_stream = backend_cuda_stream(backend, options.device_id);
    GpuBatchPreprocessor preprocessor(1, static_cast<int>(loader.image_height()),
                                      static_cast<int>(loader.image_width()), options.device_id, at::kFloat);
    const int64_t staged_prediction_count = backend.info().num_queries > 0 ? backend.info().num_queries : 300;
    auto prediction_slots = std::make_shared<PredictionBufferSlotPool>(
        1, PredictionBufferConfig{
               1,
               staged_prediction_count,
               dataset.metric_set() == EvaluationMetricSet::BBoxAndMask
                   ? std::make_optional(std::make_pair(loader.image_height(), loader.image_width()))
                   : std::nullopt,
               options.device_id,
           });
    const auto started = std::chrono::steady_clock::now();
    auto evaluation_profile = make_standalone_evaluation_profile(options, backend.info(), dataset);
    if (evaluation_profile) {
        evaluation_profile->started_at = started;
        evaluation_profile->peak_in_flight_tasks = 1;
        evaluation_profile->peak_in_flight_slots = 1;
    }
    std::unique_ptr<EvaluationCudaTimingPool> cuda_timing_pool;
    if (evaluation_profile) {
        cuda_timing_pool = std::make_unique<EvaluationCudaTimingPool>(1);
    }

    loader.begin_epoch();
    Batch batch{};
    size_t processed = 0;
    while (loader.next_batch(batch)) {
        if (processed >= dataset.num_images()) {
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
        void* consumer_stream = reinterpret_cast<void*>(inference_stream.stream());
        c10::cuda::CUDAStreamGuard stream_guard(inference_stream);
        LoaderBatchGuard batch_guard(loader, batch, options.device_id);
        EvaluationCudaTimingLease batch_timing;
        const auto cuda_stream = reinterpret_cast<cudaStream_t>(consumer_stream);
        if (evaluation_profile) {
            batch_timing = cuda_timing_pool->acquire();
            batch_timing.timing->record_start(EvaluationCudaBatchTiming::Phase::Preprocessing, cuda_stream);
        }
        auto normalized = preprocessor.run(batch);
        const int image_id = static_cast<int>(batch.image_indices[0]) + 1;
        if (batch_timing) {
            batch_timing.timing->record_stop(EvaluationCudaBatchTiming::Phase::Preprocessing, cuda_stream);
            batch_timing.timing->record_start(EvaluationCudaBatchTiming::Phase::ModelForward, cuda_stream);
        }
        batch_guard.release();
        auto raw_outputs = backend.run(normalized);
        preprocessor.record_consumer(inference_stream.stream());
        if (batch_timing) {
            batch_timing.timing->record_stop(EvaluationCudaBatchTiming::Phase::ModelForward, cuda_stream);
            batch_timing.timing->record_start(EvaluationCudaBatchTiming::Phase::Postprocess, cuda_stream);
        }
        auto results =
            postprocess_outputs_fixed_size(raw_outputs, static_cast<int64_t>(loader.image_height()),
                                           static_cast<int64_t>(loader.image_width()), staged_prediction_count);
        if (batch_timing) {
            batch_timing.timing->record_stop(EvaluationCudaBatchTiming::Phase::Postprocess, cuda_stream);
        }
        if (dataset.has_image(image_id)) {
            if (evaluation_profile) {
                const TensorMap& result = results.front();
                const auto tensor_bytes = [](const torch::Tensor& tensor) {
                    return static_cast<size_t>(tensor.numel()) * static_cast<size_t>(tensor.element_size());
                };
                evaluation_profile->transferred_bytes += tensor_bytes(result.at("scores")) +
                                                         tensor_bytes(result.at("labels")) +
                                                         tensor_bytes(result.at("boxes"));
                if (result.contains("masks")) {
                    const torch::Tensor& masks = result.at("masks");
                    const size_t selected_mask_count = static_cast<size_t>(result.at("scores").size(0));
                    const size_t pixels_per_mask =
                        static_cast<size_t>(masks.size(-1)) * static_cast<size_t>(masks.size(-2));
                    const size_t mask_bytes = selected_mask_count * ((pixels_per_mask + 7U) / 8U);
                    evaluation_profile->transferred_bytes += mask_bytes;
                    evaluation_profile->mask_transferred_bytes += mask_bytes;
                }
            }
            std::vector<PredictionBatchMetadata> metadata;
            metadata.push_back(PredictionBatchMetadata{
                static_cast<int64_t>(batch.image_indices[0]),
                image_id,
                {},
            });
            StagedPredictionBatch staged = stage_prediction_batch(
                std::move(metadata), postprocessed_batch_from_result(results.front()), dataset.num_categories(),
                options.eval_max_dets, prediction_slots->acquire(), options.device_id, consumer_stream);
            std::vector<PredictionBatchItem> completed =
                collect_prediction_batch_encoding(enqueue_prediction_batch_encoding(
                    runtime.cpu_pool(), std::move(staged), evaluation_profile.get(), &dataset));
            if (completed.size() != 1) {
                throw std::logic_error("sequential validation task omitted compact evaluation matches");
            }
            auto& evaluation_matches = completed.front().evaluation_matches;
            if (!evaluation_matches) {
                throw std::logic_error("sequential validation task omitted compact evaluation matches");
            }
            dataset.merge_matches(std::move(*evaluation_matches));
            ++processed;
            if (bar) {
                bar->add(1);
            }
        }
        if (batch_timing) {
            batch_timing.timing->accumulate(*evaluation_profile);
            cuda_timing_pool->release(batch_timing);
        }
    }
    if (bar) {
        bar->close();
    }

    return ValidationBackendResult{
        backend.info(),
        dataset.evaluate(options.eval_max_dets, evaluation_profile.get(), &runtime.cpu_pool()),
        elapsed_timing(started, dataset.num_images()),
    };
}

json result_to_json(const ValidationOptions& options, const ValidationRunResult& result) {
    json report;
    report["images"] = result.images;
    report["batch_size"] = options.batch_size;
    report["eval_order"] = result.eval_order;
    for (const auto& backend_name : result.eval_order) {
        const auto found = result.backends.find(backend_name);
        if (found == result.backends.end()) {
            continue;
        }
        report[backend_name] = summary_to_json(found->second.summary);
    }
    if (result.alignment_probe.has_value()) {
        const AlignmentStats& alignment = *result.alignment_probe;
        report["alignment_probe"] = json{
            {"images_compared", alignment.images_compared},
            {"top1_score_abs_diff_mean", alignment.top1_score_abs_diff_mean},
            {"top1_score_abs_diff_max", alignment.top1_score_abs_diff_max},
            {"top1_box_abs_diff_px_mean", alignment.top1_box_abs_diff_px_mean},
            {"top1_box_abs_diff_px_max", alignment.top1_box_abs_diff_px_max},
            {"top1_mask_xor_pixels_mean", alignment.top1_mask_xor_pixels_mean},
            {"top1_mask_xor_pixels_max", alignment.top1_mask_xor_pixels_max},
        };
    }
    if (result.delta_tensorrt_minus_onnx.has_value()) {
        const ValidationDeltaSummary& delta = *result.delta_tensorrt_minus_onnx;
        report["delta_tensorrt_minus_onnx"] = json{
            {"bbox_ap", delta.bbox_ap},
            {"bbox_ap50", delta.bbox_ap50},
            {"mask_ap", delta.mask_ap.has_value() ? json(*delta.mask_ap) : json(nullptr)},
            {"mask_ap50", delta.mask_ap50.has_value() ? json(*delta.mask_ap50) : json(nullptr)},
        };
    }
    if (options.profile) {
        report["profile"] = json::object();
        for (const auto& backend_name : result.eval_order) {
            const auto found = result.backends.find(backend_name);
            if (found == result.backends.end()) {
                continue;
            }
            report["profile"][backend_name] = timing_to_json(found->second.timing);
        }
        if (result.total_timing.has_value()) {
            report["profile"]["total"] = timing_to_json(*result.total_timing);
        }
    }
    return report;
}

}  

void print_model_metadata(const ModelInfo& info, size_t images, size_t categories, ValidationLogMode log_mode) {
    print_model_metadata_impl(info, images, categories, log_mode);
}

ValidationRunResult run_validation(const ValidationOptions& options) {
    validate_loader_shape(options);
    if (options.compiled_path.empty()) {
        throw std::runtime_error("native RF-DETR validation requires compiled_path");
    }
    if (options.weights_path.empty() && options.onnx_path.empty() && options.tensorrt_path.empty()) {
        throw std::runtime_error("native RF-DETR validation requires at least one backend");
    }

    const auto total_started = std::chrono::steady_clock::now();
    ensure_compiled_dataset(options);
    ValidationOptions effective_options = options;
    effective_options.resolution = compiled_input_resolution(effective_options.compiled_path);
    if (options.profile) {
        const std::filesystem::path profile_path = validation_profile_jsonl_path(options);
        if (!profile_path.parent_path().empty()) {
            std::filesystem::create_directories(profile_path.parent_path());
        }
        std::error_code ignored_error;
        std::filesystem::remove(profile_path, ignored_error);
    }

    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    InferenceBackendFactory backend_factory(options.onnx_path, options.tensorrt_path, options.device_id,
                                            options.allow_fp16, options.save_engine_path);

    std::unordered_map<std::string, std::unique_ptr<InferenceBackend>> backends;
    if (!options.onnx_path.empty()) {
        log_line(options, "backend[onnx]: loading " + options.onnx_path.string());
        backends["onnx"] = backend_factory.make_backend("onnx");
        validate_backend_input_resolution(*backends["onnx"], static_cast<int>(effective_options.resolution));
    }
    if (!options.tensorrt_path.empty()) {
        log_line(options, "backend[tensorrt]: loading " + options.tensorrt_path.string());
        backends["tensorrt"] = backend_factory.make_backend("tensorrt");
        validate_backend_input_resolution(*backends["tensorrt"], static_cast<int>(effective_options.resolution));
    }

    ValidationRunResult result;
    result.eval_order = split_csv(options.eval_order);
    if (result.eval_order.empty()) {
        throw std::runtime_error("validation eval_order must name at least one backend");
    }

    std::unordered_map<std::string, ModelInfo> requested_model_info;
    std::optional<bool> requested_masks;
    for (const std::string& backend_name : result.eval_order) {
        ModelInfo info;
        if (backend_name == "weights") {
            if (options.weights_path.empty()) {
                throw std::runtime_error("eval order references unavailable backend: weights");
            }
            info = inspect_weights_model_info(effective_options);
        } else {
            const auto found = backends.find(backend_name);
            if (found == backends.end()) {
                throw std::runtime_error("eval order references unavailable backend: " + backend_name);
            }
            info = found->second->info();
        }
        if (requested_masks.has_value() && *requested_masks != info.has_masks) {
            throw std::runtime_error("all validation backends must agree on bbox-only versus mask evaluation");
        }
        requested_masks = info.has_masks;
        requested_model_info.emplace(backend_name, std::move(info));
    }

    const EvaluationMetricSet metric_set =
        requested_masks.value_or(false) ? EvaluationMetricSet::BBoxAndMask : EvaluationMetricSet::BBox;
    log_line(options, "dataset: loading " + options.compiled_path.string());
    CocoDataset dataset = CocoDataset::load_from_binary(options.compiled_path, metric_set);
    if (options.limit_images > 0) {
        dataset.limit_images(options.limit_images);
    }
    result.images = dataset.num_images();
    result.categories = dataset.num_categories();
    for (const std::string& backend_name : result.eval_order) {
        print_model_metadata_impl(requested_model_info.at(backend_name), dataset.num_images(), dataset.num_categories(),
                                  options.log_mode);
    }

    log_line(options, "eval: split=" + options.split + " images=" + std::to_string(dataset.num_images()) +
                          " eval_max_dets=" + std::to_string(options.eval_max_dets) +
                          " batch_size=" + std::to_string(options.batch_size));

    if (options.profile && backends.size() >= 2 && result.eval_order.size() >= 2 && result.eval_order[0] != "weights" &&
        result.eval_order[1] != "weights" && options.alignment_images > 0) {
        result.alignment_probe =
            run_alignment(options, dataset, *backends.at(result.eval_order[0]), *backends.at(result.eval_order[1]));
    }

    for (const std::string& backend_name : result.eval_order) {
        if (backend_name == "weights") {
            if (options.weights_path.empty()) {
                throw std::runtime_error("eval order references unavailable backend: weights");
            }
            ValidationBackendResult weights = run_weights_validation_backend(effective_options, dataset);
            result.backends.emplace(backend_name, std::move(weights));
        } else if (options.batch_size > 1) {
            result.backends.emplace(backend_name,
                                    run_backend_eval_parallel_impl(options, dataset, *backends.at(backend_name)));
        } else {
            result.backends.emplace(backend_name, run_backend_eval_impl(options, dataset, *backends.at(backend_name)));
        }
    }

    if (result.backends.contains("onnx") && result.backends.contains("tensorrt")) {
        const EvalSummary& onnx = result.backends.at("onnx").summary;
        const EvalSummary& trt = result.backends.at("tensorrt").summary;
        result.delta_tensorrt_minus_onnx = ValidationDeltaSummary{
            trt.bbox.ap - onnx.bbox.ap,
            trt.bbox.ap50 - onnx.bbox.ap50,
            onnx.mask.has_value() && trt.mask.has_value() ? std::optional<double>(trt.mask->ap - onnx.mask->ap)
                                                          : std::nullopt,
            onnx.mask.has_value() && trt.mask.has_value() ? std::optional<double>(trt.mask->ap50 - onnx.mask->ap50)
                                                          : std::nullopt,
        };
    }

    result.total_timing = elapsed_timing(total_started, dataset.num_images());
    return result;
}

ValidationBackendResult run_validation_backend(const ValidationOptions& options, const CocoDataset& source_dataset,
                                               InferenceBackend& backend) {
    validate_loader_shape(options);
    const bool expects_masks = source_dataset.metric_set() == EvaluationMetricSet::BBoxAndMask;
    if (backend.info().has_masks != expects_masks) {
        throw std::runtime_error("validation backend mask outputs do not match the dataset evaluation mode");
    }
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    if (options.batch_size > 1) {
        return run_backend_eval_parallel_impl(options, source_dataset, backend);
    }
    return run_backend_eval_impl(options, source_dataset, backend);
}

void write_validation_report(const ValidationOptions& options, const ValidationRunResult& result) {
    if (!options.write_report_json) {
        return;
    }
    std::filesystem::create_directories(options.report_json_path.parent_path());
    std::ofstream stream(options.report_json_path);
    if (!stream) {
        throw std::runtime_error("failed to open validation report path: " + options.report_json_path.string());
    }
    stream << result_to_json(options, result).dump(2) << '\n';
}

void print_validation_run_summary(const ValidationOptions& options, const ValidationRunResult& result) {
    if (!interactive_logs(options)) {
        return;
    }
    for (const std::string& backend_name : result.eval_order) {
        const auto found = result.backends.find(backend_name);
        if (found == result.backends.end()) {
            continue;
        }
        spdmon::ProgressBar::log(validate_detail::format_validation_summary_line(backend_name, found->second.summary));
    }
    if (result.delta_tensorrt_minus_onnx.has_value()) {
        spdmon::ProgressBar::log(
            validate_detail::format_validation_delta_summary_line(*result.delta_tensorrt_minus_onnx));
    }
    if (options.profile && result.total_timing.has_value()) {
        spdmon::ProgressBar::log("profile: total=" + std::to_string(result.total_timing->seconds) + "s");
    }
    if (options.write_report_json) {
        spdmon::ProgressBar::log("report: " + options.report_json_path.string());
    }
}

}  
