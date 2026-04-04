#include "activity_pane_presenter.h"

#include "layout_primitives.h"
#include "ui_controls.h"
#include "ui_style.h"

#include <imgui.h>

#include <algorithm>
#include <ios>
#include <sstream>
#include <string>

namespace mmltk::gui {

namespace {

std::string format_decimal(const double value, const int precision) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(precision);
    stream << value;
    return stream.str();
}

void draw_banner(const char* id,
                 const ImVec4& background_color,
                 const ImVec4& text_color,
                 std::string_view message) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background_color);
    ImGui::BeginChild(
        "banner",
        ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 2.4f),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    ImGui::TextWrapped("%.*s", static_cast<int>(message.size()), message.data());
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

constexpr ImVec4 kBannerBackgroundColor{0.33f, 0.17f, 0.12f, 0.95f};
constexpr ImVec4 kBannerTextColor{0.98f, 0.90f, 0.82f, 1.00f};
constexpr ImVec4 kErrorTextColor{0.94f, 0.45f, 0.41f, 1.00f};

void draw_training_activity_content(const TrainingActivityViewModel& view_model) {
    ImGui::Text("Target: %.*s",
                static_cast<int>(view_model.execution_target_label.size()),
                view_model.execution_target_label.data());

    if (!view_model.local_gpu_summary.empty()) {
        ImGui::TextWrapped("Local GPUs: %.*s",
                           static_cast<int>(view_model.local_gpu_summary.size()),
                           view_model.local_gpu_summary.data());
        if (!view_model.selected_device_ids_summary.empty()) {
            ImGui::TextWrapped("Device IDs: %.*s",
                               static_cast<int>(view_model.selected_device_ids_summary.size()),
                               view_model.selected_device_ids_summary.data());
        }
        if (!view_model.local_gpu_error.empty()) {
            ImGui::Spacing();
            draw_banner("activity_local_gpu_error_banner",
                        kBannerBackgroundColor,
                        kBannerTextColor,
                        view_model.local_gpu_error);
        }
        return;
    }

    ImGui::TextWrapped("Remote Families: %.*s",
                       static_cast<int>(view_model.remote_gpu_family_summary.size()),
                       view_model.remote_gpu_family_summary.data());
    ImGui::TextUnformatted(view_model.vast_api_key_configured
                               ? "Vast API Key: configured"
                               : "Vast API Key: missing");
    if (!view_model.armed_offer_summary.empty()) {
        ImGui::TextWrapped("Armed Offer: %.*s",
                           static_cast<int>(view_model.armed_offer_summary.size()),
                           view_model.armed_offer_summary.data());
    } else {
        ImGui::TextUnformatted("Armed Offer: none");
    }
    if (view_model.remote_query_running) {
        ImGui::TextUnformatted("Remote Query: running");
    } else if (!view_model.remote_query_summary.empty()) {
        ImGui::TextWrapped("Remote Query: %.*s",
                           static_cast<int>(view_model.remote_query_summary.size()),
                           view_model.remote_query_summary.data());
    }
    if (!view_model.remote_query_error.empty()) {
        ImGui::Spacing();
        draw_banner("activity_remote_query_error_banner",
                    kBannerBackgroundColor,
                    kBannerTextColor,
                    view_model.remote_query_error);
    }
}

