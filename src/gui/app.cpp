#include "app.h"
#include "app_api.h"
#include "activity_pane_presenter.h"
#include "console_output.h"
#include "default_state.h"
#include "file_picker.h"
#include "gui/annotation/annotation_ui.h"
#include "gui/annotation/editor.h"
#include "gui/annotation/render/renderer.h"
#include "gui/annotation/sidebar.h"
#include "gui/annotation/sidebar_actions.h"
#include "gui/annotation/tools/skeleton_tool_helpers.h"
#include "gui/annotation/tools/spline_tool_helpers.h"
#include "gui/annotation/workflow.h"
#include "gui/annotation/workspace/canvas.h"
#include "gui/annotation/workspace/interaction.h"
#include "gui/annotation/workspace/model.h"
#include "live_preview_texture.h"
#include "live_session_utils.h"
#include "preview_interaction_overlay.h"
#include "rfdetr_module.h"
#include "rfdetr_workflows.h"
#include "source_runtime.h"
#include "ui_controls.h"
#include "ui_style.h"
#include "mmltk_logging.h"

#include "mmltk/rfdetr/live_predict.h"
#include "mmltk/rfdetr/model.h"
#include "mmltk/rfdetr/model_config.h"
#include "mmltk/rfdetr/predict.h"
#include "mmltk/rfdetr/validate.h"
#include "rfdetr/backends.h"

#if MMLTK_RFDETR_LIVE_CAPTURE
#include "mmltk/live/live_session_controller.h"
#endif

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <GLFW/glfw3.h>

#include <array>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mmltk::gui {

void AppDeleter::operator()(App *app) const noexcept { delete app; }

void apply_app_style() { App::apply_style(); }

AppHandle make_app(GLFWwindow *main_window, std::string vast_api_key,
                   std::string settings_path) {
  return AppHandle(
      new App(main_window, std::move(vast_api_key), std::move(settings_path)));
}

void poll_background_work(App &app) { app.poll_background_work(); }

void render(App &app) { app.render(); }

void shutdown(App &app) { app.shutdown(); }

namespace {

using namespace mmltk::rfdetr;
namespace console_output = mmltk::gui::console_output;

constexpr int kWhiteCropStrokeWidth = 2;

float crop_edge_hit_half_width_px() {
  return ui_scaled(current_ui_settings().crop_edge_hit_half_width);
}

float crop_corner_hit_size_px() {
  return ui_scaled(current_ui_settings().crop_corner_hit_size);
}

int crop_handle_radius_pixels(float screen_width, float screen_height,
                              float image_width, float image_height) {
  const float desired_screen_radius =
      ui_scaled(current_ui_settings().crop_handle_radius);
  if (screen_width <= 0.0f || screen_height <= 0.0f || image_width <= 0.0f ||
      image_height <= 0.0f) {
    return std::max(1, static_cast<int>(std::ceil(desired_screen_radius)));
  }

  const float scale_x = screen_width / image_width;
  const float scale_y = screen_height / image_height;
  const float image_to_screen_scale =
      std::max(1.0e-3f, std::min(scale_x, scale_y));
  return std::max(1, static_cast<int>(std::ceil(desired_screen_radius /
                                                image_to_screen_scale)));
}

void preview_uvs_for_region(const LiveCaptureRegion &texture_region,
                            const LiveCaptureRegion &display_region,
                            ImVec2 *uv0, ImVec2 *uv1) {
  if (uv0 == nullptr || uv1 == nullptr || texture_region.width == 0U ||
      texture_region.height == 0U) {
    return;
  }
  const float inv_width = 1.0f / static_cast<float>(texture_region.width);
  const float inv_height = 1.0f / static_cast<float>(texture_region.height);
  *uv0 = ImVec2(
      static_cast<float>(display_region.x - texture_region.x) * inv_width,
      static_cast<float>(display_region.y - texture_region.y) * inv_height);
  *uv1 = ImVec2(static_cast<float>(display_region.x + display_region.width -
                                   texture_region.x) *
                    inv_width,
                static_cast<float>(display_region.y + display_region.height -
                                   texture_region.y) *
                    inv_height);
}

const char *view_label(View view) {
  switch (view) {
  case View::Train:
    return "Train";
  case View::Validate:
    return "Validate";
  case View::Predict:
    return "Predict";
  case View::Annotate:
    return "Annotate";
  case View::Export:
    return "Export";
  case View::Live:
    return "Live";
  }
  return "Unknown";
}

const char *ui_density_label(const UiDensity density) {
  switch (density) {
  case UiDensity::Compact:
    return "Compact";
  case UiDensity::Balanced:
    return "Balanced";
  case UiDensity::Comfortable:
    return "Comfortable";
  }
  return "Unknown";
}

const char *train_execution_target_label(TrainExecutionTarget target) {
  switch (target) {
  case TrainExecutionTarget::Local:
    return "Local";
  case TrainExecutionTarget::Remote:
    return "Remote";
  }
  return "Unknown";
}

const char *train_optimizer_label(TrainOptimizerMode mode) {
  switch (mode) {
  case TrainOptimizerMode::AdamW:
    return "AdamW";
  case TrainOptimizerMode::Muon:
    return "Muon";
  }
  return "Unknown";
}

TrainOptimizerKind train_optimizer_kind(TrainOptimizerMode mode) {
  switch (mode) {
  case TrainOptimizerMode::AdamW:
    return TrainOptimizerKind::AdamW;
  case TrainOptimizerMode::Muon:
    return TrainOptimizerKind::Muon;
  }
  return TrainOptimizerKind::AdamW;
}

const char *lr_scheduler_label(const std::string &value) {
  return value == "cosine" ? "Cosine" : "Step";
}

void log_gui_error(const char *context, const std::string &message) {
  if (message.empty()) {
    return;
  }
  mmltk::logging::logger("gui")->error("mmltk gui {}: {}", context, message);
}

void draw_banner(const char *id, const ImVec4 &background_color,
                 const ImVec4 &text_color, const std::string &message) {
  ImGui::PushID(id);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, background_color);
  ImGui::BeginChild(
      "banner", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 2.4f),
      true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PushStyleColor(ImGuiCol_Text, text_color);
  ImGui::TextWrapped("%s", message.c_str());
  ImGui::PopStyleColor();
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopID();
}

bool draw_train_optimizer_combo(const char *label, TrainOptimizerMode &mode) {
  bool changed = false;
  draw_labeled_combo(
      label, train_optimizer_label(mode), 180.0f, [&mode, &changed]() {
        for (const auto option :
             {TrainOptimizerMode::AdamW, TrainOptimizerMode::Muon}) {
          const bool selected = option == mode;
          if (ImGui::Selectable(train_optimizer_label(option), selected)) {
            mode = option;
            changed = true;
          }
          if (selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
      });
  return changed;
}

bool draw_lr_scheduler_combo(const char *label, std::string &scheduler) {
  bool changed = false;
  draw_labeled_combo(
      label, lr_scheduler_label(scheduler), 180.0f, [&scheduler, &changed]() {
        for (const char *option : {"step", "cosine"}) {
          const bool selected = scheduler == option;
          if (ImGui::Selectable(lr_scheduler_label(option), selected)) {
            scheduler = option;
            changed = true;
          }
          if (selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
      });
  return changed;
}

template <typename Options>
void apply_model_input(ModelInputMode mode, const std::string &weights_path,
                       const std::string &onnx_path,
                       const std::string &tensorrt_path, Options &options) {
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

std::string annotate_error() {
  return "Annotate saves full-scene PNG + JSONL labels and per-object RGBA "
         "crops. "
         "Use single image, image folder, or live video sources.";
}

std::string annotation_source_signature(const SourceSelectionState &source) {
  std::ostringstream stream;
  stream << static_cast<int>(source.kind) << "|" << source.compiled_path << "|"
         << source.single_image_path << "|" << source.image_directory << "|"
         << source.recursive << "|" << source.device_index << "|"
         << source.capture_width << "|" << source.capture_height << "|"
         << source.capture_fps << "|" << source.v4l2_buffer_count;
  // Annotate crop is preview state only. Excluding it here keeps live annotate
  // running when the crop box is committed or the crop fields are edited.
  return stream.str();
}

ImVec4 annotation_range_swatch_color(const AnnotationColorRange &range) {
  const float hue = std::fmod(range.center.hue_degrees / 60.0f, 6.0f);
  const float saturation = std::clamp(range.center.saturation, 0.0f, 1.0f);
  const float value = std::clamp(range.center.value, 0.0f, 1.0f);
  const float chroma = value * saturation;
  const float x = chroma * (1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f));
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  if (hue < 1.0f) {
    r = chroma;
    g = x;
  } else if (hue < 2.0f) {
    r = x;
    g = chroma;
  } else if (hue < 3.0f) {
    g = chroma;
    b = x;
  } else if (hue < 4.0f) {
    g = x;
    b = chroma;
  } else if (hue < 5.0f) {
    r = x;
    b = chroma;
  } else {
    r = chroma;
    b = x;
  }
  const float match = value - chroma;
  return {r + match, g + match, b + match, 1.0f};
}

std::string predict_combo_error(const PredictViewState &state) {
  const bool raw_source = state.source.kind == SourceKind::SingleImage ||
                          state.source.kind == SourceKind::ImageFolder;
  if (raw_source && state.model_input == ModelInputMode::Weights &&
      state.lanes > 1) {
    return "Weights + raw-image predict requires lanes <= 1.";
  }
  return {};
}

std::string live_predict_blocker(const PredictViewState &state,
                                 const JobState &job) {
  if (state.source.kind != SourceKind::VideoStream) {
    return {};
  }
  if (job.running) {
    return job.label.empty()
               ? "Finish the active workflow before starting live prediction."
               : "Finish the active workflow before starting live "
                 "prediction: " +
                     job.label + ".";
  }
  std::string source_error = validate_predict_source(state.source);
  if (!source_error.empty()) {
    return source_error;
  }
  if (!live_capture_supported()) {
    return "RF-DETR live video predict is not available in this build.";
  }
  return {};
}

void set_rectangle_drag_cursor(RectDragKind kind) {
  if (kind == RectDragKind::ResizeTopLeft ||
      kind == RectDragKind::ResizeTopRight ||
      kind == RectDragKind::ResizeBottomLeft ||
      kind == RectDragKind::ResizeBottomRight) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
  } else if (kind == RectDragKind::Move) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }
}

bool annotation_boxes_equal(const AnnotationBox &lhs,
                            const AnnotationBox &rhs) {
  return lhs.x1 == rhs.x1 && lhs.y1 == rhs.y1 && lhs.x2 == rhs.x2 &&
         lhs.y2 == rhs.y2;
}

void draw_box_handles(ImDrawList *draw_list, const ImVec2 &image_min,
                      float scale_x, float scale_y, const AnnotationBox &box,
                      ImU32 fill_color, ImU32 outline_color) {
  if (draw_list == nullptr || !annotation_box_has_area(box)) {
    return;
  }

  const float handle_radius = 4.0f;
  const std::array<ImVec2, 4> corners = {
      ImVec2(image_min.x + scale_x * static_cast<float>(box.x1),
             image_min.y + scale_y * static_cast<float>(box.y1)),
      ImVec2(image_min.x + scale_x * static_cast<float>(box.x2),
             image_min.y + scale_y * static_cast<float>(box.y1)),
      ImVec2(image_min.x + scale_x * static_cast<float>(box.x1),
             image_min.y + scale_y * static_cast<float>(box.y2)),
      ImVec2(image_min.x + scale_x * static_cast<float>(box.x2),
             image_min.y + scale_y * static_cast<float>(box.y2)),
  };
  for (const ImVec2 &corner : corners) {
    draw_list->AddRectFilled(
        ImVec2(corner.x - handle_radius, corner.y - handle_radius),
        ImVec2(corner.x + handle_radius, corner.y + handle_radius), fill_color,
        1.5f);
    draw_list->AddRect(
        ImVec2(corner.x - handle_radius, corner.y - handle_radius),
        ImVec2(corner.x + handle_radius, corner.y + handle_radius),
        outline_color, 1.5f, 0, 1.0f);
  }
}

void append_overlay_box(PreviewInteractionOverlaySnapshot &snapshot,
                        const AnnotationBox &box, std::uint8_t r,
                        std::uint8_t g, std::uint8_t b, int thickness,
                        bool draw_handles = false, int handle_radius = 4) {
  if (!annotation_box_has_area(box)) {
    return;
  }
  snapshot.boxes.push_back(PreviewInteractionOverlayBox{
      box,
      r,
      g,
      b,
      thickness,
      draw_handles,
      std::max(1, handle_radius),
  });
}

const ModelPresetConfig &resolve_selected_preset(std::string &preset_name) {
  if (const auto *preset = find_model_preset(preset_name)) {
    return *preset;
  }
  const auto *fallback = find_model_preset(kDefaultGuiPresetName);
  if (fallback == nullptr) {
    throw std::runtime_error("missing default RF-DETR preset");
  }
  preset_name = std::string(fallback->preset_name);
  return *fallback;
}

LivePreviewTextureState snapshot_preview_state(
    const std::unique_ptr<LivePreviewTexture> &preview_texture) {
  return preview_texture ? preview_texture->snapshot()
                         : LivePreviewTextureState{};
}

std::string format_decimal(double value, int precision) {
  std::ostringstream stream;
  stream.setf(std::ios::fixed);
  stream.precision(precision);
  stream << value;
  return stream.str();
}

TrainRecipeConfig current_train_recipe(const std::string &preset_name,
                                       TrainOptimizerMode optimizer) {
  return resolve_train_recipe(preset_name, train_optimizer_kind(optimizer));
}

void update_train_recipe_override(bool &dirty, double current,
                                  double recipe_value) {
  dirty = !train_recipe_value_matches(current, recipe_value);
}

void update_train_recipe_override(bool &dirty, int current, int recipe_value) {
  dirty = current != recipe_value;
}

void update_train_recipe_override(bool &dirty, const std::string &current,
                                  std::string_view recipe_value) {
  dirty = current != recipe_value;
}

constexpr std::array<RemoteGpuFamily, 5> kRemoteGpuFamilyOrder{
    RemoteGpuFamily::A100, RemoteGpuFamily::B200,    RemoteGpuFamily::H100,
    RemoteGpuFamily::H200, RemoteGpuFamily::LSeries,
};

std::string local_gpu_selection_summary(const std::vector<LocalGpuInfo> &gpus,
                                        const std::vector<bool> &selected) {
  if (gpus.empty()) {
    return "No visible GPUs";
  }
  size_t selected_count = 0;
  for (size_t index = 0; index < std::min(gpus.size(), selected.size());
       ++index) {
    if (selected[index]) {
      ++selected_count;
    }
  }
  if (selected_count == 0) {
    return "No GPUs selected";
  }
  if (selected_count == gpus.size()) {
    return std::to_string(selected_count) + " visible GPUs selected";
  }
  return std::to_string(selected_count) + " / " + std::to_string(gpus.size()) +
         " GPUs selected";
}

std::string format_memory_gib(std::uint64_t bytes) {
  constexpr double gib_divisor = 1024.0 * 1024.0 * 1024.0;
  return format_decimal(static_cast<double>(bytes) / gib_divisor, 1) + " GiB";
}

std::string local_gpu_label(const LocalGpuInfo &gpu) {
  std::ostringstream stream;
  stream << "GPU " << gpu.device_id << " · " << gpu.name;
  if (gpu.total_memory_bytes > 0) {
    stream << " · " << format_memory_gib(gpu.total_memory_bytes);
  }
  return stream.str();
}

void draw_remote_offer_card(const VastOfferSummary &offer, bool selected) {
  if (selected) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.20f, 0.28f, 0.21f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.66f, 0.34f, 1.00f));
  }
  ImGui::BeginChild("offer_card", ImVec2(0.0f, 88.0f), true,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::Text("%s · %d GPUs · %s", offer.gpu_name.c_str(), offer.num_gpus,
              remote_gpu_family_label(offer.family));
  ImGui::Text("DLPerf/$ %s   DLPerf %s   $/hr %s",
              format_decimal(offer.dlperf_usd, 2).c_str(),
              format_decimal(offer.dlperf, 2).c_str(),
              format_decimal(offer.dph, 2).c_str());
  ImGui::Text("Reliability %s   GPU RAM %s GiB   Inet %s Mb/s",
              format_decimal(offer.reliability, 3).c_str(),
              format_decimal(offer.gpu_ram, 0).c_str(),
              format_decimal(offer.inet_down, 0).c_str());
  if (!offer.geolocation.empty()) {
    ImGui::Text("Region %s", offer.geolocation.c_str());
  }
  if (selected) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Armed for the next remote train cycle");
  }
  ImGui::EndChild();
  if (selected) {
    ImGui::PopStyleColor(2);
  }
}

} // namespace

