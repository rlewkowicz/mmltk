#pragma once

#include "train_process_runtime.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

struct ImFont;

namespace mmltk::gui {

struct TrainingActivityViewModel {
    std::string_view execution_target_label;
    std::string_view local_gpu_summary;
    std::string_view selected_device_ids_summary;
    std::string_view local_gpu_error;
    std::string_view remote_gpu_family_summary;
    bool vast_api_key_configured = false;
    std::string_view armed_offer_summary;
    bool remote_query_running = false;
    std::string_view remote_query_summary;
    std::string_view remote_query_error;
};

struct JobActivityViewModel {
    bool running = false;
    std::string_view label;
    std::string_view last_summary;
    std::string_view last_error;
    std::string_view output_tail;
    std::string_view picker_error;
};

struct AnnotateActivityViewModel {
    bool has_frame = false;
    std::string_view frame_source_name;
    std::size_t object_count = 0;
    std::size_t resolved_count = 0;
    bool assist_running = false;
    std::string_view assist_summary;
    bool save_running = false;
    std::string_view save_summary;
    bool live_capture_running = false;
};

struct LivePredictActivityViewModel {
    bool show_running_section = false;
    bool show_static_preview = false;
    bool preview_has_frame = false;
    std::uint64_t preview_frame_id = 0;
    std::uint32_t preview_width = 0;
    std::uint32_t preview_height = 0;
    std::string_view static_preview_source_name;
    bool controller_running = false;
    bool analyzer_model_hot = false;
    bool analyzer_running = false;
    std::uint64_t frames_analyzed = 0;
    std::uint64_t frames_skipped = 0;
    std::uint64_t frames_composited = 0;
    double last_latency_ms = 0.0;
    std::string_view analyzer_backend_name;
    std::string_view last_error;
    std::string_view start_error;
    std::string_view action_error;
    std::string_view preview_error;
    bool show_idle_start_error = false;
};

void draw_output_console(const char* id,
                         std::string_view output_tail,
                         float height,
                         bool running,
                         const char* waiting_message);

void draw_training_activity_section(const TrainingActivityViewModel& view_model);
void draw_local_train_activity_section(const LocalTrainSessionState& state,
                                       const std::function<void(bool)>& request_stop_local_training,
                                       ImFont* compact_font);
void draw_job_activity_section(const JobActivityViewModel& view_model, ImFont* compact_font);
void draw_annotate_activity_section(const AnnotateActivityViewModel& view_model, ImFont* compact_font);
void draw_live_predict_activity_section(const LivePredictActivityViewModel& view_model, ImFont* compact_font);

} // namespace mmltk::gui
