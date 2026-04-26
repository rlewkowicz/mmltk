#include "rfdetr_workflows.h"

#include "source_selection.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mmltk::gui::rfdetr_workflows {

namespace {

using namespace mmltk::rfdetr;

template <typename Options>
void apply_model_input(ModelInputMode mode, const std::string& weights_path, const std::string& onnx_path,
                       const std::string& tensorrt_path, Options& options) {
    switch (mode) {
        case ModelInputMode::Weights:
            options.weights_path = weights_path;
            break;
        case ModelInputMode::Onnx:
            options.onnx_path = onnx_path;
            break;
        case ModelInputMode::TensorRt:
            options.tensorrt_path = tensorrt_path;
            break;
        case ModelInputMode::None:
            break;
    }
}

ModelInputMode model_input_mode_from_request(const mmltk::rfdetr::ModelArtifactRequest& request) {
    if (!request.weights_path.empty()) {
        return ModelInputMode::Weights;
    }
    if (!request.onnx_path.empty()) {
        return ModelInputMode::Onnx;
    }
    if (!request.tensorrt_path.empty()) {
        return ModelInputMode::TensorRt;
    }
    return ModelInputMode::None;
}

std::filesystem::path default_export_onnx_output_path(const std::filesystem::path& weights_path) {
    if (weights_path.empty()) {
        return {};
    }
    return weights_path.parent_path() / (weights_path.stem().string() + ".onnx");
}

}  // namespace

namespace {

[[nodiscard]] std::string validation_error(std::string_view field_name, std::string_view requirement) {
    std::string message;
    message.reserve(field_name.size() + requirement.size());
    message.append(field_name);
    message.append(requirement);
    return message;
}

[[nodiscard]] std::string validation_error(std::string_view context, std::string_view field_name,
                                           std::string_view requirement) {
    std::string message;
    message.reserve(context.size() + 1U + field_name.size() + requirement.size());
    message.append(context);
    message.push_back(' ');
    message.append(field_name);
    message.append(requirement);
    return message;
}

int require_non_negative_execution_tuning_int(int value, std::string_view context, std::string_view field_name) {
    if (value < 0) {
        throw std::runtime_error(validation_error(context, field_name, " must not be negative"));
    }
    return value;
}

void apply_execution_tuning_to_request(const ExecutionTuningState& state, const char* context,
                                       mmltk::rfdetr::PredictRequest& request) {
    request.workers = require_non_negative_execution_tuning_int(state.workers, context, "workers");
    request.lanes = require_non_negative_execution_tuning_int(state.lanes, context, "lanes");
    request.cpu_affinity = state.cpu_affinity;
    request.progress_bar = state.progress_bar;
    request.device_id = require_non_negative_execution_tuning_int(state.device_id, context, "device_id");
    request.allow_fp16 = state.allow_fp16;
    request.compilation_mode = state.compile_mode;
}

void apply_execution_tuning_to_request(const ExecutionTuningState& state, const char* context,
                                       mmltk::rfdetr::LivePredictOptions& request) {
    request.device_id = require_non_negative_execution_tuning_int(state.device_id, context, "device_id");
    request.allow_fp16 = state.allow_fp16;
    request.compilation_mode = state.compile_mode;
}

void apply_execution_tuning_to_request(const ExecutionTuningState& state, const char* context,
                                       mmltk::rfdetr::PredictOptions& request) {
    request.cpu_affinity = state.cpu_affinity;
    request.device_id = require_non_negative_execution_tuning_int(state.device_id, context, "device_id");
    request.allow_fp16 = state.allow_fp16;
    request.compilation_mode = state.compile_mode;
}

void apply_execution_tuning_to_request(const ExecutionTuningState& state, const char* context,
                                       mmltk::rfdetr::ValidateRequest& request) {
    request.device_id = require_non_negative_execution_tuning_int(state.device_id, context, "device_id");
    request.workers = require_non_negative_execution_tuning_int(state.workers, context, "workers");
    request.cpu_affinity = state.cpu_affinity;
}

void apply_execution_tuning_to_request(const ExecutionTuningState& state, const char* context,
                                       mmltk::rfdetr::TrainRequest& request) {
    request.cpu_affinity = state.cpu_affinity;
    request.workers = require_non_negative_execution_tuning_int(state.workers, context, "workers");
    request.lanes = require_non_negative_execution_tuning_int(state.lanes, context, "lanes");
    request.progress_bar = state.progress_bar;
    request.compilation_mode = state.compile_mode;
}

}  // namespace