App::App(GLFWwindow *main_window, std::string vast_api_key,
         std::string settings_path)
    : vast_query_controller_(background_executor_, ui_callbacks_),
      local_train_controller_(background_executor_, ui_callbacks_),
      vast_api_key_(std::move(vast_api_key)),
      settings_persistence_(std::move(settings_path)) {
  model_registry_.register_module(make_rfdetr_model_module());
  apply_default_gui_state(selected_preset_name_, train_, validate_, predict_,
                          annotate_, export_);
  bool loaded_settings = false;
  {
    GuiSettingsSnapshot snap{current_view_, selected_preset_name_,
                             &ui_settings_, &train_,
                             &validate_,    &predict_,
                             &annotate_,    &export_};
    if (settings_persistence_.load(snap)) {
      loaded_settings = true;
      current_view_ = snap.current_view;
      selected_preset_name_ = snap.selected_preset;
    }
  }
  float xscale = 1.0f;
  float yscale = 1.0f;
  glfwGetWindowContentScale(main_window, &xscale, &yscale);
  ui_settings_.content_scale = std::max(xscale, yscale);

  const std::string requested_preset = selected_preset_name_;
  resolve_selected_preset(selected_preset_name_);
  apply_ui_settings_now(true);
  if (!loaded_settings || selected_preset_name_ != requested_preset) {
    apply_selected_preset_defaults();
  }
  local_train_controller_.initialize(train_.local_device_ids);

  live_preview_texture_ = std::make_unique<LivePreviewTexture>(main_window);
  live_interaction_overlay_ =
      std::make_unique<PreviewInteractionOverlaySurface>(main_window);
  annotate_interaction_overlay_ =
      std::make_unique<PreviewInteractionOverlaySurface>(main_window);
  std::string preview_initialize_error;
  if (!live_preview_texture_->initialize(&preview_initialize_error)) {
    live_preview_error_ = std::move(preview_initialize_error);
    log_gui_error("preview init error", live_preview_error_);
  }
  std::string interaction_overlay_error;
  if (live_interaction_overlay_ != nullptr &&
      !live_interaction_overlay_->initialize(&interaction_overlay_error)) {
    log_gui_error("interaction overlay init error",
                            interaction_overlay_error);
  }
  interaction_overlay_error.clear();
  if (annotate_interaction_overlay_ != nullptr &&
      !annotate_interaction_overlay_->initialize(&interaction_overlay_error)) {
    log_gui_error("interaction overlay init error",
                            interaction_overlay_error);
  }
  (void)annotate_controller_.set_active_tool(AnnotationToolKind::Direct,
                                             annotate_document_,
                                             annotate_session_);
}

App::~App() { shutdown(); }

void App::apply_ui_settings_now(const bool rebuild_font_texture) {
  apply_ui_settings(ui_settings_, rebuild_font_texture);
  compact_font_ = current_ui_fonts().compact;
  mono_font_ = current_ui_fonts().mono;
}

void App::shutdown() {
  if (shutting_down_) {
    return;
  }
  shutting_down_ = true;
  stop_live_predict_session();
  local_train_controller_.shutdown();
  settings_persistence_.flush();
  background_executor_.wait_idle();
  ui_callbacks_.drain();
  reset_annotation_workflow_live_session_shell(
      annotate_workflow_, &live_controller_, &live_session_status_,
      &annotate_frame_, &annotate_resolved_instances_,
      live_preview_texture_.get(),
      [this]() { cancel_annotation_canvas_interactions(); });
  active_live_mode_ = ActiveLiveMode::None;
  if (live_preview_texture_) {
    live_preview_texture_->shutdown();
  }
  if (live_interaction_overlay_) {
    live_interaction_overlay_->shutdown();
  }
  if (annotate_interaction_overlay_) {
    annotate_interaction_overlay_->shutdown();
  }
}

void App::apply_style() { apply_ui_settings(UiSettingsState{}, false); }

void App::poll_background_work() {
  if (ui_settings_apply_pending_) {
    apply_ui_settings_now(true);
    ui_settings_apply_pending_ = false;
  }
  ui_callbacks_.drain();
  if (live_preview_texture_) {
    live_preview_texture_->pump();
  }
  if (live_interaction_overlay_) {
    live_interaction_overlay_->pump();
  }
  if (annotate_interaction_overlay_) {
    annotate_interaction_overlay_->pump();
  }
  local_train_controller_.poll();
  live_predict_controller_.poll_status();
  {
    GuiSettingsSnapshot snap{current_view_, selected_preset_name_,
                             &ui_settings_, &train_,
                             &validate_,    &predict_,
                             &annotate_,    &export_};
    settings_persistence_.notify_frame(snap);
  }
  const LivePreviewTextureState preview_state =
      snapshot_preview_state(live_preview_texture_);
  live_preview_error_ = preview_state.last_error;
  poll_annotate_work();

  if (live_controller_) {
    if (!live_session_status_) {
      live_session_status_ = std::make_unique<mmltk::live::LiveSessionStatus>();
    }
    *live_session_status_ = live_controller_->snapshot_status();
  }
}

void App::launch_job(const std::string &label, std::function<JobOutcome()> fn) {
  if (job_.running) {
    return;
  }
  job_.running = true;
  job_.label = label;
  job_.last_summary.clear();
  job_.last_error.clear();
  {
    std::lock_guard<std::mutex> lock(job_.output_mutex);
    job_.output_tail.clear();
  }
  mmltk::runtime::submit_background_task(
      background_executor_, ui_callbacks_,
      [this, task = std::move(fn)]() mutable {
        rfdetr::ScopedTensorRtLogSink trt_log_sink(
            [this](const std::string &line) { append_job_output(line); });
        return task();
      },
      [this](JobOutcome outcome) mutable {
        job_.last_summary = std::move(outcome.summary);
        job_.last_error.clear();
        if (outcome.preview.has_value()) {
          apply_static_preview(std::move(*outcome.preview));
          current_view_ = View::Live;
        }
        job_.running = false;
      },
      [this](const std::string &error) {
        job_.last_error = error;
        job_.last_summary.clear();
        job_.running = false;
        log_gui_error("job error", job_.last_error);
      });
}

void App::launch_file_picker(FileBrowseField field, const char *dialog_title,
                             std::string *target) {
  if (active_file_browse_field_ != FileBrowseField::None || target == nullptr ||
      shutting_down_) {
    return;
  }
  active_file_browse_field_ = field;
  picker_error_.clear();
  mmltk::runtime::submit_background_task(
      background_executor_, ui_callbacks_,
      [dialog = std::string(dialog_title), initial = *target]() {
        return pick_file_with_zenity(dialog.c_str(), initial);
      },
      [this, field, target](std::optional<std::string> picked) {
        if (picked.has_value()) {
          *target = std::move(*picked);
        }
        picker_error_.clear();
        if (active_file_browse_field_ == field) {
          active_file_browse_field_ = FileBrowseField::None;
        }
      },
      [this, field](const std::string &error) {
        picker_error_ = error;
        if (active_file_browse_field_ == field) {
          active_file_browse_field_ = FileBrowseField::None;
        }
      });
}

bool App::file_picker_busy(FileBrowseField field) const {
  return active_file_browse_field_ == field;
}

void App::append_job_output(std::string_view chunk) {
  std::lock_guard<std::mutex> lock(job_.output_mutex);
  console_output::append_console_output(job_.output_tail, chunk, 65536);
  if (job_.output_tail.empty() || job_.output_tail.back() != '\n') {
    job_.output_tail.push_back('\n');
    console_output::trim_output_tail(job_.output_tail, 65536);
  }
}

std::string App::snapshot_job_output() {
  std::lock_guard<std::mutex> lock(job_.output_mutex);
  return job_.output_tail;
}

void App::draw_annotation_range_controls(const char *label,
                                         AnnotationColorRange &range,
                                         AnnotationColorRange &sibling_range) {
  ImGui::PushID(label);
  const AnnotationColorRange original = range;
  const bool compact_layout = ImGui::GetContentRegionAvail().x < 520.0f;
  const float slider_width = compact_layout ? -FLT_MIN : 160.0f;
  draw_section_heading(label);
  const ImVec4 swatch_color = annotation_range_swatch_color(range);
  const ImVec2 swatch_size(96.0f, 22.0f);
  if (range.sampling) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.86f, 0.60f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.92f, 0.67f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(0.74f, 0.50f, 0.11f, 1.0f));
    (void)ImGui::Button("Eyedrop", swatch_size);
    ImGui::PopStyleColor(3);
  } else {
    (void)ImGui::ColorButton("##swatch", swatch_color,
                             ImGuiColorEditFlags_NoTooltip, swatch_size);
  }
  ImGui::SameLine();
  if (ImGui::Button(range.sampling ? "Cancel Picker" : "Arm Picker")) {
    const bool next_sampling = !range.sampling;
    range.sampling = next_sampling;
    if (next_sampling) {
      sibling_range.sampling = false;
    }
    invalidate_annotation_preview();
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset Range")) {
    recenter_annotation_range(range, AnnotationHsv{});
    sibling_range.sampling = false;
    invalidate_annotation_preview();
  }
  ImGui::Text("Target HSV: H %.1f  S %.1f%%  V %.1f%%",
              range.center.hue_degrees, range.center.saturation * 100.0f,
              range.center.value * 100.0f);
  draw_labeled_percent_slider("H-", range.tolerance.hue_minus_pct,
                              slider_width);
  draw_labeled_percent_slider("H+", range.tolerance.hue_plus_pct, slider_width);
  draw_labeled_percent_slider("S-", range.tolerance.saturation_minus_pct,
                              slider_width);
  draw_labeled_percent_slider("S+", range.tolerance.saturation_plus_pct,
                              slider_width);
  draw_labeled_percent_slider("V-", range.tolerance.value_minus_pct,
                              slider_width);
  draw_labeled_percent_slider("V+", range.tolerance.value_plus_pct,
                              slider_width);
  const bool changed =
      original.sampling != range.sampling ||
      original.center.hue_degrees != range.center.hue_degrees ||
      original.center.saturation != range.center.saturation ||
      original.center.value != range.center.value ||
      original.tolerance.hue_minus_pct != range.tolerance.hue_minus_pct ||
      original.tolerance.hue_plus_pct != range.tolerance.hue_plus_pct ||
      original.tolerance.saturation_minus_pct !=
          range.tolerance.saturation_minus_pct ||
      original.tolerance.saturation_plus_pct !=
          range.tolerance.saturation_plus_pct ||
      original.tolerance.value_minus_pct != range.tolerance.value_minus_pct ||
      original.tolerance.value_plus_pct != range.tolerance.value_plus_pct;
  if (changed) {
    invalidate_annotation_preview();
  }
  ImGui::PopID();
}

void App::invalidate_annotation_preview() {
  invalidate_annotation_workflow_preview(annotate_workflow_);
}

void App::cancel_annotation_canvas_interactions() {
  (void)cancel_active_grouped_edit(annotate_document_, annotate_session_);
  annotate_controller_.reset_active_drawing(annotate_session_);
  annotate_document_.cancel_transaction();
}

void App::reset_annotation_instances() {
  reset_annotation_workflow_runtime(annotate_workflow_);
  annotate_document_.clear();
  annotate_document_.clear_history();
  annotate_resolved_instances_.clear();
  select_object(annotate_session_, annotate_document_, std::nullopt);
  cancel_annotation_canvas_interactions();
  invalidate_annotation_preview();
}

void App::sync_annotation_categories() {
  if (annotate_.output_dir.empty()) {
    annotate_categories_loaded_ = false;
    annotate_categories_output_dir_.clear();
    return;
  }
  if (annotate_categories_loaded_ &&
      annotate_categories_output_dir_ == annotate_.output_dir) {
    return;
  }
  try {
    AnnotationCategories loaded_categories =
        load_annotation_categories(annotate_.output_dir);
    annotate_categories_ = std::move(loaded_categories);
    annotate_categories_loaded_ = true;
    annotate_categories_output_dir_ = annotate_.output_dir;
    (void)AnnotationEditor::repair_object_category_indices(annotate_document_,
                                                           annotate_categories_);
  } catch (const std::exception &error) {
    annotate_categories_loaded_ = true;
    annotate_categories_output_dir_ = annotate_.output_dir;
    annotate_save_error_ = error.what();
    log_gui_error("annotate categories error", annotate_save_error_);
  }
}

void App::prepare_annotation_source() {
  const std::string signature = annotation_source_signature(annotate_.source);
  if (signature == annotate_source_signature_) {
    return;
  }

  annotate_source_signature_ = signature;
  ++annotate_prepare_request_id_;
    annotate_inputs_.clear();
    annotate_current_input_index_ = 0;
    annotate_frame_.reset();
    annotate_prepare_running_ = false;
    annotate_frame_load_running_ = false;
  annotate_assist_summary_.clear();
  annotate_assist_error_.clear();
  annotate_save_summary_.clear();
  annotate_save_error_.clear();
  reset_annotation_instances();
  reset_annotation_workflow_live_session_shell(
      annotate_workflow_, &live_controller_, &live_session_status_,
      &annotate_frame_, &annotate_resolved_instances_,
      live_preview_texture_.get(),
      [this]() { cancel_annotation_canvas_interactions(); });
  active_live_mode_ = ActiveLiveMode::None;
  if (annotate_.source.kind == SourceKind::VideoStream) {
    return;
  }

  if (annotate_.source.kind == SourceKind::CompiledDataset) {
    annotate_save_error_ =
        "Annotate only supports single images, image folders, and live video.";
    return;
  }

  const SourceSelectionState source = annotate_.source;
  const std::uint64_t request_id = annotate_prepare_request_id_;
  annotate_prepare_running_ = true;
  mmltk::runtime::submit_background_task(
      background_executor_, ui_callbacks_,
      [source]() {
        PreparedPredictSource prepared = prepare_predict_source(source);
        return std::move(prepared.image_inputs);
      },
      [this, request_id,
       signature](std::vector<mmltk::rfdetr::PredictImageInput> inputs) {
        if (request_id != annotate_prepare_request_id_ ||
            signature != annotate_source_signature_) {
          return;
        }
        annotate_prepare_running_ = false;
        annotate_inputs_ = std::move(inputs);
        annotate_save_error_.clear();
        if (!annotate_inputs_.empty()) {
          load_annotation_current_frame();
        }
      },
      [this, request_id, signature](const std::string &error) {
        if (request_id != annotate_prepare_request_id_ ||
            signature != annotate_source_signature_) {
          return;
        }
        annotate_prepare_running_ = false;
        annotate_save_error_ = error;
        log_gui_error("annotate source error", annotate_save_error_);
      });
}

void App::load_annotation_current_frame() {
  if (annotate_current_input_index_ >= annotate_inputs_.size()) {
    annotate_frame_.reset();
    annotate_resolved_instances_.clear();
    cancel_annotation_canvas_interactions();
    invalidate_annotation_preview();
    return;
  }
  ++annotate_frame_request_id_;
  const std::uint64_t request_id = annotate_frame_request_id_;
  const mmltk::rfdetr::PredictImageInput input =
      annotate_inputs_[annotate_current_input_index_];
  annotate_frame_load_running_ = true;
  mmltk::runtime::submit_background_task(
      background_executor_, ui_callbacks_,
      [input]() { return mmltk::gui::load_annotation_frame(input); },
      [this, request_id](AnnotationFrame frame) {
        if (request_id != annotate_frame_request_id_) {
          return;
        }
        annotate_frame_load_running_ = false;
        load_annotation_frame(std::move(frame));
        annotate_save_error_.clear();
      },
      [this, request_id](const std::string &error) {
        if (request_id != annotate_frame_request_id_) {
          return;
        }
        annotate_frame_load_running_ = false;
        annotate_frame_.reset();
        annotate_resolved_instances_.clear();
        cancel_annotation_canvas_interactions();
        invalidate_annotation_preview();
        annotate_save_error_ = error;
        log_gui_error("annotate frame load error",
                                annotate_save_error_);
      });
}

void App::step_annotation_current_frame(const int step) {
  if (step == 0) {
    load_annotation_current_frame();
    return;
  }
  const auto next_index =
      static_cast<std::ptrdiff_t>(annotate_current_input_index_) + step;
  if (next_index < 0) {
    return;
  }
  annotate_current_input_index_ = static_cast<std::size_t>(next_index);
  load_annotation_current_frame();
}

void App::handle_annotation_setup_browse_request(
    const AnnotationSetupBrowseRequest browse_request) {
  switch (browse_request) {
  case AnnotationSetupBrowseRequest::SingleImage:
    launch_file_picker(FileBrowseField::AnnotateSingleImage, "Select Image",
                       &annotate_.source.single_image_path);
    break;
  case AnnotationSetupBrowseRequest::Weights:
    launch_file_picker(FileBrowseField::AnnotateWeights, "Select Weights",
                       &annotate_.weights_path);
    break;
  case AnnotationSetupBrowseRequest::Onnx:
    launch_file_picker(FileBrowseField::AnnotateOnnx, "Select ONNX Model",
                       &annotate_.onnx_path);
    break;
  case AnnotationSetupBrowseRequest::TensorRt:
    launch_file_picker(FileBrowseField::AnnotateTensorRt,
                       "Select TensorRT Engine", &annotate_.tensorrt_path);
    break;
  case AnnotationSetupBrowseRequest::None:
    break;
  }
}

void App::load_annotation_frame(AnnotationFrame frame) {
  const bool reset_canvas_view = annotation_workflow_should_reset_canvas_view(
      annotation_frame_ptr(annotate_frame_), frame);
  if (annotate_.source.kind != SourceKind::VideoStream) {
    reset_annotation_instances();
  } else {
    annotate_resolved_instances_.clear();
  }
  if (reset_canvas_view) {
    annotate_canvas_scale_ = 0.0f;
    annotate_canvas_pan_x_ = 0.0f;
    annotate_canvas_pan_y_ = 0.0f;
    annotate_canvas_auto_fit_ = true;
  }
  annotate_frame_ = std::move(frame);
  if (annotate_.source.kind != SourceKind::VideoStream &&
      annotate_frame_.has_value() && !annotate_.output_dir.empty()) {
    sync_annotation_categories();
    try {
      const std::optional<std::vector<AnnotationObject>> loaded_objects =
          load_saved_annotation_scene_for_frame(
              annotate_.output_dir, *annotate_frame_, &annotate_categories_);
      if (loaded_objects.has_value()) {
        annotate_document_.set_objects(*loaded_objects, true);
        if (!annotate_document_.empty()) {
          select_object(annotate_session_, annotate_document_, 0U);
        }
      }
    } catch (const std::exception &error) {
      annotate_save_error_ = error.what();
      log_gui_error("annotate scene load error",
                              annotate_save_error_);
    }
  }
  invalidate_annotation_preview();
}