void draw_local_train_activity_content(const LocalTrainSessionState& state,
                                       const std::function<void(bool)>& request_stop_local_training,
                                       ImFont* compact_font) {
    if (state.running) {
        ImGui::TextWrapped("Running: %s", state.label.c_str());
        if (ImGui::Button(state.stop_requested ? "Force Kill" : "Stop Training")) {
            request_stop_local_training(state.stop_requested);
        }
        if (state.stop_requested) {
            ImGui::TextUnformatted("Stopping...");
        }
    } else if (!state.label.empty()) {
        ImGui::TextWrapped("Idle after: %s", state.label.c_str());
    } else {
        ImGui::TextUnformatted("Idle");
    }

    if (state.progress.has_value()) {
        const TrainArtifactProgress& progress = *state.progress;
        ImGui::TextWrapped("Phase: %s", progress.phase.empty() ? "train" : progress.phase.c_str());
        ImGui::Text("Epoch: %d / %d",
                    progress.epoch >= 0 ? progress.epoch + 1 : 0,
                    std::max(0, progress.total_epochs));
        ImGui::Text("Batches: %d / %d", progress.completed_batches, progress.total_batches);
        ImGui::Text("Waves: %d", progress.completed_waves);
        ImGui::Text("Optimizer Steps: %d / %d", progress.optimizer_steps, progress.steps_per_epoch);
        ImGui::Text("Current Loss: %s", format_decimal(progress.step_loss, 4).c_str());
        ImGui::Text("Current cls/box: %s / %s",
                    format_decimal(progress.step_class_loss, 4).c_str(),
                    format_decimal(progress.step_box_loss, 4).c_str());
        ImGui::Text("Average Loss: %s", format_decimal(progress.train_loss, 4).c_str());
        ImGui::Text("Average cls/box: %s / %s",
                    format_decimal(progress.class_loss, 4).c_str(),
                    format_decimal(progress.box_loss, 4).c_str());
        ImGui::Text("Throughput: %s img/s", format_decimal(progress.images_per_second, 2).c_str());
        ImGui::Text("Elapsed: %ss", format_decimal(progress.elapsed_seconds, 1).c_str());
        if (progress.val_loss > 0.0 || progress.val_bbox_ap > 0.0 || progress.val_mask_ap > 0.0) {
            ImGui::Text("Val Loss: %s", format_decimal(progress.val_loss, 4).c_str());
            ImGui::Text("BBox AP: %s", format_decimal(progress.val_bbox_ap, 4).c_str());
            ImGui::Text("Mask AP: %s", format_decimal(progress.val_mask_ap, 4).c_str());
        }
        if (!progress.checkpoint_path.empty()) {
            draw_with_optional_font(compact_font, [&progress]() {
                ImGui::TextWrapped("Checkpoint: %s", progress.checkpoint_path.c_str());
            });
        }
    }

    if (!state.last_summary.empty()) {
        ImGui::Spacing();
        draw_with_optional_font(compact_font, [&state]() {
            ImGui::TextWrapped("%s", state.last_summary.c_str());
        });
    }
    if (!state.last_error.empty()) {
        ImGui::Spacing();
        draw_banner("activity_train_error_banner",
                    kBannerBackgroundColor,
                    kBannerTextColor,
                    state.last_error);
    }
    if (state.running || !state.output_tail.empty()) {
        ImGui::Spacing();
        draw_with_optional_font(compact_font, [&state]() {
            draw_console_tile("activity_train_output_tile",
                              state.running ? "Live Output" : "Recent Output",
                              [&state]() {
                                  draw_output_console("activity_train_output_tail",
                                                      state.output_tail,
                                                      state.running ? 140.0f : 96.0f,
                                                      state.running,
                                                      "Waiting for local train output...");
                              });
        });
    }
}

void draw_job_activity_content(const JobActivityViewModel& view_model, ImFont* compact_font) {
    if (view_model.running) {
        ImGui::TextWrapped("Running: %.*s",
                           static_cast<int>(view_model.label.size()),
                           view_model.label.data());
    } else if (!view_model.label.empty()) {
        ImGui::TextWrapped("Idle after: %.*s",
                           static_cast<int>(view_model.label.size()),
                           view_model.label.data());
    } else {
        ImGui::TextUnformatted("Idle");
    }

    if (!view_model.last_summary.empty()) {
        ImGui::Spacing();
        draw_with_optional_font(compact_font, [&view_model]() {
            ImGui::TextWrapped("%.*s",
                               static_cast<int>(view_model.last_summary.size()),
                               view_model.last_summary.data());
        });
    }
    if (view_model.running || !view_model.output_tail.empty()) {
        ImGui::Spacing();
        draw_with_optional_font(compact_font, [&view_model]() {
            draw_console_tile("activity_job_output_tile",
                              view_model.running ? "Live Output" : "Recent Output",
                              [&view_model]() {
                                  draw_output_console("activity_job_output_tail",
                                                      view_model.output_tail,
                                                      180.0f,
                                                      view_model.running,
                                                      "Waiting for job output...");
                              });
        });
    }
    if (!view_model.last_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kErrorTextColor, "Error");
        ImGui::TextWrapped("%.*s",
                           static_cast<int>(view_model.last_error.size()),
                           view_model.last_error.data());
    }
    if (!view_model.picker_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kErrorTextColor, "UI Error");
        ImGui::TextWrapped("%.*s",
                           static_cast<int>(view_model.picker_error.size()),
                           view_model.picker_error.data());
    }
}

