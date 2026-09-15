#include "src/backend/models/rfdetr/inference/validate.h"

#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/backend/models/rfdetr/core/model_info.h"
#include "src/backend/models/rfdetr/core/model_state.h"

// CLEANUP-IGNORE: This global module fragment declares the direct validation implementation dependencies.

#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// CLEANUP-IGNORE: Validation directly imports the implementation owners required by its native pipeline.
import mmltk.backend.ml.cuda.torch_scope;
import mmltk.backend.models.rfdetr.core.artifact_resolution;
import mmltk.backend.models.rfdetr.core.dataset_limit_resolution;
import mmltk.backend.models.rfdetr.core.dataset_utils;
import mmltk.backend.models.rfdetr.core.runtime;
import mmltk.backend.models.rfdetr.core.tool_launch_utils;
import mmltk.backend.models.rfdetr.core.evaluator;
import mmltk.backend.models.rfdetr.inference.prediction;
import mmltk.backend.models.rfdetr.inference.loader;
import mmltk.backend.models.rfdetr.inference.runtime_backend;

namespace mmltk::backend::models::rfdetr {
namespace runtime = mmltk::backend::ml::runtime;
namespace torch_cuda = mmltk::backend::ml::cuda;

namespace {

struct AlignmentSample final {
    float score = 0.0F;
    std::array<float, 4U> bbox_xyxy{};
};

[[nodiscard]] ModelArtifactRequest selected_artifact(const ValidateRequest& request, const ResolvedInferenceArtifact& resolved) {
    return select_inference_artifact(request, resolved);
}

[[nodiscard]] ValidationBackendResult evaluate_backend(const ValidateRequest& request, const ResolvedInferenceArtifact& artifact,
                                                       EvaluationDatasetOwner dataset, PredictionSession& prediction_session,
                                                       std::vector<std::optional<AlignmentSample>>* captured_predictions,
                                                       const runtime::BorrowedCommandStream command_stream, const ValidationDelivery& delivery) {
    PredictRequest predict;
    static_cast<ModelArtifactRequest&>(predict) = selected_artifact(request, artifact);
    static_cast<InferenceExecutionConfig&>(predict) = request;
    predict.source_kind = PredictSourceKind::CompiledDataset;
    predict.compiled_path = request.compiled_path;
    predict.backend = artifact.backend_name;
    predict.batch_size = request.batch_size;
    predict.max_dets_per_image = request.eval_max_dets == 0U ? 500U : request.eval_max_dets;
    predict.output_path =
        request.report_json_path.empty() ? request.compiled_path.parent_path() / "validation_predictions.json" : request.report_json_path;
    predict.progress_bar = request.log_mode == ValidationLogMode::Interactive;
    predict.limit_images = request.limit_images;
    predict.include_masks = false;
    std::vector<float> scores;
    std::vector<std::int64_t> labels;
    std::vector<float> boxes;
    const auto predictions = prediction_session.RunResolved(predict, artifact, command_stream, {
      .stop = delivery.stop,
      .completed = [&](const PredictionRecord& record, PredictionPixels, const runtime::AnalysisAnnotationStorage&) {
        scores.clear();
        labels.clear();
        boxes.clear();
        scores.reserve(record.detections.size());
        labels.reserve(record.detections.size());
        boxes.reserve(record.detections.size() * 4U);
        for (const auto& detection : record.detections) {
            scores.push_back(detection.score);
            labels.push_back(detection.category_id - 1);
            boxes.insert(boxes.end(), detection.bbox_xyxy.begin(), detection.bbox_xyxy.end());
        }
        dataset.merge_bbox_predictions(record.dataset_index,
                                       {.image_id = static_cast<int>(record.image_id),
                                        .scores = scores.data(),
                                        .labels_zero_based = labels.data(),
                                        .boxes_xyxy = boxes.data(),
                                        .count = scores.size()},
                                       predict.max_dets_per_image);
        if (captured_predictions != nullptr && captured_predictions->size() < request.alignment_images) {
            if (record.detections.empty()) captured_predictions->push_back(std::nullopt);
            else captured_predictions->push_back(AlignmentSample{record.detections.front().score, record.detections.front().bbox_xyxy});
        }
      },
      .progress = delivery.progress,
    });

    ValidationBackendResult result;
    result.model_info.backend = artifact.backend_name;
    result.model_info.model_path = artifact.path.string();
    result.model_info.num_queries = static_cast<std::int64_t>(predict.max_dets_per_image);
    result.model_info.num_classes = static_cast<std::int64_t>(dataset.category_count());
    result.summary = dataset.evaluate(predict.max_dets_per_image);
    result.timing = predictions.timing;
    return result;
}

[[nodiscard]] nlohmann::json metric_json(const MetricSummary& metric) {
    return {{"ap", metric.ap}, {"ap50", metric.ap50}, {"ap75", metric.ap75}};
}

[[nodiscard]] std::vector<std::string> evaluation_order(const std::string& value) {
    std::vector<std::string> result;
    std::istringstream stream(value);
    for (std::string backend; std::getline(stream, backend, ',');) {
        if ((backend != "weights" && backend != "onnx" && backend != "tensorrt") || std::ranges::find(result, backend) != result.end()) {
            throw std::invalid_argument("RF-DETR eval_order contains an invalid backend");
        }
        result.push_back(std::move(backend));
    }
    if (result.empty()) { throw std::invalid_argument("RF-DETR eval_order is empty"); }
    return result;
}

}  // namespace

struct ValidationSession::State final {
    std::array<PredictionSession, 3U> predictions;

