#include "gui_settings.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <utility>

namespace fastloader::gui {

namespace {

template <typename T>
void get_optional(const nlohmann::json& j, const char* key, T& out) {
    if (j.contains(key)) {
        j.at(key).get_to(out);
    }
}

} // namespace

// ---- SourceSelectionState ----

void to_json(nlohmann::json& j, const SourceSelectionState& s) {
    j = nlohmann::json{
        {"kind", static_cast<int>(s.kind)},
        {"compiled_path", s.compiled_path},
        {"single_image_path", s.single_image_path},
        {"image_directory", s.image_directory},
        {"recursive", s.recursive},
        {"device_index", s.device_index},
        {"capture_width", s.capture_width},
        {"capture_height", s.capture_height},
        {"capture_fps", s.capture_fps},
        {"v4l2_buffer_count", s.v4l2_buffer_count},
        {"crop_x", s.crop_x},
        {"crop_y", s.crop_y},
        {"crop_width", s.crop_width},
        {"crop_height", s.crop_height},
    };
}

void from_json(const nlohmann::json& j, SourceSelectionState& s) {
    int kind = static_cast<int>(s.kind);
    get_optional(j, "kind", kind);
    s.kind = static_cast<SourceKind>(kind);
    get_optional(j, "compiled_path", s.compiled_path);
    get_optional(j, "single_image_path", s.single_image_path);
    get_optional(j, "image_directory", s.image_directory);
    get_optional(j, "recursive", s.recursive);
    get_optional(j, "device_index", s.device_index);
    get_optional(j, "capture_width", s.capture_width);
    get_optional(j, "capture_height", s.capture_height);
    get_optional(j, "capture_fps", s.capture_fps);
    get_optional(j, "v4l2_buffer_count", s.v4l2_buffer_count);
    get_optional(j, "crop_x", s.crop_x);
    get_optional(j, "crop_y", s.crop_y);
    get_optional(j, "crop_width", s.crop_width);
    get_optional(j, "crop_height", s.crop_height);
}

// ---- TrainViewState ----

void to_json(nlohmann::json& j, const TrainViewState& s) {
    j = nlohmann::json{
        {"train_compiled_path", s.train_compiled_path},
        {"val_compiled_path", s.val_compiled_path},
        {"test_compiled_path", s.test_compiled_path},
        {"output_dir", s.output_dir},
        {"weights_path", s.weights_path},
        {"resume_path", s.resume_path},
        {"cpu_affinity", s.cpu_affinity},
        {"input_mode", static_cast<int>(s.input_mode)},
        {"batch_size", s.batch_size},
        {"val_batch_size", s.val_batch_size},
        {"epochs", s.epochs},
        {"grad_accum_steps", s.grad_accum_steps},
        {"eval_max_dets", s.eval_max_dets},
        {"lr_drop", s.lr_drop},
        {"prefetch_factor", s.prefetch_factor},
        {"seed", s.seed},
        {"workers", s.workers},
        {"lanes", s.lanes},
        {"lr", s.lr},
        {"lr_encoder", s.lr_encoder},
        {"lr_component_decay", s.lr_component_decay},
        {"encoder_layer_decay", s.encoder_layer_decay},
        {"momentum", s.momentum},
        {"weight_decay", s.weight_decay},
        {"warmup_epochs", s.warmup_epochs},
        {"warmup_momentum", s.warmup_momentum},
        {"lr_min_factor", s.lr_min_factor},
        {"clip_max_norm", s.clip_max_norm},
        {"lr_scheduler", s.lr_scheduler},
        {"use_ema", s.use_ema},
        {"amp", s.amp},
        {"progress_bar", s.progress_bar},
        {"freeze_encoder", s.freeze_encoder},
        {"optimizer", static_cast<int>(s.optimizer)},
        {"compile_mode", s.compile_mode},
        {"execution_target", static_cast<int>(s.execution_target)},
        {"remote_family_enabled", s.remote_family_enabled},
        {"recipe_overrides",
         {
             {"lr", s.recipe_overrides.lr},
             {"lr_encoder", s.recipe_overrides.lr_encoder},
             {"lr_component_decay", s.recipe_overrides.lr_component_decay},
             {"encoder_layer_decay", s.recipe_overrides.encoder_layer_decay},
             {"momentum", s.recipe_overrides.momentum},
             {"weight_decay", s.recipe_overrides.weight_decay},
             {"warmup_epochs", s.recipe_overrides.warmup_epochs},
             {"warmup_momentum", s.recipe_overrides.warmup_momentum},
             {"lr_min_factor", s.recipe_overrides.lr_min_factor},
             {"lr_drop", s.recipe_overrides.lr_drop},
             {"lr_scheduler", s.recipe_overrides.lr_scheduler},
         }},
    };
}

void from_json(const nlohmann::json& j, TrainViewState& s) {
    get_optional(j, "train_compiled_path", s.train_compiled_path);
    get_optional(j, "val_compiled_path", s.val_compiled_path);
    get_optional(j, "test_compiled_path", s.test_compiled_path);
    get_optional(j, "output_dir", s.output_dir);
    get_optional(j, "weights_path", s.weights_path);
    get_optional(j, "resume_path", s.resume_path);
    get_optional(j, "cpu_affinity", s.cpu_affinity);
    int input_mode = static_cast<int>(s.input_mode);
    get_optional(j, "input_mode", input_mode);
    s.input_mode = static_cast<TrainInputMode>(input_mode);
    get_optional(j, "batch_size", s.batch_size);
    get_optional(j, "val_batch_size", s.val_batch_size);
    get_optional(j, "epochs", s.epochs);
    get_optional(j, "grad_accum_steps", s.grad_accum_steps);
    get_optional(j, "eval_max_dets", s.eval_max_dets);
    get_optional(j, "lr_drop", s.lr_drop);
    get_optional(j, "prefetch_factor", s.prefetch_factor);
    get_optional(j, "seed", s.seed);
    get_optional(j, "workers", s.workers);
    get_optional(j, "lanes", s.lanes);
    get_optional(j, "lr", s.lr);
    get_optional(j, "lr_encoder", s.lr_encoder);
    get_optional(j, "lr_component_decay", s.lr_component_decay);
    get_optional(j, "encoder_layer_decay", s.encoder_layer_decay);
    get_optional(j, "momentum", s.momentum);
    get_optional(j, "weight_decay", s.weight_decay);
    get_optional(j, "warmup_epochs", s.warmup_epochs);
    get_optional(j, "warmup_momentum", s.warmup_momentum);
    get_optional(j, "lr_min_factor", s.lr_min_factor);
    get_optional(j, "clip_max_norm", s.clip_max_norm);
    get_optional(j, "lr_scheduler", s.lr_scheduler);
    get_optional(j, "use_ema", s.use_ema);
    get_optional(j, "amp", s.amp);
    get_optional(j, "progress_bar", s.progress_bar);
    get_optional(j, "freeze_encoder", s.freeze_encoder);
    int optimizer = static_cast<int>(s.optimizer);
    get_optional(j, "optimizer", optimizer);
    s.optimizer = static_cast<TrainOptimizerMode>(optimizer);
    get_optional(j, "compile_mode", s.compile_mode);
    int execution_target = static_cast<int>(s.execution_target);
    get_optional(j, "execution_target", execution_target);
    s.execution_target = static_cast<TrainExecutionTarget>(execution_target);
    get_optional(j, "remote_family_enabled", s.remote_family_enabled);
    if (const auto overrides = j.find("recipe_overrides"); overrides != j.end() && overrides->is_object()) {
        get_optional(*overrides, "lr", s.recipe_overrides.lr);
        get_optional(*overrides, "lr_encoder", s.recipe_overrides.lr_encoder);
        get_optional(*overrides, "lr_component_decay", s.recipe_overrides.lr_component_decay);
        get_optional(*overrides, "encoder_layer_decay", s.recipe_overrides.encoder_layer_decay);
        get_optional(*overrides, "momentum", s.recipe_overrides.momentum);
        get_optional(*overrides, "weight_decay", s.recipe_overrides.weight_decay);
        get_optional(*overrides, "warmup_epochs", s.recipe_overrides.warmup_epochs);
        get_optional(*overrides, "warmup_momentum", s.recipe_overrides.warmup_momentum);
        get_optional(*overrides, "lr_min_factor", s.recipe_overrides.lr_min_factor);
        get_optional(*overrides, "lr_drop", s.recipe_overrides.lr_drop);
        get_optional(*overrides, "lr_scheduler", s.recipe_overrides.lr_scheduler);
    }
}

// ---- ValidateViewState ----

void to_json(nlohmann::json& j, const ValidateViewState& s) {
    j = nlohmann::json{
        {"compiled_path", s.compiled_path},
        {"source_dir", s.source_dir},
        {"onnx_path", s.onnx_path},
        {"tensorrt_path", s.tensorrt_path},
        {"save_engine_path", s.save_engine_path},
        {"report_json_path", s.report_json_path},
        {"split", s.split},
        {"eval_order", s.eval_order},
        {"cpu_affinity", s.cpu_affinity},
        {"resolution", s.resolution},
        {"limit_images", s.limit_images},
        {"alignment_images", s.alignment_images},
        {"eval_max_dets", s.eval_max_dets},
        {"batch_size", s.batch_size},
        {"prefetch_factor", s.prefetch_factor},
        {"device_id", s.device_id},
        {"workers", s.workers},
        {"recompile", s.recompile},
        {"profile", s.profile},
        {"allow_fp16", s.allow_fp16},
        {"write_report_json", s.write_report_json},
    };
}

void from_json(const nlohmann::json& j, ValidateViewState& s) {
    get_optional(j, "compiled_path", s.compiled_path);
    get_optional(j, "source_dir", s.source_dir);
    get_optional(j, "onnx_path", s.onnx_path);
    get_optional(j, "tensorrt_path", s.tensorrt_path);
    get_optional(j, "save_engine_path", s.save_engine_path);
    get_optional(j, "report_json_path", s.report_json_path);
    get_optional(j, "split", s.split);
    get_optional(j, "eval_order", s.eval_order);
    get_optional(j, "cpu_affinity", s.cpu_affinity);
    get_optional(j, "resolution", s.resolution);
    get_optional(j, "limit_images", s.limit_images);
    get_optional(j, "alignment_images", s.alignment_images);
    get_optional(j, "eval_max_dets", s.eval_max_dets);
    get_optional(j, "batch_size", s.batch_size);
    get_optional(j, "prefetch_factor", s.prefetch_factor);
    get_optional(j, "device_id", s.device_id);
    get_optional(j, "workers", s.workers);
    get_optional(j, "recompile", s.recompile);
    get_optional(j, "profile", s.profile);
    get_optional(j, "allow_fp16", s.allow_fp16);
    get_optional(j, "write_report_json", s.write_report_json);
}

// ---- PredictViewState ----

void to_json(nlohmann::json& j, const PredictViewState& s) {
    j = nlohmann::json{
        {"source", s.source},
        {"weights_path", s.weights_path},
        {"onnx_path", s.onnx_path},
        {"tensorrt_path", s.tensorrt_path},
        {"output_path", s.output_path},
        {"backend", s.backend},
        {"cpu_affinity", s.cpu_affinity},
        {"model_input", static_cast<int>(s.model_input)},
        {"batch_size", s.batch_size},
        {"max_dets_per_image", s.max_dets_per_image},
        {"live_split_count", s.live_split_count},
        {"device_id", s.device_id},
        {"workers", s.workers},
        {"lanes", s.lanes},
        {"threshold", s.threshold},
        {"allow_fp16", s.allow_fp16},
        {"progress_bar", s.progress_bar},
        {"compile_mode", s.compile_mode},
    };
}

void from_json(const nlohmann::json& j, PredictViewState& s) {
    get_optional(j, "source", s.source);
    get_optional(j, "weights_path", s.weights_path);
    get_optional(j, "onnx_path", s.onnx_path);
    get_optional(j, "tensorrt_path", s.tensorrt_path);
    get_optional(j, "output_path", s.output_path);
    get_optional(j, "backend", s.backend);
    get_optional(j, "cpu_affinity", s.cpu_affinity);
    int model_input = static_cast<int>(s.model_input);
    get_optional(j, "model_input", model_input);
    s.model_input = static_cast<ModelInputMode>(model_input);
    get_optional(j, "batch_size", s.batch_size);
    get_optional(j, "max_dets_per_image", s.max_dets_per_image);
    get_optional(j, "live_split_count", s.live_split_count);
    get_optional(j, "device_id", s.device_id);
    get_optional(j, "workers", s.workers);
    get_optional(j, "lanes", s.lanes);
    get_optional(j, "threshold", s.threshold);
    get_optional(j, "allow_fp16", s.allow_fp16);
    get_optional(j, "progress_bar", s.progress_bar);
    get_optional(j, "compile_mode", s.compile_mode);
}

// ---- UiSettingsState ----

void to_json(nlohmann::json& j, const UiSettingsState& s) {
    j = nlohmann::json{
        {"ui_scale", s.ui_scale},
        {"font_size", s.font_size},
        {"secondary_font_size", s.secondary_font_size},
        {"mono_font_size", s.mono_font_size},
        {"property_label_width", s.property_label_width},
        {"density", static_cast<int>(s.density)},
    };
}

void from_json(const nlohmann::json& j, UiSettingsState& s) {
    get_optional(j, "ui_scale", s.ui_scale);
    get_optional(j, "font_size", s.font_size);
    get_optional(j, "secondary_font_size", s.secondary_font_size);
    get_optional(j, "mono_font_size", s.mono_font_size);
    get_optional(j, "property_label_width", s.property_label_width);
    int density = static_cast<int>(s.density);
    get_optional(j, "density", density);
    s.density = static_cast<UiDensity>(density);
}

// ---- AnnotateViewState ----

void to_json(nlohmann::json& j, const AnnotateViewState& s) {
    j = nlohmann::json{
        {"source", s.source},
        {"weights_path", s.weights_path},
        {"onnx_path", s.onnx_path},
        {"tensorrt_path", s.tensorrt_path},
        {"output_dir", s.output_dir},
        {"split", s.split},
        {"backend", s.backend},
        {"model_input", static_cast<int>(s.model_input)},
        {"device_id", s.device_id},
        {"max_dets_per_image", s.max_dets_per_image},
        {"threshold", s.threshold},
        {"allow_fp16", s.allow_fp16},
        {"full_frame", s.full_frame},
        {"compile_mode", s.compile_mode},
    };
}

void from_json(const nlohmann::json& j, AnnotateViewState& s) {
    get_optional(j, "source", s.source);
    get_optional(j, "weights_path", s.weights_path);
    get_optional(j, "onnx_path", s.onnx_path);
    get_optional(j, "tensorrt_path", s.tensorrt_path);
    get_optional(j, "output_dir", s.output_dir);
    get_optional(j, "split", s.split);
    get_optional(j, "backend", s.backend);
    int model_input = static_cast<int>(s.model_input);
    get_optional(j, "model_input", model_input);
    s.model_input = static_cast<ModelInputMode>(model_input);
    get_optional(j, "device_id", s.device_id);
    get_optional(j, "max_dets_per_image", s.max_dets_per_image);
    get_optional(j, "threshold", s.threshold);
    get_optional(j, "allow_fp16", s.allow_fp16);
    get_optional(j, "full_frame", s.full_frame);
    get_optional(j, "compile_mode", s.compile_mode);
}

// ---- ExportViewState ----

void to_json(nlohmann::json& j, const ExportViewState& s) {
    j = nlohmann::json{
        {"weights_path", s.weights_path},
        {"onnx_path", s.onnx_path},
        {"output_path", s.output_path},
        {"device_id", s.device_id},
        {"opset_version", s.opset_version},
        {"allow_fp16", s.allow_fp16},
        {"build_tensorrt", s.build_tensorrt},
        {"simplify", s.simplify},
    };
}

void from_json(const nlohmann::json& j, ExportViewState& s) {
    get_optional(j, "weights_path", s.weights_path);
    get_optional(j, "onnx_path", s.onnx_path);
    get_optional(j, "output_path", s.output_path);
    get_optional(j, "device_id", s.device_id);
    get_optional(j, "opset_version", s.opset_version);
    get_optional(j, "allow_fp16", s.allow_fp16);
    get_optional(j, "build_tensorrt", s.build_tensorrt);
    get_optional(j, "simplify", s.simplify);
}

// ---- Snapshot / Apply ----

nlohmann::json snapshot_gui_settings(const GuiSettingsSnapshot& snap) {
    nlohmann::json j;
    j["current_view"] = static_cast<int>(snap.current_view);
    j["selected_preset"] = snap.selected_preset;
    if (snap.ui_settings) j["ui"] = *snap.ui_settings;
    if (snap.train) j["train"] = *snap.train;
    if (snap.validate) j["validate"] = *snap.validate;
    if (snap.predict) j["predict"] = *snap.predict;
    if (snap.annotate) j["annotate"] = *snap.annotate;
    if (snap.export_state) j["export"] = *snap.export_state;
    return j;
}

void apply_gui_settings(const nlohmann::json& j, GuiSettingsSnapshot& snap) {
    if (j.contains("current_view")) {
        int v = j.at("current_view").get<int>();
        snap.current_view = static_cast<View>(v);
    }
    get_optional(j, "selected_preset", snap.selected_preset);
    if (j.contains("ui") && snap.ui_settings) j.at("ui").get_to(*snap.ui_settings);
    if (j.contains("train") && snap.train) j.at("train").get_to(*snap.train);
    if (j.contains("validate") && snap.validate) j.at("validate").get_to(*snap.validate);
    if (j.contains("predict") && snap.predict) j.at("predict").get_to(*snap.predict);
    if (j.contains("annotate") && snap.annotate) j.at("annotate").get_to(*snap.annotate);
    if (j.contains("export") && snap.export_state) j.at("export").get_to(*snap.export_state);
}

// ---- GuiSettingsPersistence ----

GuiSettingsPersistence::GuiSettingsPersistence(std::string path)
    : path_(std::move(path)),
      writer_thread_(&GuiSettingsPersistence::writer_main, this) {}

GuiSettingsPersistence::~GuiSettingsPersistence() {
    flush();
    {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        writer_should_stop_ = true;
    }
    writer_cv_.notify_all();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
}

bool GuiSettingsPersistence::load(GuiSettingsSnapshot& snap) {
    std::ifstream file(path_);
    if (!file.is_open()) {
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(file);
        apply_gui_settings(j, snap);
        last_saved_ = std::move(j);
        return true;
    } catch (const nlohmann::json::exception& e) {
        std::fprintf(stderr, "[gui] failed to load settings from %s: %s\n", path_.c_str(), e.what());
        return false;
    }
}

void GuiSettingsPersistence::notify_frame(const GuiSettingsSnapshot& snap) {
    nlohmann::json current = snapshot_gui_settings(snap);
    if (current != last_saved_) {
        if (!dirty_) {
            dirty_ = true;
        }
        last_change_time_ = std::chrono::steady_clock::now();
        last_saved_ = current;
    }
    if (dirty_) {
        const auto elapsed = std::chrono::steady_clock::now() - last_change_time_;
        if (elapsed >= kSaveDelay) {
            enqueue_save(last_saved_);
            dirty_ = false;
        }
    }
}

void GuiSettingsPersistence::flush() {
    if (dirty_) {
        enqueue_save(last_saved_);
        dirty_ = false;
    }
    std::unique_lock<std::mutex> lock(writer_mutex_);
    writer_cv_.wait(lock, [this]() {
        return !pending_save_.has_value() && !save_in_flight_;
    });
}

void GuiSettingsPersistence::enqueue_save(nlohmann::json j) {
    {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        pending_save_ = std::move(j);
    }
    writer_cv_.notify_all();
}

void GuiSettingsPersistence::writer_main() {
    while (true) {
        std::optional<nlohmann::json> save_request;
        {
            std::unique_lock<std::mutex> lock(writer_mutex_);
            writer_cv_.wait(lock, [this]() {
                return writer_should_stop_ || pending_save_.has_value();
            });
            if (writer_should_stop_ && !pending_save_.has_value()) {
                return;
            }
            save_in_flight_ = true;
            save_request = std::move(pending_save_);
            pending_save_.reset();
        }

        if (save_request.has_value()) {
            save_to_disk(*save_request);
        }

        {
            std::lock_guard<std::mutex> lock(writer_mutex_);
            save_in_flight_ = false;
        }
        writer_cv_.notify_all();
    }
}

void GuiSettingsPersistence::save_to_disk(const nlohmann::json& j) {
    const std::string tmp_path = path_ + ".tmp";
    {
        std::ofstream file(tmp_path);
        if (!file.is_open()) {
            std::fprintf(stderr, "[gui] failed to write settings to %s\n", tmp_path.c_str());
            return;
        }
        file << j.dump(2) << '\n';
    }
    if (std::rename(tmp_path.c_str(), path_.c_str()) != 0) {
        std::fprintf(stderr, "[gui] failed to rename %s -> %s\n", tmp_path.c_str(), path_.c_str());
    }
}

} // namespace fastloader::gui