void draw_annotate_activity_content(const AnnotateActivityViewModel& view_model, ImFont* compact_font) {
    if (view_model.has_frame) {
        draw_with_optional_font(compact_font, [&view_model]() {
            ImGui::TextWrapped("Frame: %.*s",
                               static_cast<int>(view_model.frame_source_name.size()),
                               view_model.frame_source_name.data());
        });
        ImGui::Text("Objects: %zu", view_model.object_count);
        ImGui::Text("Resolved: %zu", view_model.resolved_count);
    } else {
        ImGui::TextUnformatted("No annotation frame loaded");
    }
    if (view_model.assist_running) {
        ImGui::TextUnformatted("Assist: running");
    } else if (!view_model.assist_summary.empty()) {
        ImGui::TextWrapped("Assist: %.*s",
                           static_cast<int>(view_model.assist_summary.size()),
                           view_model.assist_summary.data());
    }
    if (view_model.save_running) {
        ImGui::TextUnformatted("Save: running");
    } else if (!view_model.save_summary.empty()) {
        ImGui::TextWrapped("Save: %.*s",
                           static_cast<int>(view_model.save_summary.size()),
                           view_model.save_summary.data());
    }
    if (view_model.live_capture_running) {
        ImGui::TextUnformatted("Live annotate capture is active");
    }
}

void draw_live_predict_running_content(const LivePredictActivityViewModel& view_model) {
    ImGui::Text("Controller: %s", view_model.controller_running ? "running" : "stopped");
    ImGui::Text("Analyzer: %s",
                view_model.analyzer_model_hot ? "hot"
                : view_model.analyzer_running ? "running"
                                              : "idle");
    ImGui::Text("Frames analyzed/skipped: %llu / %llu",
                static_cast<unsigned long long>(view_model.frames_analyzed),
                static_cast<unsigned long long>(view_model.frames_skipped));
    ImGui::Text("Compositor frames: %llu  Last latency: %.3f ms",
                static_cast<unsigned long long>(view_model.frames_composited),
                view_model.last_latency_ms);
    if (view_model.analyzer_backend_name.empty()) {
        ImGui::TextUnformatted("Analyzer backend: n/a");
    } else {
        ImGui::TextWrapped("Analyzer backend: %.*s",
                           static_cast<int>(view_model.analyzer_backend_name.size()),
                           view_model.analyzer_backend_name.data());
    }
    if (view_model.preview_has_frame) {
        ImGui::Text("Preview frame: %llu",
                    static_cast<unsigned long long>(view_model.preview_frame_id));
    }
    if (!view_model.last_error.empty()) {
        ImGui::Spacing();
        draw_banner("live_predict_error_banner",
                    kBannerBackgroundColor,
                    kBannerTextColor,
                    view_model.last_error);
    } else if (!view_model.start_error.empty()) {
        ImGui::Spacing();
        draw_banner("live_predict_start_error_banner",
                    kBannerBackgroundColor,
                    kBannerTextColor,
                    view_model.start_error);
    } else if (!view_model.action_error.empty()) {
        ImGui::Spacing();
        draw_banner("live_predict_action_activity_error_banner",
                    kBannerBackgroundColor,
                    kBannerTextColor,
                    view_model.action_error);
    } else if (!view_model.preview_error.empty()) {
        ImGui::Spacing();
        draw_banner("live_preview_activity_error_banner",
                    kBannerBackgroundColor,
                    kBannerTextColor,
                    view_model.preview_error);
    }
}