    [[nodiscard]] PredictionSession& For(const InferenceArtifactKind kind) noexcept { return predictions[static_cast<std::size_t>(kind)]; }

    [[nodiscard]] ValidationRunResult Run(ValidateRequest& options, runtime::BorrowedCommandStream command_stream, const ValidationDelivery& delivery);
};

ValidationSession::ValidationSession() : state_(std::make_unique<State>()) {}
ValidationSession::~ValidationSession() { static_cast<void>(Close()); }

mmltk::backend::ml::runtime::RuntimeStatus ValidationSession::Close() noexcept {
    mmltk::backend::ml::runtime::RuntimeStatus first = mmltk::backend::ml::runtime::kRuntimeSuccess;
    for (auto& prediction : state_->predictions) {
        const auto status = prediction.Close();
        if (first == mmltk::backend::ml::runtime::kRuntimeSuccess && status != mmltk::backend::ml::runtime::kRuntimeSuccess) first = status;
    }
    return first;
}

ValidateRequest finalize_validate_request(ValidateRequest request) {
    validate_validate_request(request);
    request.compiled_path = std::filesystem::absolute(request.compiled_path);
    return request;
}

ValidationRunResult run_validation(const ValidateRequest& request) {
    ValidationSession session;
    const auto stream = torch_cuda::current_torch_cuda_stream(request.device_id);
    return session.Run(request, {.native_handle = stream, .valid = true});
}

ValidationRunResult ValidationSession::Run(const ValidateRequest& request, const runtime::BorrowedCommandStream command_stream, const ValidationDelivery& delivery) {
    if (!command_stream) throw std::invalid_argument("RF-DETR validation command stream is invalid");
    auto options = finalize_validate_request(request);
    ValidationRunResult result;
    struct BoundValidationCall final {
        State* state;
        ValidateRequest* options;
        runtime::BorrowedCommandStream command_stream;
        ValidationRunResult* result;
        const ValidationDelivery* delivery;
    } call{
        .state = state_.get(),
        .options = &options,
        .command_stream = command_stream,
        .result = &result,
        .delivery = &delivery,
    };
    torch_cuda::run_on_torch_cuda_stream(options.device_id, command_stream.native_handle, &call, [](void* opaque) {
        auto& bound = *static_cast<BoundValidationCall*>(opaque);
        *bound.result = bound.state->Run(*bound.options, bound.command_stream, *bound.delivery);
    });
    return result;
}

std::size_t ValidationSession::RunImageCount(const ValidateRequest& request, const runtime::BorrowedCommandStream command_stream, const ValidationDelivery& delivery) {
    return Run(request, command_stream, delivery).processed_images;
}

ValidationRunResult ValidationSession::State::Run(ValidateRequest& options, const runtime::BorrowedCommandStream command_stream, const ValidationDelivery& delivery) {
    if (options.tensorrt_path.empty() && !options.save_engine_path.empty()) {
        BuildEngineRequest build;
        static_cast<ModelArtifactRequest&>(build) = options;
        build.weights_path.clear();
        build.tensorrt_path.clear();
        build.output_path = options.save_engine_path;
        build.device_id = options.device_id;
        build.allow_fp16 = options.allow_fp16;
        build_tensorrt_engine(build, command_stream);
        options.tensorrt_path = std::filesystem::absolute(options.save_engine_path);
    }
    auto loader = inference_detail::make_loader(options.compiled_path, options.batch_size, options, options.prefetch_factor);
    EvaluationDatasetOwner dataset(*loader, EvaluationMetricSet::BBox);
    if (options.limit_images != 0U) { dataset.limit_images(options.limit_images); }

    ValidationRunResult result;
    result.images = dataset.image_count();
    result.categories = dataset.category_count();
    result.limits.persisted_max_instances_per_image = loader->max_instances_per_image();
    result.limits.resolved_num_queries = options.num_queries == 0U ? 500U : options.num_queries;
    result.limits.resolved_eval_max_dets = options.eval_max_dets == 0U ? result.limits.resolved_num_queries : options.eval_max_dets;
    result.limits.num_queries_automatic = options.num_queries == 0U;
    result.limits.eval_max_dets_automatic = options.eval_max_dets == 0U;

    std::vector<std::optional<AlignmentSample>> onnx_predictions;
    std::vector<std::optional<AlignmentSample>> tensorrt_predictions;
    for (const auto& requested : evaluation_order(options.eval_order)) {
        if (delivery.stop.stop_requested()) break;
        const auto artifact = resolve_inference_artifact(options, requested);
        result.eval_order.push_back(artifact.backend_name);
        std::vector<std::optional<AlignmentSample>>* captured_result = nullptr;
        switch (artifact.kind) {
            case InferenceArtifactKind::Weights:
                break;
            case InferenceArtifactKind::Onnx:
                captured_result = &onnx_predictions;
                break;
            case InferenceArtifactKind::TensorRt:
                captured_result = &tensorrt_predictions;
                break;
            default:
                throw std::invalid_argument("invalid RF-DETR inference artifact kind");
        }
        result.backends.emplace(artifact.backend_name,
                                evaluate_backend(options, artifact, dataset, For(artifact.kind), captured_result, command_stream, delivery));
    }
    if (const auto onnx = result.backends.find("onnx"); onnx != result.backends.end()) {
        if (const auto trt = result.backends.find("tensorrt"); trt != result.backends.end()) {
            result.delta_tensorrt_minus_onnx = {
                .bbox_ap = trt->second.summary.bbox.ap - onnx->second.summary.bbox.ap,
                .bbox_ap50 = trt->second.summary.bbox.ap50 - onnx->second.summary.bbox.ap50,
                .mask_ap = std::nullopt,
                .mask_ap50 = std::nullopt,
            };
            AlignmentStats alignment;
            double score_sum = 0.0;
            double box_sum = 0.0;
            const auto count = std::min({options.alignment_images, onnx_predictions.size(), tensorrt_predictions.size()});
            for (std::size_t index = 0U; index < count; ++index) {
                const auto& lhs = onnx_predictions[index];
                const auto& rhs = tensorrt_predictions[index];
                if (!lhs || !rhs) { continue; }
                const double score = std::abs(static_cast<double>(lhs->score) - rhs->score);
                double box = 0.0;
                for (std::size_t axis = 0U; axis < 4U; ++axis) {
                    box = std::max(box, std::abs(static_cast<double>(lhs->bbox_xyxy[axis] - rhs->bbox_xyxy[axis])));
                }
                score_sum += score;
                box_sum += box;
                alignment.top1_score_abs_diff_max = std::max(alignment.top1_score_abs_diff_max, score);
                alignment.top1_box_abs_diff_px_max = std::max(alignment.top1_box_abs_diff_px_max, box);
                ++alignment.images_compared;
            }
            if (alignment.images_compared != 0U) {
                const auto compared = static_cast<double>(alignment.images_compared);
                alignment.top1_score_abs_diff_mean = score_sum / compared;
                alignment.top1_box_abs_diff_px_mean = box_sum / compared;
                result.alignment_probe = alignment;
            }
        }
    }
    result.cancelled = delivery.stop.stop_requested();
    result.FinalizeTiming();
    return result;
}

void ValidationRunResult::FinalizeTiming() {
    processed_images = 0U;
    double total_seconds = 0.0;
    for (const auto& [name, backend] : backends) {
        static_cast<void>(name);
        total_seconds += backend.timing.seconds;
        processed_images += backend.timing.images;
    }
    total_timing = PhaseTiming{
        .seconds = total_seconds,
        .img_per_s = total_seconds > 0.0 ? static_cast<double>(processed_images) / total_seconds : 0.0,
        .images = processed_images,
    };
}

void write_validation_report(const ValidateRequest& request, const ValidationRunResult& result) {
    if (!request.write_report_json || request.report_json_path.empty()) { return; }
    nlohmann::json backends = nlohmann::json::object();
    for (const auto& [name, value] : result.backends) {
        backends[name] = {
            {"bbox", metric_json(value.summary.bbox)},
            {"seconds", value.timing.seconds},
            {"images_per_second", value.timing.img_per_s},
        };
    }
    nlohmann::json report = {
        {"images", result.images},
        {"categories", result.categories},
        {"backends", std::move(backends)},
    };
    std::ofstream stream(request.report_json_path);
    if (!stream) { throw std::runtime_error("failed to open RF-DETR validation report"); }
    stream << report.dump(2);
}

void print_model_metadata(const ModelInfo& info, std::size_t images, std::size_t categories, ValidationLogMode log_mode) {
    if (log_mode != ValidationLogMode::Interactive) return;
    std::cout << "model[" << info.backend << "]: path=" << info.model_path << " input=" << info.input.name << ' '
              << format_shape(info.input.shape) << ' ' << info.input.dtype << " outputs=" << info.outputs.size()
              << " queries=" << info.num_queries << " classes=" << info.num_classes << " dataset_images=" << images
              << " dataset_classes=" << categories << '\n';
    for (const TensorInfo& output : info.outputs) {
        std::cout << "  output: " << output.name << ' ' << format_shape(output.shape) << ' ' << output.dtype << '\n';
    }
}

void print_validation_run_summary(const ValidateRequest& request, const ValidationRunResult& result) {
    if (request.log_mode != ValidationLogMode::Interactive) { return; }
    for (const auto& [name, value] : result.backends) {
        std::cout << name << ": bbox AP=" << value.summary.bbox.ap << ", AP50=" << value.summary.bbox.ap50 << '\n';
    }
}

}  // namespace mmltk::backend::models::rfdetr
