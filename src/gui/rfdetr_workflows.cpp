#include "rfdetr_workflows.h"

#include "source_selection.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fastloader::gui::rfdetr_workflows {

namespace {

using namespace fastloader::rfdetr;

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

const char* train_optimizer_cli_value(TrainOptimizerMode mode) {
    switch (mode) {
    case TrainOptimizerMode::AdamW:
        return "adamw";
    case TrainOptimizerMode::Muon:
        return "muon";
    }
    return "adamw";
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

fastloader::rfdetr::CompilationMode compilation_mode_from_index(int index) {
    using fastloader::rfdetr::CompilationMode;
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

std::string compilation_mode_cli_value(int index) {
    switch (index) {
    case 0:
        return "none";
    case 1:
        return "selective";
    case 2:
        return "full";
    default:
        throw std::runtime_error("invalid compilation mode index");
    }
}

fastloader::rfdetr::PredictOptions build_annotate_predict_options(
    const AnnotateViewState& state,
    const std::string& preset_name,
    fastloader::rfdetr::PredictImageInput input) {
    fastloader::rfdetr::PredictOptions options;
    options.preset_name = preset_name;
    options.source_kind = fastloader::rfdetr::PredictSourceKind::ImageFiles;
    options.image_inputs.push_back(std::move(input));
    options.output_path = std::filesystem::temp_directory_path() / "fastloader-annotate-predict.json";
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

fastloader::rfdetr::ValidationOptions build_validate_options(const ValidateViewState& state) {
    fastloader::rfdetr::ValidationOptions options;
    options.compiled_path = state.compiled_path;
    options.source_dir = state.source_dir;
    options.onnx_path = state.onnx_path;
    options.tensorrt_path = state.tensorrt_path;
    options.save_engine_path = state.save_engine_path;
    options.report_json_path = state.report_json_path;
    options.split = state.split;
    options.eval_order = state.eval_order;
    options.resolution = static_cast<std::uint32_t>(require_positive_size(state.resolution, "validate resolution"));
    options.limit_images = require_non_negative_size(state.limit_images, "validate limit_images");
    options.alignment_images = require_non_negative_size(state.alignment_images, "validate alignment_images");
    options.eval_max_dets = require_positive_size(state.eval_max_dets, "validate eval_max_dets");
    options.batch_size = require_positive_size(state.batch_size, "validate batch_size");
    options.prefetch_factor = require_positive_size(state.prefetch_factor, "validate prefetch_factor");
    options.device_id = require_non_negative_int(state.device_id, "validate device_id");
    options.workers = require_non_negative_int(state.workers, "validate workers");
    options.cpu_affinity = state.cpu_affinity;
    options.recompile = state.recompile;
    options.profile = state.profile;
    options.allow_fp16 = state.allow_fp16;
    options.write_report_json = state.write_report_json;
    options.log_mode = fastloader::rfdetr::ValidationLogMode::Quiet;
    return options;
}

fastloader::rfdetr::PredictOptions build_predict_options(const PredictViewState& state,
                                                         const std::string& preset_name) {
    if (state.source.kind == SourceKind::VideoStream) {
        throw std::runtime_error("batch predict options do not support live video sources");
    }

    fastloader::rfdetr::PredictOptions options;
    options.preset_name = preset_name;
    options.source_kind = state.source.kind == SourceKind::CompiledDataset
                              ? fastloader::rfdetr::PredictSourceKind::CompiledDataset
                              : fastloader::rfdetr::PredictSourceKind::ImageFiles;
    if (state.source.kind == SourceKind::CompiledDataset) {
        options.compiled_path = state.source.compiled_path;
    }
    options.output_path = state.output_path;
    options.backend = state.backend;
    options.batch_size = state.source.kind == SourceKind::SingleImage
                             ? 1U
                             : require_positive_size(state.batch_size, "predict batch_size");
    options.max_dets_per_image = require_positive_size(state.max_dets_per_image, "predict max_dets_per_image");
    options.device_id = require_non_negative_int(state.device_id, "predict device_id");
    options.workers = require_non_negative_int(state.workers, "predict workers");
    options.lanes = require_non_negative_int(state.lanes, "predict lanes");
    options.threshold = state.threshold;
    options.cpu_affinity = state.cpu_affinity;
    options.allow_fp16 = state.allow_fp16;
    options.progress_bar = state.progress_bar;
    options.compilation_mode = compilation_mode_from_index(state.compile_mode);
    apply_model_input(state.model_input, state.weights_path, state.onnx_path, state.tensorrt_path, options);
    return options;
}

fastloader::rfdetr::LivePredictOptions build_live_predict_options(const PredictViewState& state,
                                                                  const std::string& preset_name) {
    if (state.source.kind != SourceKind::VideoStream) {
        throw std::runtime_error("live predict options require a video device source");
    }

    fastloader::rfdetr::LivePredictOptions options;
    options.preset_name = preset_name;
    options.source.device_path = "/dev/video" + std::to_string(std::max(0, state.source.device_index));
    options.source.width = static_cast<std::uint32_t>(require_positive_size(state.source.capture_width, "live capture width"));
    options.source.height = static_cast<std::uint32_t>(require_positive_size(state.source.capture_height, "live capture height"));
    options.source.fps = static_cast<std::uint32_t>(require_positive_size(state.source.capture_fps, "live capture fps"));
    options.source.v4l2_buffer_count =
        static_cast<std::uint32_t>(require_positive_size(state.source.v4l2_buffer_count, "live v4l2_buffer_count"));
    options.source.initial_region = fastloader::rfdetr::LiveCaptureRegion{
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

TrainCommandConfig build_train_command_config(const TrainViewState& state,
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

    TrainCommandConfig config;
    config.train_compiled_path = state.train_compiled_path;
    config.val_compiled_path = state.val_compiled_path;
    config.test_compiled_path = state.test_compiled_path;
    config.output_dir = state.output_dir;
    config.weights_path = state.weights_path;
    config.resume_path = state.resume_path;
    config.cpu_affinity = state.cpu_affinity;
    config.input_mode = state.input_mode == TrainInputMode::Weights
                            ? TrainCommandInputMode::Weights
                            : TrainCommandInputMode::Resume;
    config.device_ids = device_ids;
    config.batch_size = state.batch_size;
    config.val_batch_size = state.val_batch_size;
    config.epochs = state.epochs;
    config.grad_accum_steps = state.grad_accum_steps;
    config.eval_max_dets = state.eval_max_dets;
    config.lr_drop = state.lr_drop;
    config.prefetch_factor = state.prefetch_factor;
    config.seed = state.seed;
    config.workers = state.workers;
    config.lanes = state.lanes;
    config.lr = state.lr;
    config.lr_encoder = state.lr_encoder;
    config.lr_component_decay = state.lr_component_decay;
    config.encoder_layer_decay = state.encoder_layer_decay;
    config.momentum = state.momentum;
    config.weight_decay = state.weight_decay;
    config.warmup_epochs = state.warmup_epochs;
    config.warmup_momentum = state.warmup_momentum;
    config.lr_min_factor = state.lr_min_factor;
    config.clip_max_norm = state.clip_max_norm;
    config.use_ema = state.use_ema;
    config.amp = state.amp;
    config.progress_bar = state.progress_bar;
    config.freeze_encoder = state.freeze_encoder;
    config.optimizer = train_optimizer_cli_value(state.optimizer);
    config.lr_scheduler = state.lr_scheduler;
    config.compile_mode = compilation_mode_cli_value(state.compile_mode);
    return config;
}

std::string summarize_annotation_save_result(const AnnotationSaveResult& result) {
    std::ostringstream stream;
    stream << "annotate saved: scene=" << result.scene_image_path
           << " instances=" << result.entity_paths.size();
    return stream.str();
}

std::string summarize_validation_result(const fastloader::rfdetr::ValidationRunResult& result) {
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

std::string summarize_prediction_result(const fastloader::rfdetr::PredictionRunResult& result) {
    std::ostringstream stream;
    stream << "predict completed: backend=" << result.backend_name
           << " images=" << result.records.size()
           << " img_per_s=" << result.timing.img_per_s;
    return stream.str();
}

std::optional<StillImagePreview> maybe_make_single_image_preview(
    const PredictViewState& state,
    const fastloader::rfdetr::PredictOptions& options,
    const fastloader::rfdetr::PredictionRunResult& result) {
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

AnnotationBox prediction_to_annotation_box(const fastloader::rfdetr::Prediction& prediction,
                                           std::uint32_t width,
                                           std::uint32_t height) {
    AnnotationBox box;
    box.x1 = static_cast<int>(std::floor(prediction.bbox_xyxy[0]));
    box.y1 = static_cast<int>(std::floor(prediction.bbox_xyxy[1]));
    box.x2 = static_cast<int>(std::ceil(prediction.bbox_xyxy[2]));
    box.y2 = static_cast<int>(std::ceil(prediction.bbox_xyxy[3]));
    return normalize_annotation_box(box, width, height);
}

} // namespace fastloader::gui::rfdetr_workflows