std::size_t require_positive_size(int value, std::string_view field_name) {
    if (value <= 0) {
        throw std::runtime_error(validation_error(field_name, " must be greater than zero"));
    }
    return static_cast<std::size_t>(value);
}

std::size_t require_non_negative_size(int value, std::string_view field_name) {
    if (value < 0) {
        throw std::runtime_error(validation_error(field_name, " must not be negative"));
    }
    return static_cast<std::size_t>(value);
}

int require_non_negative_int(int value, std::string_view field_name) {
    if (value < 0) {
        throw std::runtime_error(validation_error(field_name, " must not be negative"));
    }
    return value;
}

int require_positive_int(int value, std::string_view field_name) {
    if (value <= 0) {
        throw std::runtime_error(validation_error(field_name, " must be greater than zero"));
    }
    return value;
}

template <typename Options>
void apply_predict_runtime_selection(ModelInputMode model_input, const ModelArtifactSelectionState& artifacts,
                                     const std::string& backend, const int max_dets_per_image,
                                     const char* max_dets_field_name, const float threshold, Options& options) {
    options.backend = backend;
    options.max_dets_per_image = require_positive_size(max_dets_per_image, max_dets_field_name);
    options.threshold = threshold;
    apply_model_input(model_input, artifacts.weights_path, artifacts.onnx_path, artifacts.tensorrt_path, options);
}

mmltk::rfdetr::BuildEngineRequest build_build_engine_request(const ExportViewState& state) {
    return build_build_engine_request(state, std::filesystem::path(state.onnx_path));
}

mmltk::rfdetr::ExportOnnxRequest build_export_onnx_request(const ExportViewState& state) {
    return build_export_onnx_request(state, std::filesystem::path(state.onnx_path));
}

mmltk::rfdetr::BuildEngineRequest build_build_engine_request(const ExportViewState& state,
                                                             const std::filesystem::path& onnx_path_override) {
    const std::filesystem::path onnx_path =
        onnx_path_override.empty() ? std::filesystem::path(state.onnx_path) : onnx_path_override;

    mmltk::rfdetr::BuildEngineRequest request;
    request.onnx_path = onnx_path;
    request.output_path = state.output_path;
    request.device_id = state.device_id;
    request.allow_fp16 = state.allow_fp16;
    validate_build_engine_request(request);
    return request;
}

mmltk::rfdetr::ExportOnnxRequest build_export_onnx_request(const ExportViewState& state,
                                                           const std::filesystem::path& output_path_override) {
    return build_export_onnx_request(state.weights_path,
                                     output_path_override.empty() ? state.onnx_path : output_path_override.string(),
                                     state.device_id, state.opset_version, state.simplify);
}

mmltk::rfdetr::BuildEngineRequest build_build_engine_request(const mmltk::rfdetr::ExportOnnxRequest& export_request,
                                                             const ExportViewState& state) {
    return build_build_engine_request(export_request, state.output_path, state.device_id, state.allow_fp16);
}

mmltk::rfdetr::BuildEngineRequest build_build_engine_request(const mmltk::rfdetr::ExportOnnxRequest& export_request,
                                                             const std::filesystem::path& output_path, int device_id,
                                                             bool allow_fp16) {
    mmltk::rfdetr::BuildEngineRequest request;
    request.onnx_path = export_request.output_path;
    request.output_path = output_path;
    request.device_id = device_id;
    request.allow_fp16 = allow_fp16;
    validate_build_engine_request(request);
    return request;
}

mmltk::rfdetr::ExportOnnxRequest build_export_onnx_request(const std::string& weights_path,
                                                           const std::string& output_path, int device_id,
                                                           int opset_version, bool simplify) {
    mmltk::rfdetr::ExportOnnxRequest request;
    request.weights_path = weights_path;
    request.output_path =
        output_path.empty() ? default_export_onnx_output_path(weights_path) : std::filesystem::path(output_path);
    request.device_id = device_id;
    request.opset_version = opset_version;
    request.simplify = simplify;
    validate_export_onnx_request(request);
    return request;
}