void draw_live_predict_preview_content(const LivePredictActivityViewModel& view_model,
                                       ImFont* compact_font) {
    draw_with_optional_font(compact_font, [&view_model]() {
        ImGui::TextWrapped("Source: %.*s",
                           static_cast<int>(view_model.static_preview_source_name.size()),
                           view_model.static_preview_source_name.data());
    });
    ImGui::Text("Frame: %llu",
                static_cast<unsigned long long>(view_model.preview_frame_id));
    ImGui::Text("Image: %u x %u",
                view_model.preview_width,
                view_model.preview_height);
}

} // namespace

void draw_output_console(const char* id,
                         const std::string_view output_tail,
                         const float height,
                         const bool running,
                         const char* waiting_message) {
    ImGui::BeginChild(id, ImVec2(0.0f, height), true);
    const bool stick_to_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f;
    ImGui::PushTextWrapPos();
    if (current_ui_fonts().mono != nullptr) {
        ImGui::PushFont(current_ui_fonts().mono);
    }
    if (output_tail.empty()) {
        ImGui::TextUnformatted(waiting_message);
    } else {
        ImGui::TextUnformatted(output_tail.data(), output_tail.data() + output_tail.size());
    }
    if (current_ui_fonts().mono != nullptr) {
        ImGui::PopFont();
    }
    if (running && stick_to_bottom) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void draw_activity_sidebar_tiles(const ActivitySidebarTilesViewModel& view_model,
                                 const ActivitySidebarTileActions& actions) {
    draw_training_activity_section(view_model.training);
    if (view_model.local_train != nullptr) {
        draw_local_train_activity_section(*view_model.local_train,
                                          actions.request_stop_local_training,
                                          actions.compact_font);
    }
    draw_job_activity_section(view_model.job, actions.compact_font);
    if (view_model.show_annotate) {
        draw_annotate_activity_section(view_model.annotate, actions.compact_font);
    }
    draw_live_predict_activity_section(view_model.live_predict, actions.compact_font);
}

void draw_training_activity_section(const TrainingActivityViewModel& view_model) {
    draw_section_tile("training_activity_tile", "Training", [&view_model]() {
        draw_training_activity_content(view_model);
    });
}

void draw_local_train_activity_section(const LocalTrainSessionState& state,
                                       const std::function<void(bool)>& request_stop_local_training,
                                       ImFont* compact_font) {
    draw_section_tile("local_train_activity_tile", "Local Train", [&]() {
        draw_local_train_activity_content(state, request_stop_local_training, compact_font);
    });
}

void draw_job_activity_section(const JobActivityViewModel& view_model, ImFont* compact_font) {
    draw_section_tile("job_activity_tile", "Job State", [&]() {
        draw_job_activity_content(view_model, compact_font);
    });
}

void draw_annotate_activity_section(const AnnotateActivityViewModel& view_model, ImFont* compact_font) {
    draw_section_tile("annotate_activity_tile", "Annotate", [&]() {
        draw_annotate_activity_content(view_model, compact_font);
    });
}

void draw_live_predict_activity_section(const LivePredictActivityViewModel& view_model, ImFont* compact_font) {
    if (view_model.show_running_section) {
        draw_section_tile("live_predict_activity_tile", "Live Predict", [&]() {
            draw_live_predict_running_content(view_model);
        });
    } else if (view_model.show_static_preview) {
        draw_section_tile("live_predict_preview_tile", "Preview", [&]() {
            draw_live_predict_preview_content(view_model, compact_font);
        });
    }

    if (view_model.show_idle_start_error) {
        draw_banner_tile("live_predict_start_error_tile", "Live Predict", [&]() {
            draw_banner("live_predict_start_error_banner",
                        kBannerBackgroundColor,
                        kBannerTextColor,
                        view_model.start_error);
        });
    }
}

} // namespace mmltk::gui