void App::submit_annotation_preview() {
  const AnnotationFrame *frame_ptr = annotation_frame_ptr(annotate_frame_);
  if (frame_ptr == nullptr || !live_preview_texture_ ||
      annotation_workflow_preview_running(annotate_workflow_)) {
    return;
  }
  const bool live_mode = annotation_live_running();
  if (live_mode) {
    AnnotationWorkflowLivePreviewRefreshResult refreshed =
        refresh_annotation_workflow_live_preview(
            annotate_workflow_, annotate_document_, annotate_session_,
            *frame_ptr, annotate_categories_, !frame_ptr->pixels_bgr.empty(),
            [this](mmltk::live::ManualOverlayDocumentSnapshot snapshot) {
              live_controller_->manual_overlay_document().publish_snapshot(
                  std::move(snapshot));
            });
    if (refreshed.ok) {
      if (!frame_ptr->pixels_bgr.empty()) {
        annotate_resolved_instances_ = std::move(refreshed.resolved_objects);
      }
      live_preview_error_.clear();
    } else {
      annotate_save_error_ = std::move(refreshed.error_message);
      live_preview_error_ = annotate_save_error_;
    }
    return;
  }

  const AnnotationFrame frame_snapshot = *frame_ptr;
  const AnnotationCategories categories_snapshot = annotate_categories_;
  const AnnotationDocumentSnapshot document_snapshot =
      annotate_document_.snapshot();
  const std::shared_ptr<const AnnotationProjectedScene>
      projected_scene_snapshot =
          resolve_annotation_projected_scene_for_document_generation(
              annotate_workflow_, annotate_document_, annotate_session_,
              annotate_frame_, document_snapshot.generation);
  const std::uint64_t generation =
      begin_annotation_workflow_preview(annotate_workflow_);
  mmltk::runtime::submit_background_task(
      background_executor_, ui_callbacks_,
      [frame_snapshot, categories_snapshot, document_snapshot,
       projected_scene_snapshot]() {
        return prepare_annotation_workflow_preview(
            frame_snapshot, categories_snapshot, document_snapshot,
            projected_scene_snapshot.get());
      },
      [this,
       generation](AnnotationWorkflowPreparedPreview prepared) mutable {
        end_annotation_workflow_preview(annotate_workflow_);
        if (annotation_workflow_preview_generation_matches(annotate_workflow_,
                                                           generation) &&
            annotate_frame_.has_value() &&
            annotate_frame_->frame_id == prepared.frame_id &&
            live_preview_texture_) {
          annotate_resolved_instances_ = prepared.preview.resolved_objects;
          const LiveCaptureRegion region{0U, 0U, prepared.width,
                                         prepared.height};
          std::string preview_error;
          if (!live_preview_texture_->submit_host_bgr(
                  std::move(prepared.preview.preview_bgr), prepared.width,
                  prepared.height, region, prepared.frame_id, &preview_error)) {
            live_preview_error_ = std::move(preview_error);
            log_gui_error("annotate preview error",
                                    live_preview_error_);
          } else {
            live_preview_error_.clear();
            note_annotation_workflow_preview_ready(annotate_workflow_,
                                                   *annotate_frame_);
          }
        }
        if (annotation_workflow_preview_pending(
                annotate_workflow_, annotation_frame_ptr(annotate_frame_))) {
          submit_annotation_preview();
        }
      },
      [this](const std::string &error) {
        annotate_save_error_ = error;
        annotate_resolved_instances_.clear();
        live_preview_error_ = error;
        note_annotation_workflow_preview_error(annotate_workflow_);
        constexpr int kMaxPreviewRetries = 3;
        if (annotation_workflow_preview_can_retry(annotate_workflow_,
                                                  kMaxPreviewRetries) &&
            annotation_workflow_preview_pending(
                annotate_workflow_, annotation_frame_ptr(annotate_frame_))) {
          submit_annotation_preview();
        }
      });
}

bool App::annotation_live_running() const {
  return active_live_mode_ == ActiveLiveMode::Annotate &&
         live_controller_ != nullptr;
}

void App::poll_annotate_work() {
  if (annotation_live_running() && live_preview_texture_) {
    std::optional<AnnotationFrame> synced_frame =
        make_annotation_workflow_live_annotation_frame_from_preview(
            annotate_.source, annotate_.full_frame, *live_preview_texture_,
            annotate_frame_);
    if (synced_frame.has_value()) {
      load_annotation_frame(std::move(*synced_frame));
    }
    if (live_controller_) {
      const std::string controller_error = live_controller_->last_error();
      if (!controller_error.empty()) {
        annotate_save_error_ = controller_error;
      }
    }
  }

  const AnnotationWorkflowSaveRequest save_request =
      make_annotation_workflow_save_request(
          &annotate_workflow_, annotate_, annotation_live_running(),
          annotate_save_running_, annotation_frame_ptr(annotate_frame_),
          !annotate_resolved_instances_.empty(), annotate_current_input_index_,
          annotate_inputs_.size());
  const AnnotationWorkflowPollPlan workflow_poll =
      plan_annotation_workflow_poll(save_request);

  drive_annotation_save_pipeline(
      workflow_poll.pipeline_plan,
      [this]() -> std::optional<AnnotationSaveSnapshot> {
        return make_annotation_workflow_current_save_snapshot(
            AnnotationWorkflowCurrentSaveSnapshotRequest{
                &annotate_workflow_,
                &annotate_document_,
                &annotate_session_,
                &annotate_frame_,
                &annotate_categories_,
                &annotate_resolved_instances_,
                true,
                true,
                &annotate_save_error_,
                "annotate hold-save resolve error",
            },
            [this](std::string *error_message) {
              return ensure_annotation_workflow_live_annotation_frame_pixels(
                  annotate_.source, annotate_frame_, annotation_live_running(),
                  live_controller_.get(), annotate_.full_frame, error_message);
            },
            [this](const char *error_context, const std::string &error) {
              log_gui_error(
                  error_context != nullptr ? error_context
                                           : "annotate resolve error",
                  error);
            });
      },
      [this](AnnotationSaveSnapshot save_snapshot) {
        queue_annotation_workflow_save(
            annotate_workflow_, std::move(save_snapshot.frame),
            std::move(save_snapshot.objects),
            std::move(save_snapshot.projected_scene));
      },
      [this](AnnotationSaveSnapshot save_snapshot) {
        sync_annotation_categories();
        (void)launch_annotation_workflow_save(
            annotate_workflow_, annotate_, annotate_categories_,
            std::move(save_snapshot), true,
            AnnotationWorkflowSaveLaunchState{&annotate_save_running_,
                                              &annotate_save_summary_,
                                              &annotate_save_error_,
                                              &annotate_categories_loaded_,
                                              &annotate_categories_output_dir_},
            background_executor_, ui_callbacks_,
            [this](const std::string &error) {
              log_gui_error("annotate save error", error);
            });
      },
      [this]() { clear_annotation_workflow_save_queue(annotate_workflow_); },
      [this]() {
        return take_annotation_workflow_queued_save(annotate_workflow_);
      },
      [this](AnnotationQueuedSave queued_save) {
        sync_annotation_categories();
        (void)launch_annotation_workflow_save(
            annotate_workflow_, annotate_, annotate_categories_,
            AnnotationSaveSnapshot{
                std::move(queued_save.frame),
                std::move(queued_save.objects),
                std::move(queued_save.projected_scene),
            },
            true,
            AnnotationWorkflowSaveLaunchState{&annotate_save_running_,
                                              &annotate_save_summary_,
                                              &annotate_save_error_,
                                              &annotate_categories_loaded_,
                                              &annotate_categories_output_dir_},
            background_executor_, ui_callbacks_,
            [this](const std::string &error) {
              log_gui_error("annotate save error", error);
            });
      },
      [this]() {
        mark_annotation_hold_save_overflow(annotate_workflow_);
        annotate_save_error_ =
            "Hold Save overflowed the single-frame writer queue. Capture "
            "paused to avoid dropping frames silently.";
        log_gui_error("annotate hold-save overflow",
                                annotate_save_error_);
      });
}

std::vector<RemoteGpuFamily> App::selected_remote_gpu_families() const {
  std::vector<RemoteGpuFamily> families;
  for (int index = 0;
       index < static_cast<int>(train_.remote_family_enabled.size()); ++index) {
    if (!train_.remote_family_enabled[static_cast<size_t>(index)]) {
      continue;
    }
    families.push_back(static_cast<RemoteGpuFamily>(index));
  }
  return families;
}

void App::render() {
  draw_menu_bar();
  draw_workflow_window();
  draw_settings_window();
}

bool App::live_predict_running() const {
  return live_predict_controller_.running();
}

bool App::live_predict_active() const {
  return live_predict_controller_.active();
}

bool App::has_static_preview() const {
  const LivePreviewTextureState preview_state =
      snapshot_preview_state(live_preview_texture_);
  return !live_predict_running() && preview_state.has_frame &&
         !live_static_preview_source_name_.empty();
}

void App::clear_static_preview() {
  if (live_predict_running()) {
    return;
  }
  if (live_preview_texture_) {
    live_preview_texture_->clear_frame();
  }
  live_static_preview_source_name_.clear();
  live_predict_controller_.clear_errors();
  live_preview_error_.clear();
}

void App::apply_static_preview(StillImagePreview preview) {
  if (live_predict_running()) {
    throw std::runtime_error(
        "cannot apply a static preview while live predict is running");
  }
  if (!live_preview_texture_) {
    throw std::runtime_error("preview uploader is not available");
  }

  const LiveCaptureRegion region{0U, 0U, preview.width, preview.height};
  std::string preview_error;
  if (!live_preview_texture_->submit_host_bgr(std::move(preview.pixels_bgr),
                                              preview.width, preview.height,
                                              region, 1U, &preview_error)) {
    live_preview_error_ = std::move(preview_error);
    log_gui_error("preview error", live_preview_error_);
    throw std::runtime_error(live_preview_error_.empty()
                                 ? "failed to upload static preview"
                                 : live_preview_error_);
  }

  live_static_preview_source_name_ = preview.source_name;
  live_predict_controller_.clear_errors();
  live_preview_error_.clear();
}

void App::launch_live_predict_session() {
  if (live_predict_running() || shutting_down_) {
    return;
  }

  clear_static_preview();
  live_predict_controller_.clear_errors();
  live_preview_error_.clear();
  live_predict_controller_.launch(
      background_executor_, ui_callbacks_, predict_, selected_preset_name_,
      [this](const int device_id) {
        active_live_mode_ = ActiveLiveMode::Predict;
        if (live_preview_texture_ && live_predict_controller_.controller()) {
          live_preview_texture_->clear_frame();
          live_preview_texture_->begin_live_stream(
              *live_predict_controller_.controller(), device_id);
        }
        current_view_ = View::Live;
      },
      [this](const std::string &error) {
        log_gui_error("live start error", error);
      });
}

void App::stop_live_predict_session() {
  if (live_predict_controller_.starting()) {
    live_predict_controller_.stop(background_executor_, ui_callbacks_,
                                  &predict_.source);
  }
  if (live_predict_controller_.stopping()) {
    return;
  }
  if (active_live_mode_ != ActiveLiveMode::Predict ||
      live_predict_controller_.controller() == nullptr) {
    if (!live_predict_running() && active_live_mode_ == ActiveLiveMode::None &&
        live_preview_texture_) {
      live_preview_texture_->clear_frame();
    }
    live_static_preview_source_name_.clear();
    live_preview_error_.clear();
    live_predict_controller_.clear_errors();
    live_crop_overlay_mode_ = false;
    live_crop_drag_session_ = {};
    return;
  }

  live_static_preview_source_name_.clear();
  live_crop_overlay_mode_ = false;
  live_crop_drag_session_ = {};
  live_predict_controller_.stop(
      background_executor_, ui_callbacks_, &predict_.source,
      [this]() {
        active_live_mode_ = ActiveLiveMode::None;
        if (live_preview_texture_) {
          live_preview_texture_->end_live_stream();
        }
      },
      [this]() {
        if (live_preview_texture_) {
          live_preview_texture_->clear_frame();
        }
        live_preview_error_.clear();
      },
      [this](const std::string &error) {
        if (live_preview_texture_) {
          live_preview_texture_->clear_frame();
        }
        log_gui_error("live stop error", error);
      });
}

void App::draw_live_preview_panel() {
  const LivePreviewTextureState preview_state =
      snapshot_preview_state(live_preview_texture_);
  const bool static_preview_visible = !live_predict_running() &&
                                      preview_state.has_frame &&
                                      !live_static_preview_source_name_.empty();
  if (!live_predict_running() && !static_preview_visible &&
      live_preview_error_.empty()) {
    return;
  }

  draw_section_heading("Live Preview");

  if (!live_preview_error_.empty()) {
    draw_banner("live_preview_error_banner", ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                ImVec4(0.98f, 0.90f, 0.82f, 1.00f), live_preview_error_);
    ImGui::Spacing();
  }
  if (!live_predict_controller_.action_error().empty()) {
    draw_banner("live_predict_action_error_banner",
                ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                live_predict_controller_.action_error());
    ImGui::Spacing();
  }

  if (!preview_state.has_frame) {
    ImGui::BeginChild(
        "live_preview_panel",
        ImVec2(0.0f, live_preview_fit_to_capture_ ? 0.0f : 420.0f), true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextUnformatted(live_predict_running()
                               ? "Waiting for preview frame..."
                               : "No preview frame loaded.");
    ImGui::EndChild();
    return;
  }

  const LiveCaptureRegion texture_region = preview_state.displayed_region;
  LiveCaptureRegion display_region = texture_region;
  ImVec2 uv0 = LivePreviewTexture::uv0();
  ImVec2 uv1 = LivePreviewTexture::uv1();
  const bool can_crop_live_preview =
      active_live_mode_ == ActiveLiveMode::Predict &&
      live_predict_controller_.controller() != nullptr &&
      predict_.source.kind == SourceKind::VideoStream &&
      texture_region.width > 0U && texture_region.height > 0U;
  if (can_crop_live_preview && !live_crop_overlay_mode_) {
    const AnnotationBox crop_box = active_live_crop_box();
    const int tex_x1 = static_cast<int>(texture_region.x);
    const int tex_y1 = static_cast<int>(texture_region.y);
    const int tex_x2 = tex_x1 + static_cast<int>(texture_region.width);
    const int tex_y2 = tex_y1 + static_cast<int>(texture_region.height);
    const int crop_x1 =
        std::clamp(crop_box.x1, tex_x1, std::max(tex_x1, tex_x2 - 1));
    const int crop_y1 =
        std::clamp(crop_box.y1, tex_y1, std::max(tex_y1, tex_y2 - 1));
    const int crop_x2 = std::clamp(crop_box.x2, crop_x1 + 1, tex_x2);
    const int crop_y2 = std::clamp(crop_box.y2, crop_y1 + 1, tex_y2);
    display_region = LiveCaptureRegion{
        static_cast<std::uint32_t>(crop_x1),
        static_cast<std::uint32_t>(crop_y1),
        static_cast<std::uint32_t>(std::max(1, crop_x2 - crop_x1)),
        static_cast<std::uint32_t>(std::max(1, crop_y2 - crop_y1)),
    };
    const float inv_width = 1.0f / static_cast<float>(texture_region.width);
    const float inv_height = 1.0f / static_cast<float>(texture_region.height);
    uv0 = ImVec2(
        static_cast<float>(display_region.x - texture_region.x) * inv_width,
        static_cast<float>(display_region.y - texture_region.y) * inv_height);
    uv1 = ImVec2(static_cast<float>(display_region.x + display_region.width -
                                    texture_region.x) *
                     inv_width,
                 static_cast<float>(display_region.y + display_region.height -
                                    texture_region.y) *
                     inv_height);
  }
  const auto width = static_cast<float>(display_region.width);
  const auto height = static_cast<float>(display_region.height);

  ImGui::BeginChild(
      "live_preview_panel",
      ImVec2(0.0f, live_preview_fit_to_capture_ ? 0.0f : 420.0f), true,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::Text("Frame: %llu",
              static_cast<unsigned long long>(preview_state.last_frame_id));
  if (predict_.source.kind == SourceKind::VideoStream &&
      active_live_mode_ == ActiveLiveMode::Predict &&
      live_predict_controller_.controller() != nullptr) {
    const AnnotationBox active_box = active_live_crop_box();
#if MMLTK_RFDETR_LIVE_CAPTURE
    const AnnotationBox committed_box = runtime_crop_box_for_ui_state(
        live_predict_controller_.controller()->ui_crop_state(), predict_.source);
#else
    const AnnotationBox committed_box =
        resolved_video_crop_box(predict_.source);
#endif
    ImGui::TextUnformatted(live_crop_overlay_mode_ ? "Display: full frame"
                                                   : "Display: crop");
    ImGui::Text("Crop: x=%d y=%d w=%d h=%d", active_box.x1, active_box.y1,
                std::max(0, active_box.x2 - active_box.x1),
                std::max(0, active_box.y2 - active_box.y1));
    if (live_crop_overlay_mode_ &&
        !annotation_boxes_equal(active_box, committed_box)) {
      ImGui::TextDisabled("Committed: x=%d y=%d w=%d h=%d", committed_box.x1,
                          committed_box.y1,
                          std::max(0, committed_box.x2 - committed_box.x1),
                          std::max(0, committed_box.y2 - committed_box.y1));
    }
  } else {
    ImGui::Text("ROI: x=%u y=%u w=%u h=%u", display_region.x, display_region.y,
                display_region.width, display_region.height);
  }
  ImGui::Separator();

  ImVec2 image_size(width, height);
  if (!live_preview_fit_to_capture_) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float scale = std::min(available.x / width, available.y / height);
    const float clamped_scale = scale > 0.0f ? scale : 1.0f;
    image_size = ImVec2(width * clamped_scale, height * clamped_scale);
    const ImVec2 cursor = ImGui::GetCursorPos();
    const float offset_x = std::max(0.0f, (available.x - image_size.x) * 0.5f);
    const float offset_y = std::max(0.0f, (available.y - image_size.y) * 0.5f);
    ImGui::SetCursorPos(ImVec2(cursor.x + offset_x, cursor.y + offset_y));
  }
  const ImVec2 image_origin = ImGui::GetCursorScreenPos();
  ImGui::Image(preview_state.texture_id, image_size, uv0, uv1);
  if (live_crop_overlay_mode_ && active_live_mode_ == ActiveLiveMode::Predict &&
      live_predict_controller_.controller() != nullptr &&
      predict_.source.kind == SourceKind::VideoStream) {
    draw_crop_overlay(image_origin.x, image_origin.y, image_size.x,
                      image_size.y, static_cast<float>(texture_region.width),
                      static_cast<float>(texture_region.height));
  }
  ImGui::EndChild();
}

AnnotationBox App::active_live_crop_box() const {
  if (live_crop_drag_session_.active) {
    return live_crop_drag_session_.draft_box;
  }
#if MMLTK_RFDETR_LIVE_CAPTURE
  if (active_live_mode_ == ActiveLiveMode::Predict &&
      live_predict_controller_.controller() != nullptr) {
    return runtime_crop_box_for_ui_state(
        live_predict_controller_.controller()->ui_crop_state(), predict_.source);
  }
#endif
  return resolved_video_crop_box(predict_.source);
}

void App::draw_crop_overlay(float img_ox, float img_oy, float img_w,
                            float img_h, float capture_w, float capture_h) {
  if (capture_w <= 0.0f || capture_h <= 0.0f) {
    return;
  }

  ImGui::SetCursorScreenPos(ImVec2(img_ox, img_oy));
  ImGui::InvisibleButton("crop_overlay_hit", ImVec2(img_w, img_h));
  const bool overlay_hovered = ImGui::IsItemHovered();

  const CanvasViewport viewport = make_canvas_viewport(
      img_ox, img_oy, img_w, img_h,
      static_cast<std::uint32_t>(std::max(0.0f, capture_w)),
      static_cast<std::uint32_t>(std::max(0.0f, capture_h)));
  const ImVec2 mouse = ImGui::GetMousePos();
  const CanvasPointerState pointer{
      mouse.x,
      mouse.y,
      overlay_hovered,
      false,
      ImGui::IsMouseDown(ImGuiMouseButton_Left),
  };
  const RectDragKind hover_kind =
      overlay_hovered
          ? rectangle_hover_kind_with_options(
                pointer, viewport, active_live_crop_box(), true,
                crop_edge_hit_half_width_px(), crop_corner_hit_size_px())
          : RectDragKind::None;
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
      hover_kind != RectDragKind::None) {
    start_preview_rect_drag(live_crop_drag_session_, hover_kind, mouse.x,
                            mouse.y, active_live_crop_box(), 32);
  }
  const PreviewRectDragResult interaction = update_preview_rect_drag(
      live_crop_drag_session_, ImGui::IsMouseDown(ImGuiMouseButton_Left),
      mouse.x, mouse.y, viewport, static_cast<int>(capture_w),
      static_cast<int>(capture_h), 32);
#if MMLTK_RFDETR_LIVE_CAPTURE
  if (live_predict_controller_.controller() != nullptr &&
      (interaction.changed || interaction.commit)) {
    publish_runtime_crop_box(
        live_predict_controller_.controller()->ui_crop_state(), predict_.source,
        interaction.box);
  }
  if (interaction.commit) {
#endif
    persist_crop_box_to_source(interaction.box, &predict_.source);
  }

  const RectDragKind cursor_kind = live_crop_drag_session_.active
                                       ? live_crop_drag_session_.drag.kind
                                       : hover_kind;
  set_rectangle_drag_cursor(cursor_kind);

  if (live_interaction_overlay_ != nullptr) {
    PreviewInteractionOverlaySnapshot snapshot;
    snapshot.width = static_cast<std::uint32_t>(std::max(0.0f, capture_w));
    snapshot.height = static_cast<std::uint32_t>(std::max(0.0f, capture_h));
    snapshot.cuda_device_index = predict_.device_id;
    const int crop_handle_radius =
        crop_handle_radius_pixels(img_w, img_h, capture_w, capture_h);
    snapshot.boxes.push_back(PreviewInteractionOverlayBox{
        active_live_crop_box(),
        255U,
        255U,
        255U,
        kWhiteCropStrokeWidth,
        true,
        crop_handle_radius,
    });
    live_interaction_overlay_->publish_snapshot(std::move(snapshot));

    const PreviewInteractionOverlayState overlay_state =
        live_interaction_overlay_->snapshot();
    if (overlay_state.has_frame &&
        overlay_state.width ==
            static_cast<std::uint32_t>(std::max(0.0f, capture_w)) &&
        overlay_state.height ==
            static_cast<std::uint32_t>(std::max(0.0f, capture_h))) {
      ImGui::GetWindowDrawList()->AddImage(
          overlay_state.texture_id, ImVec2(img_ox, img_oy),
          ImVec2(img_ox + img_w, img_oy + img_h));
    }
  }
}