void apply_build_engine_request(ExportViewState& state, const mmltk::rfdetr::BuildEngineRequest& request) {
    state.weights_path.clear();
    state.onnx_path = request.onnx_path.string();
    state.output_path = request.output_path.string();
    state.device_id = request.device_id;
    state.allow_fp16 = request.allow_fp16;
    state.build_tensorrt = true;
}

void apply_export_onnx_request(ExportViewState& state, const mmltk::rfdetr::ExportOnnxRequest& request) {
    state.weights_path = request.weights_path.string();
    state.onnx_path = request.output_path.string();
    state.device_id = request.device_id;
    state.opset_version = request.opset_version;
    state.simplify = request.simplify;
    state.build_tensorrt = false;
}

void apply_predict_request(PredictViewState& state, const mmltk::rfdetr::PredictRequest& request) {
    state.source = {};
    if (request.source_kind == mmltk::rfdetr::PredictSourceKind::CompiledDataset) {
        state.source.kind = SourceKind::CompiledDataset;
        state.source.compiled_path = request.compiled_path.string();
    } else if (request.image_inputs.size() == 1U) {
        state.source.kind = SourceKind::SingleImage;
        state.source.single_image_path = request.image_inputs.front().image_path.string();
    } else if (!request.image_inputs.empty()) {
        state.source.kind = SourceKind::ImageFolder;
        state.source.image_directory = request.image_inputs.front().image_path.parent_path().string();
    } else {
        state.source.kind = SourceKind::ImageFolder;
    }

    state.weights_path.clear();
    state.onnx_path.clear();
    state.tensorrt_path.clear();
    state.model_input = model_input_mode_from_request(request);
    apply_model_input(state.model_input, request.weights_path.string(), request.onnx_path.string(),
                      request.tensorrt_path.string(), state);
    state.output_path = request.output_path.string();
    state.backend = request.backend;
    state.cpu_affinity = request.cpu_affinity;
    state.batch_size = static_cast<int>(request.batch_size);
    state.max_dets_per_image = static_cast<int>(request.max_dets_per_image);
    state.device_id = request.device_id;
    state.workers = request.workers;
    state.lanes = request.lanes;
    state.threshold = request.threshold;
    state.allow_fp16 = request.allow_fp16;
    state.progress_bar = request.progress_bar;
    state.compile_mode = request.compilation_mode;
}

void apply_validate_request(ValidateViewState& state, const mmltk::rfdetr::ValidateRequest& request) {
    state.compiled_path = request.compiled_path.string();
    state.source_dir = request.source_dir.string();
    state.onnx_path = request.onnx_path.string();
    state.tensorrt_path = request.tensorrt_path.string();
    state.save_engine_path = request.save_engine_path.string();
    state.report_json_path = request.report_json_path.string();
    state.split = request.split;
    state.eval_order = request.eval_order;
    state.resolution = static_cast<int>(request.resolution);
    state.limit_images = static_cast<int>(request.limit_images);
    state.alignment_images = static_cast<int>(request.alignment_images);
    state.eval_max_dets = static_cast<int>(request.eval_max_dets);
    state.batch_size = static_cast<int>(request.batch_size);
    state.prefetch_factor = static_cast<int>(request.prefetch_factor);
    state.device_id = request.device_id;
    state.workers = request.workers;
    state.compile_workers = request.compile_workers;
    state.compile_cuda_mask_batch_size = request.compile_cuda_mask_batch_size;
    state.compile_cuda_device_id = request.compile_cuda_device_id;
    state.cpu_affinity = request.cpu_affinity;
    state.recompile = request.recompile;
    state.profile = request.profile;
    state.allow_fp16 = request.allow_fp16;
    state.write_report_json = request.write_report_json;
    state.log_mode = request.log_mode;
}

void apply_train_request(TrainViewState& state, const mmltk::rfdetr::TrainRequest& request) {
    apply_train_options(state, request);
    state.recipe_overrides = request.recipe_overrides;
}

mmltk::rfdetr::PredictOptions build_annotate_predict_options(const AnnotateViewState& state,
                                                             const std::string& preset_name,
                                                             mmltk::rfdetr::PredictImageInput input) {
    mmltk::rfdetr::PredictOptions options;
    options.preset_name = preset_name;
    options.source_kind = mmltk::rfdetr::PredictSourceKind::ImageFiles;
    options.image_inputs.push_back(std::move(input));
    options.output_path = std::filesystem::temp_directory_path() / "mmltk-annotate-predict.json";
    options.batch_size = 1U;
    options.progress_bar = false;
    apply_execution_tuning_to_request(execution_tuning(state), "annotate", options);
    apply_predict_runtime_selection(state.model_input, model_artifacts(state), state.backend, state.max_dets_per_image,
                                    "annotate max_dets_per_image", state.threshold, options);
    return options;
}

