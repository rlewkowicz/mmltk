#pragma once

#include "browser/api_contract_dsl.h"

#ifndef MMLTK_BROWSER_SETTINGS_CONTRACT_METADATA_ONLY
#include "gui/view_state.h"
#endif

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>

namespace mmltk::browser {

struct BrowserSettingsPatchValueTypeSpec {
    std::string_view path;
    std::string_view value_type;
};

struct BrowserNumericEnumSchemaSpec {
    std::string_view schema_name;
    std::string_view value_type;
    std::array<int, 4U> values;
    std::size_t value_count;
};

inline constexpr std::array<BrowserNumericEnumSchemaSpec, 4U> kBrowserSettingsNumericEnumSchemas{{
    {"TrainInputMode", "train_input_mode", {0, 1, 0, 0}, 2U},
    {"ModelInputMode", "model_input", {0, 1, 2, 3}, 4U},
    {"ModelSelectionSource", "model_source", {0, 1, 0, 0}, 2U},
    {"CompileMode", "compile_mode", {0, 1, 2, 0}, 3U},
}};

inline constexpr std::array<BrowserSettingsPatchValueTypeSpec, 191U> kBrowserSettingsPatchValueTypes{{
    {"current_view", "workflow"},
    {"ui.dark_mode", "boolean"},
    {"ui.ui_scale", "number"},
    {"ui.accent_color", "string"},
    {"ui.font_size", "number"},
    {"ui.secondary_font_size", "number"},
    {"ui.mono_font_size", "number"},
    {"ui.text_input_font_size", "number"},
    {"ui.crop_edge_hit_half_width", "number"},
    {"ui.crop_corner_hit_size", "number"},
    {"ui.crop_handle_radius", "number"},
    {"ui.density", "number"},
    {"ui.workspace_aspect_ratio", "number"},
    {"ui.annotation_brush_radius", "number"},
    {"ui.mask_cleanup_radius", "number"},
    {"workflows.train.dataset_paths.train_compiled_path", "string"},
    {"workflows.train.dataset_paths.val_compiled_path", "string"},
    {"workflows.train.dataset_paths.test_compiled_path", "string"},
    {"workflows.train.dataset_paths.source_dir", "string"},
    {"workflows.train.dataset_paths.compiled_directory", "string"},
    {"workflows.train.dataset_paths.use_compiled_directory_defaults", "boolean"},
    {"workflows.train.dataset_paths.overwrite", "boolean"},
    {"workflows.train.dataset_paths.compile_dimensions", "boolean"},
    {"workflows.train.model_artifacts.weights_path", "string"},
    {"workflows.train.model_artifacts.preset_name", "preset"},
    {"workflows.train.model_artifacts.resolution", "number"},
    {"workflows.train.model_artifacts.source", "model_source"},
    {"workflows.train.model_artifacts.input", "model_input"},
    {"workflows.train.execution.cpu_affinity", "string"},
    {"workflows.train.execution.workers", "number"},
    {"workflows.train.execution.lanes", "number"},
    {"workflows.train.execution.progress_bar", "boolean"},
    {"workflows.train.execution.compile_mode", "compile_mode"},
    {"workflows.train.training.output_dir", "string"},
    {"workflows.train.training.distributed_store_path", "string"},
    {"workflows.train.training.resume_path", "string"},
    {"workflows.train.training.input_mode", "train_input_mode"},
    {"workflows.train.training.execution_target", "number"},
    {"workflows.train.training.batch_size", "number"},
    {"workflows.train.training.val_batch_size", "number"},
    {"workflows.train.training.epochs", "number"},
    {"workflows.train.training.grad_accum_steps", "number"},
    {"workflows.train.training.eval_max_dets", "number"},
    {"workflows.train.training.print_freq", "number"},
    {"workflows.train.training.prefetch_factor", "number"},
    {"workflows.train.training.optimizer", "number"},
    {"workflows.train.training.momentum", "number"},
    {"workflows.train.training.lr", "number"},
    {"workflows.train.training.lr_encoder", "number"},
    {"workflows.train.training.weight_decay", "number"},
    {"workflows.train.training.lr_component_decay", "number"},
    {"workflows.train.training.encoder_layer_decay", "number"},
    {"workflows.train.training.warmup_epochs", "number"},
    {"workflows.train.training.warmup_momentum", "number"},
    {"workflows.train.training.lr_min_factor", "number"},
    {"workflows.train.training.clip_max_norm", "number"},
    {"workflows.train.training.lr_drop", "number"},
    {"workflows.train.training.ema_tau", "number"},
    {"workflows.train.training.seed", "number"},
    {"workflows.train.training.ema_decay", "number"},
    {"workflows.train.training.lr_scheduler", "string"},
    {"workflows.train.training.amp", "boolean"},
    {"workflows.train.training.use_ema", "boolean"},
    {"workflows.train.training.validation_loss", "boolean"},
    {"workflows.train.training.validation_profile", "boolean"},
    {"workflows.train.training.freeze_encoder", "boolean"},
    {"workflows.train.training.fused_optimizer", "boolean"},
    {"workflows.train.training.distributed_rank", "number"},
    {"workflows.train.training.distributed_world_size", "number"},
    {"workflows.train.training.distributed_worker", "boolean"},
    {"workflows.train.training.local_device_ids", "number_array"},
    {"workflows.train.training.remote_family_enabled", "boolean_array"},
    {"workflows.train.training.remote_container_image", "string"},
    {"workflows.train.training.remote_launch_template", "string"},
    {"workflows.train.augmentation.enabled", "boolean"},
    {"workflows.train.augmentation.geometry.probability", "number"},
    {"workflows.train.augmentation.geometry.min_strength", "number"},
    {"workflows.train.augmentation.geometry.max_strength", "number"},
    {"workflows.train.augmentation.resize.probability", "number"},
    {"workflows.train.augmentation.resize.min_strength", "number"},
    {"workflows.train.augmentation.resize.max_strength", "number"},
    {"workflows.train.augmentation.color.probability", "number"},
    {"workflows.train.augmentation.color.min_strength", "number"},
    {"workflows.train.augmentation.color.max_strength", "number"},
    {"workflows.train.augmentation.noise.probability", "number"},
    {"workflows.train.augmentation.noise.min_strength", "number"},
    {"workflows.train.augmentation.noise.max_strength", "number"},
    {"workflows.train.augmentation.blur.probability", "number"},
    {"workflows.train.augmentation.blur.min_strength", "number"},
    {"workflows.train.augmentation.blur.max_strength", "number"},
    {"workflows.train.augmentation.occlusion.probability", "number"},
    {"workflows.train.augmentation.occlusion.min_strength", "number"},
    {"workflows.train.augmentation.occlusion.max_strength", "number"},
    {"workflows.train.augmentation.copy_paste_probability", "number"},
    {"workflows.validate.dataset_paths.compiled_path", "string"},
    {"workflows.validate.dataset_paths.source_dir", "string"},
    {"workflows.validate.model_artifacts.weights_path", "string"},
    {"workflows.validate.model_artifacts.onnx_path", "string"},
    {"workflows.validate.model_artifacts.tensorrt_path", "string"},
    {"workflows.validate.model_artifacts.preset_name", "preset"},
    {"workflows.validate.model_artifacts.resolution", "number"},
    {"workflows.validate.model_artifacts.source", "model_source"},
    {"workflows.validate.model_artifacts.input", "model_input"},
    {"workflows.validate.execution.cpu_affinity", "string"},
    {"workflows.validate.execution.device_id", "number"},
    {"workflows.validate.execution.workers", "number"},
    {"workflows.validate.execution.allow_fp16", "boolean"},
    {"workflows.validate.validation.save_engine_path", "string"},
    {"workflows.validate.validation.report_json_path", "string"},
    {"workflows.validate.validation.split", "string"},
    {"workflows.validate.validation.eval_order", "string"},
    {"workflows.validate.validation.resolution", "number"},
    {"workflows.validate.validation.limit_images", "number"},
    {"workflows.validate.validation.alignment_images", "number"},
    {"workflows.validate.validation.eval_max_dets", "number"},
    {"workflows.validate.validation.batch_size", "number"},
    {"workflows.validate.validation.prefetch_factor", "number"},
    {"workflows.validate.validation.recompile", "boolean"},
    {"workflows.validate.validation.profile", "boolean"},
    {"workflows.validate.validation.write_report_json", "boolean"},
    {"workflows.validate.validation.compile_workers", "number"},
    {"workflows.validate.validation.compile_cuda_mask_batch_size", "number"},
    {"workflows.validate.validation.compile_cuda_device_id", "number"},
    {"workflows.validate.validation.log_mode", "number"},
    {"workflows.predict.source.kind", "number"},
    {"workflows.predict.source.recursive", "boolean"},
    {"workflows.predict.source.device_index", "number"},
    {"workflows.predict.source.capture_width", "number"},
    {"workflows.predict.source.capture_height", "number"},
    {"workflows.predict.source.capture_fps", "number"},
    {"workflows.predict.source.v4l2_buffer_count", "number"},
    {"workflows.predict.source.compiled_path", "string"},
    {"workflows.predict.source.single_image_path", "string"},
    {"workflows.predict.source.image_directory", "string"},
    {"workflows.predict.model_artifacts.weights_path", "string"},
    {"workflows.predict.model_artifacts.onnx_path", "string"},
    {"workflows.predict.model_artifacts.tensorrt_path", "string"},
    {"workflows.predict.model_artifacts.preset_name", "preset"},
    {"workflows.predict.model_artifacts.resolution", "number"},
    {"workflows.predict.model_artifacts.source", "model_source"},
    {"workflows.predict.model_artifacts.input", "model_input"},
    {"workflows.predict.execution.cpu_affinity", "string"},
    {"workflows.predict.execution.device_id", "number"},
    {"workflows.predict.execution.workers", "number"},
    {"workflows.predict.execution.lanes", "number"},
    {"workflows.predict.execution.allow_fp16", "boolean"},
    {"workflows.predict.execution.progress_bar", "boolean"},
    {"workflows.predict.execution.compile_mode", "compile_mode"},
    {"workflows.predict.predict.output_path", "string"},
    {"workflows.predict.predict.backend", "string"},
    {"workflows.predict.predict.batch_size", "number"},
    {"workflows.predict.predict.max_dets_per_image", "number"},
    {"workflows.predict.predict.live_split_count", "number"},
    {"workflows.predict.predict.threshold", "number"},
    {"workflows.annotate.source.kind", "number"},
    {"workflows.annotate.source.recursive", "boolean"},
    {"workflows.annotate.source.device_index", "number"},
    {"workflows.annotate.source.capture_width", "number"},
    {"workflows.annotate.source.capture_height", "number"},
    {"workflows.annotate.source.capture_fps", "number"},
    {"workflows.annotate.source.v4l2_buffer_count", "number"},
    {"workflows.annotate.source.single_image_path", "string"},
    {"workflows.annotate.source.image_directory", "string"},
    {"workflows.annotate.model_artifacts.weights_path", "string"},
    {"workflows.annotate.model_artifacts.onnx_path", "string"},
    {"workflows.annotate.model_artifacts.tensorrt_path", "string"},
    {"workflows.annotate.model_artifacts.preset_name", "preset"},
    {"workflows.annotate.model_artifacts.resolution", "number"},
    {"workflows.annotate.model_artifacts.source", "model_source"},
    {"workflows.annotate.model_artifacts.input", "model_input"},
    {"workflows.annotate.execution.device_id", "number"},
    {"workflows.annotate.execution.allow_fp16", "boolean"},
    {"workflows.annotate.execution.compile_mode", "compile_mode"},
    {"workflows.annotate.annotate.output_dir", "string"},
    {"workflows.annotate.annotate.split", "string"},
    {"workflows.annotate.annotate.backend", "string"},
    {"workflows.annotate.annotate.max_dets_per_image", "number"},
    {"workflows.annotate.annotate.threshold", "number"},
    {"workflows.annotate.annotate.full_frame", "boolean"},
    {"workflows.export.model_artifacts.weights_path", "string"},
    {"workflows.export.model_artifacts.onnx_path", "string"},
    {"workflows.export.model_artifacts.preset_name", "preset"},
    {"workflows.export.model_artifacts.resolution", "number"},
    {"workflows.export.model_artifacts.source", "model_source"},
    {"workflows.export.model_artifacts.input", "model_input"},
    {"workflows.export.execution.device_id", "number"},
    {"workflows.export.execution.allow_fp16", "boolean"},
    {"workflows.export.export.output_path", "string"},
    {"workflows.export.export.opset_version", "number"},
    {"workflows.export.export.build_tensorrt", "boolean"},
    {"workflows.export.export.simplify", "boolean"},
}};

[[nodiscard]] consteval bool browser_settings_value_type_is_known(const std::string_view value_type) {
    if (value_type == "workflow" || value_type == "preset" || value_type == "string" || value_type == "number" ||
        value_type == "boolean" || value_type == "number_array" || value_type == "boolean_array") {
        return true;
    }
    for (const BrowserNumericEnumSchemaSpec& schema : kBrowserSettingsNumericEnumSchemas) {
        if (schema.value_type == value_type) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] consteval bool browser_settings_contract_is_valid() {
    for (std::size_t i = 0U; i < kBrowserSettingsNumericEnumSchemas.size(); ++i) {
        if (kBrowserSettingsNumericEnumSchemas[i].schema_name.empty() ||
            kBrowserSettingsNumericEnumSchemas[i].value_type.empty() ||
            kBrowserSettingsNumericEnumSchemas[i].value_count == 0U ||
            kBrowserSettingsNumericEnumSchemas[i].value_count > kBrowserSettingsNumericEnumSchemas[i].values.size()) {
            return false;
        }
        for (std::size_t j = i + 1U; j < kBrowserSettingsNumericEnumSchemas.size(); ++j) {
            if (kBrowserSettingsNumericEnumSchemas[i].schema_name ==
                    kBrowserSettingsNumericEnumSchemas[j].schema_name ||
                kBrowserSettingsNumericEnumSchemas[i].value_type == kBrowserSettingsNumericEnumSchemas[j].value_type) {
                return false;
            }
        }
    }
    for (std::size_t i = 0U; i < kBrowserSettingsPatchValueTypes.size(); ++i) {
        if (kBrowserSettingsPatchValueTypes[i].path.empty() ||
            !browser_settings_value_type_is_known(kBrowserSettingsPatchValueTypes[i].value_type)) {
            return false;
        }
        for (std::size_t j = i + 1U; j < kBrowserSettingsPatchValueTypes.size(); ++j) {
            if (kBrowserSettingsPatchValueTypes[i].path == kBrowserSettingsPatchValueTypes[j].path) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] consteval auto browser_settings_patch_path_array() {
    std::array<std::string_view, kBrowserSettingsPatchValueTypes.size()> paths{};
    for (std::size_t i = 0U; i < kBrowserSettingsPatchValueTypes.size(); ++i) {
        paths[i] = kBrowserSettingsPatchValueTypes[i].path;
    }
    return paths;
}

static_assert(browser_settings_contract_is_valid(), "native browser settings patch contract is invalid");

#ifndef MMLTK_BROWSER_SETTINGS_CONTRACT_METADATA_ONLY

template <typename T>
struct BrowserSettingsContract;

template <typename State>
[[nodiscard]] constexpr auto train_execution_fields() {
    return std::tuple{
        api::field<&State::execution_target>("execution_target"),
        api::field<&State::local_device_ids>("local_device_ids"),
        api::field<&State::remote_family_enabled>("remote_family_enabled"),
        api::field<&State::remote_container_image>("remote_container_image"),
        api::field<&State::remote_launch_template>("remote_launch_template"),
    };
}

template <>
struct BrowserSettingsContract<mmltk::gui::UiSettingsState> {
    [[nodiscard]] static constexpr auto fields() {
        using State = mmltk::gui::UiSettingsState;
        return std::tuple{
            api::field<&State::dark_mode>("dark_mode"),
            api::field<&State::ui_scale>("ui_scale"),
            api::field<&State::accent_color>("accent_color"),
            api::field<&State::font_size>("font_size"),
            api::field<&State::secondary_font_size>("secondary_font_size"),
            api::field<&State::mono_font_size>("mono_font_size"),
            api::field<&State::text_input_font_size>("text_input_font_size"),
            api::field<&State::crop_edge_hit_half_width>("crop_edge_hit_half_width"),
            api::field<&State::crop_corner_hit_size>("crop_corner_hit_size"),
            api::field<&State::crop_handle_radius>("crop_handle_radius"),
            api::field<&State::density>("density"),
            api::field<&State::workspace_aspect_ratio>("workspace_aspect_ratio"),
            api::field<&State::annotation_brush_radius>("annotation_brush_radius"),
            api::field<&State::mask_cleanup_radius>("mask_cleanup_radius"),
        };
    }
};

template <>
struct BrowserSettingsContract<mmltk::gui::DatasetPathState> {
    [[nodiscard]] static constexpr auto fields() {
        using State = mmltk::gui::DatasetPathState;
        return std::tuple{
            api::field<&State::compiled_path>("compiled_path"),
            api::field<&State::source_dir>("source_dir"),
            api::field<&State::compiled_directory>("compiled_directory"),
            api::field<&State::use_compiled_directory_defaults>("use_compiled_directory_defaults"),
            api::field<&State::train_compiled_path>("train_compiled_path"),
            api::field<&State::val_compiled_path>("val_compiled_path"),
            api::field<&State::test_compiled_path>("test_compiled_path"),
            api::field<&State::overwrite>("overwrite"),
            api::field<&State::compile_dimensions>("compile_dimensions"),
        };
    }
};

template <>
struct BrowserSettingsContract<mmltk::gui::ModelArtifactSelectionState> {
    [[nodiscard]] static constexpr auto fields() {
        using State = mmltk::gui::ModelArtifactSelectionState;
        return std::tuple{
            api::field<&State::weights_path>("weights_path"),
            api::field<&State::onnx_path>("onnx_path"),
            api::field<&State::tensorrt_path>("tensorrt_path"),
            api::field<&State::preset_name>("preset_name"),
            api::field<&State::resolution>("resolution"),
            api::field<&State::source>("source"),
            api::field<&State::input>("input"),
        };
    }
};

template <>
struct BrowserSettingsContract<mmltk::gui::ExecutionTuningState> {
    [[nodiscard]] static constexpr auto fields() {
        using State = mmltk::gui::ExecutionTuningState;
        return std::tuple{
            api::field<&State::cpu_affinity>("cpu_affinity"), api::field<&State::device_id>("device_id"),
            api::field<&State::workers>("workers"),           api::field<&State::lanes>("lanes"),
            api::field<&State::allow_fp16>("allow_fp16"),     api::field<&State::progress_bar>("progress_bar"),
            api::field<&State::compile_mode>("compile_mode"),
        };
    }
};

template <>
struct BrowserSettingsContract<mmltk::gui::TrainExecutionPaneState> {
    [[nodiscard]] static constexpr auto fields() {
        using State = mmltk::gui::TrainExecutionPaneState;
        return train_execution_fields<State>();
    }
};

template <>
struct BrowserSettingsContract<mmltk::gui::TrainPaneState> {
    [[nodiscard]] static constexpr auto fields() {
        using State = mmltk::gui::TrainPaneState;
        return std::tuple_cat(train_execution_fields<State>(), std::tuple{
                                                                   api::field<&State::output_dir>("output_dir"),
                                                                   api::field<&State::resume_path>("resume_path"),
                                                                   api::field<&State::input_mode>("input_mode"),
                                                               });
    }
};

template <>
struct BrowserSettingsContract<mmltk::gui::TrainViewState> {
    [[nodiscard]] static constexpr auto fields() {
        using State = mmltk::gui::TrainViewState;
        return std::tuple_cat(
            std::tuple{
                api::field<&State::train_compiled_path>("train_compiled_path"),
                api::field<&State::val_compiled_path>("val_compiled_path"),
                api::field<&State::test_compiled_path>("test_compiled_path"),
                api::field<&State::dataset_source_dir>("source_dir"),
                api::field<&State::compiled_dataset_dir>("compiled_directory"),
                api::field<&State::use_compiled_directory_defaults>("use_compiled_directory_defaults"),
                api::field<&State::overwrite_compiled_dataset>("overwrite"),
                api::field<&State::compile_dimensions>("compile_dimensions"),
                api::field<&State::weights_path>("weights_path"),
                api::field<&State::preset_name>("preset_name"),
                api::field<&State::model_resolution>("model_resolution"),
                api::field<&State::model_source>("model_source"),
                api::field<&State::model_input>("model_input"),
                api::field<&State::cpu_affinity>("cpu_affinity"),
                api::field<&State::workers>("workers"),
                api::field<&State::lanes>("lanes"),
                api::field<&State::progress_bar>("progress_bar"),
                api::field<&State::compile_mode>("compile_mode"),
            },
            train_execution_fields<State>(),
            std::tuple{
                api::field<&State::output_dir>("output_dir"),
                api::field<&State::resume_path>("resume_path"),
                api::field<&State::input_mode>("input_mode"),
                api::field<&State::batch_size>("batch_size"),
                api::field<&State::epochs>("epochs"),
                api::field<&State::optimizer>("optimizer"),
                api::field<&State::amp>("amp"),
                api::field<&State::use_ema>("use_ema"),
                api::field<&State::validation_loss>("validation_loss"),
                api::field<&State::validation_profile>("validation_profile"),
                api::field<&State::freeze_encoder>("freeze_encoder"),
            });
    }
};

template <>
struct BrowserSettingsContract<mmltk::gui::ValidateViewState> {
    [[nodiscard]] static constexpr auto fields() {
        using State = mmltk::gui::ValidateViewState;
        return std::tuple{
            api::field<&State::compiled_path>("compiled_path"),
            api::field<&State::source_dir>("source_dir"),
            api::field<&State::weights_path>("weights_path"),
            api::field<&State::onnx_path>("onnx_path"),
            api::field<&State::tensorrt_path>("tensorrt_path"),
            api::field<&State::preset_name>("preset_name"),
            api::field<&State::model_resolution>("model_resolution"),
            api::field<&State::model_source>("model_source"),
            api::field<&State::model_input>("model_input"),
            api::field<&State::save_engine_path>("save_engine_path"),
            api::field<&State::report_json_path>("report_json_path"),
            api::field<&State::split>("split"),
            api::field<&State::resolution>("resolution"),
            api::field<&State::batch_size>("batch_size"),
            api::field<&State::device_id>("device_id"),
            api::field<&State::workers>("workers"),
            api::field<&State::allow_fp16>("allow_fp16"),
        };
    }
};

template <>
struct BrowserSettingsContract<mmltk::gui::PredictViewState> {
    [[nodiscard]] static constexpr auto fields() {
        using State = mmltk::gui::PredictViewState;
        return std::tuple{
            api::field<&State::source>("source"),
            api::field<&State::weights_path>("weights_path"),
            api::field<&State::onnx_path>("onnx_path"),
            api::field<&State::tensorrt_path>("tensorrt_path"),
            api::field<&State::preset_name>("preset_name"),
            api::field<&State::model_resolution>("model_resolution"),
            api::field<&State::model_source>("model_source"),
            api::field<&State::output_path>("output_path"),
            api::field<&State::backend>("backend"),
            api::field<&State::cpu_affinity>("cpu_affinity"),
            api::field<&State::model_input>("model_input"),
            api::field<&State::batch_size>("batch_size"),
            api::field<&State::max_dets_per_image>("max_dets_per_image"),
            api::field<&State::live_split_count>("live_split_count"),
            api::field<&State::device_id>("device_id"),
            api::field<&State::workers>("workers"),
            api::field<&State::lanes>("lanes"),
            api::field<&State::threshold>("threshold"),
            api::field<&State::allow_fp16>("allow_fp16"),
            api::field<&State::progress_bar>("progress_bar"),
            api::field<&State::compile_mode>("compile_mode"),
        };
    }
};

template <>
struct BrowserSettingsContract<mmltk::gui::AnnotateViewState> {
    [[nodiscard]] static constexpr auto fields() {
        using State = mmltk::gui::AnnotateViewState;
        return std::tuple{
            api::field<&State::source>("source"),
            api::field<&State::weights_path>("weights_path"),
            api::field<&State::onnx_path>("onnx_path"),
            api::field<&State::tensorrt_path>("tensorrt_path"),
            api::field<&State::preset_name>("preset_name"),
            api::field<&State::model_resolution>("model_resolution"),
            api::field<&State::model_source>("model_source"),
            api::field<&State::output_dir>("output_dir"),
            api::field<&State::split>("split"),
            api::field<&State::backend>("backend"),
            api::field<&State::model_input>("model_input"),
            api::field<&State::device_id>("device_id"),
            api::field<&State::max_dets_per_image>("max_dets_per_image"),
            api::field<&State::threshold>("threshold"),
            api::field<&State::allow_fp16>("allow_fp16"),
            api::field<&State::full_frame>("full_frame"),
            api::field<&State::compile_mode>("compile_mode"),
        };
    }
};

template <>
struct BrowserSettingsContract<mmltk::gui::ExportViewState> {
    [[nodiscard]] static constexpr auto fields() {
        using State = mmltk::gui::ExportViewState;
        return std::tuple{
            api::field<&State::weights_path>("weights_path"),
            api::field<&State::onnx_path>("onnx_path"),
            api::field<&State::preset_name>("preset_name"),
            api::field<&State::model_resolution>("model_resolution"),
            api::field<&State::model_source>("model_source"),
            api::field<&State::model_input>("model_input"),
            api::field<&State::output_path>("output_path"),
            api::field<&State::device_id>("device_id"),
            api::field<&State::opset_version>("opset_version"),
            api::field<&State::allow_fp16>("allow_fp16"),
            api::field<&State::build_tensorrt>("build_tensorrt"),
            api::field<&State::simplify>("simplify"),
        };
    }
};

template <>
struct BrowserSettingsContract<mmltk::gui::SourceSelectionState> {
    [[nodiscard]] static constexpr auto fields() {
        using State = mmltk::gui::SourceSelectionState;
        return std::tuple{
            api::field<&State::kind>("kind"),
            api::field<&State::compiled_path>("compiled_path"),
            api::field<&State::single_image_path>("single_image_path"),
            api::field<&State::image_directory>("image_directory"),
            api::field<&State::recursive>("recursive"),
            api::field<&State::device_index>("device_index"),
            api::field<&State::capture_width>("capture_width"),
            api::field<&State::capture_height>("capture_height"),
            api::field<&State::capture_fps>("capture_fps"),
            api::field<&State::v4l2_buffer_count>("v4l2_buffer_count"),
            api::field<&State::crop_x>("crop_x"),
            api::field<&State::crop_y>("crop_y"),
            api::field<&State::crop_width>("crop_width"),
            api::field<&State::crop_height>("crop_height"),
        };
    }
};

#endif

[[nodiscard]] inline constexpr auto browser_settings_patch_paths() noexcept {
    return browser_settings_patch_path_array();
}

[[nodiscard]] inline constexpr auto browser_settings_patch_value_types() noexcept {
    return kBrowserSettingsPatchValueTypes;
}

}  
