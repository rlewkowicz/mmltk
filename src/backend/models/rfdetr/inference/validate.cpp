#include "src/backend/models/rfdetr/core/evaluator.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/backend/models/rfdetr/core/model_info.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/core/class_artifact.h"
#include "src/backend/models/rfdetr/core/detail/class_artifact_files.h"
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
#include <meta>
#include <type_traits>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
// CLEANUP-IGNORE: Validation directly imports the implementation owners required by its native pipeline.
#include "src/backend/models/rfdetr/core/runtime.h"
import mmltk.backend.ml.cuda.torch_scope;
import mmltk.backend.models.rfdetr.core.dataset_limit_resolution;
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
                                                       mmltk::backend::data::DatasetLoader& loader, std::optional<EvaluationDatasetOwner>& dataset,
                                                       PredictionSession& prediction_session, std::vector<std::optional<AlignmentSample>>* captured_predictions,
                                                       const runtime::BorrowedCommandStream command_stream, const ValidationDelivery& delivery,
                                                       std::span<const std::uint32_t> samples) {
    PredictRequest predict;
    static_cast<ModelArtifactRequest&>(predict) = selected_artifact(request, artifact);
    static_cast<InferenceExecutionConfig&>(predict) = request;
    predict.source_kind = PredictSourceKind::CompiledDataset;
    predict.compiled_path = request.compiled_path;
    predict.backend = artifact.backend_name;
    predict.batch_size = request.batch_size;
    predict.max_dets_per_image = request.num_queries == 0U ? 500U : request.num_queries;
    const auto evaluation_cap = std::min(predict.max_dets_per_image, request.eval_max_dets == 0U ? predict.max_dets_per_image : request.eval_max_dets);
    predict.output_path = request.report_json_path.empty() ? request.compiled_path.parent_path() / "validation_predictions.json" : request.report_json_path;
    predict.progress_bar = request.log_mode == ValidationLogMode::Interactive;
    predict.limit_images = request.limit_images;
    predict.include_masks = false;
    std::vector<std::uint32_t> evaluator_order, model_order;
    std::vector<float> scores;
    std::vector<std::int64_t> labels;
    std::vector<float> boxes;
    std::vector<Prediction> ground_truth;
    bool masks = false;
    const auto is_sample = [&](std::int64_t index) { return index >= 0 && std::ranges::binary_search(samples, static_cast<std::uint32_t>(index)); };
    const auto predictions = prediction_session.RunResolved(
        predict, artifact, command_stream,
        {
            .stop = delivery.stop,
            .demand =
                [&](std::int64_t index) {
                    const bool selected = delivery.sample && is_sample(index);
                    return PredictionDemand{.source_pixels = selected, .encoded_masks = masks, .preview_masks = selected};
                },
            .begin =
                [&](const PredictionRunResult& result) {
                    if (result.class_domain != mmltk::backend::data::catalog::ClassReferenceDomain::Foreground || !result.class_catalog)
                        throw std::invalid_argument("semantic evaluation requires a fully bound model class layout");
                    masks = delivery.mask_metrics && result.masks_available &&
                            std::ranges::all_of(std::span{loader.label_data(), loader.num_label_instances()},
                                                [](const auto& label) { return label.mask_rle_pairs != 0U; });
                    const auto mode = masks ? EvaluationMetricSet::BBoxAndMask : EvaluationMetricSet::BBox;
                    if (!dataset || dataset->facts().metric_set != mode)
                        dataset.emplace(loader, mode);
                    else
                        dataset->clear_predictions();
                    if (request.limit_images != 0U) dataset->limit_images(request.limit_images);
                    evaluator_order = result.class_catalog->permutation_to(*dataset->class_catalog());
                    model_order.resize(evaluator_order.size());
                    for (std::size_t category = 0; category < evaluator_order.size(); ++category)
                        model_order[evaluator_order[category]] = static_cast<std::uint32_t>(category);
                },
            .completed =
                [&](const PredictionRecord& record, PredictionPixels pixels, const runtime::AnalysisAnnotationStorage& annotations) {
                    scores.clear();
                    labels.clear();
                    boxes.clear();
                    scores.reserve(record.detections.size());
                    labels.reserve(record.detections.size());
                    boxes.reserve(record.detections.size() * 4U);
                    for (const auto& detection : record.detections) {
                        scores.push_back(detection.score);
                        if (detection.class_domain != mmltk::backend::data::catalog::ClassReferenceDomain::Foreground || detection.class_reference < 0 ||
                            static_cast<std::size_t>(detection.class_reference) >= evaluator_order.size())
                            throw std::invalid_argument("invalid semantic evaluator class reference");
                        labels.push_back(evaluator_order[detection.class_reference]);
                        boxes.insert(boxes.end(), detection.bbox_xyxy.begin(), detection.bbox_xyxy.end());
                    }
                    dataset->merge_matches(dataset->match_predictions(record.dataset_index,
                                                                      {.image_id = static_cast<int>(record.image_id),
                                                                       .scores = scores.data(),
                                                                       .labels_zero_based = labels.data(),
                                                                       .boxes_xyxy = boxes.data(),
                                                                       .count = scores.size()},
                                                                      std::nullopt, evaluation_cap,
                                                                      masks ? std::span<const Prediction>{record.detections} : std::span<const Prediction>{}));
                    if (delivery.sample && is_sample(record.dataset_index)) {
                        ground_truth.clear();
                        const auto& entry = loader.label_index()[record.dataset_index];
                        for (std::size_t ordinal = 0; ordinal < entry.num_instances; ++ordinal) {
                            const auto& packed = loader.label_data()[entry.label_begin + ordinal];
                            Prediction gt;
                            gt.image_id = static_cast<int>(record.image_id);
                            // GT is expressed in the producing sample catalog, while metrics
                            // consume the already-admitted model-to-dataset permutation.
                            gt.class_reference = static_cast<int>(model_order[packed.class_id]);
                            gt.bbox_xyxy = {static_cast<float>(packed.bbox_x1), static_cast<float>(packed.bbox_y1), static_cast<float>(packed.bbox_x2),
                                            static_cast<float>(packed.bbox_y2)};
                            gt.has_mask = packed.mask_rle_pairs != 0U;
                            gt.mask.width = loader.image_width();
                            gt.mask.height = loader.image_height();
                            for (std::size_t run = 0; run < packed.mask_rle_pairs; ++run) {
                                const auto pair = loader.rle_data()[packed.mask_rle_offset / sizeof(mmltk::backend::data::RLEPair) + run];
                                gt.mask.runs.emplace_back(pair.start, pair.length);
                                gt.mask.area += pair.length;
                            }
                            ground_truth.push_back(std::move(gt));
                        }
                        delivery.sample({record, std::move(pixels), annotations, ground_truth});
                    }
                    if (captured_predictions != nullptr && captured_predictions->size() < request.alignment_images) {
                        if (record.detections.empty())
                            captured_predictions->push_back(std::nullopt);
                        else
                            captured_predictions->push_back(AlignmentSample{record.detections.front().score, record.detections.front().bbox_xyxy});
                    }
                },
            .progress = delivery.progress,
        });
    ValidationBackendResult result;
    result.artifacts = predictions.artifacts;
    result.model_info.backend = artifact.backend_name;
    result.model_info.model_path = artifact.path.string();
    result.model_info.num_queries = predictions.artifacts.config.num_queries;
    result.model_info.num_classes = predictions.artifacts.config.num_classes;
    result.model_info.class_layout = predictions.artifacts.class_layout;
    if (!dataset) {
        if (!predictions.cancelled) throw std::logic_error("validation did not admit an evaluator");
        // Cancellation can win between selected-backend admission and Begin.
        // No matching has occurred, so this backend has no available metrics.
        result.summary.model_detection_budget = static_cast<std::uint32_t>(predict.max_dets_per_image);
        result.timing = predictions.timing;
        return result;
    }
    if (predictions.cancelled) dataset->limit_images(predictions.processed_images);
    result.summary = dataset->evaluate(evaluation_cap, EvaluationDetailRetention::Detailed);
    result.summary.model_detection_budget = static_cast<std::uint32_t>(predict.max_dets_per_image);
    result.details = dataset->take_details();
    result.class_catalog = dataset->class_catalog();
    result.timing = predictions.timing;
    return result;
}
// The report is a genuinely distinct external JSON form. Derive its fields
// from the metric contract so additional summary facts have one declaration.
template <class T>
[[nodiscard]] nlohmann::json metric_json(const T& value) {
    if constexpr (std::is_arithmetic_v<T>)
        return value;
    else if constexpr (requires {
                           value.has_value();
                           *value;
                       })
        return value ? metric_json(*value) : nlohmann::json(nullptr);
    else if constexpr (requires {
                           value.begin();
                           value.end();
                       }) {
        auto result = nlohmann::json::array();
        for (const auto& item : value) result.push_back(metric_json(item));
        return result;
    } else {
        auto result = nlohmann::json::object();
        template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
            result[std::string(std::meta::identifier_of(member))] = metric_json(value.[:member:]);
        }
        return result;
    }
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
    // At most three selected artifacts and one consumed source; no batch-time I/O.
    std::array<std::shared_ptr<const ClassArtifactAdmission>, 4U> admissions;
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
ValidationRunResult ValidationSession::Run(const ValidateRequest& request, const runtime::BorrowedCommandStream command_stream,
                                           const ValidationDelivery& delivery) {
    if (delivery.stop.stop_requested()) return {.cancelled = true};
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
    try {
        torch_cuda::run_on_torch_cuda_stream(options.device_id, command_stream.native_handle, &call, [](void* opaque) {
            auto& bound = *static_cast<BoundValidationCall*>(opaque);
            *bound.result = bound.state->Run(*bound.options, bound.command_stream, *bound.delivery);
        });
    } catch (const ArtifactPublicationCancelled&) { result.cancelled = true; }
    return result;
}
ValidationRunResult ValidationSession::State::Run(ValidateRequest& options, const runtime::BorrowedCommandStream command_stream,
                                                  const ValidationDelivery& delivery) {
    struct PreparedArtifact final {
        ResolvedInferenceArtifact artifact;
        std::shared_ptr<const mmltk::common::io::FileDigests> file{};
        std::string preset_name{};
    };
    std::vector<PreparedArtifact> artifacts;
    for (const auto& requested : evaluation_order(options.eval_order)) artifacts.push_back({resolve_inference_artifact(options, requested)});
    const bool materialize = options.tensorrt_path.empty() && !options.save_engine_path.empty();
    std::optional<PreparedArtifact> consumed_source;
    if (materialize) consumed_source = PreparedArtifact{resolve_inference_artifact(options, "onnx")};
    std::optional<ModelClassDescriptor> selected_descriptor;
    std::optional<mmltk::common::io::FileSnapshot> selected_snapshot;
    if (!options.class_layout_path.empty()) {
        selected_snapshot = mmltk::common::io::FileSnapshot::Read(options.class_layout_path);
        selected_descriptor = detail::read_class_descriptor(options.class_layout_path);
        selected_snapshot->RequireUnchanged(options.class_layout_path);
    }
    auto previous_admissions = std::exchange(admissions, {});
    // Descriptor routing needs only digest facts. Companion admission stays at
    // each backend's existing execution boundary, preserving partial progress.
    const auto file_for = [&](PreparedArtifact& prepared) {
        if (prepared.file) return prepared.file;
        const auto path = std::filesystem::absolute(prepared.artifact.path).lexically_normal();
        for (const auto& other : artifacts)
            if (other.file && std::filesystem::absolute(other.artifact.path).lexically_normal() == path) return prepared.file = other.file;
        if (consumed_source && consumed_source->file && std::filesystem::absolute(consumed_source->artifact.path).lexically_normal() == path)
            return prepared.file = consumed_source->file;
        const auto snapshot = mmltk::common::io::FileSnapshot::Read(path);
        const auto retained = std::ranges::find_if(
            previous_admissions, [&](const auto& item) { return item && item->artifact_path() == path && item->file()->snapshot == snapshot; });
        if (retained != previous_admissions.end()) return prepared.file = (*retained)->file();
        auto digest = mmltk::common::io::try_file_digests(path, true, [&] { return delivery.stop.stop_requested(); });
        if (!digest) throw ArtifactPublicationCancelled{};
        return prepared.file = std::make_shared<const mmltk::common::io::FileDigests>(std::move(*digest));
    };
    const auto descriptor_matches = [&](PreparedArtifact& prepared) {
        return selected_descriptor && selected_descriptor->artifact_sha256 == mmltk::common::io::sha256_hex(file_for(prepared)->sha256);
    };
    const auto admit = [&](PreparedArtifact& prepared) {
        const auto proof = file_for(prepared);
        const auto descriptor_path = descriptor_matches(prepared) ? options.class_layout_path : std::filesystem::path{};
        auto& artifact = prepared.artifact;
        for (const auto* candidates : {&admissions, &previous_admissions}) {
            const auto found = std::ranges::find_if(*candidates, [&](const auto& item) {
                return item && item->artifact_path() == std::filesystem::absolute(artifact.path).lexically_normal() && item->file() == proof &&
                       item->Matches(artifact.path, descriptor_path);
            });
            if (found != candidates->end()) {
                artifact.admission = *found;
                break;
            }
        }
        if (!artifact.admission) artifact.admission = std::make_shared<const ClassArtifactAdmission>(artifact.path, descriptor_path, proof, delivery.stop);
        artifact.admission->RequireUnchanged(delivery.stop);
        if (std::ranges::find(admissions, artifact.admission) == admissions.end()) {
            const auto available = std::ranges::find_if(admissions, [](const auto& item) { return !item; });
            if (available == admissions.end()) throw std::logic_error("validation artifact admission capacity exceeded");
            *available = artifact.admission;
        }
        if (selected_snapshot) selected_snapshot->RequireUnchanged(options.class_layout_path);
    };
    bool source_descriptor_matches = false;
    if (selected_descriptor) {
        bool matched = false;
        for (auto& artifact : artifacts) matched = descriptor_matches(artifact) || matched;
        if (consumed_source) source_descriptor_matches = descriptor_matches(*consumed_source);
        if (!matched && !source_descriptor_matches)
            throw std::invalid_argument("selected class descriptor does not bind any consumed or selected validation artifact");
        selected_snapshot->RequireUnchanged(options.class_layout_path);
    }
    if (materialize) {
        BuildEngineRequest build;
        static_cast<ModelArtifactRequest&>(build) = options;
        build.weights_path.clear();
        build.tensorrt_path.clear();
        build.output_path = options.save_engine_path;
        build.device_id = options.device_id;
        build.allow_fp16 = options.allow_fp16;
        if (selected_descriptor && !source_descriptor_matches) build.class_layout_path.clear();
        admit(*consumed_source);
        if (build.preset_name.empty()) {
            if (const auto* preset = infer_model_preset_from_path(consumed_source->artifact.path)) build.preset_name = preset->preset_name;
        }
        build_tensorrt_engine(build, command_stream, consumed_source->artifact.admission, delivery.stop);
        if (delivery.stop.stop_requested()) return {.cancelled = true};
        options.tensorrt_path = std::filesystem::absolute(options.save_engine_path);
        for (auto& prepared : artifacts)
            if (prepared.artifact.compile_onnx_to_tensorrt) {
                prepared = PreparedArtifact{resolve_inference_artifact(options, "tensorrt")};
                prepared.preset_name = build.preset_name;
                if (selected_descriptor) static_cast<void>(file_for(prepared));
            }
    }
    if (delivery.stop.stop_requested()) return {.cancelled = true};
    auto loader = inference_detail::make_loader(options.compiled_path, options.batch_size, options, options.prefetch_factor);
    std::optional<EvaluationDatasetOwner> dataset;
    const auto population = options.limit_images == 0U ? loader->num_images() : std::min(options.limit_images, loader->num_images());
    std::vector<std::uint32_t> samples;
    if (delivery.sample) {
        // Floyd selection uses at most six entries, independent of population.
        std::mt19937_64 random(std::random_device{}());
        const auto count = std::min<std::size_t>(kValidationSampleCapacity, population);
        for (std::size_t index = population - count; index < population; ++index) {
            auto selected = static_cast<std::uint32_t>(std::uniform_int_distribution<std::size_t>(0U, index)(random));
            if (std::ranges::find(samples, selected) != samples.end()) selected = static_cast<std::uint32_t>(index);
            samples.push_back(selected);
        }
        std::ranges::sort(samples);
        if (delivery.samples_selected) delivery.samples_selected(samples, loader->class_catalog());
    }
    ValidationRunResult result;
    result.images = population;
    result.categories = loader->num_classes();
    result.limits.persisted_max_instances_per_image = loader->max_instances_per_image();
    result.limits.resolved_num_queries = options.num_queries == 0U ? 500U : options.num_queries;
    result.limits.resolved_eval_max_dets =
        std::min(result.limits.resolved_num_queries, options.eval_max_dets == 0U ? result.limits.resolved_num_queries : options.eval_max_dets);
    result.limits.num_queries_automatic = options.num_queries == 0U;
    result.limits.eval_max_dets_automatic = options.eval_max_dets == 0U;
    std::vector<std::optional<AlignmentSample>> onnx_predictions;
    std::vector<std::optional<AlignmentSample>> tensorrt_predictions;
    for (auto& prepared : artifacts) {
        if (delivery.stop.stop_requested()) break;
        admit(prepared);
        const auto& artifact = prepared.artifact;
        auto backend_request = options;
        if (!prepared.preset_name.empty()) backend_request.preset_name = prepared.preset_name;
        if (artifact.admission->descriptor_path().empty()) backend_request.class_layout_path.clear();
        result.eval_order.push_back(artifact.backend_name);
        std::vector<std::optional<AlignmentSample>>* captured_result = nullptr;
        switch (artifact.kind) {
            case InferenceArtifactKind::Weights: break;
            case InferenceArtifactKind::Onnx: captured_result = &onnx_predictions; break;
            case InferenceArtifactKind::TensorRt: captured_result = &tensorrt_predictions; break;
            default: throw std::invalid_argument("invalid RF-DETR inference artifact kind");
        }
        result.backends.emplace(artifact.backend_name, evaluate_backend(backend_request, artifact, *loader, dataset, For(artifact.kind), captured_result,
                                                                        command_stream, delivery, samples));
    }
    if (const auto onnx = result.backends.find("onnx"); onnx != result.backends.end()) {
        if (const auto trt = result.backends.find("tensorrt"); trt != result.backends.end()) {
            result.delta_tensorrt_minus_onnx = {
                .bbox_ap = trt->second.summary.bbox.ap - onnx->second.summary.bbox.ap,
                .bbox_ap50 = trt->second.summary.bbox.ap50 - onnx->second.summary.bbox.ap50,
                .mask_ap = std::nullopt,
                .mask_ap50 = std::nullopt,
            };
            if (onnx->second.summary.mask && trt->second.summary.mask && onnx->second.summary.mask->available && trt->second.summary.mask->available) {
                result.delta_tensorrt_minus_onnx->mask_ap = trt->second.summary.mask->ap - onnx->second.summary.mask->ap;
                result.delta_tensorrt_minus_onnx->mask_ap50 = trt->second.summary.mask->ap50 - onnx->second.summary.mask->ap50;
            }
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
        backends[name] = metric_json(value.summary);
        backends[name]["seconds"] = value.timing.seconds;
        backends[name]["images_per_second"] = value.timing.img_per_s;
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
    std::cout << "model[" << info.backend << "]: path=" << info.model_path << " input=" << info.input.name << ' ' << format_shape(info.input.shape) << ' '
              << info.input.dtype << " outputs=" << info.outputs.size() << " queries=" << info.num_queries << " classes=" << info.num_classes
              << " dataset_images=" << images << " dataset_classes=" << categories << '\n';
    for (const TensorInfo& output : info.outputs) {
        std::cout << "  output: " << output.name << ' ' << format_shape(output.shape) << ' ' << output.dtype << '\n';
    }
}
void print_validation_run_summary(const ValidateRequest& request, const ValidationRunResult& result) {
    if (request.log_mode != ValidationLogMode::Interactive) { return; }
    for (const auto& [name, value] : result.backends) {
        std::cout << name << ": bbox AP=";
        if (value.summary.bbox.available)
            std::cout << value.summary.bbox.ap << ", AP50=" << value.summary.bbox.ap50;
        else
            std::cout << "unavailable";
        const auto& caps = value.summary.bbox.detection_limits;
        std::cout << " model budget=" << value.summary.model_detection_budget << " AR caps=" << caps[0] << '/' << caps[1] << '/' << caps[2] << '\n';
    }
}
}  // namespace mmltk::backend::models::rfdetr
