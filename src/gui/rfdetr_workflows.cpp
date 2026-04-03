#include "rfdetr_workflows.h"

#include "source_selection.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mmltk::gui::rfdetr_workflows {

namespace {

using namespace mmltk::rfdetr;

template <typename Options>
void apply_model_input(ModelInputMode mode,
                       const std::string& weights_path,
                       const std::string& onnx_path,
                       const std::string& tensorrt_path,
                       Options& options) {
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

TrainOptimizerKind train_optimizer_kind_from_mode(TrainOptimizerMode mode) {
    switch (mode) {
    case TrainOptimizerMode::AdamW:
        return TrainOptimizerKind::AdamW;
    case TrainOptimizerMode::Muon:
        return TrainOptimizerKind::Muon;
    }
    return TrainOptimizerKind::AdamW;
}

std::filesystem::path default_export_onnx_output_path(const std::filesystem::path& weights_path) {
    if (weights_path.empty()) {
        return {};
    }
    return weights_path.parent_path() / (weights_path.stem().string() + ".onnx");
}

} // namespace

std::size_t require_positive_size(int value, const char* field_name) {
    if (value <= 0) {
        throw std::runtime_error(std::string(field_name) + " must be greater than zero");
    }
    return static_cast<std::size_t>(value);
}

std::size_t require_non_negative_size(int value, const char* field_name) {
    if (value < 0) {
        throw std::runtime_error(std::string(field_name) + " must not be negative");
    }
    return static_cast<std::size_t>(value);
}

int require_non_negative_int(int value, const char* field_name) {
    if (value < 0) {
        throw std::runtime_error(std::string(field_name) + " must not be negative");
    }
    return value;
}

int require_positive_int(int value, const char* field_name) {
    if (value <= 0) {
        throw std::runtime_error(std::string(field_name) + " must be greater than zero");
    }
    return value;
}

mmltk::rfdetr::CompilationMode compilation_mode_from_index(int index) {
    using mmltk::rfdetr::CompilationMode;
    switch (index) {
    case 0:
        return CompilationMode::kNone;
    case 1:
        return CompilationMode::kSelective;
    case 2:
        return CompilationMode::kFullTrace;
    default:
        throw std::runtime_error("invalid compilation mode index");
    }
}

mmltk::rfdetr::BuildEngineRequest build_build_engine_request(const ExportViewState& state) {
    return build_build_engine_request(state, std::filesystem::path(state.onnx_path));
}

mmltk::rfdetr::ExportOnnxRequest build_export_onnx_request(const ExportViewState& state) {
    return build_export_onnx_request(state, std::filesystem::path(state.onnx_path));
}

mmltk::rfdetr::BuildEngineRequest build_build_engine_request(
    const ExportViewState& state,
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

mmltk::rfdetr::ExportOnnxRequest build_export_onnx_request(
    const ExportViewState& state,
    const std::filesystem::path& output_path_override) {
    return build_export_onnx_request(state.weights_path,
                                     output_path_override.empty() ? state.onnx_path : output_path_override.string(),
                                     state.device_id,
                                     state.opset_version,
                                     state.simplify);
}

mmltk::rfdetr::BuildEngineRequest build_build_engine_request(
    const mmltk::rfdetr::ExportOnnxRequest& export_request,
    const ExportViewState& state) {
    return build_build_engine_request(export_request,
                                      state.output_path,
                                      state.device_id,
                                      state.allow_fp16);
}

mmltk::rfdetr::BuildEngineRequest build_build_engine_request(
    const mmltk::rfdetr::ExportOnnxRequest& export_request,
    const std::filesystem::path& output_path,
    int device_id,
    bool allow_fp16) {
    mmltk::rfdetr::BuildEngineRequest request;
    request.onnx_path = export_request.output_path;
    request.output_path = output_path;
    request.device_id = device_id;
    request.allow_fp16 = allow_fp16;
    validate_build_engine_request(request);
    return request;
}

mmltk::rfdetr::ExportOnnxRequest build_export_onnx_request(
    const std::string& weights_path,
    const std::string& output_path,
    int device_id,
    int opset_version,
    bool simplify) {
    mmltk::rfdetr::ExportOnnxRequest request;
    request.weights_path = weights_path;
    request.output_path = output_path.empty() ? default_export_onnx_output_path(weights_path) : std::filesystem::path(output_path);
    request.device_id = device_id;
    request.opset_version = opset_version;
    request.simplify = simplify;
    validate_export_onnx_request(request);
    return request;
}

void apply_build_engine_request(ExportViewState& state,
                                const mmltk::rfdetr::BuildEngineRequest& request) {
    state.weights_path.clear();
    state.onnx_path = request.onnx_path.string();
    state.output_path = request.output_path.string();
    state.device_id = request.device_id;
    state.allow_fp16 = request.allow_fp16;
    state.build_tensorrt = true;
}

void apply_export_onnx_request(ExportViewState& state,
                               const mmltk::rfdetr::ExportOnnxRequest& request) {
    state.weights_path = request.weights_path.string();
    state.onnx_path = request.output_path.string();
    state.device_id = request.device_id;
    state.opset_version = request.opset_version;
    state.simplify = request.simplify;
    state.build_tensorrt = false;
}

mmltk::rfdetr::PredictOptions build_annotate_predict_options(
    const AnnotateViewState& state,
    const std::string& preset_name,
    mmltk::rfdetr::PredictImageInput input) {
    mmltk::rfdetr::PredictOptions options;
    options.preset_name = preset_name;
    options.source_kind = mmltk::rfdetr::PredictSourceKind::ImageFiles;
    options.image_inputs.push_back(std::move(input));
    options.output_path = std::filesystem::temp_directory_path() / "mmltk-annotate-predict.json";
    options.backend = state.backend;
    options.batch_size = 1U;
    options.max_dets_per_image = require_positive_size(state.max_dets_per_image, "annotate max_dets_per_image");
    options.device_id = require_non_negative_int(state.device_id, "annotate device_id");
    options.threshold = state.threshold;
    options.allow_fp16 = state.allow_fp16;
    options.progress_bar = false;
    options.compilation_mode = compilation_mode_from_index(state.compile_mode);
    apply_model_input(state.model_input, state.weights_path, state.onnx_path, state.tensorrt_path, options);
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
    request.device_id = require_non_negative_int(state.device_id, "validate device_id");
    request.workers = require_non_negative_int(state.workers, "validate workers");
    request.cpu_affinity = state.cpu_affinity;
    request.recompile = state.recompile;
    request.profile = state.profile;
    request.allow_fp16 = state.allow_fp16;
    request.write_report_json = state.write_report_json;
    request.log_mode = mmltk::rfdetr::ValidationLogMode::Quiet;
    finalize_validate_request(request);
    request.log_mode = mmltk::rfdetr::ValidationLogMode::Quiet;
    request.write_report_json = state.write_report_json;
    return request;
}

mmltk::rfdetr::PredictRequest build_predict_request(
    const PredictViewState& state,
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
    request.backend = state.backend;
    request.batch_size = state.source.kind == SourceKind::SingleImage
                             ? 1U
                             : require_positive_size(state.batch_size, "predict batch_size");
    request.max_dets_per_image = require_positive_size(state.max_dets_per_image, "predict max_dets_per_image");
    request.device_id = require_non_negative_int(state.device_id, "predict device_id");
    request.workers = require_non_negative_int(state.workers, "predict workers");
    request.lanes = require_non_negative_int(state.lanes, "predict lanes");
    request.threshold = state.threshold;
    request.cpu_affinity = state.cpu_affinity;
    request.allow_fp16 = state.allow_fp16;
    request.progress_bar = state.progress_bar;
    request.compilation_mode = compilation_mode_from_index(state.compile_mode);
    apply_model_input(state.model_input, state.weights_path, state.onnx_path, state.tensorrt_path, request);
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
    options.source.width = static_cast<std::uint32_t>(require_positive_size(state.source.capture_width, "live capture width"));
    options.source.height = static_cast<std::uint32_t>(require_positive_size(state.source.capture_height, "live capture height"));
    options.source.fps = static_cast<std::uint32_t>(require_positive_size(state.source.capture_fps, "live capture fps"));
    options.source.v4l2_buffer_count =
        static_cast<std::uint32_t>(require_positive_size(state.source.v4l2_buffer_count, "live v4l2_buffer_count"));
    options.source.initial_region = mmltk::rfdetr::LiveCaptureRegion{
        0U,
        0U,
        options.source.width,
        options.source.height,
    };
    options.backend = state.backend;
    options.max_dets_per_image = require_positive_size(state.max_dets_per_image, "live max_dets_per_image");
    options.split_count = static_cast<std::uint32_t>(require_positive_size(state.live_split_count, "live split_count"));
    options.device_id = require_non_negative_int(state.device_id, "live device_id");
    options.threshold = state.threshold;
    options.include_masks = true;
    options.include_status_detections = false;
    options.allow_fp16 = state.allow_fp16;
    options.compilation_mode = compilation_mode_from_index(state.compile_mode);
    apply_model_input(state.model_input, state.weights_path, state.onnx_path, state.tensorrt_path, options);
    return options;
}

mmltk::rfdetr::TrainRequest build_train_request(const TrainViewState& state,
                                                const std::vector<int>& device_ids) {
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

    mmltk::rfdetr::TrainRequest request;
    request.train_compiled_path = state.train_compiled_path;
    request.val_compiled_path = state.val_compiled_path;
    request.test_compiled_path = state.test_compiled_path;
    request.output_dir = state.output_dir;
    if (state.input_mode == TrainInputMode::Weights) {
        request.weights_path = state.weights_path;
        request.resume_path.clear();
    } else {
        request.resume_path = state.resume_path;
        request.weights_path.clear();
    }
    request.cpu_affinity = state.cpu_affinity;
    request.device_id = device_ids.size() == 1U ? device_ids.front() : -1;
    request.device_ids = device_ids;
    request.batch_size = require_positive_size(state.batch_size, "train batch_size");
    request.val_batch_size = require_non_negative_size(state.val_batch_size, "train val_batch_size");
    request.epochs = state.epochs;
    request.grad_accum_steps = state.grad_accum_steps;
    request.eval_max_dets = require_positive_size(state.eval_max_dets, "train eval_max_dets");
    request.lr_drop = state.lr_drop;
    request.print_freq = state.print_freq;
    request.prefetch_factor = state.prefetch_factor;
    request.seed = state.seed;
    request.workers = state.workers;
    request.lanes = state.lanes;
    request.lr = state.lr;
    request.lr_encoder = state.lr_encoder;
    request.lr_component_decay = state.lr_component_decay;
    request.encoder_layer_decay = state.encoder_layer_decay;
    request.momentum = state.momentum;
    request.weight_decay = state.weight_decay;
    request.warmup_epochs = state.warmup_epochs;
    request.warmup_momentum = state.warmup_momentum;
    request.lr_min_factor = state.lr_min_factor;
    request.clip_max_norm = state.clip_max_norm;
    request.use_ema = state.use_ema;
    request.amp = state.amp;
    request.progress_bar = state.progress_bar;
    request.freeze_encoder = state.freeze_encoder;
    request.optimizer = train_optimizer_kind_from_mode(state.optimizer);
    request.lr_scheduler = state.lr_scheduler;
    request.compilation_mode = compilation_mode_from_index(state.compile_mode);
    request.recipe_overrides = state.recipe_overrides;
    validate_train_request(request);
    return request;
}

std::string summarize_annotation_save_result(const AnnotationSaveResult& result) {
    std::ostringstream stream;
    stream << "annotate saved: scene=" << result.scene_image_path
           << " instances=" << result.entity_paths.size();
    return stream.str();
}

std::string summarize_validation_result(const mmltk::rfdetr::ValidationRunResult& result) {
    std::ostringstream stream;
    stream << "validate completed: images=" << result.images
           << " categories=" << result.categories;
    for (const std::string& backend_name : result.eval_order) {
        const auto found = result.backends.find(backend_name);
        if (found == result.backends.end()) {
            continue;
        }
        stream << " " << backend_name << "_bbox_ap=" << found->second.summary.bbox.ap
               << " " << backend_name << "_mask_ap=" << found->second.summary.mask.ap;
    }
    return stream.str();
}

std::string summarize_prediction_result(const mmltk::rfdetr::PredictionRunResult& result) {
    std::ostringstream stream;
    stream << "predict completed: backend=" << result.backend_name
           << " images=" << result.records.size()
           << " img_per_s=" << result.timing.img_per_s;
    return stream.str();
}

std::optional<StillImagePreview> maybe_make_single_image_preview(
    const PredictViewState& state,
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
    return render_single_image_prediction_preview(options.image_inputs.front(),
                                                  result.records.front(),
                                                  static_cast<int>(result.class_names.size()),
                                                  state.device_id);
}

AnnotationBox prediction_to_annotation_box(const mmltk::rfdetr::Prediction& prediction,
                                           std::uint32_t width,
                                           std::uint32_t height) {
    AnnotationBox box;
    box.x1 = static_cast<int>(std::floor(prediction.bbox_xyxy[0]));
    box.y1 = static_cast<int>(std::floor(prediction.bbox_xyxy[1]));
    box.x2 = static_cast<int>(std::ceil(prediction.bbox_xyxy[2]));
    box.y2 = static_cast<int>(std::ceil(prediction.bbox_xyxy[3]));
    return normalize_annotation_box(box, width, height);
}

} // namespace mmltk::gui::rfdetr_workflows