void App::draw_preset_selector() {
  draw_section_heading("Model Config");
  const auto &active_preset = resolve_selected_preset(selected_preset_name_);
  const auto *active_module =
      model_registry_.find_module_for_preset(selected_preset_name_);
  const std::vector<mmltk::runtime::ModelPresetDescriptor> available_presets =
      model_registry_.presets();
  const bool compact_layout = ImGui::GetContentRegionAvail().x < 520.0f;

  if (active_module != nullptr) {
    ImGui::Text("Module: %s",
                std::string(active_module->display_name()).c_str());
  }

  draw_labeled_combo("Preset", selected_preset_name_.c_str(), 240.0f, [&]() {
    for (const auto &preset : available_presets) {
      const bool selected = preset.preset_name == selected_preset_name_;
      if (ImGui::Selectable(preset.display_name.c_str(), selected)) {
        selected_preset_name_ = preset.preset_name;
        apply_selected_preset_defaults();
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
  });

  ImGui::TextWrapped(
      "Canonical Weights: %s",
      std::string(active_preset.canonical_weight_filename).c_str());
  ImGui::Text("Resolution: %d", active_preset.resolution);
  if (!compact_layout) {
    ImGui::SameLine();
  }
  ImGui::Text("Queries: %d", active_preset.num_queries);
  if (!compact_layout) {
    ImGui::SameLine();
  }
  ImGui::Text("Max Dets: %d", active_preset.num_select);
}

void App::apply_selected_preset_defaults() {
  const auto &preset = resolve_selected_preset(selected_preset_name_);
  train_.eval_max_dets = preset.num_select;
  apply_train_recipe(
      train_, current_train_recipe(selected_preset_name_, train_.optimizer),
      train_.recipe_overrides);
  validate_.resolution = preset.resolution;
  validate_.eval_max_dets = preset.num_select;
  predict_.max_dets_per_image = preset.num_select;
  annotate_.max_dets_per_image = preset.num_select;
}

void App::draw_menu_bar() {
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }
  for (const View view : {View::Train, View::Validate, View::Predict,
                          View::Annotate, View::Export}) {
    const bool selected = current_view_ == view;
    if (ImGui::MenuItem(view_label(view), nullptr, selected)) {
      current_view_ = view;
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  const bool live_active = live_predict_active();
  ImGui::PushStyleColor(ImGuiCol_Text,
                        live_active ? ImVec4(0.91f, 0.24f, 0.20f, 1.00f)
                                    : ImVec4(0.47f, 0.49f, 0.52f, 1.00f));
  if (ImGui::MenuItem("LIVE", nullptr, current_view_ == View::Live)) {
    current_view_ = View::Live;
  }
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (ImGui::MenuItem("SETTINGS", nullptr, ui_settings_window_open_)) {
    ui_settings_window_open_ = !ui_settings_window_open_;
  }
  ImGui::EndMainMenuBar();
}

void App::draw_settings_window() {
  if (!ui_settings_window_open_) {
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(ui_scaled(520.0f), ui_scaled(420.0f)),
                           ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Settings", &ui_settings_window_open_)) {
    ImGui::End();
    return;
  }

  bool changed = false;
  draw_section_heading("Typography");
  draw_labeled_widget("UI Scale", 180.0f, [&]() {
    changed |= ImGui::SliderFloat("##value", &ui_settings_.ui_scale, 0.85f,
                                  1.75f, "%.2fx");
  });
  draw_labeled_widget("Primary Font", 180.0f, [&]() {
    changed |= ImGui::SliderFloat("##value", &ui_settings_.font_size, 13.0f,
                                  28.0f, "%.1f px");
  });
  draw_labeled_widget("Secondary Font", 180.0f, [&]() {
    changed |= ImGui::SliderFloat("##value", &ui_settings_.secondary_font_size,
                                  12.0f, 24.0f, "%.1f px");
  });
  draw_labeled_widget("Mono Font", 180.0f, [&]() {
    changed |= ImGui::SliderFloat("##value", &ui_settings_.mono_font_size,
                                  12.0f, 24.0f, "%.1f px");
  });

  draw_section_heading("Layout");
  draw_labeled_widget("Density", 180.0f, [&]() {
    if (ImGui::BeginCombo("##value", ui_density_label(ui_settings_.density))) {
      for (const UiDensity option :
           {UiDensity::Compact, UiDensity::Balanced, UiDensity::Comfortable}) {
        const bool selected = option == ui_settings_.density;
        if (ImGui::Selectable(ui_density_label(option), selected)) {
          ui_settings_.density = option;
          changed = true;
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
  });
  draw_labeled_widget("Label Width", 180.0f, [&]() {
    changed |= ImGui::SliderFloat("##value", &ui_settings_.property_label_width,
                                  110.0f, 260.0f, "%.0f px");
  });

  draw_section_heading("Crop Interaction");
  draw_labeled_widget("Edge Hit Width", 180.0f, [&]() {
    changed |=
        ImGui::SliderFloat("##value", &ui_settings_.crop_edge_hit_half_width,
                           4.0f, 16.0f, "%.1f px");
  });
  draw_labeled_widget("Corner Hit Size", 180.0f, [&]() {
    changed |= ImGui::SliderFloat("##value", &ui_settings_.crop_corner_hit_size,
                                  12.0f, 32.0f, "%.1f px");
  });
  draw_labeled_widget("Handle Radius", 180.0f, [&]() {
    changed |= ImGui::SliderFloat("##value", &ui_settings_.crop_handle_radius,
                                  3.0f, 12.0f, "%.1f px");
  });

  draw_section_heading("Preview");
  draw_with_optional_font(compact_font_, []() {
    ImGui::TextWrapped("This window persists UI scale, font sizes, density, "
                       "crop interaction sizes, and property label width in "
                       "gui.json. Changes apply on the next frame.");
  });
  draw_with_optional_font(mono_font_, []() {
    ImGui::TextUnformatted("SourceSansPro-Regular / SourceCodePro-Semibold");
  });

  if (changed) {
    ui_settings_apply_pending_ = true;
  }

  if (ImGui::Button("Reset UI")) {
    const float saved_content_scale = ui_settings_.content_scale;
    ui_settings_ = UiSettingsState{};
    ui_settings_.content_scale = saved_content_scale;
    ui_settings_apply_pending_ = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Close")) {
    ui_settings_window_open_ = false;
  }

  ImGui::End();
}

void App::draw_workflow_window() {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  constexpr ImGuiWindowFlags kShellFlags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings;
  ImGui::Begin("RF-DETR Studio Shell", nullptr, kShellFlags);

  const ImVec2 available = ImGui::GetContentRegionAvail();
  if (ImGui::BeginTable("studio_shell", 2,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_BordersInnerV,
                        available)) {
    ImGui::TableSetupColumn("workspace", ImGuiTableColumnFlags_WidthStretch,
                            3.0f);
    ImGui::TableSetupColumn("sidebar", ImGuiTableColumnFlags_WidthStretch,
                            1.0f);
    ImGui::TableNextRow(ImGuiTableRowFlags_None, available.y);

    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("workspace_pane", ImVec2(0.0f, 0.0f), false);
    ImGui::Text("Workflow: %s", view_label(current_view_));
    ImGui::Separator();
    if (current_view_ != View::Annotate) {
      draw_preset_selector();
    }
    switch (current_view_) {
    case View::Train:
      draw_train_view();
      break;
    case View::Validate:
      draw_validate_view();
      break;
    case View::Predict:
      draw_predict_view();
      break;
    case View::Annotate:
      draw_annotate_workspace();
      break;
    case View::Export:
      draw_export_view();
      break;
    case View::Live:
      draw_live_view();
      break;
    }
    ImGui::EndChild();

    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("sidebar_pane", ImVec2(0.0f, 0.0f), false);
    if (current_view_ == View::Annotate) {
      draw_annotate_utilities_pane();
    } else {
      draw_info_pane();
    }
    ImGui::EndChild();

    ImGui::EndTable();
  }
  ImGui::End();
}

void App::draw_info_pane() {
  const LivePreviewTextureState preview_state =
      snapshot_preview_state(live_preview_texture_);
  const bool static_preview_visible = !live_predict_running() &&
                                      preview_state.has_frame &&
                                      !live_static_preview_source_name_.empty();
  const std::vector<int> selected_devices =
      local_train_controller_.selected_device_ids();
  const std::vector<RemoteGpuFamily> selected_families =
      selected_remote_gpu_families();
  const std::string job_output = snapshot_job_output();
  std::string selected_device_ids_summary;
  if (!selected_devices.empty()) {
    std::ostringstream stream;
    for (size_t index = 0; index < selected_devices.size(); ++index) {
      if (index > 0) {
        stream << ", ";
      }
      stream << selected_devices[index];
    }
    selected_device_ids_summary = stream.str();
  }
  const std::string local_gpu_summary =
      train_.execution_target == TrainExecutionTarget::Local
          ? local_gpu_selection_summary(local_train_controller_.gpus(),
                                        local_train_controller_.gpu_selection())
          : std::string();
  const std::string remote_gpu_family_summary =
      train_.execution_target == TrainExecutionTarget::Remote
          ? summarize_selected_remote_gpu_families(selected_families)
          : std::string();
  const std::string armed_offer_summary =
      vast_query_controller_.armed_offer_summary();
  const VastQueryState &vast_query_state = vast_query_controller_.state();
  const LocalTrainSessionState &train_state =
      local_train_controller_.session_state();
  const TrainingActivityViewModel training_view_model{
      train_execution_target_label(train_.execution_target),
      local_gpu_summary,
      selected_device_ids_summary,
      local_train_controller_.gpu_error(),
      remote_gpu_family_summary,
      !vast_api_key_.empty(),
      armed_offer_summary,
      vast_query_state.running,
      vast_query_state.last_summary,
      vast_query_state.last_error,
  };
  draw_training_activity_section(training_view_model);
  draw_local_train_activity_section(
      train_state,
      [this](bool force) { local_train_controller_.request_stop(force); },
      compact_font_);
  const JobActivityViewModel job_view_model{
      job_.running,      job_.label,       job_.last_summary,
      job_.last_error,   job_output,       picker_error_,
  };
  draw_job_activity_section(job_view_model, compact_font_);

  if (current_view_ == View::Annotate) {
    const AnnotateActivityViewModel annotate_view_model{
        annotate_frame_.has_value(),
        annotate_frame_.has_value() ? std::string_view(annotate_frame_->source_name)
                                    : std::string_view(),
        annotate_document_.size(),
        annotate_resolved_instances_.size(),
        annotate_assist_running_,
        annotate_assist_summary_,
        annotate_save_running_,
        annotate_save_summary_,
        annotation_live_running(),
    };
    draw_annotate_activity_section(annotate_view_model, compact_font_);
  }

  LivePredictActivityViewModel live_view_model;
  live_view_model.show_running_section =
      active_live_mode_ == ActiveLiveMode::Predict &&
      live_predict_controller_.status() != nullptr;
  live_view_model.show_static_preview = static_preview_visible;
  live_view_model.preview_has_frame = preview_state.has_frame;
  live_view_model.preview_frame_id = preview_state.last_frame_id;
  live_view_model.preview_width = preview_state.displayed_region.width;
  live_view_model.preview_height = preview_state.displayed_region.height;
  live_view_model.static_preview_source_name = live_static_preview_source_name_;
  live_view_model.start_error = live_predict_controller_.start_error();
  live_view_model.action_error = live_predict_controller_.action_error();
  live_view_model.preview_error = live_preview_error_;
  live_view_model.show_idle_start_error =
      !live_predict_running() && !live_predict_controller_.start_error().empty();
  if (live_view_model.show_running_section) {
    const auto &live_status = *live_predict_controller_.status();
    live_view_model.controller_running = live_status.running;
    live_view_model.analyzer_model_hot = live_status.analyzer.model_hot;
    live_view_model.analyzer_running = live_status.analyzer.running;
    live_view_model.frames_analyzed = live_status.analyzer.frames_analyzed;
    live_view_model.frames_skipped = live_status.analyzer.frames_skipped;
    live_view_model.frames_composited =
        live_status.compositor.frames_composited;
    live_view_model.last_latency_ms = live_status.analyzer.last_latency_ms;
    live_view_model.analyzer_backend_name = live_status.analyzer.backend_name;
    live_view_model.last_error = live_status.last_error;
  }
  draw_live_predict_activity_section(live_view_model, compact_font_);

  draw_section_heading("Source Notes");
  if (current_view_ == View::Predict) {
    draw_with_optional_font(compact_font_, [this]() {
      ImGui::TextWrapped("%s", describe_source(predict_.source).c_str());
    });
    const std::string source_error = validate_predict_source(predict_.source);
    if (!source_error.empty()) {
      ImGui::Spacing();
      draw_with_optional_font(compact_font_, [&source_error]() {
        ImGui::TextWrapped("%s", source_error.c_str());
      });
    } else if (predict_.source.kind == SourceKind::VideoStream) {
      ImGui::Spacing();
      if (live_capture_supported()) {
        draw_with_optional_font(compact_font_, []() {
          ImGui::TextWrapped("Video device predict uses the public RF-DETR "
                             "live session. The model stays hot and "
                             "runs against the newest CUDA frame without "
                             "saving frames to disk.");
        });
      } else {
        draw_with_optional_font(compact_font_, []() {
          ImGui::TextWrapped(
              "RF-DETR live video predict is not available in this build.");
        });
      }
    } else {
      ImGui::Spacing();
      draw_with_optional_font(compact_font_, []() {
        ImGui::TextWrapped(
            "Predict accepts compiled datasets directly. Single-image predict "
            "opens an annotated preview in LIVE, "
            "and folder predict resolves into the public RF-DETR image-file "
            "workflow before inference.");
      });
    }
    const std::string combo_error = predict_combo_error(predict_);
    if (!combo_error.empty()) {
      ImGui::Spacing();
      draw_banner("predict_combo_activity_banner",
                  ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                  ImVec4(0.98f, 0.90f, 0.82f, 1.00f), combo_error);
    }
  } else if (current_view_ == View::Annotate) {
    draw_with_optional_font(compact_font_, [this]() {
      ImGui::TextWrapped("%s", describe_source(annotate_.source).c_str());
    });
    ImGui::Spacing();
    draw_with_optional_font(compact_font_, []() {
      ImGui::TextWrapped("%s", annotate_error().c_str());
    });
    if (annotate_frame_.has_value()) {
      ImGui::Spacing();
      draw_with_optional_font(compact_font_, [this]() {
        ImGui::TextWrapped("Frame: %s", annotate_frame_->source_name.c_str());
      });
      ImGui::Text("Size: %u x %u", annotate_frame_->width,
                  annotate_frame_->height);
      ImGui::Text("Objects: %zu", annotate_document_.size());
      ImGui::Text("Resolved Masks: %zu", annotate_resolved_instances_.size());
    }
    if (annotation_live_running()) {
      ImGui::Spacing();
      ImGui::TextUnformatted(
          "Live annotate is running against a raw capture feed.");
    }
  } else if (current_view_ == View::Train) {
    if (train_.execution_target == TrainExecutionTarget::Local) {
      draw_with_optional_font(compact_font_, []() {
        ImGui::TextWrapped(
            "Local train now runs through the sibling mmltk binary so the GUI "
            "can drive both "
            "single-GPU and multi-GPU launches, stream per-wave progress into "
            "the Train tab, and stop "
            "or force-kill the local subprocess tree when needed.");
      });
    } else {
      draw_with_optional_font(compact_font_, []() {
        ImGui::TextWrapped("Remote train is query-and-arm only right now. "
                           "Query returns the top 2 DLPerf/$ Vast offers "
                           "across the selected GPU families, and selecting "
                           "one arms it for the future remote launch path.");
      });
    }
  } else {
    draw_with_optional_font(compact_font_, []() {
      ImGui::TextWrapped("Train and validate map directly onto the public "
                         "RF-DETR compiled-dataset workflows.");
    });
  }
}

void App::draw_train_view() {
  const VastQueryState &vast_query_state = vast_query_controller_.state();
  const LocalTrainSessionState &train_state =
      local_train_controller_.session_state();
  const bool block_actions =
      job_.running || live_predict_running() || train_state.running;
  draw_section_heading("Datasets");
  draw_full_width_input("Train Compiled", train_.train_compiled_path);
  draw_full_width_input("Val Compiled", train_.val_compiled_path);
  draw_full_width_input("Test Compiled", train_.test_compiled_path);
  draw_full_width_input("Output Dir", train_.output_dir);

  draw_section_heading("Initialization");
  int input_mode =
      train_.input_mode == TrainInputMode::Resume
          ? static_cast<int>(TrainInputMode::Resume)
          : static_cast<int>(TrainInputMode::Weights);
  if (ImGui::RadioButton(
          "Weights", input_mode == static_cast<int>(TrainInputMode::Weights))) {
    input_mode = static_cast<int>(TrainInputMode::Weights);
  }
  ImGui::SameLine();
  if (ImGui::RadioButton(
          "Resume", input_mode == static_cast<int>(TrainInputMode::Resume))) {
    input_mode = static_cast<int>(TrainInputMode::Resume);
  }
  train_.input_mode = input_mode == static_cast<int>(TrainInputMode::Resume)
                          ? TrainInputMode::Resume
                          : TrainInputMode::Weights;
  if (train_.input_mode == TrainInputMode::Weights) {
    if (draw_file_picker_input(
            "Weights Path", train_.weights_path,
            file_picker_busy(FileBrowseField::TrainWeights))) {
      launch_file_picker(FileBrowseField::TrainWeights, "Select Weights",
                         &train_.weights_path);
    }
  } else {
    if (draw_file_picker_input(
            "Resume Path", train_.resume_path,
            file_picker_busy(FileBrowseField::TrainResume))) {
      launch_file_picker(FileBrowseField::TrainResume, "Select Checkpoint",
                         &train_.resume_path);
    }
  }

  draw_section_heading("Execution");
  draw_labeled_combo(
      "Train Target", train_execution_target_label(train_.execution_target),
      140.0f, [&]() {
        for (const TrainExecutionTarget option :
             {TrainExecutionTarget::Local, TrainExecutionTarget::Remote}) {
          const bool selected = option == train_.execution_target;
          if (ImGui::Selectable(train_execution_target_label(option),
                                selected)) {
            train_.execution_target = option;
          }
          if (selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
      });
  const bool local_target =
      train_.execution_target == TrainExecutionTarget::Local;

  if (local_target) {
    const std::vector<int> selected_devices =
        local_train_controller_.selected_device_ids();
    const std::string local_preview =
        local_gpu_selection_summary(local_train_controller_.gpus(),
                                    local_train_controller_.gpu_selection());
    draw_labeled_combo("Local GPUs", local_preview.c_str(), 320.0f, [&]() {
      const auto &local_gpus = local_train_controller_.gpus();
      const auto &gpu_selection = local_train_controller_.gpu_selection();
      for (size_t index = 0; index < local_gpus.size(); ++index) {
        const std::string label = local_gpu_label(local_gpus[index]);
        const bool selected =
            index < gpu_selection.size() ? gpu_selection[index] : false;
        bool enabled = selected;
        if (ImGui::Checkbox(label.c_str(), &enabled)) {
          local_train_controller_.set_device_selected(index, enabled);
          train_.local_device_ids =
              local_train_controller_.selected_device_ids();
        }
      }
    });
    ImGui::BeginDisabled(local_train_controller_.gpu_refresh_running());
    if (ImGui::Button(local_train_controller_.gpu_refresh_running()
                          ? "Refreshing GPUs..."
                          : "Refresh Visible GPUs")) {
      local_train_controller_.refresh_visible_gpus(train_.local_device_ids);
    }
    ImGui::EndDisabled();
    if (!local_train_controller_.gpu_error().empty()) {
      ImGui::Spacing();
      draw_banner("train_local_gpu_error_banner",
                  ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                  ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                  local_train_controller_.gpu_error());
    } else if (local_train_controller_.gpus().empty()) {
      ImGui::Spacing();
      draw_banner("train_local_gpu_missing_banner",
                  ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                  ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                  "No visible CUDA GPUs were found for local training.");
    } else if (selected_devices.empty()) {
      ImGui::Spacing();
      draw_banner("train_local_gpu_select_banner",
                  ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                  ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                  "Select at least one local GPU before running training.");
    }
  } else {
    const std::vector<RemoteGpuFamily> selected_families =
        selected_remote_gpu_families();
    const std::string remote_preview =
        summarize_selected_remote_gpu_families(selected_families);
    draw_labeled_combo(
        "Remote Families", remote_preview.c_str(), 220.0f, [&]() {
          for (const RemoteGpuFamily family : kRemoteGpuFamilyOrder) {
            const auto index = static_cast<size_t>(family);
            ImGui::Checkbox(remote_gpu_family_label(family),
                            &train_.remote_family_enabled[index]);
          }
        });
    const bool can_query = !block_actions && !vast_query_state.running &&
                           !vast_api_key_.empty() && !selected_families.empty();
    ImGui::BeginDisabled(!can_query);
    if (ImGui::Button(vast_query_state.running ? "Querying..." : "Query")) {
      vast_query_controller_.launch(vast_api_key_, selected_families);
    }
    ImGui::EndDisabled();
    if (vast_query_state.running) {
      ImGui::SameLine();
      ImGui::TextUnformatted("Fetching top DLPerf/$ offers...");
    }
    if (vast_api_key_.empty()) {
      ImGui::Spacing();
      draw_banner("train_remote_key_banner", ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                  ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                  "Remote query requires --vast-api-key or VAST_API_KEY.");
    } else if (selected_families.empty()) {
      ImGui::Spacing();
      draw_banner(
          "train_remote_family_banner", ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
          ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
          "Select at least one remote GPU family before querying Vast.");
    }
    if (!vast_query_state.last_error.empty()) {
      ImGui::Spacing();
      draw_banner("train_remote_query_error_banner",
                  ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                  ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                  vast_query_state.last_error);
    } else if (!vast_query_state.last_summary.empty()) {
      ImGui::Spacing();
      ImGui::TextWrapped("%s", vast_query_state.last_summary.c_str());
    }
    if (!vast_query_state.results.empty()) {
      ImGui::Spacing();
      draw_section_heading("Remote Offers");
      for (const VastOfferSummary &offer : vast_query_state.results) {
        ImGui::PushID(offer.offer_id);
        draw_remote_offer_card(
            offer, vast_query_controller_.armed_offer().has_value() &&
                       vast_query_controller_.armed_offer()->offer_id ==
                           offer.offer_id);
        if (ImGui::IsItemHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          vast_query_controller_.arm_offer(offer);
        }
        ImGui::PopID();
        ImGui::Spacing();
      }
    }
    ImGui::Spacing();
    draw_banner(
        "train_remote_launch_banner", ImVec4(0.20f, 0.24f, 0.18f, 0.95f),
        ImVec4(0.92f, 0.95f, 0.86f, 1.00f),
        vast_query_controller_.armed_offer().has_value()
            ? "Remote offer is armed, but launch is intentionally disabled "
              "until the training container and Vast launch template are "
              "defined."
            : "Remote launch is intentionally disabled until the training "
              "container and Vast launch template are defined.");
  }

  draw_labeled_int_input("Workers", train_.workers, 120.0f);
  draw_labeled_int_input("Lanes", train_.lanes, 120.0f);
  draw_full_width_input("CPU Affinity", train_.cpu_affinity);
  draw_labeled_checkbox("AMP", train_.amp);
  draw_labeled_checkbox("EMA", train_.use_ema);
  draw_labeled_checkbox("Progress", train_.progress_bar);
  draw_labeled_checkbox("Freeze Encoder", train_.freeze_encoder);
  (void)draw_compile_mode_combo("Compile Mode", train_.compile_mode);

  draw_section_heading("Optimization");
  draw_labeled_int_input("Batch Size", train_.batch_size, 120.0f);
  draw_labeled_int_input("Val Batch", train_.val_batch_size, 120.0f);
  draw_labeled_int_input("Epochs", train_.epochs, 120.0f);
  draw_labeled_int_input("Grad Accum", train_.grad_accum_steps, 120.0f);
  draw_labeled_int_input("Eval Max Dets", train_.eval_max_dets, 120.0f);
  draw_labeled_int_input("Print Freq", train_.print_freq, 120.0f);
  draw_labeled_int_input("Prefetch", train_.prefetch_factor, 120.0f);
  if (draw_train_optimizer_combo("Optimizer", train_.optimizer)) {
    apply_selected_preset_defaults();
  }
  const TrainRecipeConfig recipe =
      current_train_recipe(selected_preset_name_, train_.optimizer);
  if (draw_labeled_double_input("Momentum", train_.momentum, 180.0f, "%.4f")) {
    update_train_recipe_override(train_.recipe_overrides.momentum,
                                 train_.momentum, recipe.momentum);
  }
  if (draw_labeled_double_input("Learning Rate", train_.lr, 180.0f, "%.6f")) {
    update_train_recipe_override(train_.recipe_overrides.lr, train_.lr,
                                 recipe.lr);
  }
  if (draw_labeled_double_input("LR Encoder", train_.lr_encoder, 180.0f,
                                "%.6f")) {
    update_train_recipe_override(train_.recipe_overrides.lr_encoder,
                                 train_.lr_encoder, recipe.lr_encoder);
  }
  if (draw_labeled_double_input("Weight Decay", train_.weight_decay, 180.0f,
                                "%.6f")) {
    update_train_recipe_override(train_.recipe_overrides.weight_decay,
                                 train_.weight_decay, recipe.weight_decay);
  }
  if (draw_labeled_double_input("LR Component Decay", train_.lr_component_decay,
                                180.0f, "%.4f")) {
    update_train_recipe_override(train_.recipe_overrides.lr_component_decay,
                                 train_.lr_component_decay,
                                 recipe.lr_component_decay);
  }
  if (draw_labeled_double_input("Encoder Layer Decay",
                                train_.encoder_layer_decay, 180.0f, "%.4f")) {
    update_train_recipe_override(train_.recipe_overrides.encoder_layer_decay,
                                 train_.encoder_layer_decay,
                                 recipe.encoder_layer_decay);
  }
  if (draw_labeled_double_input("Warmup Epochs", train_.warmup_epochs, 180.0f,
                                "%.4f")) {
    update_train_recipe_override(train_.recipe_overrides.warmup_epochs,
                                 train_.warmup_epochs, recipe.warmup_epochs);
  }
  if (draw_labeled_double_input("Warmup Momentum", train_.warmup_momentum,
                                180.0f, "%.4f")) {
    update_train_recipe_override(train_.recipe_overrides.warmup_momentum,
                                 train_.warmup_momentum,
                                 recipe.warmup_momentum);
  }
  if (draw_labeled_double_input("LR Min Factor", train_.lr_min_factor, 180.0f,
                                "%.4f")) {
    update_train_recipe_override(train_.recipe_overrides.lr_min_factor,
                                 train_.lr_min_factor, recipe.lr_min_factor);
  }
  draw_labeled_double_input("Clip Max Norm", train_.clip_max_norm, 180.0f,
                            "%.4f");
  if (draw_labeled_int_input("LR Drop", train_.lr_drop, 120.0f)) {
    update_train_recipe_override(train_.recipe_overrides.lr_drop,
                                 train_.lr_drop, recipe.lr_drop);
  }
  if (draw_lr_scheduler_combo("LR Scheduler", train_.lr_scheduler)) {
    update_train_recipe_override(train_.recipe_overrides.lr_scheduler,
                                 train_.lr_scheduler, recipe.lr_scheduler);
  }

  const std::vector<int> selected_devices =
      local_train_controller_.selected_device_ids();
  const bool can_run_local = local_target && !block_actions &&
                             !selected_devices.empty() &&
                             local_train_controller_.gpu_error().empty();
  ImGui::BeginDisabled(!can_run_local);
  if (ImGui::Button("Run Training")) {
    local_train_controller_.start(train_, selected_preset_name_);
  }
  ImGui::EndDisabled();
  if (local_target && train_state.running) {
    ImGui::SameLine();
    if (ImGui::Button(train_state.stop_requested ? "Force Kill"
                                                 : "Stop Training")) {
      local_train_controller_.request_stop(train_state.stop_requested);
    }
    if (train_state.stop_requested) {
      ImGui::SameLine();
      ImGui::TextUnformatted("Stop requested");
    }
  }
  if (local_target &&
      (train_state.running || train_state.progress.has_value() ||
       !train_state.last_summary.empty() || !train_state.last_error.empty())) {
    ImGui::Spacing();
    draw_section_heading("Local Training Status");
    if (train_state.progress.has_value()) {
      const TrainArtifactProgress &progress = *train_state.progress;
      ImGui::TextWrapped("Phase: %s", progress.phase.empty()
                                          ? "train"
                                          : progress.phase.c_str());
      ImGui::Text("Epoch: %d / %d",
                  progress.epoch >= 0 ? progress.epoch + 1 : 0,
                  std::max(0, progress.total_epochs));
      ImGui::SameLine();
      ImGui::Text("Batches: %d / %d", progress.completed_batches,
                  progress.total_batches);
      ImGui::Text("Current Loss: %s  cls %s  box %s",
                  format_decimal(progress.step_loss, 4).c_str(),
                  format_decimal(progress.step_class_loss, 4).c_str(),
                  format_decimal(progress.step_box_loss, 4).c_str());
      ImGui::Text("Average Loss: %s  cls %s  box %s",
                  format_decimal(progress.train_loss, 4).c_str(),
                  format_decimal(progress.class_loss, 4).c_str(),
                  format_decimal(progress.box_loss, 4).c_str());
      ImGui::Text("Throughput: %s img/s",
                  format_decimal(progress.images_per_second, 2).c_str());
    }
    if (!train_state.last_summary.empty()) {
      ImGui::TextWrapped("%s", train_state.last_summary.c_str());
    }
    if (!train_state.last_error.empty()) {
      draw_banner(
          "train_view_train_error_banner", ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
          ImVec4(0.98f, 0.90f, 0.82f, 1.00f), train_state.last_error);
    }
  }
  if (local_target &&
      (train_state.running || !train_state.output_tail.empty())) {
    ImGui::Spacing();
    draw_output_console(
        "train_view_train_output_tail", train_state.output_tail, 180.0f,
        train_state.running, "Waiting for compiler or training output...");
  }
}

void App::draw_validate_view() {
  const bool block_actions = job_.running || live_predict_running();
  draw_section_heading("Datasets");
  draw_full_width_input("Compiled Dataset", validate_.compiled_path);
  draw_full_width_input("Source Root", validate_.source_dir);
  draw_full_width_input("Report JSON", validate_.report_json_path);

  draw_section_heading("Backends");
  draw_full_width_input("ONNX Path", validate_.onnx_path);
  draw_full_width_input("TensorRT Path", validate_.tensorrt_path);
  draw_full_width_input("Save Engine Path", validate_.save_engine_path);
  draw_full_width_input("Eval Order", validate_.eval_order);

  draw_section_heading("Execution");
  draw_full_width_input("Split", validate_.split);
  draw_full_width_input("CPU Affinity", validate_.cpu_affinity);
  draw_labeled_int_input("Resolution", validate_.resolution, 120.0f);
  draw_labeled_int_input("Batch Size", validate_.batch_size, 120.0f);
  draw_labeled_int_input("Workers", validate_.workers, 120.0f);
  draw_labeled_int_input("Device ID", validate_.device_id, 120.0f);
  draw_labeled_int_input("Limit Images", validate_.limit_images, 120.0f);
  draw_labeled_int_input("Alignment Images", validate_.alignment_images,
                         120.0f);
  draw_labeled_int_input("Eval Max Dets", validate_.eval_max_dets, 120.0f);
  draw_labeled_int_input("Prefetch", validate_.prefetch_factor, 120.0f);
  draw_labeled_checkbox("Recompile From Source", validate_.recompile);
  draw_labeled_checkbox("Profile", validate_.profile);
  draw_labeled_checkbox("FP16", validate_.allow_fp16);
  draw_labeled_checkbox("Write Report", validate_.write_report_json);

  ImGui::BeginDisabled(block_actions);
  if (ImGui::Button("Run Validation")) {
    const ValidateViewState state = validate_;
    launch_job("validate", [state]() {
      const mmltk::rfdetr::ValidateRequest request =
          rfdetr_workflows::build_validate_request(state);
      const ValidationOptions options = mmltk::rfdetr::to_validate_options(request);
      const ValidationRunResult result = run_validation(options);
      print_validation_run_summary(options, result);
      JobOutcome outcome;
      outcome.summary = rfdetr_workflows::summarize_validation_result(result);
      return outcome;
    });
  }
  ImGui::EndDisabled();
}

void App::draw_predict_view() {
  draw_section_heading("Source");
  ImGui::BeginDisabled(live_predict_running());
  const SourceSelectionUiActions source_actions = draw_source_selection(
      predict_.source, "predict_source", true, true, true, true,
      file_picker_busy(FileBrowseField::PredictSingleImage));
  ImGui::EndDisabled();
  if (source_actions.browse_single_image) {
    launch_file_picker(FileBrowseField::PredictSingleImage, "Select Image",
                       &predict_.source.single_image_path);
  }
  const bool live_video = predict_.source.kind == SourceKind::VideoStream;
  const bool single_image = predict_.source.kind == SourceKind::SingleImage;
  if (single_image) {
    predict_.batch_size = 1;
  }

  draw_section_heading("Model");
  ImGui::BeginDisabled(live_predict_running());
  const ModelInputBrowseRequest model_browse = draw_model_input_selector(
      predict_.model_input, predict_.weights_path, predict_.onnx_path,
      predict_.tensorrt_path, file_picker_busy(FileBrowseField::PredictWeights),
      file_picker_busy(FileBrowseField::PredictOnnx),
      file_picker_busy(FileBrowseField::PredictTensorRt));
  draw_full_width_input("Backend Preference", predict_.backend);
  if (!live_video) {
    draw_full_width_input("Output JSON", predict_.output_path);
  }
  ImGui::EndDisabled();
  switch (model_browse) {
  case ModelInputBrowseRequest::Weights:
    launch_file_picker(FileBrowseField::PredictWeights, "Select Weights",
                       &predict_.weights_path);
    break;
  case ModelInputBrowseRequest::Onnx:
    launch_file_picker(FileBrowseField::PredictOnnx, "Select ONNX Model",
                       &predict_.onnx_path);
    break;
  case ModelInputBrowseRequest::TensorRt:
    launch_file_picker(FileBrowseField::PredictTensorRt,
                       "Select TensorRT Engine", &predict_.tensorrt_path);
    break;
  case ModelInputBrowseRequest::None:
    break;
  }

  draw_section_heading("Execution");
  if (live_video) {
    ImGui::BeginDisabled(live_predict_running());
    draw_labeled_int_input("Device ID", predict_.device_id, 120.0f);
    draw_labeled_int_input("Live Splits", predict_.live_split_count, 120.0f);
    draw_labeled_int_input("Max Dets", predict_.max_dets_per_image, 120.0f);
    draw_labeled_float_input("Threshold", predict_.threshold, 120.0f, 0.01f,
                             0.10f, "%.3f");
    draw_labeled_checkbox("FP16", predict_.allow_fp16);
    (void)draw_compile_mode_combo("Compile Mode", predict_.compile_mode);
    ImGui::EndDisabled();
    ImGui::TextWrapped(
        "Live video predict keeps the selected model hot and runs against the "
        "newest available device frame. "
        "The active model resolution comes from the selected RF-DETR preset.");
  } else {
    draw_labeled_int_input("Device ID", predict_.device_id, 120.0f);
    draw_full_width_input("CPU Affinity", predict_.cpu_affinity);
    if (single_image) {
      ImGui::BeginDisabled();
      draw_labeled_int_input("Batch Size", predict_.batch_size, 120.0f);
      ImGui::EndDisabled();
      ImGui::TextWrapped("Single-image predict always uses batch size 1.");
    } else {
      draw_labeled_int_input("Batch Size", predict_.batch_size, 120.0f);
    }
    draw_labeled_int_input("Workers", predict_.workers, 120.0f);
    draw_labeled_int_input("Lanes", predict_.lanes, 120.0f);
    draw_labeled_int_input("Max Dets", predict_.max_dets_per_image, 120.0f);
    draw_labeled_float_input("Threshold", predict_.threshold, 120.0f, 0.01f,
                             0.10f, "%.3f");
    draw_labeled_checkbox("FP16", predict_.allow_fp16);
    draw_labeled_checkbox("Progress", predict_.progress_bar);
    (void)draw_compile_mode_combo("Compile Mode", predict_.compile_mode);
  }

  const std::string source_error = validate_predict_source(predict_.source);
  const std::string combo_error =
      live_video ? std::string{} : predict_combo_error(predict_);
  const std::string live_blocker =
      live_video ? live_predict_blocker(predict_, job_) : std::string{};
  if (!source_error.empty()) {
    ImGui::Spacing();
    if (live_video) {
      draw_banner("predict_live_source_error_banner",
                  ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                  ImVec4(0.98f, 0.90f, 0.82f, 1.00f), source_error);
    } else {
      ImGui::TextWrapped("%s", source_error.c_str());
    }
  }
  if (!live_blocker.empty() && (source_error.empty() || job_.running)) {
    ImGui::Spacing();
    draw_banner("predict_live_blocker_banner",
                ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                ImVec4(0.98f, 0.90f, 0.82f, 1.00f), live_blocker);
  }
  if (!combo_error.empty()) {
    ImGui::Spacing();
    draw_banner("predict_combo_banner", ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                ImVec4(0.98f, 0.90f, 0.82f, 1.00f), combo_error);
  }
  if (live_video && !live_predict_controller_.start_error().empty()) {
    ImGui::Spacing();
    draw_banner("predict_live_start_error_banner",
                ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                live_predict_controller_.start_error());
  }

  if (live_video) {
    if (active_live_mode_ != ActiveLiveMode::Predict &&
        !live_predict_controller_.starting() &&
        !live_predict_controller_.stopping()) {
      const bool block_live_start =
          !live_blocker.empty() ||
          active_live_mode_ == ActiveLiveMode::Annotate;
      ImGui::BeginDisabled(block_live_start);
      if (ImGui::Button("Start Live Prediction")) {
        launch_live_predict_session();
      }
      ImGui::EndDisabled();
    } else if (live_predict_controller_.starting()) {
      if (ImGui::Button("Cancel Live Start")) {
        stop_live_predict_session();
      }
      ImGui::SameLine();
      ImGui::TextUnformatted("Starting live prediction...");
    } else if (live_predict_controller_.stopping()) {
      ImGui::BeginDisabled();
      (void)ImGui::Button("Stopping Live Prediction");
      ImGui::EndDisabled();
    } else {
      if (ImGui::Button("Stop Live Prediction")) {
        stop_live_predict_session();
      }
    }
  } else {
    const bool block_prediction = job_.running || live_predict_running() ||
                                  !source_error.empty() || !combo_error.empty();
    ImGui::BeginDisabled(block_prediction);
    if (ImGui::Button("Run Prediction")) {
      clear_static_preview();
      const PredictViewState state = predict_;
      const std::string preset_name = selected_preset_name_;
      launch_job("predict", [state, preset_name]() {
        PreparedPredictSource prepared_source =
            prepare_predict_source(state.source);
        const mmltk::rfdetr::PredictRequest request =
            rfdetr_workflows::build_predict_request(
                state, std::move(prepared_source.image_inputs));
        PredictOptions options =
            mmltk::rfdetr::to_predict_options(request, preset_name);
        const PredictionRunResult result = run_prediction(options);
        write_prediction_json(options, result);
        print_prediction_summary(options, result);
        JobOutcome outcome;
        outcome.summary = rfdetr_workflows::summarize_prediction_result(result);
        outcome.preview = rfdetr_workflows::maybe_make_single_image_preview(
            state, options, result);
        return outcome;
      });
    }
    ImGui::EndDisabled();
  }
}

void App::draw_export_view() {
  const bool block_actions = job_.running || live_predict_running();

  draw_section_heading("Model Input");
  if (draw_file_picker_input(
          "Weights Path (.pt)", export_.weights_path,
          file_picker_busy(FileBrowseField::ExportWeights))) {
    launch_file_picker(FileBrowseField::ExportWeights, "Select Weights",
                       &export_.weights_path);
  }
  if (draw_file_picker_input("ONNX Path", export_.onnx_path,
                             file_picker_busy(FileBrowseField::ExportOnnx))) {
    launch_file_picker(FileBrowseField::ExportOnnx, "Select ONNX Model",
                       &export_.onnx_path);
  }

  draw_section_heading("ONNX Export");
  draw_labeled_int_input("Opset Version (19 only)", export_.opset_version,
                         120.0f);
  draw_labeled_checkbox("Simplify", export_.simplify);

  draw_section_heading("TensorRT");
  draw_labeled_checkbox("Build TensorRT Engine", export_.build_tensorrt);
  if (export_.build_tensorrt) {
    if (draw_file_picker_input(
            "Engine Output Path", export_.output_path,
            file_picker_busy(FileBrowseField::ExportEngine))) {
      launch_file_picker(FileBrowseField::ExportEngine, "Select Engine Output",
                         &export_.output_path);
    }
    draw_labeled_checkbox("FP16", export_.allow_fp16);
  }

  draw_section_heading("Execution");
  draw_labeled_int_input("Device ID", export_.device_id, 120.0f);

  ImGui::Spacing();
  ImGui::BeginDisabled(block_actions);
  if (ImGui::Button("Export")) {
    const ExportViewState state = export_;
    launch_job("export", [this, state]() {
      append_job_output("[export] start");
      std::filesystem::path onnx_path = state.onnx_path;

      if (!state.weights_path.empty()) {
        const ExportOnnxRequest export_request =
            rfdetr_workflows::build_export_onnx_request(state, onnx_path);
        append_job_output("[export] writing ONNX: " +
                          export_request.output_path.string());
        rfdetr::export_weights_to_onnx(export_request.weights_path,
                                       export_request.output_path,
                                       export_request.device_id,
                                       export_request.opset_version,
                                       export_request.simplify);
        onnx_path = export_request.output_path;
      }

      if (onnx_path.empty()) {
        throw std::runtime_error("Provide weights path or ONNX path");
      }

      std::string summary = "Exported ONNX: " + onnx_path.string();

      if (state.build_tensorrt) {
        const BuildEngineRequest build_request =
            rfdetr_workflows::build_build_engine_request(state, onnx_path);
        append_job_output("[export] building TensorRT engine: " +
                          build_request.output_path.string());
        rfdetr::make_tensorrt_backend(build_request.onnx_path,
                                      build_request.device_id,
                                      build_request.allow_fp16,
                                      build_request.output_path);
        append_job_output("[export] TensorRT engine ready: " +
                          build_request.output_path.string());
        summary += " | TRT engine: " + build_request.output_path.string();
      }

      JobOutcome outcome;
      outcome.summary = std::move(summary);
      return outcome;
    });
  }
  ImGui::EndDisabled();

  const bool export_job_active_or_recent = job_.label == "export";
  const std::string export_job_output = snapshot_job_output();
  if (export_job_active_or_recent || !export_job_output.empty()) {
    draw_section_heading("Export Job");
    if (job_.running && job_.label == "export") {
      ImGui::TextWrapped("Running: %s", job_.label.c_str());
    } else if (job_.label == "export") {
      ImGui::TextWrapped("Idle after: %s", job_.label.c_str());
    }
    if (job_.label == "export" && !job_.last_summary.empty()) {
      ImGui::TextWrapped("%s", job_.last_summary.c_str());
    }
    if (!export_job_output.empty() ||
        (job_.running && job_.label == "export")) {
      draw_output_console("export_view_job_output_tail", export_job_output,
                          180.0f, job_.running && job_.label == "export",
                          "Waiting for export output...");
    }
    if (job_.label == "export" && !job_.last_error.empty()) {
      ImGui::TextColored(ImVec4(0.94f, 0.45f, 0.41f, 1.00f), "Error");
      ImGui::TextWrapped("%s", job_.last_error.c_str());
    }
  }
}

void App::draw_live_view() {
  if (active_live_mode_ == ActiveLiveMode::Predict &&
      live_predict_controller_.controller() != nullptr) {
    const bool is_video_stream =
        predict_.source.kind == SourceKind::VideoStream;
    draw_labeled_checkbox("Fit To Capture", live_preview_fit_to_capture_);
    if (is_video_stream) {
      bool was_overlay = live_crop_overlay_mode_;
      draw_labeled_checkbox("Full Frame", live_crop_overlay_mode_);
      if (live_crop_overlay_mode_ != was_overlay) {
        live_crop_drag_session_ = {};
      }
    }
    draw_live_preview_panel();
    return;
  }

  if (live_predict_controller_.starting() ||
      live_predict_controller_.stopping()) {
    draw_section_heading("Live Preview");
    ImGui::TextWrapped(
        "%s", live_predict_controller_.starting()
                  ? "Live prediction is starting..."
                  : "Live prediction is stopping...");
    return;
  }

  if (has_static_preview()) {
    draw_labeled_checkbox("Fit To Capture", live_preview_fit_to_capture_);
    ImGui::TextWrapped("Source: %s", live_static_preview_source_name_.c_str());
    draw_live_preview_panel();
    return;
  }

  draw_section_heading("Live Preview");
  if (!live_predict_controller_.start_error().empty()) {
    draw_banner("live_view_start_error_banner",
                ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                live_predict_controller_.start_error());
    ImGui::Spacing();
  }
  if (!live_preview_error_.empty()) {
    draw_banner("live_view_preview_error_banner",
                ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                ImVec4(0.98f, 0.90f, 0.82f, 1.00f), live_preview_error_);
    ImGui::Spacing();
  }
  ImGui::TextWrapped(
      "Start live prediction from Predict with a video stream source, or run "
      "single-image predict to push an annotated still here.");
}

void App::draw_annotate_workspace() {
  const bool live_video = annotate_.source.kind == SourceKind::VideoStream;
  if (annotation_workflow_preview_pending(
          annotate_workflow_, annotation_frame_ptr(annotate_frame_))) {
    submit_annotation_preview();
  }

  draw_section_heading("Workspace");
  if (annotate_frame_.has_value()) {
    draw_with_optional_font(compact_font_, [this]() {
      ImGui::TextWrapped("Frame: %s", annotate_frame_->source_name.c_str());
    });
    ImGui::Text("Size: %u x %u", annotate_frame_->width,
                annotate_frame_->height);
    ImGui::SameLine();
    ImGui::Text("Objects: %zu", annotate_document_.size());
    ImGui::SameLine();
    ImGui::Text("Resolved Masks: %zu", annotate_resolved_instances_.size());
  } else if (annotate_prepare_running_ || annotate_frame_load_running_) {
    ImGui::TextUnformatted("Loading annotation source...");
  } else {
    ImGui::TextUnformatted(live_video
                               ? "Start live annotate to stream frames here."
                               : "Load a source image to begin annotating.");
  }
  const auto selected_index = [this]() {
    return normalize_selected_object_index(annotate_document_, annotate_session_);
  };
  const auto selected_annotation_object = [this]() -> const AnnotationObject * {
    return mmltk::gui::selected_object(annotate_document_, annotate_session_);
  };
  const auto active_tool = [this]() { return annotate_session_.active_tool(); };
  const auto selected_supports_mask_editing = [&]() {
    const AnnotationObject *object = selected_annotation_object();
    return object != nullptr &&
           annotation_object_supports_mask_editing(*object);
  };
  PreviewRectDragSession &create_drag_session =
      annotate_session_.create_drag_session();

  if (active_tool() == AnnotationToolKind::Box || create_drag_session.active) {
    draw_with_optional_font(compact_font_, []() {
      ImGui::TextWrapped("Box is active. Drag on the image to create a new "
                         "object. Use the mouse wheel to zoom and middle-drag "
                         "to pan for tighter geometry work.");
    });
  } else if (active_tool() == AnnotationToolKind::Direct) {
    draw_with_optional_font(compact_font_, []() {
      ImGui::TextWrapped(
          "Direct is active. Drag boxes to move or resize them, and drag "
          "selected point, spline, or skeleton handles for precise native "
          "edits. Selected splines now expose latent bezier handles that can "
          "be pulled directly from the path.");
    });
  } else if (active_tool() == AnnotationToolKind::MaskPaint &&
             !selected_supports_mask_editing()) {
    draw_with_optional_font(compact_font_, []() {
      ImGui::TextWrapped(
          "Paint is active, but it only applies to box and mask objects. "
          "Select one of those shapes to edit a dense mask.");
    });
  } else if (active_tool() == AnnotationToolKind::MaskPaint) {
    draw_with_optional_font(compact_font_, [this]() {
      ImGui::TextWrapped("Paint is active. Brush radius: %d px capture-space.",
                         annotate_brush_radius_);
    });
  } else if (active_tool() == AnnotationToolKind::MaskErase &&
             !selected_supports_mask_editing()) {
    draw_with_optional_font(compact_font_, []() {
      ImGui::TextWrapped(
          "Erase is active, but it only applies to box and mask objects. "
          "Select one of those shapes to edit a dense mask.");
    });
  } else if (active_tool() == AnnotationToolKind::MaskErase) {
    draw_with_optional_font(compact_font_, [this]() {
      ImGui::TextWrapped("Erase is active. Brush radius: %d px capture-space.",
                         annotate_brush_radius_);
    });
  } else if (active_tool() == AnnotationToolKind::MaskFill &&
             !selected_supports_mask_editing()) {
    draw_with_optional_font(compact_font_, []() {
      ImGui::TextWrapped(
          "Fill is active, but it only applies to box and mask objects. Select "
          "one of those shapes to flood-fill a dense mask.");
    });
  } else if (active_tool() == AnnotationToolKind::MaskFill) {
    draw_with_optional_font(compact_font_, []() {
      ImGui::TextWrapped("Fill is active. Click a background gap inside the "
                         "selected ROI to flood it.");
    });
  } else if (active_tool() == AnnotationToolKind::Spline) {
    draw_with_optional_font(compact_font_, []() {
      ImGui::TextWrapped(
          "Spline is active. Click to add knots to the selected spline. "
          "Double-click to close the current spline, then switch to Direct to "
          "pull out and edit bezier handles.");
    });
  } else if (active_tool() == AnnotationToolKind::Point) {
    draw_with_optional_font(compact_font_, []() {
      ImGui::TextWrapped("Point is active. Click to place a point object, or "
                         "reposition the selected point.");
    });
  } else if (active_tool() == AnnotationToolKind::Skeleton) {
    draw_with_optional_font(compact_font_, []() {
      ImGui::TextWrapped(
          "Skeleton is active. Click to place the next joint from the selected "
          "skeleton or create a new joint chain.");
    });
  } else if (annotation_live_running()) {
    draw_with_optional_font(compact_font_, []() {
      ImGui::TextWrapped(
          "Live annotate is running against the raw capture feed.");
    });
  }

  draw_section_heading("Canvas");
  const LivePreviewTextureState preview_state =
      snapshot_preview_state(live_preview_texture_);
  ImGui::BeginChild("annotate_canvas", ImVec2(0.0f, 0.0f), true,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
  if (!annotate_frame_.has_value()) {
    ImGui::TextUnformatted(
        annotate_frame_load_running_ || annotate_prepare_running_
            ? "Loading annotation source..."
            : (live_video ? "Start live annotate to stream frames here."
                          : "Load a source image to begin annotating."));
    ImGui::EndChild();
    return;
  }
  if (!preview_state.has_frame) {
    ImGui::TextUnformatted(
        annotation_workflow_preview_running(annotate_workflow_)
            ? "Preparing annotation preview..."
            : "Waiting for preview frame...");
    ImGui::EndChild();
    return;
  }

  const auto width = static_cast<float>(annotate_frame_->width);
  const auto height = static_cast<float>(annotate_frame_->height);
  const ImVec2 mouse = ImGui::GetMousePos();

  const auto set_active_tool_kind = [&](const AnnotationToolKind tool) {
    if (active_tool() == tool) {
      return;
    }
    (void)annotate_controller_.set_active_tool(tool, annotate_document_,
                                               annotate_session_);
    cancel_annotation_canvas_interactions();
  };

  const auto draw_tool_button = [&](const AnnotationToolKind tool,
                                    const bool disabled = false) {
    const bool selected = active_tool() == tool;
    const bool clicked = annotation_ui::draw_small_button(
        annotation_tool_kind_label(tool), !disabled,
        selected ? annotation_ui::ButtonTone::Primary
                 : annotation_ui::ButtonTone::Default);
    if (clicked) {
      set_active_tool_kind(tool);
    }
    return clicked;
  };

  ImGui::TextUnformatted("Mode");
  ImGui::SameLine();
  (void)draw_tool_button(AnnotationToolKind::Select);
  ImGui::SameLine();
  (void)draw_tool_button(AnnotationToolKind::Direct);
  ImGui::SameLine();
  (void)draw_tool_button(AnnotationToolKind::Box);
  ImGui::SameLine();
  (void)draw_tool_button(AnnotationToolKind::Spline);
  ImGui::SameLine();
  (void)draw_tool_button(AnnotationToolKind::Point);
  ImGui::SameLine();
  (void)draw_tool_button(AnnotationToolKind::Skeleton);
  ImGui::SameLine();
  (void)draw_tool_button(AnnotationToolKind::MaskPaint,
                         !selected_supports_mask_editing());
  ImGui::SameLine();
  (void)draw_tool_button(AnnotationToolKind::MaskErase,
                         !selected_supports_mask_editing());
  ImGui::SameLine();
  (void)draw_tool_button(AnnotationToolKind::MaskFill,
                         !selected_supports_mask_editing());
  if (selected_supports_mask_editing() &&
      annotation_tool_kind_uses_brush(active_tool())) {
    ImGui::SameLine();
    draw_labeled_int_input("Brush", annotate_brush_radius_, 70.0f);
    annotate_brush_radius_ = std::clamp(annotate_brush_radius_, 1, 128);
  }

  const auto apply_tool_action = [&](const AnnotationToolActionKind action) {
    const bool changed = annotate_controller_.handle_tool_action(
        annotate_document_, annotate_session_, *annotate_frame_,
        annotate_categories_, action);
    if (changed) {
      invalidate_annotation_preview();
    }
  };
  const std::optional<std::size_t> active_selected_index = selected_index();
  const AnnotationObject *active_selected_object = selected_annotation_object();
  if (active_tool() == AnnotationToolKind::Spline &&
      active_selected_index.has_value() &&
      active_selected_object != nullptr &&
      std::holds_alternative<AnnotationSplineShape>(
          active_selected_object->shape)) {
    const AnnotationSidebarSelectedSplineViewModel spline_model =
        tool_detail::make_selected_spline_sidebar_view_model(
            annotate_session_, *active_selected_index,
            std::get<AnnotationSplineShape>(active_selected_object->shape));
    if (annotation_ui::draw_small_button("Close", spline_model.can_close)) {
      apply_tool_action(AnnotationToolActionKind::Confirm);
    }
    ImGui::SameLine();
    if (annotation_ui::draw_small_button("Reopen", spline_model.can_reopen)) {
      apply_tool_action(AnnotationToolActionKind::ReopenSpline);
    }
    ImGui::SameLine();
    if (annotation_ui::draw_small_button("Delete Knot",
                                         spline_model.can_delete_active_knot)) {
      apply_tool_action(AnnotationToolActionKind::DeleteActiveElement);
    }
    ImGui::SameLine();
    if (annotation_ui::draw_small_button("Cycle Handle",
                                         spline_model.can_edit_active_handle_mode)) {
      apply_tool_action(AnnotationToolActionKind::CycleHandleMode);
    }
    ImGui::SameLine();
    if (annotation_ui::draw_small_button("Cancel Edit",
                                         spline_model.can_cancel_edit)) {
      apply_tool_action(AnnotationToolActionKind::Cancel);
    }
  } else if (active_tool() == AnnotationToolKind::Skeleton &&
             active_selected_index.has_value() &&
             active_selected_object != nullptr &&
             std::holds_alternative<AnnotationSkeletonShape>(
                 active_selected_object->shape)) {
    const AnnotationSidebarSelectedSkeletonViewModel skeleton_model =
        tool_detail::make_selected_skeleton_sidebar_view_model(
            annotate_session_, *active_selected_index,
            std::get<AnnotationSkeletonShape>(active_selected_object->shape));
    if (annotation_ui::draw_small_button("Skip Joint",
                                         skeleton_model.can_skip_joint)) {
      apply_tool_action(AnnotationToolActionKind::SkipJoint);
    }
    ImGui::SameLine();
    if (annotation_ui::draw_small_button("Hide Joint",
                                         skeleton_model.can_hide_active_joint)) {
      apply_tool_action(AnnotationToolActionKind::HideJoint);
    }
    ImGui::SameLine();
    if (annotation_ui::draw_small_button(
            "Show Joint", skeleton_model.can_reactivate_active_joint)) {
      apply_tool_action(AnnotationToolActionKind::ReactivateJoint);
    }
    ImGui::SameLine();
    if (annotation_ui::draw_small_button("Reseed Joint",
                                         skeleton_model.can_reseed_active_joint)) {
      apply_tool_action(AnnotationToolActionKind::ReseedJoint);
    }
    ImGui::SameLine();
    if (annotation_ui::draw_small_button("Cancel Edit",
                                         skeleton_model.can_cancel_edit)) {
      apply_tool_action(AnnotationToolActionKind::Cancel);
    }
  }
  ImGui::Separator();

  const std::shared_ptr<const AnnotationProjectedScene> projected_scene =
      resolve_annotation_projected_scene(annotate_workflow_, annotate_document_,
                                         annotate_session_, annotate_frame_);
  const AnnotationWorkspaceViewModel workspace_view =
      AnnotationWorkspaceModelBuilder::build(
          *annotate_frame_, annotate_document_, annotate_.source,
          live_video && annotate_.full_frame, projected_scene);
  const std::optional<AnnotationBox> selected_capture_box =
      workspace_view.selection.selected_capture_box;
  const bool has_selected_capture_box = selected_capture_box.has_value();

  const bool click_fit = ImGui::SmallButton("Fit");
  ImGui::SameLine();
  const bool click_one_to_one = ImGui::SmallButton("1:1");
  ImGui::SameLine();
  const bool click_zoom_in = ImGui::SmallButton("Zoom +");
  ImGui::SameLine();
  const bool click_zoom_out = ImGui::SmallButton("Zoom -");
  bool click_focus_selection = false;
  if (has_selected_capture_box) {
    ImGui::SameLine();
    click_focus_selection = ImGui::SmallButton("Focus Box");
  }
  bool click_focus_crop = false;
  if (live_video && annotate_.full_frame) {
    ImGui::SameLine();
    click_focus_crop = ImGui::SmallButton("Focus Crop");
  }
  ImGui::Separator();

  const ImVec2 available = ImGui::GetContentRegionAvail();
  if (available.x <= 1.0f || available.y <= 1.0f || width <= 0.0f ||
      height <= 0.0f) {
    ImGui::EndChild();
    return;
  }

  const ImVec2 viewport_screen_min = ImGui::GetCursorScreenPos();
  const auto current_canvas_state = [this]() {
    return AnnotationCanvasState{
        annotate_canvas_scale_,
        annotate_canvas_pan_x_,
        annotate_canvas_pan_y_,
        annotate_canvas_auto_fit_,
    };
  };
  const auto apply_canvas_state = [this](const AnnotationCanvasState &state) {
    annotate_canvas_scale_ = state.scale;
    annotate_canvas_pan_x_ = state.pan_x;
    annotate_canvas_pan_y_ = state.pan_y;
    annotate_canvas_auto_fit_ = state.auto_fit;
  };
  const auto build_canvas_layout = [&, this]() {
    return build_annotation_canvas_layout(
        *annotate_frame_,
        AnnotationCanvasLayoutInput{
            current_canvas_state(),
            available.x,
            available.y,
            viewport_screen_min.x,
            viewport_screen_min.y,
            mouse.x,
            mouse.y,
            ImGui::IsWindowHovered(
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem),
        });
  };

  AnnotationCanvasLayout canvas_layout = build_canvas_layout();
  if (click_fit) {
    apply_canvas_state(annotation_canvas_fit_state(canvas_layout));
    canvas_layout = build_canvas_layout();
  } else if (click_one_to_one) {
    apply_canvas_state(annotation_canvas_one_to_one_state());
    canvas_layout = build_canvas_layout();
  }

  if (click_zoom_in) {
    apply_canvas_state(annotation_canvas_zoom_around_point(
        canvas_layout, canvas_layout.state.scale * 1.25f,
        viewport_screen_min.x + available.x * 0.5f,
        viewport_screen_min.y + available.y * 0.5f));
    canvas_layout = build_canvas_layout();
  } else if (click_zoom_out) {
    apply_canvas_state(annotation_canvas_zoom_around_point(
        canvas_layout, canvas_layout.state.scale / 1.25f,
        viewport_screen_min.x + available.x * 0.5f,
        viewport_screen_min.y + available.y * 0.5f));
    canvas_layout = build_canvas_layout();
  }
  if (click_focus_selection &&
      workspace_view.selection.selected_frame_box.has_value()) {
    apply_canvas_state(annotation_canvas_focus_box(
        canvas_layout, *workspace_view.selection.selected_frame_box));
    canvas_layout = build_canvas_layout();
  }
  if (click_focus_crop && live_video && annotate_.full_frame) {
    apply_canvas_state(annotation_canvas_focus_box(
        canvas_layout,
        annotation_box_to_frame(*annotate_frame_,
                                resolved_video_crop_box(annotate_.source))));
    canvas_layout = build_canvas_layout();
  }

  if (annotate_canvas_panning_) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      apply_canvas_state(annotation_canvas_pan_by_delta(
          canvas_layout, ImGui::GetIO().MouseDelta.x,
          ImGui::GetIO().MouseDelta.y));
      canvas_layout = build_canvas_layout();
    } else {
      annotate_canvas_panning_ = false;
    }
  }

  if (canvas_layout.viewport_hovered &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
    annotate_canvas_panning_ = true;
  }
  if (canvas_layout.viewport_hovered && ImGui::GetIO().MouseWheel != 0.0f) {
    const ImVec2 zoom_anchor =
        canvas_layout.overlay_hovered
            ? mouse
            : ImVec2(viewport_screen_min.x + available.x * 0.5f,
                     viewport_screen_min.y + available.y * 0.5f);
    apply_canvas_state(annotation_canvas_zoom_around_point(
        canvas_layout,
        canvas_layout.state.scale * std::pow(1.15f, ImGui::GetIO().MouseWheel),
        zoom_anchor.x, zoom_anchor.y));
    canvas_layout = build_canvas_layout();
  }

  apply_canvas_state(canvas_layout.state);

  const ImVec2 viewport_screen_max(canvas_layout.viewport_screen_max_x,
                                   canvas_layout.viewport_screen_max_y);
  const ImVec2 image_min(canvas_layout.image_screen_x,
                         canvas_layout.image_screen_y);
  const ImVec2 image_size(canvas_layout.image_width,
                          canvas_layout.image_height);
  const ImVec2 image_max(canvas_layout.image_screen_max_x,
                         canvas_layout.image_screen_max_y);
  const bool overlay_hovered = canvas_layout.overlay_hovered;
  const float current_scale = canvas_layout.state.scale;

  ImGui::SetCursorScreenPos(image_min);
  ImGui::InvisibleButton("annotate_canvas_hit", image_size);
  ImDrawList *draw_list = ImGui::GetWindowDrawList();
  draw_list->PushClipRect(viewport_screen_min, viewport_screen_max, true);
  const LiveCaptureRegion texture_region = preview_state.displayed_region;
  const LiveCaptureRegion display_region = preview_region_for_source(
      texture_region, annotate_.source, annotate_.full_frame);
  ImVec2 preview_uv0 = LivePreviewTexture::uv0();
  ImVec2 preview_uv1 = LivePreviewTexture::uv1();
  preview_uvs_for_region(texture_region, display_region, &preview_uv0,
                         &preview_uv1);

  const int image_x = canvas_layout.image_x;
  const int image_y = canvas_layout.image_y;
  const int capture_x = canvas_layout.capture_x;
  const int capture_y = canvas_layout.capture_y;

  const std::optional<AnnotationBox> selected_box =
      workspace_view.selection.selected_frame_box;
  const bool selected_box_fully_visible =
      workspace_view.selection.selected_box_fully_visible;

  bool color_sampled = false;
  AnnotationHsv sample_hsv{};
  if (overlay_hovered) {
    bool sampled_from_pixels = !annotate_frame_->pixels_bgr.empty();
    const std::optional<std::size_t> active_index = selected_index();
    const AnnotationObject *active_object =
        active_index.has_value() ? annotate_document_.object(*active_index)
                                 : nullptr;
    if (!sampled_from_pixels && annotation_live_running() &&
        active_object != nullptr) {
      const AnnotationColorRange *armed_range =
          active_object->sup.sampling     ? &active_object->sup
          : active_object->nosup.sampling ? &active_object->nosup
                                          : nullptr;
      if (armed_range != nullptr) {
        std::string sample_error;
        if (!ensure_annotation_workflow_live_annotation_frame_pixels(
                annotate_.source, annotate_frame_,
                annotation_live_running(), live_controller_.get(),
                annotate_.full_frame, &sample_error)) {
          annotate_save_error_ = std::move(sample_error);
        } else {
          sampled_from_pixels = true;
        }
      }
    }
    if (sampled_from_pixels) {
      sample_hsv = sample_annotation_hsv(*annotate_frame_, image_x, image_y);
    }
    if (active_index.has_value() && active_object != nullptr) {
      AnnotationObject updated_object = *active_object;
      AnnotationColorRange *armed_range =
          updated_object.sup.sampling     ? &updated_object.sup
          : updated_object.nosup.sampling ? &updated_object.nosup
                                          : nullptr;
      if (armed_range != nullptr && sampled_from_pixels &&
          ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        recenter_annotation_range(*armed_range, sample_hsv);
        updated_object.sup.sampling = false;
        updated_object.nosup.sampling = false;
        if (AnnotationEditor::update_selected_object_color_ranges(
                annotate_document_, annotate_session_, updated_object.sup,
                updated_object.nosup)) {
          invalidate_annotation_preview();
          color_sampled = true;
        }
      }
    }
  }

  const CanvasPointerState canvas_pointer{
      mouse.x,
      mouse.y,
      overlay_hovered,
      false,
      ImGui::IsMouseDown(ImGuiMouseButton_Left),
  };
  const bool overlay_left_clicked = !color_sampled && overlay_hovered &&
                                    ImGui::IsItemClicked(ImGuiMouseButton_Left);
  const bool overlay_left_double_clicked =
      !color_sampled && overlay_hovered &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  const int crop_handle_radius =
      (workspace_view.crop_frame_box.has_value() ||
       annotate_session_.crop_drag_session().active)
          ? crop_handle_radius_pixels(
                canvas_layout.viewport.screen_width,
                canvas_layout.viewport.screen_height,
                static_cast<float>(annotate_frame_->width),
                static_cast<float>(annotate_frame_->height))
          : 4;
  const AnnotationWorkspaceInteractionResult interaction =
      process_annotation_workspace_interaction(
          annotate_document_, annotate_session_, annotate_controller_,
          annotate_categories_, *annotate_frame_, annotate_.source,
          workspace_view, canvas_layout, canvas_pointer,
          AnnotationWorkspaceInteractionConfig{
              live_video,
              annotate_.full_frame,
              overlay_hovered,
              overlay_left_clicked,
              ImGui::IsMouseDown(ImGuiMouseButton_Left),
              overlay_left_double_clicked,
              color_sampled,
              image_x,
              image_y,
              capture_x,
              capture_y,
              annotate_brush_radius_,
              annotate_.device_id,
              crop_handle_radius,
              crop_edge_hit_half_width_px(),
              crop_corner_hit_size_px(),
          });
  if (interaction.projected_scene != nullptr) {
    store_annotation_projected_scene(
        annotate_workflow_, interaction.projected_scene, annotate_document_,
        annotate_session_, annotate_frame_);
  }
  if (interaction.preview_invalidated) {
    invalidate_annotation_preview();
  }
  set_rectangle_drag_cursor(interaction.cursor_kind);

  draw_list->AddRectFilled(viewport_screen_min, viewport_screen_max,
                           IM_COL32(18, 21, 24, 255));
  draw_list->AddImage(preview_state.texture_id, image_min, image_max,
                      preview_uv0, preview_uv1);
  draw_list->AddRect(viewport_screen_min, viewport_screen_max,
                     IM_COL32(74, 80, 88, 255), 0.0f, 0, 1.0f);
  const AnnotationBrushPreview &brush_preview =
      annotate_session_.brush_preview();
  if (brush_preview.visible) {
    const auto frame_x = static_cast<float>(
        brush_preview.capture_x - static_cast<int>(annotate_frame_->view_x));
    const auto frame_y = static_cast<float>(
        brush_preview.capture_y - static_cast<int>(annotate_frame_->view_y));
    draw_list->AddCircle(
        ImVec2(image_min.x + current_scale * frame_x,
               image_min.y + current_scale * frame_y),
        std::max(2.0f,
                 static_cast<float>(brush_preview.radius) * current_scale),
        brush_preview.erase ? IM_COL32(255, 128, 128, 230)
                            : IM_COL32(128, 255, 170, 230),
        32, 1.5f);
  }

  std::array<char, 64> zoom_label{};
  std::snprintf(zoom_label.data(), zoom_label.size(), "Zoom %.0f%%",
                current_scale * 100.0f);
  draw_list->AddRectFilled(
      ImVec2(viewport_screen_min.x + 8.0f, viewport_screen_min.y + 8.0f),
      ImVec2(viewport_screen_min.x + 328.0f, viewport_screen_min.y + 72.0f),
      IM_COL32(10, 12, 14, 196), 4.0f);
  draw_list->AddText(
      ImVec2(viewport_screen_min.x + 16.0f, viewport_screen_min.y + 14.0f),
      IM_COL32(228, 232, 236, 255), zoom_label.data());
  if (overlay_hovered) {
    std::array<char, 160> cursor_label{};
    std::snprintf(
        cursor_label.data(), cursor_label.size(),
        "Cursor view %d,%d  capture %d,%d  HSV %.1f / %.1f%% / %.1f%%", image_x,
        image_y, capture_x, capture_y, sample_hsv.hue_degrees,
        sample_hsv.saturation * 100.0f, sample_hsv.value * 100.0f);
    draw_list->AddText(
        ImVec2(viewport_screen_min.x + 16.0f, viewport_screen_min.y + 32.0f),
        IM_COL32(208, 214, 220, 255), cursor_label.data());
  }
  if (selected_box.has_value() && !selected_box_fully_visible) {
    draw_list->AddText(
        ImVec2(viewport_screen_min.x + 16.0f, viewport_screen_min.y + 50.0f),
        IM_COL32(255, 220, 96, 255), "Selection clipped by current preview");
  }
  if (annotate_interaction_overlay_ != nullptr) {
    PreviewInteractionOverlaySnapshot interaction_snapshot =
        AnnotationRenderer::build_interaction_overlay_snapshot(
            interaction.overlay_request);
    annotate_interaction_overlay_->publish_snapshot(
        std::move(interaction_snapshot));
    const PreviewInteractionOverlayState overlay_state =
        annotate_interaction_overlay_->snapshot();
    if (overlay_state.has_frame &&
        overlay_state.width == annotate_frame_->width &&
        overlay_state.height == annotate_frame_->height) {
      draw_list->AddImage(overlay_state.texture_id, image_min, image_max);
    }
  }
  draw_list->PopClipRect();
  ImGui::EndChild();
}

void App::draw_annotate_save_tab(const bool live_video) {
  const AnnotationWorkflowSaveRequest save_request =
      make_annotation_workflow_save_request(
          &annotate_workflow_, annotate_, annotation_live_running(),
          annotate_save_running_, annotation_frame_ptr(annotate_frame_),
          !annotate_resolved_instances_.empty(), annotate_current_input_index_,
          annotate_inputs_.size());
  const AnnotationWorkflowSaveTabView save_tab_view =
      make_annotation_workflow_save_tab_view(
          save_request, make_annotation_workflow_save_ui_request(
                            &annotate_, &annotate_categories_,
                            &annotate_workflow_, live_video, compact_font_));
  const AnnotationSaveTabUiActions actions =
      draw_annotation_save_tab_ui(save_tab_view.ui_state, save_tab_view.plan);

  sync_annotation_categories();
  (void)apply_annotation_workflow_save_tab_ui_actions(annotate_workflow_,
                                                      live_video, actions);
  if (actions.request_save_now) {
    (void)dispatch_annotation_workflow_save_now(
        save_tab_view.plan,
        [this](const bool refresh_live_frame)
            -> std::optional<AnnotationSaveSnapshot> {
          return make_annotation_workflow_current_save_snapshot(
              AnnotationWorkflowCurrentSaveSnapshotRequest{
                  &annotate_workflow_,
                  &annotate_document_,
                  &annotate_session_,
                  &annotate_frame_,
                  &annotate_categories_,
                  &annotate_resolved_instances_,
                  refresh_live_frame,
                  true,
                  &annotate_save_error_,
                  "annotate live save resolve error",
              },
              [this](std::string *error_message) {
                return ensure_annotation_workflow_live_annotation_frame_pixels(
                    annotate_.source, annotate_frame_,
                    annotation_live_running(), live_controller_.get(),
                    annotate_.full_frame, error_message);
              },
              [this](const char *error_context, const std::string &error) {
                log_gui_error(
                    error_context != nullptr ? error_context
                                             : "annotate resolve error",
                    error);
              });
        },
        [this](AnnotationSaveSnapshot save_snapshot, const bool live_mode) {
          return launch_annotation_workflow_save(
              annotate_workflow_, annotate_, annotate_categories_,
              std::move(save_snapshot), live_mode,
              AnnotationWorkflowSaveLaunchState{
                  &annotate_save_running_, &annotate_save_summary_,
                  &annotate_save_error_, &annotate_categories_loaded_,
                  &annotate_categories_output_dir_},
              background_executor_, ui_callbacks_,
              [this](const std::string &error) {
                log_gui_error("annotate save error", error);
              });
        },
        [this]() { step_annotation_current_frame(1); });
  }
}

void App::draw_annotate_setup_tab(const bool block_actions,
                                  const bool live_video,
                                  const bool can_use_video) {
  const AnnotationWorkflowSetupRequest setup_request =
      make_annotation_workflow_setup_request(
          &annotate_, live_video, annotation_live_running(), block_actions,
          can_use_video, annotate_prepare_running_,
          file_picker_busy(FileBrowseField::AnnotateSingleImage),
          file_picker_busy(FileBrowseField::AnnotateWeights),
          file_picker_busy(FileBrowseField::AnnotateOnnx),
          file_picker_busy(FileBrowseField::AnnotateTensorRt),
          annotate_current_input_index_, annotate_inputs_.size(),
          compact_font_);
  const AnnotationSetupTabActions actions =
      draw_annotation_setup_tab(make_annotation_setup_tab_state(setup_request),
                                [this]() { draw_preset_selector(); });
  const AnnotationWorkflowSetupDispatchPlan dispatch_plan =
      plan_annotation_workflow_setup_dispatch(actions,
                                             annotate_current_input_index_,
                                             annotate_inputs_.size());
  dispatch_annotation_workflow_setup(
      dispatch_plan,
      [this](const AnnotationSetupBrowseRequest browse_request) {
        handle_annotation_setup_browse_request(browse_request);
      },
      [this]() { prepare_annotation_source(); },
      [this]() { cancel_annotation_canvas_interactions(); },
      [this]() { invalidate_annotation_preview(); },
      [this]() {
        const AnnotationWorkflowLiveSessionShellStartResult start_result =
            restart_annotation_workflow_live_session_shell(
                annotate_workflow_, &live_controller_, &live_session_status_,
                &annotate_frame_, &annotate_resolved_instances_,
                live_preview_texture_.get(), annotate_.source,
                annotate_.device_id,
                [this]() { cancel_annotation_canvas_interactions(); });
        active_live_mode_ = start_result.started ? ActiveLiveMode::Annotate
                                                 : ActiveLiveMode::None;
        annotate_save_error_ = start_result.error_message;
      },
      [this]() {
        reset_annotation_workflow_live_session_shell(
            annotate_workflow_, &live_controller_, &live_session_status_,
            &annotate_frame_, &annotate_resolved_instances_,
            live_preview_texture_.get(),
            [this]() { cancel_annotation_canvas_interactions(); });
        active_live_mode_ = ActiveLiveMode::None;
      },
      [this]() { load_annotation_current_frame(); },
      [this](const int step) { step_annotation_current_frame(step); });
}

void App::apply_annotation_sidebar_mutation_result(
    const AnnotationSidebarMutationResult &result, const bool allow_assist) {
  apply_annotation_sidebar_shell_effects(
      result, allow_assist,
      [this]() {
        (void)launch_annotation_workflow_assist(
            AnnotationWorkflowAssistLaunchRequest{
                annotation_frame_ptr(annotate_frame_),
                annotate_assist_running_,
                annotate_.model_input,
            },
            annotation_live_running(),
            AnnotationWorkflowAssistLaunchState{
                &annotate_assist_running_, &annotate_assist_summary_,
                &annotate_assist_error_},
            background_executor_, ui_callbacks_,
            [this](std::string *error_message) {
              return ensure_annotation_workflow_live_annotation_frame_pixels(
                  annotate_.source, annotate_frame_,
                  annotation_live_running(), live_controller_.get(),
                  annotate_.full_frame, error_message);
            },
            [this]() -> std::optional<AnnotationWorkflowAssistExecutionRequest> {
              return make_annotation_workflow_assist_execution_request(
                  *annotate_frame_, annotate_, selected_preset_name_,
                  &annotate_assist_error_);
            },
            [this](const AnnotationWorkflowAssistOutcome &outcome) {
              if (!annotation_workflow_assist_outcome_matches_frame(
                      annotation_frame_ptr(annotate_frame_), outcome)) {
                return;
              }
              const AnnotationWorkflowAssistApplyResult apply_result =
                  apply_annotation_workflow_assist_outcome(
                      annotate_document_, annotate_session_,
                      annotate_categories_, outcome);
              if (!apply_result.changed) {
                return;
              }
              cancel_annotation_canvas_interactions();
              invalidate_annotation_preview();
            },
            [this](const std::string &error) {
              log_gui_error("annotate assist error", error);
            });
      },
      [this]() { cancel_annotation_canvas_interactions(); },
      [this](const AnnotationToolKind tool) {
        (void)annotate_controller_.set_active_tool(tool, annotate_document_,
                                                   annotate_session_);
      },
      [this]() { invalidate_annotation_preview(); });
}

void App::apply_annotation_objects_tab_actions(
    const AnnotationSidebarViewModel &sidebar_model,
    const AnnotationObjectsTabActions &actions, const bool assist_available) {
  const AnnotationSidebarMutationResult result =
      mmltk::gui::apply_annotation_objects_tab_actions(
          mmltk::gui::make_annotation_workflow_objects_tab_apply_context(
              AnnotationWorkflowObjectsTabRequest{
                  annotate_controller_,
                  annotate_document_,
                  annotate_session_,
                  annotate_categories_,
                  annotation_frame_ptr(annotate_frame_),
                  &annotate_pending_class_name_,
                  assist_available,
              }),
          sidebar_model, actions);
  apply_annotation_sidebar_mutation_result(result, true);
}

void App::apply_annotation_mask_tab_actions(
    const AnnotationMaskTabActions &actions) {
  const AnnotationSidebarMutationResult result =
      mmltk::gui::apply_annotation_mask_tab_actions(
          mmltk::gui::make_annotation_workflow_mask_tab_apply_context(
              AnnotationWorkflowMaskTabRequest{
                  annotate_document_,
                  annotate_session_,
                  annotation_frame_ptr(annotate_frame_),
                  &annotate_cleanup_radius_,
              }),
          actions);
  apply_annotation_sidebar_mutation_result(result, false);
}

void App::draw_annotate_utilities_pane() {
  const bool block_actions = job_.running || live_predict_running() ||
                             annotate_assist_running_ || annotate_save_running_;
  const bool live_video = annotate_.source.kind == SourceKind::VideoStream;
  const bool can_use_video = live_capture_supported();
  const bool has_annotation_frame = annotate_frame_.has_value();
  const bool assist_available = annotate_.model_input != ModelInputMode::None;

  sync_annotation_categories();
  draw_with_optional_font(compact_font_, []() {
    ImGui::TextWrapped("%s", annotate_error().c_str());
  });
  if (!annotate_save_error_.empty()) {
    draw_banner("annotate_error_banner", ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                ImVec4(0.98f, 0.90f, 0.82f, 1.00f), annotate_save_error_);
  } else if (!annotate_assist_error_.empty()) {
    draw_banner("annotate_assist_error_banner",
                ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                ImVec4(0.98f, 0.90f, 0.82f, 1.00f), annotate_assist_error_);
  } else if (!annotate_save_summary_.empty()) {
    draw_with_optional_font(compact_font_, [this]() {
      ImGui::TextWrapped("%s", annotate_save_summary_.c_str());
    });
  } else if (!annotate_assist_summary_.empty()) {
    draw_with_optional_font(compact_font_, [this]() {
      ImGui::TextWrapped("%s", annotate_assist_summary_.c_str());
    });
  }

  if (!ImGui::BeginTabBar("annotate_utilities_tabs",
                          ImGuiTabBarFlags_FittingPolicyResizeDown)) {
    return;
  }

  if (ImGui::BeginTabItem("Setup")) {
    draw_annotate_setup_tab(block_actions, live_video, can_use_video);
    ImGui::EndTabItem();
  }

  if (ImGui::BeginTabItem("Objects")) {
    const AnnotationSidebarViewModel sidebar_model =
        AnnotationSidebarModelBuilder::build(
            annotate_document_, annotate_categories_, annotate_session_,
            has_annotation_frame);
    const AnnotationObjectsTabActions actions =
        draw_annotation_objects_tab(AnnotationObjectsTabState{
            &sidebar_model,
            &annotate_categories_,
            has_annotation_frame,
            assist_available,
            annotate_assist_running_,
            &annotate_pending_class_name_,
            compact_font_,
        });
    apply_annotation_objects_tab_actions(sidebar_model, actions,
                                         assist_available);
    ImGui::EndTabItem();
  }

  if (ImGui::BeginTabItem("Mask")) {
    const AnnotationSidebarViewModel mask_sidebar_model =
        AnnotationSidebarModelBuilder::build(
            annotate_document_, annotate_categories_, annotate_session_,
            annotate_frame_.has_value());
    const AnnotationMaskTabActions actions = draw_annotation_mask_tab(
        AnnotationMaskTabState{
            &mask_sidebar_model,
            has_annotation_frame,
            annotate_cleanup_radius_,
            compact_font_,
        },
        [this](const char *label, AnnotationColorRange &range,
               AnnotationColorRange &sibling_range) {
          draw_annotation_range_controls(label, range, sibling_range);
        });
    apply_annotation_mask_tab_actions(actions);
    ImGui::EndTabItem();
  }

  if (ImGui::BeginTabItem("Save")) {
    draw_annotate_save_tab(live_video);
    ImGui::EndTabItem();
  }

  ImGui::EndTabBar();
}

} // namespace mmltk::gui