mmltk::rfdetr::ValidateRequest build_validate_request(const ValidateViewState& state) {
    mmltk::rfdetr::ValidateRequest request;
    request.compiled_path = state.compiled_path;
    request.source_dir = state.source_dir;
    request.onnx_path = state.onnx_path;
    request.tensorrt_path = state.tensorrt_path;
    request.save_engine_path = state.save_engine_path;
    request.report_json_path = state.report_json_path;
    request.split = state.split;
    request.eval_order = state.eval_order;
    request.resolution = static_cast<std::uint32_t>(require_positive_size(state.resolution, "validate resolution"));
    request.limit_images = require_non_negative_size(state.limit_images, "validate limit_images");
    request.alignment_images = require_non_negative_size(state.alignment_images, "validate alignment_images");
    request.eval_max_dets = require_positive_size(state.eval_max_dets, "validate eval_max_dets");
    request.batch_size = require_positive_size(state.batch_size, "validate batch_size");
    request.prefetch_factor = require_positive_size(state.prefetch_factor, "validate prefetch_factor");
    apply_execution_tuning_to_request(execution_tuning(state), "validate", request);
    request.compile_workers = state.compile_workers;
    request.compile_cuda_mask_batch_size = state.compile_cuda_mask_batch_size;
    request.compile_cuda_device_id = state.compile_cuda_device_id;
    request.recompile = state.recompile;
    request.profile = state.profile;
    request.allow_fp16 = state.allow_fp16;
    request.write_report_json = state.write_report_json;
    request.log_mode = state.log_mode;
    finalize_validate_request(request);
    request.log_mode = state.log_mode;
    request.write_report_json = state.write_report_json;
    return request;
}

mmltk::rfdetr::PredictRequest build_predict_request(const PredictViewState& state,
                                                    std::vector<mmltk::rfdetr::PredictImageInput> image_inputs) {
    if (state.source.kind == SourceKind::VideoStream) {
        throw std::runtime_error("batch predict requests do not support live video sources");
    }

    mmltk::rfdetr::PredictRequest request;
    request.source_kind = state.source.kind == SourceKind::CompiledDataset
                              ? mmltk::rfdetr::PredictSourceKind::CompiledDataset
                              : mmltk::rfdetr::PredictSourceKind::ImageFiles;
    if (state.source.kind == SourceKind::CompiledDataset) {
        request.compiled_path = state.source.compiled_path;
    } else {
        request.image_inputs = std::move(image_inputs);
    }
    request.output_path = state.output_path;
    request.batch_size = state.source.kind == SourceKind::SingleImage
                             ? 1U
                             : require_positive_size(state.batch_size, "predict batch_size");
    apply_execution_tuning_to_request(execution_tuning(state), "predict", request);
    apply_predict_runtime_selection(state.model_input, model_artifacts(state), state.backend, state.max_dets_per_image,
                                    "predict max_dets_per_image", state.threshold, request);
    finalize_predict_request(request);
    return request;
}

mmltk::rfdetr::LivePredictOptions build_live_predict_options(const PredictViewState& state,
                                                             const std::string& preset_name) {
    if (state.source.kind != SourceKind::VideoStream) {
        throw std::runtime_error("live predict options require a video device source");
    }

    mmltk::rfdetr::LivePredictOptions options;
    options.preset_name = preset_name;
    options.source.device_path = "/dev/video" + std::to_string(std::max(0, state.source.device_index));
    options.source.width =
        static_cast<std::uint32_t>(require_positive_size(state.source.capture_width, "live capture width"));
    options.source.height =
        static_cast<std::uint32_t>(require_positive_size(state.source.capture_height, "live capture height"));
    options.source.fps =
        static_cast<std::uint32_t>(require_positive_size(state.source.capture_fps, "live capture fps"));
    options.source.v4l2_buffer_count =
        static_cast<std::uint32_t>(require_positive_size(state.source.v4l2_buffer_count, "live v4l2_buffer_count"));
    options.source.initial_region = mmltk::rfdetr::LiveCaptureRegion{
        0U,
        0U,
        options.source.width,
        options.source.height,
    };
    options.split_count = static_cast<std::uint32_t>(require_positive_size(state.live_split_count, "live split_count"));
    options.include_masks = true;
    options.include_status_detections = false;
    apply_execution_tuning_to_request(execution_tuning(state), "live", options);
    apply_predict_runtime_selection(state.model_input, model_artifacts(state), state.backend, state.max_dets_per_image,
                                    "live max_dets_per_image", state.threshold, options);
    return options;
}

