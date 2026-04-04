#include "gui/model_input_ui.h"

#include "gui/file_picker.h"
#include "gui/ui_controls.h"

#include <imgui.h>

namespace mmltk::gui {

namespace {

constexpr ImVec4 kWarningBannerBackground{0.33f, 0.17f, 0.12f, 0.95f};
constexpr ImVec4 kWarningBannerText{0.98f, 0.90f, 0.82f, 1.00f};

void draw_warning_banner(const char* id, const std::string& message) {
    if (message.empty()) {
        return;
    }
    ImGui::Spacing();
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kWarningBannerBackground);
    ImGui::BeginChild(
        "banner", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 2.4f),
        true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_Text, kWarningBannerText);
    ImGui::TextWrapped("%s", message.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

} // namespace

const char* compilation_mode_label(const int index) noexcept {
    switch (index) {
    case 0:
        return "None";
    case 1:
        return "Selective";
    case 2:
        return "Full";
    default:
        return "Unknown";
    }
}

const char* model_input_label(const ModelInputMode mode) noexcept {
    switch (mode) {
    case ModelInputMode::Weights:
        return "Weights";
    case ModelInputMode::Onnx:
        return "ONNX";
    case ModelInputMode::TensorRt:
        return "TensorRT";
    case ModelInputMode::None:
        return "None";
    }
    return "Unknown";
}

bool draw_compile_mode_combo(const char* label, int& index) {
    bool changed = false;
    draw_labeled_combo(label, compilation_mode_label(index), 180.0f, [&index, &changed]() {
        for (int option = 0; option < 3; ++option) {
            const bool selected = option == index;
            if (ImGui::Selectable(compilation_mode_label(option), selected)) {
                index = option;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
    });
    return changed;
}

void draw_dataset_paths_section(DatasetPathState& state,
                                const DatasetPathSectionConfig& config) {
    if (config.heading != nullptr) {
        draw_section_heading(config.heading);
    }
    if (config.compiled_label != nullptr) {
        draw_full_width_input(config.compiled_label, state.compiled_path);
    }
    if (config.source_dir_label != nullptr) {
        draw_full_width_input(config.source_dir_label, state.source_dir);
    }
    if (config.train_compiled_label != nullptr) {
        draw_full_width_input(config.train_compiled_label, state.train_compiled_path);
    }
    if (config.val_compiled_label != nullptr) {
        draw_full_width_input(config.val_compiled_label, state.val_compiled_path);
    }
    if (config.test_compiled_label != nullptr) {
        draw_full_width_input(config.test_compiled_label, state.test_compiled_path);
    }
}

void draw_validate_workflow_sections(DatasetPathState& dataset_state,
                                     ModelArtifactSelectionState& artifact_state,
                                     ExecutionTuningState& execution_state,
                                     std::string& report_json_path,
                                     std::string& save_engine_path,
                                     std::string& eval_order,
                                     std::string& split,
                                     int& resolution,
                                     int& limit_images,
                                     int& alignment_images,
                                     int& eval_max_dets,
                                     int& batch_size,
                                     int& prefetch_factor,
                                     bool& recompile,
                                     bool& profile,
                                     bool& write_report_json) {
    draw_dataset_paths_section(dataset_state,
                               {.heading = "Datasets",
                                .compiled_label = "Compiled Dataset",
                                .source_dir_label = "Source Root"});
    draw_full_width_input("Report JSON", report_json_path);

    draw_section_heading("Backends");
    draw_full_width_input("ONNX Path", artifact_state.onnx_path);
    draw_full_width_input("TensorRT Path", artifact_state.tensorrt_path);
    draw_full_width_input("Save Engine Path", save_engine_path);
    draw_full_width_input("Eval Order", eval_order);

    draw_section_heading("Execution");
    draw_full_width_input("Split", split);
    draw_execution_tuning_section(
        execution_state,
        {.heading = nullptr,
         .input_width = 120.0f,
         .show_cpu_affinity = true,
         .show_device_id = true,
         .show_workers = true,
         .show_lanes = false,
         .show_allow_fp16 = true,
         .show_progress_bar = false,
         .show_compile_mode = false});
    draw_labeled_int_input("Resolution", resolution, 120.0f);
    draw_labeled_int_input("Batch Size", batch_size, 120.0f);
    draw_labeled_int_input("Limit Images", limit_images, 120.0f);
    draw_labeled_int_input("Alignment Images", alignment_images, 120.0f);
    draw_labeled_int_input("Eval Max Dets", eval_max_dets, 120.0f);
    draw_labeled_int_input("Prefetch", prefetch_factor, 120.0f);
    draw_labeled_checkbox("Recompile From Source", recompile);
    draw_labeled_checkbox("Profile", profile);
    draw_labeled_checkbox("Write Report", write_report_json);
}

ModelInputBrowseRequest draw_model_input_section(ModelInputMode& mode,
                                                 ModelArtifactSelectionState& state,
                                                 const bool weights_browse_busy,
                                                 const bool onnx_browse_busy,
                                                 const bool tensorrt_browse_busy,
                                                 const ModelInputSectionConfig& config) {
    if (config.heading != nullptr) {
        draw_section_heading(config.heading);
    }
    return draw_model_input_selector(mode,
                                     state.weights_path,
                                     state.onnx_path,
                                     state.tensorrt_path,
                                     weights_browse_busy,
                                     onnx_browse_busy,
                                     tensorrt_browse_busy,
                                     config.allow_none);
}

ModelInputBrowseRequest draw_model_input_selector(ModelInputMode& mode,
                                                  std::string& weights_path,
                                                  std::string& onnx_path,
                                                  std::string& tensorrt_path,
                                                  const bool weights_browse_busy,
                                                  const bool onnx_browse_busy,
                                                  const bool tensorrt_browse_busy,
                                                  const bool allow_none) {
    if (!allow_none && mode == ModelInputMode::None) {
        mode = ModelInputMode::Weights;
    }

    draw_labeled_combo("Model Input", model_input_label(mode), 180.0f, [&mode, allow_none]() {
        const auto draw_option = [&mode](const ModelInputMode option) {
            const bool selected = option == mode;
            if (ImGui::Selectable(model_input_label(option), selected)) {
                mode = option;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        };
        if (allow_none) {
            draw_option(ModelInputMode::None);
        }
        draw_option(ModelInputMode::Weights);
        draw_option(ModelInputMode::Onnx);
        draw_option(ModelInputMode::TensorRt);
    });

    switch (mode) {
    case ModelInputMode::None:
        ImGui::TextUnformatted("No model backing. Manual box seeds only.");
        return ModelInputBrowseRequest::None;
    case ModelInputMode::Weights:
        return draw_file_picker_input("Weights Path", weights_path, weights_browse_busy)
                   ? ModelInputBrowseRequest::Weights
                   : ModelInputBrowseRequest::None;
    case ModelInputMode::Onnx:
        return draw_file_picker_input("ONNX Path", onnx_path, onnx_browse_busy)
                   ? ModelInputBrowseRequest::Onnx
                   : ModelInputBrowseRequest::None;
    case ModelInputMode::TensorRt:
        return draw_file_picker_input("TensorRT Path", tensorrt_path, tensorrt_browse_busy)
                   ? ModelInputBrowseRequest::TensorRt
                   : ModelInputBrowseRequest::None;
    }
    return ModelInputBrowseRequest::None;
}

void draw_execution_tuning_section(ExecutionTuningState& state,
                                   const ExecutionTuningSectionConfig& config) {
    if (config.heading != nullptr) {
        draw_section_heading(config.heading);
    }
    if (config.show_device_id) {
        draw_labeled_int_input(config.device_id_label, state.device_id, config.input_width);
    }
    if (config.show_cpu_affinity) {
        draw_full_width_input("CPU Affinity", state.cpu_affinity);
    }
    if (config.show_workers) {
        draw_labeled_int_input(config.workers_label, state.workers, config.input_width);
    }
    if (config.show_lanes) {
        draw_labeled_int_input(config.lanes_label, state.lanes, config.input_width);
    }
    if (config.show_allow_fp16) {
        draw_labeled_checkbox(config.allow_fp16_label, state.allow_fp16);
    }
    if (config.show_progress_bar) {
        draw_labeled_checkbox(config.progress_bar_label, state.progress_bar);
    }
    if (config.show_compile_mode) {
        (void)draw_compile_mode_combo(config.compile_mode_label, state.compile_mode);
    }
}

ModelInputBrowseRequest draw_predict_workflow_sections(
    ModelInputMode& model_input,
    ModelArtifactSelectionState& artifact_state,
    ExecutionTuningState& execution_state,
    std::string& backend,
    std::string& output_path,
    int& batch_size,
    int& live_split_count,
    int& max_dets_per_image,
    float& threshold,
    const bool live_video,
    const bool controls_disabled,
    const bool single_image,
    const bool weights_browse_busy,
    const bool onnx_browse_busy,
    const bool tensorrt_browse_busy) {
    ImGui::BeginDisabled(controls_disabled);
    const ModelInputBrowseRequest browse_request = draw_model_input_section(
        model_input, artifact_state, weights_browse_busy, onnx_browse_busy,
        tensorrt_browse_busy, {.heading = "Model"});
    draw_full_width_input("Backend Preference", backend);
    if (!live_video) {
        draw_full_width_input("Output JSON", output_path);
    }
    ImGui::EndDisabled();

    if (live_video) {
        ImGui::BeginDisabled(controls_disabled);
        draw_execution_tuning_section(
            execution_state,
            {.heading = "Execution",
             .input_width = 120.0f,
             .show_cpu_affinity = false,
             .show_device_id = true,
             .show_workers = false,
             .show_lanes = false,
             .show_allow_fp16 = true,
             .show_progress_bar = false,
             .show_compile_mode = true});
        draw_labeled_int_input("Live Splits", live_split_count, 120.0f);
        draw_labeled_int_input("Max Dets", max_dets_per_image, 120.0f);
        draw_labeled_float_input("Threshold", threshold, 120.0f, 0.01f, 0.10f,
                                 "%.3f");
        ImGui::EndDisabled();
        ImGui::TextWrapped(
            "Live video predict keeps the selected model hot and runs against the "
            "newest available device frame. "
            "The active model resolution comes from the selected RF-DETR preset.");
        return browse_request;
    }

    draw_execution_tuning_section(
        execution_state,
        {.heading = "Execution",
         .input_width = 120.0f,
         .show_cpu_affinity = true,
         .show_device_id = true,
         .show_workers = true,
         .show_lanes = true,
         .show_allow_fp16 = true,
         .show_progress_bar = true,
         .show_compile_mode = true});
    if (single_image) {
        ImGui::BeginDisabled();
        draw_labeled_int_input("Batch Size", batch_size, 120.0f);
        ImGui::EndDisabled();
        ImGui::TextWrapped("Single-image predict always uses batch size 1.");
    } else {
        draw_labeled_int_input("Batch Size", batch_size, 120.0f);
    }
    draw_labeled_int_input("Max Dets", max_dets_per_image, 120.0f);
    draw_labeled_float_input("Threshold", threshold, 120.0f, 0.01f, 0.10f,
                             "%.3f");
    return browse_request;
}

void draw_predict_status_sections(const bool live_video,
                                  const std::string& source_error,
                                  const std::string& live_blocker,
                                  const std::string& combo_error,
                                  const std::string& live_start_error,
                                  const bool job_running) {
    if (!source_error.empty()) {
        if (live_video) {
            draw_warning_banner("predict_live_source_error_banner",
                                source_error);
        } else {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", source_error.c_str());
        }
    }
    if (!live_blocker.empty() && (source_error.empty() || job_running)) {
        draw_warning_banner("predict_live_blocker_banner", live_blocker);
    }
    draw_warning_banner("predict_combo_banner", combo_error);
    if (live_video) {
        draw_warning_banner("predict_live_start_error_banner",
                            live_start_error);
    }
}

PredictActionFooterResult draw_predict_action_footer(
    const bool live_video,
    const bool predict_session_active,
    const bool starting,
    const bool stopping,
    const bool block_live_start,
    const bool block_batch_prediction) {
    if (live_video) {
        if (!predict_session_active && !starting && !stopping) {
            ImGui::BeginDisabled(block_live_start);
            const bool clicked = ImGui::Button("Start Live Prediction");
            ImGui::EndDisabled();
            return clicked ? PredictActionFooterResult::StartLive
                           : PredictActionFooterResult::None;
        }

        if (starting) {
            const bool clicked = ImGui::Button("Cancel Live Start");
            ImGui::SameLine();
            ImGui::TextUnformatted("Starting live prediction...");
            return clicked ? PredictActionFooterResult::StopLive
                           : PredictActionFooterResult::None;
        }

        if (stopping) {
            ImGui::BeginDisabled();
            (void)ImGui::Button("Stopping Live Prediction");
            ImGui::EndDisabled();
            return PredictActionFooterResult::None;
        }

        return ImGui::Button("Stop Live Prediction")
                   ? PredictActionFooterResult::StopLive
                   : PredictActionFooterResult::None;
    }

    ImGui::BeginDisabled(block_batch_prediction);
    const bool clicked = ImGui::Button("Run Prediction");
    ImGui::EndDisabled();
    return clicked ? PredictActionFooterResult::RunBatchPrediction
                   : PredictActionFooterResult::None;
}

} // namespace mmltk::gui