mmltk::rfdetr::TrainRequest build_train_request(const TrainViewState& state, const std::vector<int>& device_ids) {
    if (state.epochs <= 0) {
        throw std::runtime_error("train epochs must be greater than zero");
    }
    if (state.grad_accum_steps <= 0) {
        throw std::runtime_error("train grad_accum_steps must be greater than zero");
    }
    if (state.batch_size <= 0) {
        throw std::runtime_error("train batch_size must be greater than zero");
    }
    if (state.prefetch_factor <= 0) {
        throw std::runtime_error("train prefetch_factor must be greater than zero");
    }

    mmltk::rfdetr::TrainOptions options = train_options(state, device_ids);
    options.batch_size = require_positive_size(state.batch_size, "train batch_size");
    options.val_batch_size = require_non_negative_size(state.val_batch_size, "train val_batch_size");
    options.eval_max_dets = require_positive_size(state.eval_max_dets, "train eval_max_dets");
    mmltk::rfdetr::TrainRequest request = train_request_from_options(options, state.recipe_overrides);
    apply_execution_tuning_to_request(execution_tuning(state), "train", request);
    validate_train_request(request);
    return request;
}

std::string summarize_annotation_save_result(const AnnotationSaveResult& result) {
    std::ostringstream stream;
    stream << "annotate saved: scene=" << result.scene_image_path << " instances=" << result.entity_paths.size();
    return stream.str();
}

std::string summarize_validation_result(const mmltk::rfdetr::ValidationRunResult& result) {
    std::ostringstream stream;
    stream << "validate completed: images=" << result.images << " categories=" << result.categories;
    for (const std::string& backend_name : result.eval_order) {
        const auto found = result.backends.find(backend_name);
        if (found == result.backends.end()) {
            continue;
        }
        stream << " " << backend_name << "_bbox_ap=" << found->second.summary.bbox.ap << " " << backend_name
               << "_mask_ap=" << found->second.summary.mask.ap;
    }
    return stream.str();
}

std::string summarize_prediction_result(const mmltk::rfdetr::PredictionRunResult& result) {
    std::ostringstream stream;
    stream << "predict completed: backend=" << result.backend_name << " images=" << result.records.size()
           << " img_per_s=" << result.timing.img_per_s;
    return stream.str();
}

std::optional<StillImagePreview> maybe_make_single_image_preview(const PredictViewState& state,
                                                                 const mmltk::rfdetr::PredictOptions& options,
                                                                 const mmltk::rfdetr::PredictionRunResult& result) {
    if (state.source.kind != SourceKind::SingleImage) {
        return std::nullopt;
    }
    if (options.image_inputs.size() != 1U) {
        throw std::runtime_error("single-image predict expected exactly one image input");
    }
    if (result.records.size() != 1U) {
        throw std::runtime_error("single-image predict expected exactly one prediction record");
    }
    return render_single_image_prediction_preview(options.image_inputs.front(), result.records.front(),
                                                  static_cast<int>(result.class_names.size()), state.device_id);
}

AnnotationBox prediction_to_annotation_box(const mmltk::rfdetr::Prediction& prediction, std::uint32_t width,
                                           std::uint32_t height) {
    AnnotationBox box;
    box.x1 = static_cast<int>(std::floor(prediction.bbox_xyxy[0]));
    box.y1 = static_cast<int>(std::floor(prediction.bbox_xyxy[1]));
    box.x2 = static_cast<int>(std::ceil(prediction.bbox_xyxy[2]));
    box.y2 = static_cast<int>(std::ceil(prediction.bbox_xyxy[3]));
    return normalize_annotation_box(box, width, height);
}

}  // namespace mmltk::gui::rfdetr_workflows
