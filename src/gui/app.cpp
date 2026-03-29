#include "app.h"
#include "file_picker.h"
#include "live_preview_texture.h"
#include "rfdetr_module.h"
#include "rfdetr_workflows.h"
#include "source_runtime.h"
#include "ui_controls.h"
#include "ui_style.h"

#include "fastloader/rfdetr/live_predict.h"
#include "fastloader/rfdetr/model_config.h"
#include "fastloader/rfdetr/predict.h"
#include "fastloader/rfdetr/validate.h"
#include "rfdetr/backends.h"
#include "fastloader/rfdetr/model.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace fastloader::gui {

namespace {

using namespace fastloader::rfdetr;
constexpr char kDefaultPresetName[] = "rf-detr-seg-medium";

enum class ModelInputBrowseRequest : int {
    None = 0,
    Weights = 1,
    Onnx = 2,
    TensorRt = 3,
};

const char* compilation_mode_label(int index) {
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

const char* view_label(View view) {
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

const char* model_input_label(ModelInputMode mode) {
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

const char* ui_density_label(const UiDensity density) {
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

const char* train_execution_target_label(TrainExecutionTarget target) {
    switch (target) {
    case TrainExecutionTarget::Local:
        return "Local";
    case TrainExecutionTarget::Remote:
        return "Remote";
    }
    return "Unknown";
}

const char* train_optimizer_label(TrainOptimizerMode mode) {
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

const char* lr_scheduler_label(const std::string& value) {
    return value == "cosine" ? "Cosine" : "Step";
}

void log_gui_error_to_stderr(const char* context, const std::string& message) {
    if (message.empty()) {
        return;
    }
    std::fprintf(stderr, "fastloader gui %s: %s\n", context, message.c_str());
    std::fflush(stderr);
}

void draw_banner(const char* id, const ImVec4& background_color, const ImVec4& text_color, const std::string& message) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background_color);
    ImGui::BeginChild("banner",
                      ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 2.4f),
                      true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    ImGui::TextWrapped("%s", message.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
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

bool draw_train_optimizer_combo(const char* label, TrainOptimizerMode& mode) {
    bool changed = false;
    draw_labeled_combo(label, train_optimizer_label(mode), 180.0f, [&mode, &changed]() {
        for (const auto option : {TrainOptimizerMode::AdamW, TrainOptimizerMode::Muon}) {
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

bool draw_lr_scheduler_combo(const char* label, std::string& scheduler) {
    bool changed = false;
    draw_labeled_combo(label, lr_scheduler_label(scheduler), 180.0f, [&scheduler, &changed]() {
        for (const char* option : {"step", "cosine"}) {
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

ModelInputBrowseRequest draw_model_input_selector(ModelInputMode& mode,
                                                  std::string& weights_path,
                                                  std::string& onnx_path,
                                                  std::string& tensorrt_path,
                                                  bool weights_browse_busy,
                                                  bool onnx_browse_busy,
                                                  bool tensorrt_browse_busy,
                                                  bool allow_none = false) {
    if (!allow_none && mode == ModelInputMode::None) {
        mode = ModelInputMode::Weights;
    }

    draw_labeled_combo("Model Input", model_input_label(mode), 180.0f, [&mode, allow_none]() {
        const auto draw_option = [&mode](ModelInputMode option) {
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

std::string annotate_error() {
    return "Annotate saves full-scene PNG + JSONL labels and per-instance RGBA crops. "
           "Use single image, image folder, or live video sources.";
}

const char* annotation_seed_kind_label(AnnotationSeedKind seed_kind) {
    switch (seed_kind) {
    case AnnotationSeedKind::Box:
        return "Box";
    case AnnotationSeedKind::ModelMask:
        return "Model Mask";
    }
    return "Unknown";
}

std::string annotation_source_signature(const SourceSelectionState& source) {
    std::ostringstream stream;
    stream << static_cast<int>(source.kind)
           << "|" << source.compiled_path
           << "|" << source.single_image_path
           << "|" << source.image_directory
           << "|" << source.recursive
           << "|" << source.device_index
           << "|" << source.capture_width
           << "|" << source.capture_height
           << "|" << source.capture_fps
           << "|" << source.v4l2_buffer_count
           << "|" << source.crop_x
           << "|" << source.crop_y
           << "|" << source.crop_width
           << "|" << source.crop_height;
    return stream.str();
}

ImVec4 annotation_range_swatch_color(const AnnotationColorRange& range) {
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
    return ImVec4(r + match, g + match, b + match, 1.0f);
}

std::string predict_combo_error(const PredictViewState& state) {
    const bool raw_source =
        state.source.kind == SourceKind::SingleImage || state.source.kind == SourceKind::ImageFolder;
    if (raw_source && state.model_input == ModelInputMode::Weights && state.lanes > 1) {
        return "Weights + raw-image predict requires lanes <= 1.";
    }
    return {};
}

std::string live_predict_blocker(const PredictViewState& state, const JobState& job) {
    if (state.source.kind != SourceKind::VideoStream) {
        return {};
    }
    if (job.running) {
        return job.label.empty()
                   ? "Finish the active workflow before starting live prediction."
                   : "Finish the active workflow before starting live prediction: " + job.label + ".";
    }
    const std::string source_error = validate_predict_source(state.source);
    if (!source_error.empty()) {
        return source_error;
    }
    if (!live_capture_supported()) {
        return "RF-DETR live video predict is not available in this build.";
    }
    return {};
}

void draw_section_heading(const char* label) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.87f, 0.89f, 0.92f, 1.00f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

template <typename DrawFn>
void draw_with_optional_font(ImFont* font, DrawFn&& draw_fn) {
    if (font != nullptr) {
        ImGui::PushFont(font);
    }
    draw_fn();
    if (font != nullptr) {
        ImGui::PopFont();
    }
}

void set_rectangle_drag_cursor(RectDragKind kind) {
    if (kind == RectDragKind::ResizeTopLeft || kind == RectDragKind::ResizeBottomRight) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    } else if (kind == RectDragKind::ResizeTopRight || kind == RectDragKind::ResizeBottomLeft) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    } else if (kind == RectDragKind::Move) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
}

bool annotation_boxes_equal(const AnnotationBox& lhs, const AnnotationBox& rhs) {
    return lhs.x1 == rhs.x1 &&
           lhs.y1 == rhs.y1 &&
           lhs.x2 == rhs.x2 &&
           lhs.y2 == rhs.y2;
}

bool annotation_box_contains_point(const AnnotationBox& box, int x, int y) {
    return annotation_box_has_area(box) &&
           x >= box.x1 && x < box.x2 &&
           y >= box.y1 && y < box.y2;
}

template <typename SampleFn>
void rasterize_line_samples(int x0, int y0, int x1, int y1, SampleFn&& sample_fn) {
    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const int steps = std::max(dx, dy);
    if (steps == 0) {
        sample_fn(x0, y0);
        return;
    }
    for (int step = 0; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const int x = static_cast<int>(std::round(static_cast<float>(x0) + static_cast<float>(x1 - x0) * t));
        const int y = static_cast<int>(std::round(static_cast<float>(y0) + static_cast<float>(y1 - y0) * t));
        sample_fn(x, y);
    }
}

float clamp_canvas_pan_axis(float pan, float image_extent, float viewport_extent) {
    if (image_extent <= viewport_extent) {
        return 0.0f;
    }
    return std::clamp(pan, viewport_extent - image_extent, 0.0f);
}

void draw_box_handles(ImDrawList* draw_list,
                      const ImVec2& image_min,
                      float scale_x,
                      float scale_y,
                      const AnnotationBox& box,
                      ImU32 fill_color,
                      ImU32 outline_color) {
    if (draw_list == nullptr || !annotation_box_has_area(box)) {
        return;
    }

    const float handle_radius = 4.0f;
    const ImVec2 corners[4] = {
        ImVec2(image_min.x + scale_x * static_cast<float>(box.x1), image_min.y + scale_y * static_cast<float>(box.y1)),
        ImVec2(image_min.x + scale_x * static_cast<float>(box.x2), image_min.y + scale_y * static_cast<float>(box.y1)),
        ImVec2(image_min.x + scale_x * static_cast<float>(box.x1), image_min.y + scale_y * static_cast<float>(box.y2)),
        ImVec2(image_min.x + scale_x * static_cast<float>(box.x2), image_min.y + scale_y * static_cast<float>(box.y2)),
    };
    for (const ImVec2& corner : corners) {
        draw_list->AddRectFilled(ImVec2(corner.x - handle_radius, corner.y - handle_radius),
                                 ImVec2(corner.x + handle_radius, corner.y + handle_radius),
                                 fill_color,
                                 1.5f);
        draw_list->AddRect(ImVec2(corner.x - handle_radius, corner.y - handle_radius),
                           ImVec2(corner.x + handle_radius, corner.y + handle_radius),
                           outline_color,
                           1.5f,
                           0,
                           1.0f);
    }
}

const ModelPresetConfig& resolve_selected_preset(std::string& preset_name) {
    if (const auto* preset = find_model_preset(preset_name)) {
        return *preset;
    }
    const auto* fallback = find_model_preset(kDefaultPresetName);
    if (fallback == nullptr) {
        throw std::runtime_error("missing default RF-DETR preset");
    }
    preset_name = std::string(fallback->preset_name);
    return *fallback;
}

LivePreviewTextureState snapshot_preview_state(const std::unique_ptr<LivePreviewTexture>& preview_texture) {
    return preview_texture ? preview_texture->snapshot() : LivePreviewTextureState{};
}

std::string format_decimal(double value, int precision) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(precision);
    stream << value;
    return stream.str();
}

TrainRecipeConfig current_train_recipe(const std::string& preset_name, TrainOptimizerMode optimizer) {
    return resolve_train_recipe(preset_name, train_optimizer_kind(optimizer));
}

void update_train_recipe_override(bool& dirty, double current, double recipe_value) {
    dirty = !train_recipe_value_matches(current, recipe_value);
}

void update_train_recipe_override(bool& dirty, int current, int recipe_value) {
    dirty = current != recipe_value;
}

void update_train_recipe_override(bool& dirty, const std::string& current, std::string_view recipe_value) {
    dirty = current != recipe_value;
}

constexpr std::array<RemoteGpuFamily, 5> kRemoteGpuFamilyOrder{
    RemoteGpuFamily::A100,
    RemoteGpuFamily::B200,
    RemoteGpuFamily::H100,
    RemoteGpuFamily::H200,
    RemoteGpuFamily::LSeries,
};

std::string local_gpu_selection_summary(const std::vector<LocalGpuInfo>& gpus, const std::vector<bool>& selected) {
    if (gpus.empty()) {
        return "No visible GPUs";
    }
    size_t selected_count = 0;
    for (size_t index = 0; index < std::min(gpus.size(), selected.size()); ++index) {
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
    return std::to_string(selected_count) + " / " + std::to_string(gpus.size()) + " GPUs selected";
}

std::string format_memory_gib(std::uint64_t bytes) {
    constexpr double gib_divisor = 1024.0 * 1024.0 * 1024.0;
    return format_decimal(static_cast<double>(bytes) / gib_divisor, 1) + " GiB";
}

std::string local_gpu_label(const LocalGpuInfo& gpu) {
    std::ostringstream stream;
    stream << "GPU " << gpu.device_id << " · " << gpu.name;
    if (gpu.total_memory_bytes > 0) {
        stream << " · " << format_memory_gib(gpu.total_memory_bytes);
    }
    return stream.str();
}

std::string armed_remote_offer_summary(const VastOfferSummary& offer) {
    std::ostringstream stream;
    stream << remote_gpu_family_label(offer.family)
           << " · " << offer.gpu_name
           << " · " << offer.num_gpus << " GPUs"
           << " · DLPerf/$ " << format_decimal(offer.dlperf_usd, 2)
           << " · $" << format_decimal(offer.dph, 2) << "/hr";
    return stream.str();
}

void draw_remote_offer_card(const VastOfferSummary& offer, bool selected) {
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.20f, 0.28f, 0.21f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.66f, 0.34f, 1.00f));
    }
    ImGui::BeginChild("offer_card",
                      ImVec2(0.0f, 88.0f),
                      true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::Text("%s · %d GPUs · %s",
                offer.gpu_name.c_str(),
                offer.num_gpus,
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

struct PreparedAnnotationPreview {
    AnnotationPreviewResult preview;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t frame_id = 0;
};

struct LivePredictStartOutcome {
    std::unique_ptr<LivePredictSession> session;
    fastloader::rfdetr::LivePredictStatus status;
    int device_id = 0;
};

void trim_output_tail(std::string& output_tail, std::size_t max_size = 8192) {
    if (output_tail.size() > max_size) {
        output_tail.erase(0, output_tail.size() - max_size);
    }
}

void append_console_output(std::string& tail, const std::string_view chunk, std::size_t max_size = 8192) {
    size_t index = 0;
    while (index < chunk.size()) {
        const char ch = chunk[index];
        if (ch == '\r') {
            const size_t newline = tail.rfind('\n');
            if (newline == std::string::npos) {
                tail.clear();
            } else {
                tail.erase(newline + 1);
            }
            ++index;
            continue;
        }
        if (ch == '\b') {
            if (!tail.empty()) {
                tail.pop_back();
            }
            ++index;
            continue;
        }
        if (ch == '\033') {
            ++index;
            if (index < chunk.size() && chunk[index] == '[') {
                ++index;
                while (index < chunk.size()) {
                    const unsigned char code = static_cast<unsigned char>(chunk[index++]);
                    if (code >= 0x40 && code <= 0x7E) {
                        break;
                    }
                }
            }
            continue;
        }
        tail.push_back(ch);
        ++index;
    }
    trim_output_tail(tail, max_size);
}

void draw_live_output_console(const char* id,
                              const std::string& output_tail,
                              float height,
                              bool running,
                              const char* waiting_message) {
    ImGui::TextUnformatted(running ? "Live Output" : "Recent Output");
    ImGui::BeginChild(id, ImVec2(0.0f, height), true);
    const bool stick_to_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f;
    ImGui::PushTextWrapPos();
    if (current_ui_fonts().mono != nullptr) {
        ImGui::PushFont(current_ui_fonts().mono);
    }
    if (output_tail.empty()) {
        ImGui::TextUnformatted(waiting_message);
    } else {
        ImGui::TextUnformatted(output_tail.c_str());
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

std::filesystem::path default_vast_python_executable() {
#ifdef FASTLOADER_GUI_PYTHON_EXECUTABLE
    return FASTLOADER_GUI_PYTHON_EXECUTABLE;
#else
    return "python3";
#endif
}

std::filesystem::path default_vast_bridge_script_path() {
#ifdef FASTLOADER_GUI_VAST_BRIDGE_SCRIPT
    return FASTLOADER_GUI_VAST_BRIDGE_SCRIPT;
#else
    return "utilities/vast_bridge.py";
#endif
}

} // namespace

App::App(GLFWwindow* main_window, std::string vast_api_key)
    : vast_api_key_(std::move(vast_api_key)) {
    model_registry_.register_module(make_rfdetr_model_module());
    selected_preset_name_ = kDefaultPresetName;

    train_.train_compiled_path = "./compiled-seg-medium-synth/train.bin";
    train_.val_compiled_path = "./compiled-seg-medium-synth/val.bin";
    train_.output_dir = "./engines/output-seg-medium/train-local";
    train_.weights_path = "./engines/output-seg-medium/train-local/checkpoint_best_regular.pt";
    train_.epochs = 12;
    train_.batch_size = 2;
    train_.grad_accum_steps = 1;
    train_.workers = 16;
    train_.prefetch_factor = 3;
    train_.progress_bar = true;
    train_.lanes = 3;
    train_.val_batch_size = 8;

    validate_.compiled_path = "./compiled-seg-medium-synth/val.bin";
    predict_.source.compiled_path = "./compiled-seg-medium-synth/val.bin";
    predict_.weights_path = train_.weights_path;
    annotate_.weights_path = train_.weights_path;

    predict_.source.kind = SourceKind::CompiledDataset;
    annotate_.source.kind = SourceKind::ImageFolder;
    annotate_.backend = "auto";
    {
        GuiSettingsSnapshot snap{current_view_, selected_preset_name_, &ui_settings_,
                                 &train_, &validate_, &predict_, &annotate_, &export_};
        if (settings_persistence_.load(snap)) {
            current_view_ = snap.current_view;
            selected_preset_name_ = snap.selected_preset;
        }
    }
    apply_ui_settings_now(true);
    apply_selected_preset_defaults();
    local_train_session_ = std::make_unique<LocalTrainSession>();
    refresh_local_gpus();

    live_preview_texture_ = std::make_unique<LivePreviewTexture>(main_window);
    std::string preview_initialize_error;
    if (!live_preview_texture_->initialize(&preview_initialize_error)) {
        live_preview_error_ = std::move(preview_initialize_error);
        log_gui_error_to_stderr("preview init error", live_preview_error_);
    }
}

App::~App() {
    shutdown();
}

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
    if (local_train_session_) {
        local_train_session_->shutdown();
        train_process_ = local_train_session_->snapshot();
    }
    settings_persistence_.flush();
    background_executor_.wait_idle();
    ui_callbacks_.drain();
    stop_annotation_live_session();
    if (live_preview_texture_) {
        live_preview_texture_->shutdown();
    }
}

void App::apply_style() {
    apply_ui_settings(UiSettingsState{}, false);
}

void App::poll_background_work() {
    if (ui_settings_apply_pending_) {
        apply_ui_settings_now(true);
        ui_settings_apply_pending_ = false;
    }
    ui_callbacks_.drain();
    if (live_preview_texture_) {
        live_preview_texture_->pump();
    }
    if (local_train_session_) {
        train_process_ = local_train_session_->snapshot();
    }
    {
        GuiSettingsSnapshot snap{current_view_, selected_preset_name_, &ui_settings_,
                                 &train_, &validate_, &predict_, &annotate_, &export_};
        settings_persistence_.notify_frame(snap);
    }
    const LivePreviewTextureState preview_state = snapshot_preview_state(live_preview_texture_);
    live_preview_error_ = preview_state.last_error;
    poll_annotate_work();

    if (live_predict_session_) {
        if (!live_predict_status_) {
            live_predict_status_ = std::make_unique<fastloader::rfdetr::LivePredictStatus>();
        }
        *live_predict_status_ = live_predict_session_->snapshot_status();
    }
}

void App::launch_job(const std::string& label, std::function<JobOutcome()> fn) {
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
    fastloader::runtime::submit_background_task(
        background_executor_,
        ui_callbacks_,
        [this, task = std::move(fn)]() mutable {
            rfdetr::ScopedTensorRtLogSink trt_log_sink([this](const std::string& line) {
                append_job_output(line);
            });
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
        [this](const std::string& error) {
            job_.last_error = error;
            job_.last_summary.clear();
            job_.running = false;
            log_gui_error_to_stderr("job error", job_.last_error);
        });
}

void App::launch_file_picker(FileBrowseField field, const char* dialog_title, std::string* target) {
    if (active_file_browse_field_ != FileBrowseField::None || target == nullptr || shutting_down_) {
        return;
    }
    active_file_browse_field_ = field;
    picker_error_.clear();
    fastloader::runtime::submit_background_task(
        background_executor_,
        ui_callbacks_,
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
        [this, field](const std::string& error) {
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
    append_console_output(job_.output_tail, chunk, 65536);
    if (job_.output_tail.empty() || job_.output_tail.back() != '\n') {
        job_.output_tail.push_back('\n');
        trim_output_tail(job_.output_tail, 65536);
    }
}

std::string App::snapshot_job_output() {
    std::lock_guard<std::mutex> lock(job_.output_mutex);
    return job_.output_tail;
}

AnnotationInstance* App::selected_annotation_instance() {
    if (!annotate_selected_instance_.has_value() ||
        *annotate_selected_instance_ >= annotate_instances_.size()) {
        return nullptr;
    }
    return &annotate_instances_[*annotate_selected_instance_];
}

void App::draw_annotation_range_controls(const char* label,
                                         AnnotationColorRange& range,
                                         AnnotationColorRange& sibling_range) {
    ImGui::PushID(label);
    const AnnotationColorRange original = range;
    const bool compact_layout = ImGui::GetContentRegionAvail().x < 520.0f;
    const float slider_width = compact_layout ? -FLT_MIN : 160.0f;
    draw_section_heading(label);
    const ImVec4 swatch_color = annotation_range_swatch_color(range);
    const ImVec2 swatch_size(96.0f, 22.0f);
    if (range.sampling) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.86f, 0.60f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.67f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.74f, 0.50f, 0.11f, 1.0f));
        (void)ImGui::Button("Eyedrop", swatch_size);
        ImGui::PopStyleColor(3);
    } else {
        (void)ImGui::ColorButton("##swatch", swatch_color, ImGuiColorEditFlags_NoTooltip, swatch_size);
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
                range.center.hue_degrees,
                range.center.saturation * 100.0f,
                range.center.value * 100.0f);
    draw_labeled_percent_slider("H-", range.tolerance.hue_minus_pct, slider_width);
    draw_labeled_percent_slider("H+", range.tolerance.hue_plus_pct, slider_width);
    draw_labeled_percent_slider("S-", range.tolerance.saturation_minus_pct, slider_width);
    draw_labeled_percent_slider("S+", range.tolerance.saturation_plus_pct, slider_width);
    draw_labeled_percent_slider("V-", range.tolerance.value_minus_pct, slider_width);
    draw_labeled_percent_slider("V+", range.tolerance.value_plus_pct, slider_width);
    const bool changed =
        original.sampling != range.sampling ||
        original.center.hue_degrees != range.center.hue_degrees ||
        original.center.saturation != range.center.saturation ||
        original.center.value != range.center.value ||
        original.tolerance.hue_minus_pct != range.tolerance.hue_minus_pct ||
        original.tolerance.hue_plus_pct != range.tolerance.hue_plus_pct ||
        original.tolerance.saturation_minus_pct != range.tolerance.saturation_minus_pct ||
        original.tolerance.saturation_plus_pct != range.tolerance.saturation_plus_pct ||
        original.tolerance.value_minus_pct != range.tolerance.value_minus_pct ||
        original.tolerance.value_plus_pct != range.tolerance.value_plus_pct;
    if (changed) {
        invalidate_annotation_preview();
    }
    ImGui::PopID();
}

void App::invalidate_annotation_preview() {
    annotate_preview_dirty_ = true;
    ++annotate_preview_generation_;
    annotate_preview_error_count_ = 0;
}

void App::clear_annotation_geometry() {
    annotate_geometry_.reset();
    annotate_direct_drag_state_ = {};
    annotate_create_drag_state_ = {};
    annotate_painting_ = false;
}

bool App::ensure_annotation_geometry() {
    if (!annotate_frame_.has_value()) {
        annotate_geometry_.reset();
        return false;
    }
    if (annotate_instances_.empty()) {
        annotate_geometry_.reset();
        return false;
    }
    const std::uint32_t capture_width = annotation_frame_capture_width(*annotate_frame_);
    const std::uint32_t capture_height = annotation_frame_capture_height(*annotate_frame_);
    const bool needs_rebuild =
        !annotate_geometry_ ||
        annotate_geometry_->capture_width() != capture_width ||
        annotate_geometry_->capture_height() != capture_height ||
        annotate_geometry_->size() != annotate_instances_.size();
    if (!needs_rebuild) {
        return true;
    }
    try {
        annotate_geometry_ =
            std::make_unique<AnnotationGeometryDocument>(annotate_.device_id, capture_width, capture_height);
        annotate_geometry_->import_instances(annotate_instances_);
        return true;
    } catch (const std::exception& error) {
        annotate_geometry_.reset();
        annotate_save_error_ = error.what();
        log_gui_error_to_stderr("annotation geometry error", annotate_save_error_);
        return false;
    }
}

void App::sync_annotation_instance_from_geometry(std::size_t index) {
    if (!annotate_geometry_ || index >= annotate_instances_.size() || index >= annotate_geometry_->size()) {
        return;
    }
    annotate_geometry_->export_instance(index, &annotate_instances_[index]);
}

void App::sync_all_annotation_instances_from_geometry() {
    if (!annotate_geometry_) {
        return;
    }
    const std::size_t limit = std::min(annotate_instances_.size(), annotate_geometry_->size());
    for (std::size_t index = 0; index < limit; ++index) {
        annotate_geometry_->export_instance(index, &annotate_instances_[index]);
    }
}

void App::reset_annotation_instances() {
    annotate_instances_.clear();
    annotate_resolved_instances_.clear();
    annotate_selected_instance_.reset();
    clear_annotation_geometry();
    clear_rect_layer_state(annotate_canvas_layer_state_);
    invalidate_annotation_preview();
}

void App::clear_annotation_save_queue() {
    annotate_queued_save_frame_.reset();
    annotate_queued_save_instances_.clear();
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
        AnnotationCategories loaded_categories = load_annotation_categories(annotate_.output_dir);
        annotate_categories_ = std::move(loaded_categories);
        annotate_categories_loaded_ = true;
        annotate_categories_output_dir_ = annotate_.output_dir;
        if (!annotate_categories_.items.empty()) {
            for (AnnotationInstance& instance : annotate_instances_) {
                if (instance.category_index >= annotate_categories_.items.size()) {
                    instance.category_index = 0U;
                }
            }
        }
    } catch (const std::exception& error) {
        annotate_categories_loaded_ = true;
        annotate_categories_output_dir_ = annotate_.output_dir;
        annotate_save_error_ = error.what();
        log_gui_error_to_stderr("annotate categories error", annotate_save_error_);
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
    annotate_preview_running_ = false;
    annotate_preview_frame_id_ = 0;
    annotate_last_saved_frame_id_ = 0;
    annotate_assist_summary_.clear();
    annotate_assist_error_.clear();
    annotate_save_summary_.clear();
    annotate_save_error_.clear();
    annotate_hold_save_blocked_ = false;
    clear_annotation_save_queue();
    reset_annotation_instances();
    if (live_preview_texture_) {
        live_preview_texture_->clear_frame();
    }

    if (annotate_.source.kind == SourceKind::VideoStream) {
        stop_annotation_live_session();
        return;
    }
    stop_annotation_live_session();

    if (annotate_.source.kind == SourceKind::CompiledDataset) {
        annotate_save_error_ = "Annotate only supports single images, image folders, and live video.";
        return;
    }

    const SourceSelectionState source = annotate_.source;
    const std::uint64_t request_id = annotate_prepare_request_id_;
    annotate_prepare_running_ = true;
    fastloader::runtime::submit_background_task(
        background_executor_,
        ui_callbacks_,
        [source]() {
            PreparedPredictSource prepared = prepare_predict_source(source);
            return std::move(prepared.image_inputs);
        },
        [this, request_id, signature](std::vector<fastloader::rfdetr::PredictImageInput> inputs) {
            if (request_id != annotate_prepare_request_id_ || signature != annotate_source_signature_) {
                return;
            }
            annotate_prepare_running_ = false;
            annotate_inputs_ = std::move(inputs);
            annotate_save_error_.clear();
            if (!annotate_inputs_.empty()) {
                load_annotation_current_frame();
            }
        },
        [this, request_id, signature](const std::string& error) {
            if (request_id != annotate_prepare_request_id_ || signature != annotate_source_signature_) {
                return;
            }
            annotate_prepare_running_ = false;
            annotate_save_error_ = error;
            log_gui_error_to_stderr("annotate source error", annotate_save_error_);
        });
}

void App::load_annotation_current_frame() {
    if (annotate_current_input_index_ >= annotate_inputs_.size()) {
        annotate_frame_.reset();
        annotate_resolved_instances_.clear();
        clear_annotation_geometry();
        invalidate_annotation_preview();
        return;
    }
    ++annotate_frame_request_id_;
    const std::uint64_t request_id = annotate_frame_request_id_;
    const fastloader::rfdetr::PredictImageInput input = annotate_inputs_[annotate_current_input_index_];
    annotate_frame_load_running_ = true;
    fastloader::runtime::submit_background_task(
        background_executor_,
        ui_callbacks_,
        [input]() {
            return fastloader::gui::load_annotation_frame(input);
        },
        [this, request_id](AnnotationFrame frame) {
            if (request_id != annotate_frame_request_id_) {
                return;
            }
            annotate_frame_load_running_ = false;
            load_annotation_frame(frame);
            annotate_save_error_.clear();
        },
        [this, request_id](const std::string& error) {
            if (request_id != annotate_frame_request_id_) {
                return;
            }
            annotate_frame_load_running_ = false;
            annotate_frame_.reset();
            annotate_resolved_instances_.clear();
            clear_annotation_geometry();
            invalidate_annotation_preview();
            annotate_save_error_ = error;
            log_gui_error_to_stderr("annotate frame load error", annotate_save_error_);
        });
}

void App::load_annotation_frame(const AnnotationFrame& frame) {
    const bool reset_canvas_view =
        !annotate_frame_.has_value() ||
        annotate_frame_->width != frame.width ||
        annotate_frame_->height != frame.height ||
        annotate_frame_->view_x != frame.view_x ||
        annotate_frame_->view_y != frame.view_y;
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
    annotate_frame_ = frame;
    invalidate_annotation_preview();
}

void App::submit_annotation_preview() {
    if (!annotate_frame_.has_value() || !live_preview_texture_ || annotate_preview_running_) {
        return;
    }
    sync_all_annotation_instances_from_geometry();
    const AnnotationFrame frame_snapshot = *annotate_frame_;
    const AnnotationCategories categories_snapshot = annotate_categories_;
    const std::vector<AnnotationInstance> instances_snapshot = annotate_instances_;
    const bool live_mode = annotate_.source.kind == SourceKind::VideoStream;
    const std::uint64_t generation = annotate_preview_generation_;
    annotate_preview_running_ = true;
    annotate_preview_dirty_ = false;
    fastloader::runtime::submit_background_task(
        background_executor_,
        ui_callbacks_,
        [frame_snapshot, categories_snapshot, instances_snapshot, live_mode]() {
            PreparedAnnotationPreview prepared;
            prepared.preview = build_annotation_preview(frame_snapshot,
                                                       categories_snapshot,
                                                       instances_snapshot,
                                                       live_mode);
            prepared.width = frame_snapshot.width;
            prepared.height = frame_snapshot.height;
            prepared.frame_id = frame_snapshot.frame_id;
            return prepared;
        },
        [this, generation](PreparedAnnotationPreview prepared) mutable {
            annotate_preview_running_ = false;
            annotate_preview_error_count_ = 0;
            if (generation == annotate_preview_generation_ &&
                annotate_frame_.has_value() &&
                annotate_frame_->frame_id == prepared.frame_id &&
                live_preview_texture_) {
                annotate_resolved_instances_ = prepared.preview.resolved_instances;
                const LiveCaptureRegion region{0U, 0U, prepared.width, prepared.height};
                std::string preview_error;
                if (!live_preview_texture_->submit_host_bgr(std::move(prepared.preview.preview_bgr),
                                                            prepared.width,
                                                            prepared.height,
                                                            region,
                                                            prepared.frame_id,
                                                            &preview_error)) {
                    live_preview_error_ = std::move(preview_error);
                    log_gui_error_to_stderr("annotate preview error", live_preview_error_);
                } else {
                    live_preview_error_.clear();
                    annotate_preview_frame_id_ = prepared.frame_id;
                }
            }
            if (annotate_frame_.has_value() &&
                (annotate_preview_dirty_ || annotate_preview_frame_id_ != annotate_frame_->frame_id)) {
                submit_annotation_preview();
            }
        },
        [this](const std::string& error) {
            annotate_preview_running_ = false;
            annotate_save_error_ = error;
            annotate_resolved_instances_.clear();
            live_preview_error_ = error;
            ++annotate_preview_error_count_;
            constexpr int kMaxPreviewRetries = 3;
            if (annotate_preview_error_count_ < kMaxPreviewRetries &&
                annotate_frame_.has_value() &&
                (annotate_preview_dirty_ || annotate_preview_frame_id_ != annotate_frame_->frame_id)) {
                submit_annotation_preview();
            }
        });
}

void App::start_annotation_live_session() {
    if (annotate_.source.kind != SourceKind::VideoStream) {
        throw std::runtime_error("annotation live capture requires a video source");
    }
    stop_annotation_live_session();
    if (live_preview_texture_) {
        live_preview_texture_->clear_frame();
    }
    reset_annotation_instances();
    clear_annotation_save_queue();
    annotate_frame_.reset();
    annotate_live_session_ = std::make_unique<AnnotationLiveCaptureSession>();
    annotate_live_session_->start(annotate_.source, annotate_.device_id, annotate_.full_frame);
}

void App::stop_annotation_live_session() {
    annotate_hold_save_ = false;
    annotate_hold_save_blocked_ = false;
    clear_annotation_geometry();
    clear_rect_layer_state(annotate_canvas_layer_state_);
    clear_annotation_save_queue();
    if (!annotate_live_session_) {
        return;
    }
    annotate_live_session_->stop();
    annotate_live_session_.reset();
}

bool App::annotation_live_running() const {
    return annotate_live_session_ != nullptr;
}

void App::launch_annotation_assist() {
    if (!annotate_frame_.has_value() || annotate_assist_running_) {
        return;
    }
    if (annotate_.model_input == ModelInputMode::None) {
        annotate_assist_error_ =
            "Annotation assist requires model backing. Use None for manual full-box masks, or select Weights, ONNX, or TensorRT.";
        annotate_assist_summary_.clear();
        return;
    }
    const AnnotationFrame frame_snapshot = *annotate_frame_;
    const AnnotateViewState state = annotate_;
    const std::string preset_name = selected_preset_name_;
    AnnotationFrame assist_frame;
    try {
        assist_frame = state.source.kind == SourceKind::VideoStream
                           ? extract_annotation_frame_region(frame_snapshot, resolved_video_crop_box(state.source))
                           : frame_snapshot;
    } catch (const std::exception& error) {
        annotate_assist_error_ = error.what();
        annotate_assist_summary_.clear();
        return;
    }
    annotate_assist_running_ = true;
    annotate_assist_error_.clear();
    annotate_assist_summary_.clear();
    fastloader::runtime::submit_background_task(
        background_executor_,
        ui_callbacks_,
        [assist_frame, state, preset_name]() {
            const std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
            const std::filesystem::path temp_path =
                temp_dir / ("fastloader-annotate-" + std::to_string(assist_frame.frame_id) + ".png");
            try {
                write_annotation_frame_png(temp_path, assist_frame);
                fastloader::rfdetr::PredictImageInput input;
                input.image_path = temp_path;
                input.source_name = assist_frame.source_name;
                input.image_id = static_cast<int64_t>(assist_frame.frame_id);
                const PredictOptions options =
                    rfdetr_workflows::build_annotate_predict_options(state, preset_name, std::move(input));
                const PredictionRunResult result = run_prediction(options);
                std::error_code ignored_error;
                std::filesystem::remove(temp_path, ignored_error);

                if (result.records.size() != 1U) {
                    throw std::runtime_error("annotation assist expected exactly one prediction record");
                }

                AnnotateAssistOutcome outcome;
                outcome.summary = rfdetr_workflows::summarize_prediction_result(result);
                for (const Prediction& prediction : result.records.front().detections) {
                    if (prediction.category_id <= 0) {
                        continue;
                    }
                    AnnotateAssistSeed seed;
                    const std::size_t class_index = static_cast<std::size_t>(prediction.category_id - 1);
                    if (class_index < result.class_names.size()) {
                        seed.class_name = result.class_names[class_index];
                    } else {
                        seed.class_name = std::to_string(prediction.category_id);
                    }
                    const AnnotationBox local_box = rfdetr_workflows::prediction_to_annotation_box(
                        prediction,
                        assist_frame.width,
                        assist_frame.height);
                    seed.box = annotation_box_from_frame(assist_frame, local_box);
                    if (prediction.has_mask) {
                        seed.mask = decode_annotation_prediction_mask(prediction.mask,
                                                                     assist_frame.width,
                                                                     assist_frame.height);
                        seed.mask_region = annotation_mask_region_from_frame(assist_frame);
                        seed.seed_frame_id = assist_frame.frame_id;
                        seed.has_mask = true;
                    }
                    outcome.seeds.push_back(std::move(seed));
                }
                return outcome;
            } catch (...) {
                std::error_code ignored_error;
                std::filesystem::remove(temp_path, ignored_error);
                throw;
            }
        },
        [this](const AnnotateAssistOutcome& outcome) {
            annotate_assist_summary_ = outcome.summary;
            annotate_assist_error_.clear();
            const std::size_t first_new_index = annotate_instances_.size();
            for (std::size_t index = 0; index < outcome.seeds.size(); ++index) {
                const AnnotateAssistSeed& seed = outcome.seeds[index];
                AnnotationInstance instance;
                instance.instance_id = "assist-" + std::to_string(first_new_index + index + 1U);
                instance.category_index = ensure_annotation_category(annotate_categories_, seed.class_name);
                instance.box = seed.box;
                if (seed.has_mask) {
                    instance.seed_kind = AnnotationSeedKind::ModelMask;
                    instance.seed_mask = seed.mask;
                    instance.seed_mask_region = seed.mask_region;
                    instance.seed_frame_id = seed.seed_frame_id;
                }
                annotate_instances_.push_back(std::move(instance));
            }
            clear_annotation_geometry();
            if (first_new_index < annotate_instances_.size()) {
                annotate_selected_instance_ = first_new_index;
            }
            annotate_assist_running_ = false;
            invalidate_annotation_preview();
        },
        [this](const std::string& error) {
            annotate_assist_error_ = error;
            annotate_assist_summary_.clear();
            annotate_assist_running_ = false;
            log_gui_error_to_stderr("annotate assist error", annotate_assist_error_);
        });
}

void App::launch_annotation_save(const AnnotationFrame& frame_snapshot,
                                 const std::vector<AnnotationResolvedInstance>& resolved_instances_snapshot) {
    if (annotate_save_running_) {
        return;
    }
    if (annotate_.output_dir.empty()) {
        annotate_save_error_ = "annotation output root must not be empty";
        return;
    }
    sync_annotation_categories();
    const AnnotationSaveConfig config{
        annotate_.output_dir,
        annotate_.split.empty() ? std::string("train") : annotate_.split,
    };
    AnnotationCategories categories_snapshot = annotate_categories_;
    annotate_save_running_ = true;
    annotate_save_error_.clear();
    fastloader::runtime::submit_background_task(
        background_executor_,
        ui_callbacks_,
        [config, frame_snapshot, resolved_instances_snapshot, categories_snapshot]() mutable {
            AnnotateSaveOutcome outcome;
            outcome.categories = std::move(categories_snapshot);
            outcome.save = save_annotation_scene(config,
                                                frame_snapshot,
                                                outcome.categories,
                                                resolved_instances_snapshot);
            outcome.summary =
                rfdetr_workflows::summarize_annotation_save_result(outcome.save);
            return outcome;
        },
        [this](AnnotateSaveOutcome outcome) mutable {
            annotate_categories_ = std::move(outcome.categories);
            annotate_categories_loaded_ = true;
            annotate_categories_output_dir_ = annotate_.output_dir;
            annotate_save_summary_ = std::move(outcome.summary);
            annotate_save_error_.clear();
            annotate_save_running_ = false;
        },
        [this](const std::string& error) {
            annotate_save_error_ = error;
            annotate_save_summary_.clear();
            annotate_save_running_ = false;
            log_gui_error_to_stderr("annotate save error", annotate_save_error_);
        });
}

void App::maybe_start_annotation_hold_save() {
    if (annotate_.source.kind != SourceKind::VideoStream) {
        clear_annotation_save_queue();
        return;
    }
    if (!annotate_hold_save_) {
        clear_annotation_save_queue();
        return;
    }
    if (!annotate_frame_.has_value() ||
        annotate_resolved_instances_.empty() ||
        annotate_frame_->frame_id == annotate_last_saved_frame_id_) {
        return;
    }
    if (annotate_save_running_) {
        if (!annotate_queued_save_frame_.has_value()) {
            annotate_queued_save_frame_ = *annotate_frame_;
            annotate_queued_save_instances_ = annotate_resolved_instances_;
            return;
        }
        if (annotate_queued_save_frame_->frame_id != annotate_frame_->frame_id) {
            annotate_hold_save_ = false;
            annotate_hold_save_blocked_ = true;
            clear_annotation_save_queue();
            annotate_save_error_ =
                "Hold Save overflowed the single-frame writer queue. Capture paused to avoid dropping frames silently.";
            log_gui_error_to_stderr("annotate hold-save overflow", annotate_save_error_);
        }
        return;
    }
    const std::uint64_t frame_id = annotate_frame_->frame_id;
    launch_annotation_save(*annotate_frame_, annotate_resolved_instances_);
    if (annotate_save_running_) {
        annotate_last_saved_frame_id_ = frame_id;
    }
}

void App::poll_annotate_work() {
    if (annotate_live_session_) {
        const AnnotationLiveCaptureSnapshot snapshot = annotate_live_session_->snapshot();
        if (!snapshot.last_error.empty()) {
            annotate_save_error_ = snapshot.last_error;
        }
        if (snapshot.has_frame &&
            (!annotate_frame_.has_value() ||
             snapshot.frame.frame_id != annotate_frame_->frame_id ||
             snapshot.frame.view_x != annotate_frame_->view_x ||
             snapshot.frame.view_y != annotate_frame_->view_y ||
             snapshot.frame.width != annotate_frame_->width ||
             snapshot.frame.height != annotate_frame_->height)) {
            load_annotation_frame(snapshot.frame);
        }
        if (!snapshot.running && !snapshot.last_error.empty()) {
            stop_annotation_live_session();
        }
    }

    if (!annotate_save_running_ && annotate_queued_save_frame_.has_value()) {
        if (annotate_hold_save_) {
            AnnotationFrame queued_frame = *annotate_queued_save_frame_;
            std::vector<AnnotationResolvedInstance> queued_instances = annotate_queued_save_instances_;
            clear_annotation_save_queue();
            if (!queued_instances.empty()) {
                launch_annotation_save(queued_frame, queued_instances);
                if (annotate_save_running_) {
                    annotate_last_saved_frame_id_ = queued_frame.frame_id;
                }
            }
        } else {
            clear_annotation_save_queue();
        }
    }

    maybe_start_annotation_hold_save();
}

void App::refresh_local_gpus() {
    if (local_gpu_refresh_running_ || shutting_down_) {
        return;
    }
    local_gpu_refresh_running_ = true;
    local_gpu_error_.clear();
    fastloader::runtime::submit_background_task(
        background_executor_,
        ui_callbacks_,
        []() {
            std::string error;
            std::vector<LocalGpuInfo> refreshed = enumerate_local_gpus(&error);
            return std::make_pair(std::move(refreshed), std::move(error));
        },
        [this](std::pair<std::vector<LocalGpuInfo>, std::string> result) {
            std::vector<LocalGpuInfo> refreshed = std::move(result.first);
            local_gpu_error_ = std::move(result.second);

            std::vector<bool> selected(refreshed.size(), true);
            for (size_t index = 0; index < refreshed.size(); ++index) {
                const auto found = std::find_if(local_gpus_.begin(), local_gpus_.end(), [&](const LocalGpuInfo& info) {
                    return info.device_id == refreshed[index].device_id;
                });
                if (found == local_gpus_.end()) {
                    continue;
                }
                const size_t previous_index = static_cast<size_t>(std::distance(local_gpus_.begin(), found));
                if (previous_index < local_gpu_selected_.size()) {
                    selected[index] = local_gpu_selected_[previous_index];
                }
            }
            local_gpus_ = std::move(refreshed);
            local_gpu_selected_ = std::move(selected);
            local_gpu_refresh_running_ = false;
        },
        [this](const std::string& error) {
            local_gpu_error_ = error;
            local_gpu_refresh_running_ = false;
        });
}

std::vector<int> App::selected_local_device_ids() const {
    std::vector<int> device_ids;
    for (size_t index = 0; index < std::min(local_gpus_.size(), local_gpu_selected_.size()); ++index) {
        if (local_gpu_selected_[index]) {
            device_ids.push_back(local_gpus_[index].device_id);
        }
    }
    return device_ids;
}

std::vector<RemoteGpuFamily> App::selected_remote_gpu_families() const {
    std::vector<RemoteGpuFamily> families;
    for (int index = 0; index < static_cast<int>(train_.remote_family_enabled.size()); ++index) {
        if (!train_.remote_family_enabled[static_cast<size_t>(index)]) {
            continue;
        }
        families.push_back(static_cast<RemoteGpuFamily>(index));
    }
    return families;
}

void App::launch_remote_query() {
    if (remote_query_.running) {
        return;
    }
    remote_query_.running = true;
    remote_query_.last_error.clear();
    remote_query_.last_summary.clear();
    remote_query_.results.clear();
    armed_remote_offer_.reset();

    VastQueryConfig config;
    config.python_executable = default_vast_python_executable();
    config.bridge_script_path = default_vast_bridge_script_path();
    config.api_key = vast_api_key_;
    config.min_gpus = 4;
    config.result_limit = 2;
    const std::vector<RemoteGpuFamily> families = selected_remote_gpu_families();
    fastloader::runtime::submit_background_task(
        background_executor_,
        ui_callbacks_,
        [config, families]() {
            return query_vast_offers(config, families);
        },
        [this](std::vector<VastOfferSummary> results) {
            remote_query_.results = std::move(results);
            remote_query_.last_error.clear();
            remote_query_.last_summary = remote_query_.results.empty()
                                             ? "query completed: no matching offers"
                                             : "query completed: " + std::to_string(remote_query_.results.size()) + " offers";
            remote_query_.running = false;
        },
        [this](const std::string& error) {
            remote_query_.last_error = error;
            remote_query_.last_summary.clear();
            remote_query_.running = false;
            log_gui_error_to_stderr("vast query error", remote_query_.last_error);
        });
}

void App::launch_local_training() {
    if (local_train_session_ && local_train_session_->running()) {
        return;
    }

    const std::vector<int> device_ids = selected_local_device_ids();
    const TrainCommandConfig config = rfdetr_workflows::build_train_command_config(train_, device_ids);
    const std::filesystem::path current_executable = current_executable_path();
    const std::filesystem::path cli_path = resolve_sibling_fastloader_cli(current_executable);
    if (!local_train_session_) {
        local_train_session_ = std::make_unique<LocalTrainSession>();
    }
    local_train_session_->start(config, cli_path);
    train_process_ = local_train_session_->snapshot();
}

void App::request_stop_local_training(bool force) {
    if (!local_train_session_ || !local_train_session_->running()) {
        return;
    }
    local_train_session_->request_stop(force);
    train_process_ = local_train_session_->snapshot();
}

void App::render() {
    draw_menu_bar();
    draw_workflow_window();
    draw_settings_window();
}

bool App::live_predict_running() const {
    return live_predict_session_ != nullptr || live_predict_starting_ || live_predict_stopping_;
}

bool App::live_predict_active() const {
    if (live_predict_starting_ || live_predict_stopping_) {
        return true;
    }
    if (!live_predict_status_) {
        return false;
    }
    return live_predict_status_->worker_running ||
           live_predict_status_->capture_running ||
           live_predict_status_->model_loading ||
           live_predict_status_->model_hot;
}

bool App::has_static_preview() const {
    const LivePreviewTextureState preview_state = snapshot_preview_state(live_preview_texture_);
    return !live_predict_running() &&
           preview_state.has_frame &&
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
    live_predict_action_error_.clear();
    live_preview_error_.clear();
}

void App::apply_static_preview(StillImagePreview preview) {
    if (live_predict_running()) {
        throw std::runtime_error("cannot apply a static preview while live predict is running");
    }
    if (!live_preview_texture_) {
        throw std::runtime_error("preview uploader is not available");
    }

    const LiveCaptureRegion region{0U, 0U, preview.width, preview.height};
    std::string preview_error;
    if (!live_preview_texture_->submit_host_bgr(std::move(preview.pixels_bgr),
                                                preview.width,
                                                preview.height,
                                                region,
                                                1U,
                                                &preview_error)) {
        live_preview_error_ = std::move(preview_error);
        log_gui_error_to_stderr("preview error", live_preview_error_);
        throw std::runtime_error(live_preview_error_.empty() ? "failed to upload static preview" : live_preview_error_);
    }

    live_static_preview_source_name_ = preview.source_name;
    live_predict_start_error_.clear();
    live_predict_action_error_.clear();
    live_preview_error_.clear();
}

void App::launch_live_predict_session() {
    if (live_predict_running() || shutting_down_) {
        return;
    }

    clear_static_preview();
    live_predict_start_error_.clear();
    live_predict_action_error_.clear();
    live_preview_error_.clear();
    live_predict_starting_ = true;
    const PredictViewState state = predict_;
    const std::string preset_name = selected_preset_name_;
    live_predict_start_task_ = fastloader::runtime::submit_background_task(
        background_executor_,
        ui_callbacks_,
        [state, preset_name]() {
            LivePredictStartOutcome outcome;
            outcome.device_id = state.device_id;
            outcome.session =
                std::make_unique<LivePredictSession>(rfdetr_workflows::build_live_predict_options(state, preset_name));
            outcome.session->start();
            outcome.status = outcome.session->snapshot_status();
            return outcome;
        },
        [this](LivePredictStartOutcome outcome) mutable {
            live_predict_starting_ = false;
            live_predict_session_ = std::move(outcome.session);
            live_predict_status_ = std::make_unique<fastloader::rfdetr::LivePredictStatus>(std::move(outcome.status));
            if (live_preview_texture_ && live_predict_session_) {
                live_preview_texture_->clear_frame();
                live_preview_texture_->begin_live_stream(*live_predict_session_, outcome.device_id);
            }
            apply_live_crop_region();
            live_predict_start_error_.clear();
            live_predict_action_error_.clear();
            current_view_ = View::Live;
        },
        [this](const std::string& error) {
            live_predict_starting_ = false;
            live_predict_start_error_ = error;
            log_gui_error_to_stderr("live start error", live_predict_start_error_);
        });
}

void App::stop_live_predict_session() {
    if (live_predict_starting_) {
        live_predict_start_task_.cancel();
        live_predict_starting_ = false;
        live_predict_start_error_.clear();
    }
    if (live_predict_stopping_) {
        return;
    }
    if (!live_predict_session_) {
        if (!live_predict_running() && live_preview_texture_) {
            live_preview_texture_->clear_frame();
        }
        live_static_preview_source_name_.clear();
        live_preview_error_.clear();
        live_predict_status_.reset();
        live_predict_start_error_.clear();
        live_predict_action_error_.clear();
        live_crop_overlay_mode_ = false;
        clear_live_crop_draft();
        return;
    }

    live_predict_stopping_ = true;
    live_static_preview_source_name_.clear();
    live_predict_status_.reset();
    live_crop_overlay_mode_ = false;
    clear_live_crop_draft();
    auto session = std::move(live_predict_session_);
    LivePreviewTexture* preview_texture = live_preview_texture_.get();
    if (preview_texture != nullptr) {
        preview_texture->end_live_stream();
    }
    fastloader::runtime::submit_background_task(
        background_executor_,
        ui_callbacks_,
        [session = std::move(session)]() mutable {
            if (session) {
                session->stop();
            }
        },
        [this]() {
            live_predict_stopping_ = false;
            if (live_preview_texture_) {
                live_preview_texture_->clear_frame();
            }
            live_preview_error_.clear();
            live_predict_start_error_.clear();
            live_predict_action_error_.clear();
        },
        [this](const std::string& error) {
            live_predict_stopping_ = false;
            if (live_preview_texture_) {
                live_preview_texture_->clear_frame();
            }
            live_predict_start_error_ = error;
            live_predict_action_error_.clear();
            log_gui_error_to_stderr("live stop error", live_predict_start_error_);
        });
}

void App::draw_live_preview_panel() {
    const LivePreviewTextureState preview_state = snapshot_preview_state(live_preview_texture_);
    const bool static_preview_visible =
        !live_predict_running() && preview_state.has_frame && !live_static_preview_source_name_.empty();
    if (!live_predict_running() &&
        !static_preview_visible &&
        live_preview_error_.empty()) {
        return;
    }

    draw_section_heading("Live Preview");

    if (!live_preview_error_.empty()) {
        draw_banner("live_preview_error_banner",
                    ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                    ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                    live_preview_error_);
        ImGui::Spacing();
    }
    if (!live_predict_action_error_.empty()) {
        draw_banner("live_predict_action_error_banner",
                    ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                    ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                    live_predict_action_error_);
        ImGui::Spacing();
    }

    if (!preview_state.has_frame) {
        ImGui::BeginChild("live_preview_panel",
                          ImVec2(0.0f, live_preview_fit_to_capture_ ? 0.0f : 420.0f),
                          true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextUnformatted(live_predict_running() ? "Waiting for preview frame..." : "No preview frame loaded.");
        ImGui::EndChild();
        return;
    }

    const LiveCaptureRegion texture_region = preview_state.displayed_region;
    LiveCaptureRegion display_region = texture_region;
    ImVec2 uv0 = LivePreviewTexture::uv0();
    ImVec2 uv1 = LivePreviewTexture::uv1();
    const bool can_crop_live_preview = live_predict_session_ && predict_.source.kind == SourceKind::VideoStream &&
                                       texture_region.width > 0U && texture_region.height > 0U;
    if (can_crop_live_preview && !live_crop_overlay_mode_) {
        const AnnotationBox crop_box = active_live_crop_box();
        const int tex_x1 = static_cast<int>(texture_region.x);
        const int tex_y1 = static_cast<int>(texture_region.y);
        const int tex_x2 = tex_x1 + static_cast<int>(texture_region.width);
        const int tex_y2 = tex_y1 + static_cast<int>(texture_region.height);
        const int crop_x1 = std::clamp(crop_box.x1, tex_x1, std::max(tex_x1, tex_x2 - 1));
        const int crop_y1 = std::clamp(crop_box.y1, tex_y1, std::max(tex_y1, tex_y2 - 1));
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
        uv0 = ImVec2(static_cast<float>(display_region.x - texture_region.x) * inv_width,
                     static_cast<float>(display_region.y - texture_region.y) * inv_height);
        uv1 = ImVec2(static_cast<float>(display_region.x + display_region.width - texture_region.x) * inv_width,
                     static_cast<float>(display_region.y + display_region.height - texture_region.y) * inv_height);
    }
    const float width = static_cast<float>(display_region.width);
    const float height = static_cast<float>(display_region.height);

    ImGui::BeginChild("live_preview_panel",
                      ImVec2(0.0f, live_preview_fit_to_capture_ ? 0.0f : 420.0f),
                      true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::Text("Frame: %llu", static_cast<unsigned long long>(preview_state.last_frame_id));
    if (predict_.source.kind == SourceKind::VideoStream && live_predict_session_) {
        const AnnotationBox active_box = active_live_crop_box();
        const AnnotationBox committed_box = resolved_video_crop_box(predict_.source);
        ImGui::TextUnformatted(live_crop_overlay_mode_ ? "Display: full frame" : "Display: crop");
        ImGui::Text("Crop: x=%d y=%d w=%d h=%d",
                    active_box.x1,
                    active_box.y1,
                    std::max(0, active_box.x2 - active_box.x1),
                    std::max(0, active_box.y2 - active_box.y1));
        if (live_crop_overlay_mode_ && !annotation_boxes_equal(active_box, committed_box)) {
            ImGui::TextDisabled("Committed: x=%d y=%d w=%d h=%d",
                                committed_box.x1,
                                committed_box.y1,
                                std::max(0, committed_box.x2 - committed_box.x1),
                                std::max(0, committed_box.y2 - committed_box.y1));
        }
    } else {
        ImGui::Text("ROI: x=%u y=%u w=%u h=%u",
                    display_region.x,
                    display_region.y,
                    display_region.width,
                    display_region.height);
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
    if (live_crop_overlay_mode_ && live_predict_session_ &&
        predict_.source.kind == SourceKind::VideoStream) {
        draw_crop_overlay(image_origin.x, image_origin.y, image_size.x, image_size.y,
                          static_cast<float>(texture_region.width),
                          static_cast<float>(texture_region.height));
    }
    ImGui::EndChild();
}

void App::apply_live_crop_region() {
    if (!live_predict_session_) {
        return;
    }
    try {
        const int capture_width = std::max(1, predict_.source.capture_width);
        const int capture_height = std::max(1, predict_.source.capture_height);
        AnnotationBox crop_box = active_live_crop_box();
        crop_box.x1 = std::clamp(crop_box.x1, 0, capture_width - 1);
        crop_box.y1 = std::clamp(crop_box.y1, 0, capture_height - 1);
        crop_box.x2 = std::clamp(crop_box.x2, crop_box.x1 + 1, capture_width);
        crop_box.y2 = std::clamp(crop_box.y2, crop_box.y1 + 1, capture_height);
        fastloader::rfdetr::LiveCaptureRegion capture_region;
        capture_region.x = 0;
        capture_region.y = 0;
        capture_region.width = static_cast<std::uint32_t>(capture_width);
        capture_region.height = static_cast<std::uint32_t>(capture_height);
        live_predict_session_->set_capture_region(capture_region);

        fastloader::rfdetr::LiveCaptureRegion inference_region;
        inference_region.x = static_cast<std::uint32_t>(std::max(0, crop_box.x1));
        inference_region.y = static_cast<std::uint32_t>(std::max(0, crop_box.y1));
        inference_region.width = static_cast<std::uint32_t>(std::max(1, crop_box.x2 - crop_box.x1));
        inference_region.height = static_cast<std::uint32_t>(std::max(1, crop_box.y2 - crop_box.y1));
        live_predict_session_->set_inference_region(inference_region);
        live_predict_action_error_.clear();
    } catch (const std::exception& error) {
        live_predict_action_error_ = error.what();
        log_gui_error_to_stderr("live crop update error", live_predict_action_error_);
    }
}

void App::sync_live_crop_draft() {
    live_crop_draft_box_ = resolved_video_crop_box(predict_.source);
    live_crop_draft_active_ = true;
}

void App::clear_live_crop_draft() {
    clear_rect_layer_state(live_crop_layer_state_);
    live_crop_draft_box_ = {};
    live_crop_draft_active_ = false;
}

AnnotationBox App::active_live_crop_box() const {
    return live_crop_draft_active_ ? live_crop_draft_box_ : resolved_video_crop_box(predict_.source);
}

void App::draw_crop_overlay(float img_ox, float img_oy, float img_w, float img_h,
                            float capture_w, float capture_h) {
    if (capture_w <= 0.0f || capture_h <= 0.0f) {
        return;
    }

    ImGui::SetCursorScreenPos(ImVec2(img_ox, img_oy));
    ImGui::InvisibleButton("crop_overlay_hit", ImVec2(img_w, img_h));
    const bool overlay_hovered = ImGui::IsItemHovered();
    if (!live_crop_draft_active_) {
        sync_live_crop_draft();
    }

    const CanvasViewport viewport =
        make_canvas_viewport(img_ox,
                             img_oy,
                             img_w,
                             img_h,
                             static_cast<std::uint32_t>(std::max(0.0f, capture_w)),
                             static_cast<std::uint32_t>(std::max(0.0f, capture_h)));
    const RectLayerSpec layer{
        1,
        active_live_crop_box(),
        static_cast<std::uint32_t>(std::max(0.0f, capture_w)),
        static_cast<std::uint32_t>(std::max(0.0f, capture_h)),
        32,
        1,
        true,
        true,
    };
    const ImVec2 mouse = ImGui::GetMousePos();
    const CanvasPointerState pointer{
        mouse.x,
        mouse.y,
        overlay_hovered,
        ImGui::IsMouseClicked(ImGuiMouseButton_Left),
        ImGui::IsMouseDown(ImGuiMouseButton_Left),
    };
    const RectLayerFrameResult interaction =
        update_rect_layers(live_crop_layer_state_, &layer, 1U, viewport, pointer);
    if (interaction.changed || interaction.commit) {
        live_crop_draft_box_ = interaction.box;
        live_crop_draft_active_ = true;
        (void)assign_video_crop_box(predict_.source, interaction.box);
        apply_live_crop_region();
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    const CanvasScreenRect screen_box = canvas_rect_from_box(viewport, active_live_crop_box());
    draw_list->AddRect(ImVec2(screen_box.x1, screen_box.y1),
                       ImVec2(screen_box.x2, screen_box.y2),
                       white,
                       0.0f,
                       0,
                       1.0f);
    set_rectangle_drag_cursor(interaction.dragging ? live_crop_layer_state_.drag.kind
                                                   : interaction.hovered_kind);
}

void App::draw_preset_selector() {
    draw_section_heading("Model Config");
    const auto& active_preset = resolve_selected_preset(selected_preset_name_);
    const auto* active_module = model_registry_.find_module_for_preset(selected_preset_name_);
    const std::vector<fastloader::runtime::ModelPresetDescriptor> available_presets = model_registry_.presets();
    const bool compact_layout = ImGui::GetContentRegionAvail().x < 520.0f;

    if (active_module != nullptr) {
        ImGui::Text("Module: %s", std::string(active_module->display_name()).c_str());
    }

    draw_labeled_combo("Preset", selected_preset_name_.c_str(), 240.0f, [&]() {
        for (const auto& preset : available_presets) {
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

    ImGui::TextWrapped("Canonical Weights: %s", std::string(active_preset.canonical_weight_filename).c_str());
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
    const auto& preset = resolve_selected_preset(selected_preset_name_);
    train_.eval_max_dets = preset.num_select;
    apply_train_recipe(
        train_,
        current_train_recipe(selected_preset_name_, train_.optimizer),
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
    for (const View view : {View::Train, View::Validate, View::Predict, View::Annotate, View::Export}) {
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

    ImGui::SetNextWindowSize(ImVec2(ui_scaled(520.0f), ui_scaled(420.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &ui_settings_window_open_)) {
        ImGui::End();
        return;
    }

    bool changed = false;
    draw_section_heading("Typography");
    draw_labeled_widget("UI Scale", 180.0f, [&]() {
        changed |= ImGui::SliderFloat("##value", &ui_settings_.ui_scale, 0.85f, 1.75f, "%.2fx");
    });
    draw_labeled_widget("Primary Font", 180.0f, [&]() {
        changed |= ImGui::SliderFloat("##value", &ui_settings_.font_size, 13.0f, 28.0f, "%.1f px");
    });
    draw_labeled_widget("Secondary Font", 180.0f, [&]() {
        changed |= ImGui::SliderFloat("##value", &ui_settings_.secondary_font_size, 12.0f, 24.0f, "%.1f px");
    });
    draw_labeled_widget("Mono Font", 180.0f, [&]() {
        changed |= ImGui::SliderFloat("##value", &ui_settings_.mono_font_size, 12.0f, 24.0f, "%.1f px");
    });

    draw_section_heading("Layout");
    draw_labeled_widget("Density", 180.0f, [&]() {
        if (ImGui::BeginCombo("##value", ui_density_label(ui_settings_.density))) {
            for (const UiDensity option : {UiDensity::Compact, UiDensity::Balanced, UiDensity::Comfortable}) {
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
        changed |= ImGui::SliderFloat("##value", &ui_settings_.property_label_width, 110.0f, 260.0f, "%.0f px");
    });

    draw_section_heading("Preview");
    draw_with_optional_font(compact_font_, []() {
        ImGui::TextWrapped("This window persists UI scale, font sizes, density, and property label width in gui.json. Changes apply on the next frame.");
    });
    draw_with_optional_font(mono_font_, []() {
        ImGui::TextUnformatted("SourceSansPro-Regular / SourceCodePro-Semibold");
    });

    if (changed) {
        ui_settings_apply_pending_ = true;
    }

    if (ImGui::Button("Reset UI")) {
        ui_settings_ = UiSettingsState{};
        ui_settings_apply_pending_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        ui_settings_window_open_ = false;
    }

    ImGui::End();
}

void App::draw_workflow_window() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags kShellFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("RF-DETR Studio Shell", nullptr, kShellFlags);

    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("studio_shell", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV, available)) {
        ImGui::TableSetupColumn("workspace", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("sidebar", ImGuiTableColumnFlags_WidthStretch, 1.0f);
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
    const LivePreviewTextureState preview_state = snapshot_preview_state(live_preview_texture_);
    const bool static_preview_visible =
        !live_predict_running() && preview_state.has_frame && !live_static_preview_source_name_.empty();
    const std::vector<int> selected_devices = selected_local_device_ids();
    const std::vector<RemoteGpuFamily> selected_families = selected_remote_gpu_families();
    draw_section_heading("Training");
    ImGui::Text("Target: %s", train_execution_target_label(train_.execution_target));
    if (train_.execution_target == TrainExecutionTarget::Local) {
        ImGui::TextWrapped("Local GPUs: %s", local_gpu_selection_summary(local_gpus_, local_gpu_selected_).c_str());
        if (!selected_devices.empty()) {
            std::ostringstream stream;
            for (size_t index = 0; index < selected_devices.size(); ++index) {
                if (index > 0) {
                    stream << ", ";
                }
                stream << selected_devices[index];
            }
            ImGui::TextWrapped("Device IDs: %s", stream.str().c_str());
        }
        if (!local_gpu_error_.empty()) {
            ImGui::Spacing();
            draw_banner("activity_local_gpu_error_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        local_gpu_error_);
        }
    } else {
        ImGui::TextWrapped("Remote Families: %s", summarize_selected_remote_gpu_families(selected_families).c_str());
        ImGui::TextUnformatted(vast_api_key_.empty() ? "Vast API Key: missing" : "Vast API Key: configured");
        if (armed_remote_offer_.has_value()) {
            ImGui::TextWrapped("Armed Offer: %s", armed_remote_offer_summary(*armed_remote_offer_).c_str());
        } else {
            ImGui::TextUnformatted("Armed Offer: none");
        }
        if (remote_query_.running) {
            ImGui::TextUnformatted("Remote Query: running");
        } else if (!remote_query_.last_summary.empty()) {
            ImGui::TextWrapped("Remote Query: %s", remote_query_.last_summary.c_str());
        }
        if (!remote_query_.last_error.empty()) {
            ImGui::Spacing();
            draw_banner("activity_remote_query_error_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        remote_query_.last_error);
        }
    }

    draw_section_heading("Local Train");
    if (train_process_.running) {
        ImGui::TextWrapped("Running: %s", train_process_.label.c_str());
        if (ImGui::Button(train_process_.stop_requested ? "Force Kill" : "Stop Training")) {
            request_stop_local_training(train_process_.stop_requested);
        }
        if (train_process_.stop_requested) {
            ImGui::TextUnformatted("Stopping...");
        }
    } else if (!train_process_.label.empty()) {
        ImGui::TextWrapped("Idle after: %s", train_process_.label.c_str());
    } else {
        ImGui::TextUnformatted("Idle");
    }
    if (train_process_.progress.has_value()) {
        const TrainArtifactProgress& progress = *train_process_.progress;
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
            draw_with_optional_font(compact_font_, [&progress]() {
                ImGui::TextWrapped("Checkpoint: %s", progress.checkpoint_path.c_str());
            });
        }
    }
    if (!train_process_.last_summary.empty()) {
        ImGui::Spacing();
        draw_with_optional_font(compact_font_, [this]() {
            ImGui::TextWrapped("%s", train_process_.last_summary.c_str());
        });
    }
    if (!train_process_.last_error.empty()) {
        ImGui::Spacing();
        draw_banner("activity_train_error_banner",
                    ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                    ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                    train_process_.last_error);
    }
    if (train_process_.running || !train_process_.output_tail.empty()) {
        ImGui::Spacing();
        draw_with_optional_font(compact_font_, [this]() {
            draw_live_output_console("activity_train_output_tail",
                                     train_process_.output_tail,
                                     train_process_.running ? 140.0f : 96.0f,
                                     train_process_.running,
                                     "Waiting for local train output...");
        });
    }

    draw_section_heading("Job State");
    if (job_.running) {
        ImGui::TextWrapped("Running: %s", job_.label.c_str());
    } else if (!job_.label.empty()) {
        ImGui::TextWrapped("Idle after: %s", job_.label.c_str());
    } else {
        ImGui::TextUnformatted("Idle");
    }

    if (!job_.last_summary.empty()) {
        ImGui::Spacing();
        draw_with_optional_font(compact_font_, [this]() {
            ImGui::TextWrapped("%s", job_.last_summary.c_str());
        });
    }
    const std::string job_output = snapshot_job_output();
    if (job_.running || !job_output.empty()) {
        ImGui::Spacing();
        draw_with_optional_font(compact_font_, [&]() {
            draw_live_output_console("activity_job_output_tail",
                                     job_output,
                                     180.0f,
                                     job_.running,
                                     "Waiting for job output...");
        });
    }
    if (!job_.last_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.94f, 0.45f, 0.41f, 1.00f), "Error");
        ImGui::TextWrapped("%s", job_.last_error.c_str());
    }
    if (!picker_error_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.94f, 0.45f, 0.41f, 1.00f), "UI Error");
        ImGui::TextWrapped("%s", picker_error_.c_str());
    }

    if (current_view_ == View::Annotate) {
        draw_section_heading("Annotate");
        if (annotate_frame_.has_value()) {
            draw_with_optional_font(compact_font_, [this]() {
                ImGui::TextWrapped("Frame: %s", annotate_frame_->source_name.c_str());
            });
            ImGui::Text("Objects: %zu", annotate_instances_.size());
            ImGui::Text("Resolved: %zu", annotate_resolved_instances_.size());
        } else {
            ImGui::TextUnformatted("No annotation frame loaded");
        }
        if (annotate_assist_running_) {
            ImGui::TextUnformatted("Assist: running");
        } else if (!annotate_assist_summary_.empty()) {
            ImGui::TextWrapped("Assist: %s", annotate_assist_summary_.c_str());
        }
        if (annotate_save_running_) {
            ImGui::TextUnformatted("Save: running");
        } else if (!annotate_save_summary_.empty()) {
            ImGui::TextWrapped("Save: %s", annotate_save_summary_.c_str());
        }
        if (annotation_live_running()) {
            ImGui::TextUnformatted("Live annotate capture is active");
        }
    }

    if (live_predict_session_) {
        const auto& live_status = *live_predict_status_;
        draw_section_heading("Live Predict");
        ImGui::Text("Worker: %s", live_status.worker_running ? "running" : "stopped");
        ImGui::Text("Capture: %s", live_status.capture_running ? "running" : "stopped");
        ImGui::Text("Model: %s", live_status.model_hot ? "hot" : (live_status.model_loading ? "loading" : "cold"));
        ImGui::Text("Frames started/completed/skipped: %llu / %llu / %llu",
                    static_cast<unsigned long long>(live_status.frames_started),
                    static_cast<unsigned long long>(live_status.frames_completed),
                    static_cast<unsigned long long>(live_status.frames_skipped));
        ImGui::Text("Splits completed: %llu  Last latency: %.3f ms",
                    static_cast<unsigned long long>(live_status.splits_completed),
                    live_status.last_latency_ms);
        ImGui::Text("Latest frame: %llu  Latest splits: %zu",
                    static_cast<unsigned long long>(live_status.last_prediction.frame_id),
                    live_status.last_prediction.splits.size());
        ImGui::Text("Capture drops: %llu  Backpressure: %llu",
                    static_cast<unsigned long long>(live_status.capture.frames_dropped),
                    static_cast<unsigned long long>(live_status.capture.inference_backpressure_drops));
        if (preview_state.has_frame) {
            ImGui::Text("Preview frame: %llu",
                        static_cast<unsigned long long>(preview_state.last_frame_id));
        }
        if (!live_status.last_error.empty()) {
            ImGui::Spacing();
            draw_banner("live_predict_error_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        live_status.last_error);
        } else if (!live_predict_start_error_.empty()) {
            ImGui::Spacing();
            draw_banner("live_predict_start_error_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        live_predict_start_error_);
        } else if (!live_predict_action_error_.empty()) {
            ImGui::Spacing();
            draw_banner("live_predict_action_activity_error_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        live_predict_action_error_);
        } else if (!live_preview_error_.empty()) {
            ImGui::Spacing();
            draw_banner("live_preview_activity_error_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        live_preview_error_);
        }
    } else if (static_preview_visible) {
        draw_section_heading("Preview");
        draw_with_optional_font(compact_font_, [this]() {
            ImGui::TextWrapped("Source: %s", live_static_preview_source_name_.c_str());
        });
        ImGui::Text("Frame: %llu",
                    static_cast<unsigned long long>(preview_state.last_frame_id));
        const LiveCaptureRegion region = preview_state.displayed_region;
        ImGui::Text("Image: %u x %u", region.width, region.height);
    }

    if (!live_predict_running() && !live_predict_start_error_.empty()) {
        draw_section_heading("Live Predict");
        draw_banner("live_predict_start_error_banner",
                    ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                    ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                    live_predict_start_error_);
    }

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
                    ImGui::TextWrapped(
                        "Video device predict uses the public RF-DETR live session. The model stays hot and "
                        "runs against the newest CUDA frame without saving frames to disk.");
                });
            } else {
                draw_with_optional_font(compact_font_, []() {
                    ImGui::TextWrapped("RF-DETR live video predict is not available in this build.");
                });
            }
        } else {
            ImGui::Spacing();
            draw_with_optional_font(compact_font_, []() {
                ImGui::TextWrapped(
                    "Predict accepts compiled datasets directly. Single-image predict opens an annotated preview in LIVE, "
                    "and folder predict resolves into the public RF-DETR image-file workflow before inference.");
            });
        }
        const std::string combo_error = predict_combo_error(predict_);
        if (!combo_error.empty()) {
            ImGui::Spacing();
            draw_banner("predict_combo_activity_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        combo_error);
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
            ImGui::Text("Size: %u x %u", annotate_frame_->width, annotate_frame_->height);
            ImGui::Text("Objects: %zu", annotate_instances_.size());
            ImGui::Text("Resolved Masks: %zu", annotate_resolved_instances_.size());
        }
        if (annotation_live_running()) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Live annotate is running against a raw capture feed.");
        }
    } else if (current_view_ == View::Train) {
        if (train_.execution_target == TrainExecutionTarget::Local) {
            draw_with_optional_font(compact_font_, []() {
                ImGui::TextWrapped(
                    "Local train now runs through the sibling fastloader_cli binary so the GUI can drive both "
                    "single-GPU and multi-GPU launches, stream per-wave progress into the Train tab, and stop "
                    "or force-kill the local subprocess tree when needed.");
            });
        } else {
            draw_with_optional_font(compact_font_, []() {
                ImGui::TextWrapped(
                    "Remote train is query-and-arm only right now. Query returns the top 2 DLPerf/$ Vast offers "
                    "across the selected GPU families, and selecting one arms it for the future remote launch path.");
            });
        }
    } else {
        draw_with_optional_font(compact_font_, []() {
            ImGui::TextWrapped(
                "Train and validate map directly onto the public RF-DETR compiled-dataset workflows.");
        });
    }
}

void App::draw_train_view() {
    const bool block_actions = job_.running || live_predict_running() || train_process_.running;
    bool local_target = train_.execution_target == TrainExecutionTarget::Local;

    draw_section_heading("Datasets");
    draw_full_width_input("Train Compiled", train_.train_compiled_path);
    draw_full_width_input("Val Compiled", train_.val_compiled_path);
    draw_full_width_input("Test Compiled", train_.test_compiled_path);
    draw_full_width_input("Output Dir", train_.output_dir);

    draw_section_heading("Initialization");
    int input_mode = static_cast<int>(train_.input_mode);
    if (ImGui::RadioButton("Weights", input_mode == static_cast<int>(TrainInputMode::Weights))) {
        input_mode = static_cast<int>(TrainInputMode::Weights);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Resume", input_mode == static_cast<int>(TrainInputMode::Resume))) {
        input_mode = static_cast<int>(TrainInputMode::Resume);
    }
    train_.input_mode = static_cast<TrainInputMode>(input_mode);
    if (train_.input_mode == TrainInputMode::Weights) {
        if (draw_file_picker_input("Weights Path",
                                   train_.weights_path,
                                   file_picker_busy(FileBrowseField::TrainWeights))) {
            launch_file_picker(FileBrowseField::TrainWeights, "Select Weights", &train_.weights_path);
        }
    } else {
        if (draw_file_picker_input("Resume Path",
                                   train_.resume_path,
                                   file_picker_busy(FileBrowseField::TrainResume))) {
            launch_file_picker(FileBrowseField::TrainResume, "Select Checkpoint", &train_.resume_path);
        }
    }

    draw_section_heading("Execution");
    draw_labeled_combo("Train Target", train_execution_target_label(train_.execution_target), 140.0f, [&]() {
        for (const TrainExecutionTarget option : {TrainExecutionTarget::Local, TrainExecutionTarget::Remote}) {
            const bool selected = option == train_.execution_target;
            if (ImGui::Selectable(train_execution_target_label(option), selected)) {
                train_.execution_target = option;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
    });
    local_target = train_.execution_target == TrainExecutionTarget::Local;

    if (local_target) {
        const std::vector<int> selected_devices = selected_local_device_ids();
        const std::string local_preview = local_gpu_selection_summary(local_gpus_, local_gpu_selected_);
        draw_labeled_combo("Local GPUs", local_preview.c_str(), 320.0f, [&]() {
            for (size_t index = 0; index < local_gpus_.size(); ++index) {
                const std::string label = local_gpu_label(local_gpus_[index]);
                bool enabled = local_gpu_selected_[index];
                if (ImGui::Checkbox(label.c_str(), &enabled)) {
                    local_gpu_selected_[index] = enabled;
                }
            }
        });
        ImGui::BeginDisabled(local_gpu_refresh_running_);
        if (ImGui::Button(local_gpu_refresh_running_ ? "Refreshing GPUs..." : "Refresh Visible GPUs")) {
            refresh_local_gpus();
        }
        ImGui::EndDisabled();
        if (!local_gpu_error_.empty()) {
            ImGui::Spacing();
            draw_banner("train_local_gpu_error_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        local_gpu_error_);
        } else if (local_gpus_.empty()) {
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
        const std::vector<RemoteGpuFamily> selected_families = selected_remote_gpu_families();
        const std::string remote_preview = summarize_selected_remote_gpu_families(selected_families);
        draw_labeled_combo("Remote Families", remote_preview.c_str(), 220.0f, [&]() {
            for (const RemoteGpuFamily family : kRemoteGpuFamilyOrder) {
                const size_t index = static_cast<size_t>(family);
                ImGui::Checkbox(remote_gpu_family_label(family), &train_.remote_family_enabled[index]);
            }
        });
        const bool can_query = !block_actions &&
                               !remote_query_.running &&
                               !vast_api_key_.empty() &&
                               !selected_families.empty();
        ImGui::BeginDisabled(!can_query);
        if (ImGui::Button(remote_query_.running ? "Querying..." : "Query")) {
            launch_remote_query();
        }
        ImGui::EndDisabled();
        if (remote_query_.running) {
            ImGui::SameLine();
            ImGui::TextUnformatted("Fetching top DLPerf/$ offers...");
        }
        if (vast_api_key_.empty()) {
            ImGui::Spacing();
            draw_banner("train_remote_key_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        "Remote query requires --vast-api-key or VAST_API_KEY.");
        } else if (selected_families.empty()) {
            ImGui::Spacing();
            draw_banner("train_remote_family_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        "Select at least one remote GPU family before querying Vast.");
        }
        if (!remote_query_.last_error.empty()) {
            ImGui::Spacing();
            draw_banner("train_remote_query_error_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        remote_query_.last_error);
        } else if (!remote_query_.last_summary.empty()) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", remote_query_.last_summary.c_str());
        }
        if (!remote_query_.results.empty()) {
            ImGui::Spacing();
            draw_section_heading("Remote Offers");
            for (const VastOfferSummary& offer : remote_query_.results) {
                ImGui::PushID(offer.offer_id);
                draw_remote_offer_card(offer,
                                       armed_remote_offer_.has_value() &&
                                           armed_remote_offer_->offer_id == offer.offer_id);
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    armed_remote_offer_ = offer;
                }
                ImGui::PopID();
                ImGui::Spacing();
            }
        }
        ImGui::Spacing();
        draw_banner("train_remote_launch_banner",
                    ImVec4(0.20f, 0.24f, 0.18f, 0.95f),
                    ImVec4(0.92f, 0.95f, 0.86f, 1.00f),
                    armed_remote_offer_.has_value()
                        ? "Remote offer is armed, but launch is intentionally disabled until the training container and Vast launch template are defined."
                        : "Remote launch is intentionally disabled until the training container and Vast launch template are defined.");
    }

    draw_labeled_int_input("Workers", train_.workers, 120.0f);
    draw_labeled_int_input("Lanes", train_.lanes, 120.0f);
    draw_full_width_input("CPU Affinity", train_.cpu_affinity);
    draw_labeled_checkbox("AMP", train_.amp);
    draw_labeled_checkbox("EMA", train_.use_ema);
    draw_labeled_checkbox("Progress", train_.progress_bar);
    draw_labeled_checkbox("Freeze Encoder", train_.freeze_encoder);
    draw_compile_mode_combo("Compile Mode", train_.compile_mode);

    draw_section_heading("Optimization");
    draw_labeled_int_input("Batch Size", train_.batch_size, 120.0f);
    draw_labeled_int_input("Val Batch", train_.val_batch_size, 120.0f);
    draw_labeled_int_input("Epochs", train_.epochs, 120.0f);
    draw_labeled_int_input("Grad Accum", train_.grad_accum_steps, 120.0f);
    draw_labeled_int_input("Eval Max Dets", train_.eval_max_dets, 120.0f);
    draw_labeled_int_input("Prefetch", train_.prefetch_factor, 120.0f);
    if (draw_train_optimizer_combo("Optimizer", train_.optimizer)) {
        apply_selected_preset_defaults();
    }
    const TrainRecipeConfig recipe = current_train_recipe(selected_preset_name_, train_.optimizer);
    if (draw_labeled_double_input("Momentum", train_.momentum, 180.0f, "%.4f")) {
        update_train_recipe_override(train_.recipe_overrides.momentum, train_.momentum, recipe.momentum);
    }
    if (draw_labeled_double_input("Learning Rate", train_.lr, 180.0f, "%.6f")) {
        update_train_recipe_override(train_.recipe_overrides.lr, train_.lr, recipe.lr);
    }
    if (draw_labeled_double_input("LR Encoder", train_.lr_encoder, 180.0f, "%.6f")) {
        update_train_recipe_override(train_.recipe_overrides.lr_encoder, train_.lr_encoder, recipe.lr_encoder);
    }
    if (draw_labeled_double_input("Weight Decay", train_.weight_decay, 180.0f, "%.6f")) {
        update_train_recipe_override(train_.recipe_overrides.weight_decay, train_.weight_decay, recipe.weight_decay);
    }
    if (draw_labeled_double_input("LR Component Decay", train_.lr_component_decay, 180.0f, "%.4f")) {
        update_train_recipe_override(
            train_.recipe_overrides.lr_component_decay,
            train_.lr_component_decay,
            recipe.lr_component_decay);
    }
    if (draw_labeled_double_input("Encoder Layer Decay", train_.encoder_layer_decay, 180.0f, "%.4f")) {
        update_train_recipe_override(
            train_.recipe_overrides.encoder_layer_decay,
            train_.encoder_layer_decay,
            recipe.encoder_layer_decay);
    }
    if (draw_labeled_double_input("Warmup Epochs", train_.warmup_epochs, 180.0f, "%.4f")) {
        update_train_recipe_override(train_.recipe_overrides.warmup_epochs, train_.warmup_epochs, recipe.warmup_epochs);
    }
    if (draw_labeled_double_input("Warmup Momentum", train_.warmup_momentum, 180.0f, "%.4f")) {
        update_train_recipe_override(
            train_.recipe_overrides.warmup_momentum,
            train_.warmup_momentum,
            recipe.warmup_momentum);
    }
    if (draw_labeled_double_input("LR Min Factor", train_.lr_min_factor, 180.0f, "%.4f")) {
        update_train_recipe_override(
            train_.recipe_overrides.lr_min_factor,
            train_.lr_min_factor,
            recipe.lr_min_factor);
    }
    draw_labeled_double_input("Clip Max Norm", train_.clip_max_norm, 180.0f, "%.4f");
    if (draw_labeled_int_input("LR Drop", train_.lr_drop, 120.0f)) {
        update_train_recipe_override(train_.recipe_overrides.lr_drop, train_.lr_drop, recipe.lr_drop);
    }
    if (draw_lr_scheduler_combo("LR Scheduler", train_.lr_scheduler)) {
        update_train_recipe_override(train_.recipe_overrides.lr_scheduler, train_.lr_scheduler, recipe.lr_scheduler);
    }

    const std::vector<int> selected_devices = selected_local_device_ids();
    const bool can_run_local = local_target &&
                               !block_actions &&
                               !selected_devices.empty() &&
                               local_gpu_error_.empty();
    ImGui::BeginDisabled(!can_run_local);
    if (ImGui::Button("Run Training")) {
        launch_local_training();
    }
    ImGui::EndDisabled();
    if (local_target && train_process_.running) {
        ImGui::SameLine();
        if (ImGui::Button(train_process_.stop_requested ? "Force Kill" : "Stop Training")) {
            request_stop_local_training(train_process_.stop_requested);
        }
        if (train_process_.stop_requested) {
            ImGui::SameLine();
            ImGui::TextUnformatted("Stop requested");
        }
    }
    if (local_target &&
        (train_process_.running || train_process_.progress.has_value() || !train_process_.last_summary.empty() || !train_process_.last_error.empty())) {
        ImGui::Spacing();
        draw_section_heading("Local Training Status");
        if (train_process_.progress.has_value()) {
            const TrainArtifactProgress& progress = *train_process_.progress;
            ImGui::TextWrapped("Phase: %s", progress.phase.empty() ? "train" : progress.phase.c_str());
            ImGui::Text("Epoch: %d / %d",
                        progress.epoch >= 0 ? progress.epoch + 1 : 0,
                        std::max(0, progress.total_epochs));
            ImGui::SameLine();
            ImGui::Text("Batches: %d / %d", progress.completed_batches, progress.total_batches);
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
        if (!train_process_.last_summary.empty()) {
            ImGui::TextWrapped("%s", train_process_.last_summary.c_str());
        }
        if (!train_process_.last_error.empty()) {
            draw_banner("train_view_train_error_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        train_process_.last_error);
        }
    }
    if (local_target && (train_process_.running || !train_process_.output_tail.empty())) {
        ImGui::Spacing();
        draw_live_output_console("train_view_train_output_tail",
                                 train_process_.output_tail,
                                 180.0f,
                                 train_process_.running,
                                 "Waiting for compiler or training output...");
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
    draw_labeled_int_input("Alignment Images", validate_.alignment_images, 120.0f);
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
            const ValidationOptions options = rfdetr_workflows::build_validate_options(state);
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
    const SourceSelectionUiActions source_actions =
        draw_source_selection(predict_.source,
                              "predict_source",
                              true,
                              true,
                              true,
                              true,
                              file_picker_busy(FileBrowseField::PredictSingleImage));
    ImGui::EndDisabled();
    if (source_actions.browse_single_image) {
        launch_file_picker(FileBrowseField::PredictSingleImage, "Select Image", &predict_.source.single_image_path);
    }
    const bool live_video = predict_.source.kind == SourceKind::VideoStream;
    const bool single_image = predict_.source.kind == SourceKind::SingleImage;
    if (single_image) {
        predict_.batch_size = 1;
    }

    draw_section_heading("Model");
    ImGui::BeginDisabled(live_predict_running());
    const ModelInputBrowseRequest model_browse =
        draw_model_input_selector(
        predict_.model_input,
        predict_.weights_path,
        predict_.onnx_path,
        predict_.tensorrt_path,
        file_picker_busy(FileBrowseField::PredictWeights),
        file_picker_busy(FileBrowseField::PredictOnnx),
        file_picker_busy(FileBrowseField::PredictTensorRt));
    draw_full_width_input("Backend Preference", predict_.backend);
    if (!live_video) {
        draw_full_width_input("Output JSON", predict_.output_path);
    }
    ImGui::EndDisabled();
    switch (model_browse) {
    case ModelInputBrowseRequest::Weights:
        launch_file_picker(FileBrowseField::PredictWeights, "Select Weights", &predict_.weights_path);
        break;
    case ModelInputBrowseRequest::Onnx:
        launch_file_picker(FileBrowseField::PredictOnnx, "Select ONNX Model", &predict_.onnx_path);
        break;
    case ModelInputBrowseRequest::TensorRt:
        launch_file_picker(FileBrowseField::PredictTensorRt, "Select TensorRT Engine", &predict_.tensorrt_path);
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
        draw_labeled_float_input("Threshold", predict_.threshold, 120.0f, 0.01f, 0.10f, "%.3f");
        draw_labeled_checkbox("FP16", predict_.allow_fp16);
        draw_compile_mode_combo("Compile Mode", predict_.compile_mode);
        ImGui::EndDisabled();
        ImGui::TextWrapped(
            "Live video predict keeps the selected model hot and runs against the newest available device frame. "
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
        draw_labeled_float_input("Threshold", predict_.threshold, 120.0f, 0.01f, 0.10f, "%.3f");
        draw_labeled_checkbox("FP16", predict_.allow_fp16);
        draw_labeled_checkbox("Progress", predict_.progress_bar);
        draw_compile_mode_combo("Compile Mode", predict_.compile_mode);
    }

    const std::string source_error = validate_predict_source(predict_.source);
    const std::string combo_error = live_video ? std::string{} : predict_combo_error(predict_);
    const std::string live_blocker = live_video ? live_predict_blocker(predict_, job_) : std::string{};
    if (!source_error.empty()) {
        ImGui::Spacing();
        if (live_video) {
            draw_banner("predict_live_source_error_banner",
                        ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                        ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                        source_error);
        } else {
            ImGui::TextWrapped("%s", source_error.c_str());
        }
    }
    if (!live_blocker.empty() && (source_error.empty() || job_.running)) {
        ImGui::Spacing();
        draw_banner("predict_live_blocker_banner",
                    ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                    ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                    live_blocker);
    }
    if (!combo_error.empty()) {
        ImGui::Spacing();
        draw_banner("predict_combo_banner",
                    ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                    ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                    combo_error);
    }
    if (live_video && !live_predict_start_error_.empty()) {
        ImGui::Spacing();
        draw_banner("predict_live_start_error_banner",
                    ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                    ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                    live_predict_start_error_);
    }

    if (live_video) {
        if (!live_predict_session_ && !live_predict_starting_ && !live_predict_stopping_) {
            const bool block_live_start = !live_blocker.empty();
            ImGui::BeginDisabled(block_live_start);
            if (ImGui::Button("Start Live Prediction")) {
                launch_live_predict_session();
            }
            ImGui::EndDisabled();
        } else if (live_predict_starting_) {
            if (ImGui::Button("Cancel Live Start")) {
                stop_live_predict_session();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Starting live prediction...");
        } else if (live_predict_stopping_) {
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
                PreparedPredictSource prepared_source = prepare_predict_source(state.source);
                PredictOptions options = rfdetr_workflows::build_predict_options(state, preset_name);
                options.image_inputs = std::move(prepared_source.image_inputs);
                const PredictionRunResult result = run_prediction(options);
                write_prediction_json(options, result);
                print_prediction_summary(options, result);
                JobOutcome outcome;
                outcome.summary = rfdetr_workflows::summarize_prediction_result(result);
                outcome.preview = rfdetr_workflows::maybe_make_single_image_preview(state, options, result);
                return outcome;
            });
        }
        ImGui::EndDisabled();
    }
}

void App::draw_export_view() {
    const bool block_actions = job_.running || live_predict_running();

    draw_section_heading("Model Input");
    if (draw_file_picker_input("Weights Path (.pt)",
                               export_.weights_path,
                               file_picker_busy(FileBrowseField::ExportWeights))) {
        launch_file_picker(FileBrowseField::ExportWeights, "Select Weights", &export_.weights_path);
    }
    if (draw_file_picker_input("ONNX Path",
                               export_.onnx_path,
                               file_picker_busy(FileBrowseField::ExportOnnx))) {
        launch_file_picker(FileBrowseField::ExportOnnx, "Select ONNX Model", &export_.onnx_path);
    }

    draw_section_heading("ONNX Export");
    draw_labeled_int_input("Opset Version (19 only)", export_.opset_version, 120.0f);
    draw_labeled_checkbox("Simplify", export_.simplify);

    draw_section_heading("TensorRT");
    draw_labeled_checkbox("Build TensorRT Engine", export_.build_tensorrt);
    if (export_.build_tensorrt) {
        if (draw_file_picker_input("Engine Output Path",
                                   export_.output_path,
                                   file_picker_busy(FileBrowseField::ExportEngine))) {
            launch_file_picker(FileBrowseField::ExportEngine, "Select Engine Output", &export_.output_path);
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
            std::string onnx_path = state.onnx_path;
            append_job_output("[export] start");

            if (!state.weights_path.empty()) {
                if (onnx_path.empty()) {
                    std::filesystem::path wp(state.weights_path);
                    onnx_path = (wp.parent_path() / (wp.stem().string() + ".onnx")).string();
                }
                append_job_output("[export] writing ONNX: " + onnx_path);
                rfdetr::export_weights_to_onnx(
                    state.weights_path,
                    onnx_path,
                    state.device_id,
                    state.opset_version,
                    state.simplify);
            }

            if (onnx_path.empty()) {
                throw std::runtime_error("Provide weights path or ONNX path");
            }

            std::string summary = "Exported ONNX: " + onnx_path;

            if (state.build_tensorrt) {
                if (state.output_path.empty()) {
                    throw std::runtime_error("Engine output path is required");
                }
                append_job_output("[export] building TensorRT engine: " + state.output_path);
                rfdetr::make_tensorrt_backend(
                    onnx_path,
                    state.device_id,
                    state.allow_fp16,
                    state.output_path);
                append_job_output("[export] TensorRT engine ready: " + state.output_path);
                summary += " | TRT engine: " + state.output_path;
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
        if (!export_job_output.empty() || (job_.running && job_.label == "export")) {
            draw_live_output_console("export_view_job_output_tail",
                                     export_job_output,
                                     180.0f,
                                     job_.running && job_.label == "export",
                                     "Waiting for export output...");
        }
        if (job_.label == "export" && !job_.last_error.empty()) {
            ImGui::TextColored(ImVec4(0.94f, 0.45f, 0.41f, 1.00f), "Error");
            ImGui::TextWrapped("%s", job_.last_error.c_str());
        }
    }
}

void App::draw_live_view() {
    if (live_predict_session_) {
        const bool is_video_stream = predict_.source.kind == SourceKind::VideoStream;
        draw_labeled_checkbox("Fit To Capture", live_preview_fit_to_capture_);
        if (is_video_stream) {
            bool was_overlay = live_crop_overlay_mode_;
            draw_labeled_checkbox("Full Frame", live_crop_overlay_mode_);
            if (live_crop_overlay_mode_ && !was_overlay) {
                if (!live_crop_draft_active_) {
                    sync_live_crop_draft();
                }
                apply_live_crop_region();
            } else if (!live_crop_overlay_mode_ && was_overlay) {
                clear_rect_layer_state(live_crop_layer_state_);
                apply_live_crop_region();
            }
        }
        draw_live_preview_panel();
        return;
    }

    if (live_predict_starting_ || live_predict_stopping_) {
        draw_section_heading("Live Preview");
        ImGui::TextWrapped("%s",
                           live_predict_starting_ ? "Live prediction is starting..."
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
    if (!live_predict_start_error_.empty()) {
        draw_banner("live_view_start_error_banner",
                    ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                    ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                    live_predict_start_error_);
        ImGui::Spacing();
    }
    if (!live_preview_error_.empty()) {
        draw_banner("live_view_preview_error_banner",
                    ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                    ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                    live_preview_error_);
        ImGui::Spacing();
    }
    ImGui::TextWrapped(
        "Start live prediction from Predict with a video stream source, or run single-image predict to push an annotated still here.");
}

void App::draw_annotate_workspace() {
    const bool live_video = annotate_.source.kind == SourceKind::VideoStream;
    if (annotate_frame_.has_value() &&
        (annotate_preview_dirty_ || annotate_preview_frame_id_ != annotate_frame_->frame_id)) {
        submit_annotation_preview();
    }

    draw_section_heading("Workspace");
    if (annotate_frame_.has_value()) {
        draw_with_optional_font(compact_font_, [this]() {
            ImGui::TextWrapped("Frame: %s", annotate_frame_->source_name.c_str());
        });
        ImGui::Text("Size: %u x %u", annotate_frame_->width, annotate_frame_->height);
        ImGui::SameLine();
        ImGui::Text("Objects: %zu", annotate_instances_.size());
        ImGui::SameLine();
        ImGui::Text("Resolved Masks: %zu", annotate_resolved_instances_.size());
    } else if (annotate_prepare_running_ || annotate_frame_load_running_) {
        ImGui::TextUnformatted("Loading annotation source...");
    } else {
        ImGui::TextUnformatted(live_video ? "Start live annotate to stream frames here."
                                          : "Load a source image to begin annotating.");
    }
    if (annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::AddBox ||
        annotate_create_drag_state_.kind == RectDragKind::Create) {
        draw_with_optional_font(compact_font_, []() {
            ImGui::TextWrapped(
                "Add Box is active. Drag on the image to create a new object. Use the mouse wheel to zoom and middle-drag to pan for tighter geometry work.");
        });
    } else if (annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Paint) {
        draw_with_optional_font(compact_font_, [this]() {
            ImGui::TextWrapped("Paint is active. Brush radius: %d px capture-space.", annotate_brush_radius_);
        });
    } else if (annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Erase) {
        draw_with_optional_font(compact_font_, [this]() {
            ImGui::TextWrapped("Erase is active. Brush radius: %d px capture-space.", annotate_brush_radius_);
        });
    } else if (annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Fill) {
        draw_with_optional_font(compact_font_, []() {
            ImGui::TextWrapped("Fill is active. Click a background gap inside the selected ROI to flood it.");
        });
    } else if (annotation_live_running()) {
        draw_with_optional_font(compact_font_, []() {
            ImGui::TextWrapped("Live annotate is running against the raw capture feed.");
        });
    }

    draw_section_heading("Canvas");
    const LivePreviewTextureState preview_state = snapshot_preview_state(live_preview_texture_);
    ImGui::BeginChild("annotate_canvas", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (!annotate_frame_.has_value()) {
        ImGui::TextUnformatted(annotate_frame_load_running_ || annotate_prepare_running_
                                   ? "Loading annotation source..."
                                   : (live_video ? "Start live annotate to stream frames here."
                                                 : "Load a source image to begin annotating."));
        ImGui::EndChild();
        return;
    }
    if (!preview_state.has_frame) {
        ImGui::TextUnformatted(annotate_preview_running_ ? "Preparing annotation preview..."
                                                         : "Waiting for preview frame...");
        ImGui::EndChild();
        return;
    }

    const std::uint32_t frame_capture_width = annotation_frame_capture_width(*annotate_frame_);
    const std::uint32_t frame_capture_height = annotation_frame_capture_height(*annotate_frame_);
    (void)ensure_annotation_geometry();
    AnnotationGeometryDocument* geometry = annotate_geometry_.get();
    const float width = static_cast<float>(annotate_frame_->width);
    const float height = static_cast<float>(annotate_frame_->height);
    const ImVec2 mouse = ImGui::GetMousePos();
    AnnotationInstance* active = selected_annotation_instance();

    const auto set_geometry_mode = [&](const AnnotationGeometryToolMode mode) {
        if (annotate_geometry_tool_mode_ == mode) {
            return;
        }
        annotate_geometry_tool_mode_ = mode;
        annotate_direct_drag_state_ = {};
        annotate_create_drag_state_ = {};
        annotate_painting_ = false;
        clear_rect_layer_state(annotate_canvas_layer_state_);
    };

    const auto draw_tool_button = [&](const char* label, AnnotationGeometryToolMode mode) {
        const bool selected = annotate_geometry_tool_mode_ == mode;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.32f, 0.39f, 0.17f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.38f, 0.47f, 0.21f, 1.00f));
        }
        const bool clicked = ImGui::SmallButton(label);
        if (selected) {
            ImGui::PopStyleColor(2);
        }
        if (clicked) {
            set_geometry_mode(mode);
        }
        return clicked;
    };

    ImGui::TextUnformatted("Mode");
    ImGui::SameLine();
    (void)draw_tool_button("Select", AnnotationGeometryToolMode::Select);
    ImGui::SameLine();
    (void)draw_tool_button("Direct", AnnotationGeometryToolMode::Direct);
    ImGui::SameLine();
    (void)draw_tool_button("Add Box", AnnotationGeometryToolMode::AddBox);
    ImGui::SameLine();
    ImGui::BeginDisabled(!annotate_selected_instance_.has_value());
    (void)draw_tool_button("Paint", AnnotationGeometryToolMode::Paint);
    ImGui::SameLine();
    (void)draw_tool_button("Erase", AnnotationGeometryToolMode::Erase);
    ImGui::SameLine();
    (void)draw_tool_button("Fill", AnnotationGeometryToolMode::Fill);
    ImGui::EndDisabled();
    if (annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Paint ||
        annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Erase) {
        ImGui::SameLine();
        draw_labeled_int_input("Brush", annotate_brush_radius_, 70.0f);
        annotate_brush_radius_ = std::clamp(annotate_brush_radius_, 1, 128);
    }
    ImGui::Separator();

    AnnotationBox selected_capture_box{};
    bool has_selected_capture_box = false;
    if (annotate_selected_instance_.has_value() &&
        *annotate_selected_instance_ < annotate_instances_.size()) {
        const std::size_t selected_index = *annotate_selected_instance_;
        if (geometry != nullptr) {
            if (const std::optional<AnnotationBox> bbox = geometry->instance_bbox(selected_index); bbox.has_value()) {
                selected_capture_box = *bbox;
                has_selected_capture_box = true;
            }
        }
        if (!has_selected_capture_box) {
            selected_capture_box =
                normalize_annotation_box(annotate_instances_[selected_index].box,
                                         frame_capture_width,
                                         frame_capture_height);
            has_selected_capture_box = annotation_box_has_area(selected_capture_box);
        }
    }

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
    if (available.x <= 1.0f || available.y <= 1.0f || width <= 0.0f || height <= 0.0f) {
        ImGui::EndChild();
        return;
    }

    const float fit_scale = std::min(available.x / width, available.y / height) > 0.0f
                                ? std::min(available.x / width, available.y / height)
                                : 1.0f;
    const ImVec2 viewport_screen_min = ImGui::GetCursorScreenPos();
    const ImVec2 viewport_screen_max(viewport_screen_min.x + available.x, viewport_screen_min.y + available.y);

    if (click_fit) {
        annotate_canvas_auto_fit_ = true;
        annotate_canvas_scale_ = fit_scale;
        annotate_canvas_pan_x_ = 0.0f;
        annotate_canvas_pan_y_ = 0.0f;
    } else if (click_one_to_one) {
        annotate_canvas_auto_fit_ = false;
        annotate_canvas_scale_ = 1.0f;
        annotate_canvas_pan_x_ = 0.0f;
        annotate_canvas_pan_y_ = 0.0f;
    }

    float current_scale = annotate_canvas_auto_fit_ || annotate_canvas_scale_ <= 0.0f
                              ? fit_scale
                              : std::clamp(annotate_canvas_scale_, 0.05f, 64.0f);
    ImVec2 image_size(width * current_scale, height * current_scale);

    const auto recalc_view = [&]() {
        current_scale = annotate_canvas_auto_fit_ || annotate_canvas_scale_ <= 0.0f
                            ? fit_scale
                            : std::clamp(annotate_canvas_scale_, 0.05f, 64.0f);
        image_size = ImVec2(width * current_scale, height * current_scale);
        annotate_canvas_pan_x_ = clamp_canvas_pan_axis(annotate_canvas_pan_x_, image_size.x, available.x);
        annotate_canvas_pan_y_ = clamp_canvas_pan_axis(annotate_canvas_pan_y_, image_size.y, available.y);
    };

    const auto set_zoom_around_point = [&](float new_scale, const ImVec2& anchor_screen) {
        recalc_view();
        const float clamped_scale = std::clamp(new_scale, 0.05f, 64.0f);
        const float centered_x = viewport_screen_min.x + std::max(0.0f, (available.x - image_size.x) * 0.5f);
        const float centered_y = viewport_screen_min.y + std::max(0.0f, (available.y - image_size.y) * 0.5f);
        const float image_local_x = std::clamp((anchor_screen.x - (centered_x + annotate_canvas_pan_x_)) / current_scale,
                                               0.0f,
                                               width);
        const float image_local_y = std::clamp((anchor_screen.y - (centered_y + annotate_canvas_pan_y_)) / current_scale,
                                               0.0f,
                                               height);
        const ImVec2 new_size(width * clamped_scale, height * clamped_scale);
        const float new_centered_x = viewport_screen_min.x + std::max(0.0f, (available.x - new_size.x) * 0.5f);
        const float new_centered_y = viewport_screen_min.y + std::max(0.0f, (available.y - new_size.y) * 0.5f);
        annotate_canvas_auto_fit_ = false;
        annotate_canvas_scale_ = clamped_scale;
        annotate_canvas_pan_x_ = anchor_screen.x - new_centered_x - image_local_x * clamped_scale;
        annotate_canvas_pan_y_ = anchor_screen.y - new_centered_y - image_local_y * clamped_scale;
        recalc_view();
    };

    const auto focus_frame_box = [&](const AnnotationBox& frame_box) {
        if (!annotation_box_has_area(frame_box)) {
            return;
        }
        const float box_width = static_cast<float>(frame_box.x2 - frame_box.x1);
        const float box_height = static_cast<float>(frame_box.y2 - frame_box.y1);
        if (box_width <= 0.0f || box_height <= 0.0f) {
            return;
        }
        const float padded_width = box_width + 48.0f;
        const float padded_height = box_height + 48.0f;
        const float focus_scale = std::clamp(std::min(available.x / padded_width, available.y / padded_height), 0.05f, 64.0f);
        annotate_canvas_auto_fit_ = false;
        annotate_canvas_scale_ = focus_scale;
        const ImVec2 new_size(width * focus_scale, height * focus_scale);
        const float new_centered_x = viewport_screen_min.x + std::max(0.0f, (available.x - new_size.x) * 0.5f);
        const float new_centered_y = viewport_screen_min.y + std::max(0.0f, (available.y - new_size.y) * 0.5f);
        const float box_center_x = (static_cast<float>(frame_box.x1 + frame_box.x2) * 0.5f) * focus_scale;
        const float box_center_y = (static_cast<float>(frame_box.y1 + frame_box.y2) * 0.5f) * focus_scale;
        annotate_canvas_pan_x_ = viewport_screen_min.x + available.x * 0.5f - new_centered_x - box_center_x;
        annotate_canvas_pan_y_ = viewport_screen_min.y + available.y * 0.5f - new_centered_y - box_center_y;
        recalc_view();
    };

    if (click_zoom_in) {
        set_zoom_around_point(current_scale * 1.25f,
                              ImVec2(viewport_screen_min.x + available.x * 0.5f,
                                     viewport_screen_min.y + available.y * 0.5f));
    } else if (click_zoom_out) {
        set_zoom_around_point(current_scale / 1.25f,
                              ImVec2(viewport_screen_min.x + available.x * 0.5f,
                                     viewport_screen_min.y + available.y * 0.5f));
    }
    if (click_focus_selection && has_selected_capture_box) {
        focus_frame_box(annotation_box_to_frame(*annotate_frame_, selected_capture_box));
    }
    if (click_focus_crop && live_video && annotate_.full_frame) {
        focus_frame_box(annotation_box_to_frame(*annotate_frame_, resolved_video_crop_box(annotate_.source)));
    }

    recalc_view();
    if (annotate_canvas_panning_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            annotate_canvas_auto_fit_ = false;
            annotate_canvas_pan_x_ += ImGui::GetIO().MouseDelta.x;
            annotate_canvas_pan_y_ += ImGui::GetIO().MouseDelta.y;
            recalc_view();
        } else {
            annotate_canvas_panning_ = false;
        }
    }

    ImVec2 image_min(viewport_screen_min.x + std::max(0.0f, (available.x - image_size.x) * 0.5f) + annotate_canvas_pan_x_,
                     viewport_screen_min.y + std::max(0.0f, (available.y - image_size.y) * 0.5f) + annotate_canvas_pan_y_);
    ImVec2 image_max(image_min.x + image_size.x, image_min.y + image_size.y);
    const bool viewport_hovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        mouse.x >= viewport_screen_min.x && mouse.y >= viewport_screen_min.y &&
        mouse.x <= viewport_screen_max.x && mouse.y <= viewport_screen_max.y;
    if (viewport_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        annotate_canvas_panning_ = true;
    }
    if (viewport_hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        const ImVec2 zoom_anchor =
            mouse.x >= image_min.x && mouse.y >= image_min.y && mouse.x <= image_max.x && mouse.y <= image_max.y
                ? mouse
                : ImVec2(viewport_screen_min.x + available.x * 0.5f, viewport_screen_min.y + available.y * 0.5f);
        set_zoom_around_point(current_scale * std::pow(1.15f, ImGui::GetIO().MouseWheel), zoom_anchor);
        recalc_view();
        image_min = ImVec2(viewport_screen_min.x + std::max(0.0f, (available.x - image_size.x) * 0.5f) + annotate_canvas_pan_x_,
                           viewport_screen_min.y + std::max(0.0f, (available.y - image_size.y) * 0.5f) + annotate_canvas_pan_y_);
        image_max = ImVec2(image_min.x + image_size.x, image_min.y + image_size.y);
    }

    ImGui::SetCursorScreenPos(image_min);
    ImGui::InvisibleButton("annotate_canvas_hit", image_size);
    const bool overlay_hovered =
        viewport_hovered &&
        mouse.x >= image_min.x && mouse.y >= image_min.y &&
        mouse.x <= image_max.x && mouse.y <= image_max.y;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->PushClipRect(viewport_screen_min, viewport_screen_max, true);

    const float scale_x = current_scale;
    const float scale_y = current_scale;
    const auto image_x_from_mouse = [&](float mouse_x) {
        const float local_x = std::clamp((mouse_x - image_min.x) / current_scale, 0.0f, width - 0.0001f);
        return std::clamp(static_cast<int>(local_x), 0, static_cast<int>(annotate_frame_->width) - 1);
    };
    const auto image_y_from_mouse = [&](float mouse_y) {
        const float local_y = std::clamp((mouse_y - image_min.y) / current_scale, 0.0f, height - 0.0001f);
        return std::clamp(static_cast<int>(local_y), 0, static_cast<int>(annotate_frame_->height) - 1);
    };
    const int image_x = image_x_from_mouse(mouse.x);
    const int image_y = image_y_from_mouse(mouse.y);
    const int capture_x =
        std::clamp(static_cast<int>(annotate_frame_->view_x) + image_x, 0, static_cast<int>(frame_capture_width) - 1);
    const int capture_y =
        std::clamp(static_cast<int>(annotate_frame_->view_y) + image_y, 0, static_cast<int>(frame_capture_height) - 1);

    struct VisibleAnnotationBox {
        std::size_t index = 0U;
        AnnotationBox capture_box{};
        AnnotationBox frame_box{};
        bool fully_visible = false;
    };
    std::vector<VisibleAnnotationBox> visible_boxes;
    visible_boxes.reserve(annotate_instances_.size());
    for (std::size_t index = 0; index < annotate_instances_.size(); ++index) {
        std::optional<AnnotationBox> bbox;
        if (geometry != nullptr) {
            bbox = geometry->instance_bbox(index);
        }
        if (!bbox.has_value()) {
            const AnnotationBox fallback =
                normalize_annotation_box(annotate_instances_[index].box,
                                         frame_capture_width,
                                         frame_capture_height);
            if (annotation_box_has_area(fallback)) {
                bbox = fallback;
            }
        }
        if (!bbox.has_value()) {
            continue;
        }
        const AnnotationBox frame_box = annotation_box_to_frame(*annotate_frame_, *bbox);
        if (!annotation_box_has_area(frame_box)) {
            continue;
        }
        visible_boxes.push_back(VisibleAnnotationBox{
            index,
            *bbox,
            frame_box,
            annotation_boxes_equal(annotation_box_from_frame(*annotate_frame_, frame_box), *bbox),
        });
    }

    AnnotationBox selected_box{};
    bool has_selected_box = false;
    bool selected_box_fully_visible = false;
    if (has_selected_capture_box) {
        selected_box = annotation_box_to_frame(*annotate_frame_, selected_capture_box);
        has_selected_box = annotation_box_has_area(selected_box);
        selected_box_fully_visible =
            has_selected_box && annotation_boxes_equal(annotation_box_from_frame(*annotate_frame_, selected_box),
                                                       selected_capture_box);
    }

    AnnotationBox crop_box{};
    bool has_crop_box = false;
    if (live_video && annotate_.full_frame) {
        crop_box = annotation_box_to_frame(*annotate_frame_, resolved_video_crop_box(annotate_.source));
        has_crop_box = annotation_box_has_area(crop_box);
    } else {
        clear_rect_layer_state(annotate_canvas_layer_state_);
    }

    const CanvasViewport canvas_viewport =
        make_canvas_viewport(image_min.x,
                             image_min.y,
                             image_size.x,
                             image_size.y,
                             annotate_frame_->width,
                             annotate_frame_->height);
    bool color_sampled = false;
    AnnotationHsv sample_hsv{};
    if (overlay_hovered) {
        sample_hsv = sample_annotation_hsv(*annotate_frame_, image_x, image_y);
        if (active != nullptr) {
            AnnotationColorRange* armed_range =
                active->sup.sampling ? &active->sup :
                active->nosup.sampling ? &active->nosup :
                nullptr;
            if (armed_range != nullptr && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                recenter_annotation_range(*armed_range, sample_hsv);
                active->sup.sampling = false;
                active->nosup.sampling = false;
                invalidate_annotation_preview();
                color_sampled = true;
            }
        }
    }

    const CanvasPointerState canvas_pointer{
        mouse.x,
        mouse.y,
        overlay_hovered,
        !color_sampled && ImGui::IsMouseClicked(ImGuiMouseButton_Left),
        ImGui::IsMouseDown(ImGuiMouseButton_Left),
    };

    std::optional<VisibleAnnotationBox> hovered_box;
    RectDragKind hovered_box_drag_kind = RectDragKind::None;
    if (overlay_hovered && annotate_direct_drag_state_.kind == RectDragKind::None) {
        for (auto it = visible_boxes.rbegin(); it != visible_boxes.rend(); ++it) {
            const RectDragKind hover_kind = rectangle_hover_kind(canvas_pointer, canvas_viewport, it->frame_box);
            if (annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Direct &&
                hover_kind != RectDragKind::None) {
                hovered_box = *it;
                hovered_box_drag_kind = hover_kind;
                break;
            }
            if (annotation_box_contains_point(it->frame_box, image_x, image_y)) {
                hovered_box = *it;
                hovered_box_drag_kind = hover_kind;
                break;
            }
        }
    }

    RectLayerFrameResult crop_layer_frame{};
    if (annotate_create_drag_state_.kind == RectDragKind::Create ||
        annotate_direct_drag_state_.kind != RectDragKind::None ||
        annotate_painting_ ||
        annotate_geometry_tool_mode_ != AnnotationGeometryToolMode::Direct ||
        !has_crop_box) {
        clear_rect_layer_state(annotate_canvas_layer_state_);
    } else {
        constexpr int kAnnotateCropLayerId = 1;
        const RectLayerSpec crop_layer{
            kAnnotateCropLayerId,
            crop_box,
            annotate_frame_->width,
            annotate_frame_->height,
            32,
            hovered_box.has_value() ? 0 : 2,
            true,
            true,
        };
        crop_layer_frame = update_rect_layers(annotate_canvas_layer_state_,
                                              &crop_layer,
                                              1U,
                                              canvas_viewport,
                                              canvas_pointer);
    }

    if (annotate_direct_drag_state_.kind != RectDragKind::None) {
        set_rectangle_drag_cursor(annotate_direct_drag_state_.kind);
    } else if (crop_layer_frame.dragging) {
        set_rectangle_drag_cursor(annotate_canvas_layer_state_.drag.kind);
    } else if (annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Direct &&
               hovered_box_drag_kind != RectDragKind::None) {
        set_rectangle_drag_cursor(hovered_box_drag_kind);
    } else if (crop_layer_frame.hovered_kind != RectDragKind::None) {
        set_rectangle_drag_cursor(crop_layer_frame.hovered_kind);
    }

    draw_list->AddRectFilled(viewport_screen_min,
                             viewport_screen_max,
                             IM_COL32(18, 21, 24, 255));
    draw_list->AddImage(preview_state.texture_id, image_min, image_max, LivePreviewTexture::uv0(), LivePreviewTexture::uv1());
    draw_list->AddRect(viewport_screen_min,
                       viewport_screen_max,
                       IM_COL32(74, 80, 88, 255),
                       0.0f,
                       0,
                       1.0f);
    if (has_crop_box) {
        draw_list->AddRect(ImVec2(image_min.x + scale_x * static_cast<float>(crop_box.x1),
                                  image_min.y + scale_y * static_cast<float>(crop_box.y1)),
                           ImVec2(image_min.x + scale_x * static_cast<float>(crop_box.x2),
                                  image_min.y + scale_y * static_cast<float>(crop_box.y2)),
                           IM_COL32(255, 255, 255, 255),
                           0.0f,
                           0,
                           1.0f);
    }
    for (const VisibleAnnotationBox& box : visible_boxes) {
        const bool selected = annotate_selected_instance_.has_value() && *annotate_selected_instance_ == box.index;
        const bool hovered = hovered_box.has_value() && hovered_box->index == box.index;
        const ImU32 color =
            selected ? IM_COL32(255, 220, 96, 255) :
            hovered ? IM_COL32(124, 198, 255, 235) :
                      IM_COL32(90, 160, 208, 196);
        draw_list->AddRect(ImVec2(image_min.x + scale_x * static_cast<float>(box.frame_box.x1),
                                  image_min.y + scale_y * static_cast<float>(box.frame_box.y1)),
                           ImVec2(image_min.x + scale_x * static_cast<float>(box.frame_box.x2),
                                  image_min.y + scale_y * static_cast<float>(box.frame_box.y2)),
                           color,
                           0.0f,
                           0,
                           selected ? 2.5f : 1.5f);
    }

    if (has_selected_box) {
        draw_box_handles(draw_list,
                         image_min,
                         scale_x,
                         scale_y,
                         selected_box,
                         IM_COL32(255, 220, 96, 240),
                         IM_COL32(28, 32, 36, 255));
    }
    if (overlay_hovered &&
        (annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Paint ||
         annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Erase)) {
        draw_list->AddCircle(ImVec2(image_min.x + current_scale * static_cast<float>(image_x),
                                    image_min.y + current_scale * static_cast<float>(image_y)),
                             std::max(2.0f, static_cast<float>(annotate_brush_radius_) * current_scale),
                             annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Erase
                                 ? IM_COL32(255, 128, 128, 230)
                                 : IM_COL32(128, 255, 170, 230),
                             32,
                             1.5f);
    }

    char zoom_label[64];
    std::snprintf(zoom_label, sizeof(zoom_label), "Zoom %.0f%%", current_scale * 100.0f);
    draw_list->AddRectFilled(ImVec2(viewport_screen_min.x + 8.0f, viewport_screen_min.y + 8.0f),
                             ImVec2(viewport_screen_min.x + 328.0f, viewport_screen_min.y + 72.0f),
                             IM_COL32(10, 12, 14, 196),
                             4.0f);
    draw_list->AddText(ImVec2(viewport_screen_min.x + 16.0f, viewport_screen_min.y + 14.0f),
                       IM_COL32(228, 232, 236, 255),
                       zoom_label);
    if (overlay_hovered) {
        char cursor_label[160];
        std::snprintf(cursor_label,
                      sizeof(cursor_label),
                      "Cursor view %d,%d  capture %d,%d  HSV %.1f / %.1f%% / %.1f%%",
                      image_x,
                      image_y,
                      capture_x,
                      capture_y,
                      sample_hsv.hue_degrees,
                      sample_hsv.saturation * 100.0f,
                      sample_hsv.value * 100.0f);
        draw_list->AddText(ImVec2(viewport_screen_min.x + 16.0f, viewport_screen_min.y + 32.0f),
                           IM_COL32(208, 214, 220, 255),
                           cursor_label);
    }
    if (has_selected_box && !selected_box_fully_visible) {
        draw_list->AddText(ImVec2(viewport_screen_min.x + 16.0f, viewport_screen_min.y + 50.0f),
                           IM_COL32(255, 220, 96, 255),
                           "Selection clipped by current preview");
    }

    if (crop_layer_frame.active_layer_id != 0 &&
        (crop_layer_frame.changed || crop_layer_frame.commit)) {
        (void)assign_video_crop_box(annotate_.source,
                                    annotation_box_from_frame(*annotate_frame_, crop_layer_frame.box));
        if (crop_layer_frame.commit && annotate_live_session_) {
            annotate_live_session_->update_preview_region(annotate_.source, annotate_.full_frame);
        }
        invalidate_annotation_preview();
    }

    if (!color_sampled && overlay_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_box.has_value()) {
        annotate_selected_instance_ = hovered_box->index;
    }

    if (!color_sampled &&
        overlay_hovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Direct &&
        geometry != nullptr &&
        hovered_box.has_value() &&
        hovered_box_drag_kind != RectDragKind::None &&
        crop_layer_frame.active_layer_id == 0) {
        annotate_selected_instance_ = hovered_box->index;
        annotate_direct_drag_state_.kind = hovered_box_drag_kind;
        annotate_direct_drag_state_.start_mouse_x = mouse.x;
        annotate_direct_drag_state_.start_mouse_y = mouse.y;
        annotate_direct_drag_state_.start_box = hovered_box->frame_box;
    }

    if (annotate_direct_drag_state_.kind != RectDragKind::None &&
        annotate_selected_instance_.has_value() &&
        geometry != nullptr) {
        const std::size_t selected_index = *annotate_selected_instance_;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const AnnotationBox updated_local =
                apply_rect_drag(annotate_direct_drag_state_,
                                canvas_pointer,
                                canvas_viewport,
                                static_cast<int>(annotate_frame_->width),
                                static_cast<int>(annotate_frame_->height),
                                1);
            const AnnotationBox updated_capture = annotation_box_from_frame(*annotate_frame_, updated_local);
            bool changed = false;
            if (annotate_direct_drag_state_.kind == RectDragKind::Move) {
                if (const std::optional<AnnotationBox> current_bbox = geometry->instance_bbox(selected_index);
                    current_bbox.has_value()) {
                    changed = geometry->move_instance(selected_index,
                                                      updated_capture.x1 - current_bbox->x1,
                                                      updated_capture.y1 - current_bbox->y1);
                }
            } else {
                changed = geometry->resize_instance(selected_index, updated_capture);
            }
            if (changed) {
                sync_annotation_instance_from_geometry(selected_index);
                invalidate_annotation_preview();
                if (const std::optional<AnnotationBox> next_bbox = geometry->instance_bbox(selected_index);
                    next_bbox.has_value()) {
                    annotate_direct_drag_state_.start_box = annotation_box_to_frame(*annotate_frame_, *next_bbox);
                } else {
                    annotate_direct_drag_state_.start_box = updated_local;
                }
                annotate_direct_drag_state_.start_mouse_x = mouse.x;
                annotate_direct_drag_state_.start_mouse_y = mouse.y;
            }
        } else {
            annotate_direct_drag_state_ = {};
        }
    }

    if (!color_sampled &&
        annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::AddBox &&
        annotate_create_drag_state_.kind == RectDragKind::None &&
        crop_layer_frame.active_layer_id == 0 &&
        overlay_hovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (annotate_categories_.items.empty()) {
            (void)ensure_annotation_category(annotate_categories_, "object");
        }
        AnnotationInstance instance;
        instance.instance_id = "manual-" + std::to_string(annotate_instances_.size() + 1U);
        instance.category_index = 0U;
        annotate_instances_.push_back(std::move(instance));
        annotate_selected_instance_ = annotate_instances_.size() - 1U;
        clear_annotation_geometry();
        (void)ensure_annotation_geometry();
        geometry = annotate_geometry_.get();
        annotate_create_drag_state_.kind = RectDragKind::Create;
        annotate_create_drag_state_.start_mouse_x = mouse.x;
        annotate_create_drag_state_.start_mouse_y = mouse.y;
        annotate_create_drag_state_.start_box = AnnotationBox{
            image_x,
            image_y,
            image_x + 1,
            image_y + 1,
        };
    }
    if (annotate_create_drag_state_.kind != RectDragKind::None &&
        annotate_selected_instance_.has_value() &&
        geometry != nullptr) {
        const std::size_t selected_index = *annotate_selected_instance_;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const AnnotationBox updated_local =
                apply_rect_drag(annotate_create_drag_state_,
                                canvas_pointer,
                                canvas_viewport,
                                static_cast<int>(annotate_frame_->width),
                                static_cast<int>(annotate_frame_->height),
                                1);
            const AnnotationBox updated = annotation_box_from_frame(*annotate_frame_, updated_local);
            if (geometry->set_instance_box(selected_index, updated)) {
                sync_annotation_instance_from_geometry(selected_index);
                invalidate_annotation_preview();
            }
        } else {
            annotate_create_drag_state_ = {};
        }
    }

    const bool paint_mode_active =
        annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Paint ||
        annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Erase;
    if (paint_mode_active &&
        annotate_selected_instance_.has_value() &&
        geometry != nullptr) {
        const std::size_t selected_index = *annotate_selected_instance_;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            annotate_painting_ = false;
        } else if (overlay_hovered &&
                   !color_sampled &&
                   (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || annotate_painting_)) {
            const bool erase = annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Erase;
            bool changed = false;
            if (!annotate_painting_) {
                annotate_painting_ = true;
                annotate_last_paint_capture_x_ = capture_x;
                annotate_last_paint_capture_y_ = capture_y;
            }
            rasterize_line_samples(annotate_last_paint_capture_x_,
                                   annotate_last_paint_capture_y_,
                                   capture_x,
                                   capture_y,
                                   [&](int sample_x, int sample_y) {
                                       changed = geometry->paint_instance(selected_index,
                                                                          sample_x,
                                                                          sample_y,
                                                                          annotate_brush_radius_,
                                                                          erase) || changed;
                                   });
            annotate_last_paint_capture_x_ = capture_x;
            annotate_last_paint_capture_y_ = capture_y;
            if (changed) {
                sync_annotation_instance_from_geometry(selected_index);
                invalidate_annotation_preview();
            }
        }
    } else {
        annotate_painting_ = false;
    }

    if (!color_sampled &&
        annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::Fill &&
        annotate_selected_instance_.has_value() &&
        geometry != nullptr &&
        overlay_hovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (geometry->fill_instance(*annotate_selected_instance_, capture_x, capture_y)) {
            sync_annotation_instance_from_geometry(*annotate_selected_instance_);
            invalidate_annotation_preview();
        }
    }
    draw_list->PopClipRect();
    ImGui::EndChild();
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
        draw_banner("annotate_error_banner",
                    ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                    ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                    annotate_save_error_);
    } else if (!annotate_assist_error_.empty()) {
        draw_banner("annotate_assist_error_banner",
                    ImVec4(0.33f, 0.17f, 0.12f, 0.95f),
                    ImVec4(0.98f, 0.90f, 0.82f, 1.00f),
                    annotate_assist_error_);
    } else if (!annotate_save_summary_.empty()) {
        draw_with_optional_font(compact_font_, [this]() {
            ImGui::TextWrapped("%s", annotate_save_summary_.c_str());
        });
    } else if (!annotate_assist_summary_.empty()) {
        draw_with_optional_font(compact_font_, [this]() {
            ImGui::TextWrapped("%s", annotate_assist_summary_.c_str());
        });
    }

    if (!ImGui::BeginTabBar("annotate_utilities_tabs", ImGuiTabBarFlags_FittingPolicyResizeDown)) {
        return;
    }

    if (ImGui::BeginTabItem("Setup")) {
        draw_preset_selector();

        draw_section_heading("Source");
        ImGui::BeginDisabled(annotation_live_running());
        const SourceSelectionUiActions source_actions =
            draw_source_selection(annotate_.source,
                                  "annotate_source",
                                  false,
                                  true,
                                  true,
                                  true,
                                  file_picker_busy(FileBrowseField::AnnotateSingleImage));
        ImGui::EndDisabled();
        if (source_actions.browse_single_image) {
            launch_file_picker(FileBrowseField::AnnotateSingleImage, "Select Image", &annotate_.source.single_image_path);
        }
        prepare_annotation_source();

        draw_section_heading("Model");
        const ModelInputBrowseRequest model_browse =
            draw_model_input_selector(
                annotate_.model_input,
                annotate_.weights_path,
                annotate_.onnx_path,
                annotate_.tensorrt_path,
                file_picker_busy(FileBrowseField::AnnotateWeights),
                file_picker_busy(FileBrowseField::AnnotateOnnx),
                file_picker_busy(FileBrowseField::AnnotateTensorRt),
                true);
        switch (model_browse) {
        case ModelInputBrowseRequest::Weights:
            launch_file_picker(FileBrowseField::AnnotateWeights, "Select Weights", &annotate_.weights_path);
            break;
        case ModelInputBrowseRequest::Onnx:
            launch_file_picker(FileBrowseField::AnnotateOnnx, "Select ONNX Model", &annotate_.onnx_path);
            break;
        case ModelInputBrowseRequest::TensorRt:
            launch_file_picker(FileBrowseField::AnnotateTensorRt, "Select TensorRT Engine", &annotate_.tensorrt_path);
            break;
        case ModelInputBrowseRequest::None:
            break;
        }
        draw_full_width_input("Backend Preference", annotate_.backend);
        if (annotate_.model_input == ModelInputMode::None) {
            draw_with_optional_font(compact_font_, []() {
                ImGui::TextWrapped(
                    "Manual annotate mode uses the full bounding box as the seed mask. Use Suppress and No Suppress to carve out the final mask.");
            });
        }

        if (live_video) {
            draw_section_heading("Preview");
            const bool previous_full_frame = annotate_.full_frame;
            draw_labeled_checkbox("Full Frame", annotate_.full_frame);
            if (annotate_.full_frame != previous_full_frame) {
                annotate_direct_drag_state_ = {};
                annotate_create_drag_state_ = {};
                annotate_painting_ = false;
                clear_rect_layer_state(annotate_canvas_layer_state_);
                if (annotate_live_session_) {
                    annotate_live_session_->update_preview_region(annotate_.source, annotate_.full_frame);
                }
                invalidate_annotation_preview();
            }
            draw_with_optional_font(compact_font_, []() {
                ImGui::TextWrapped(
                    "Preview only. Off shows the crop itself. On shows full capture with the crop box overlaid while assist still runs on the crop.");
            });
        }

        draw_section_heading("Execution");
        ImGui::BeginDisabled(annotation_live_running());
        draw_labeled_int_input("Device ID", annotate_.device_id, 140.0f);
        draw_labeled_int_input("Max Dets", annotate_.max_dets_per_image, 140.0f);
        draw_labeled_float_input("Threshold", annotate_.threshold, 140.0f, 0.01f, 0.10f, "%.3f");
        draw_labeled_checkbox("FP16", annotate_.allow_fp16);
        draw_compile_mode_combo("Compile Mode", annotate_.compile_mode);
        ImGui::EndDisabled();

        draw_section_heading("Frame");
        if (live_video) {
            const bool can_start_live = !block_actions && can_use_video;
            ImGui::BeginDisabled(!can_start_live && !annotation_live_running());
            if (!annotation_live_running()) {
                if (ImGui::Button("Start Live Annotate")) {
                    try {
                        start_annotation_live_session();
                        annotate_save_error_.clear();
                    } catch (const std::exception& error) {
                        annotate_save_error_ = error.what();
                    }
                }
            } else if (ImGui::Button("Stop Live Annotate")) {
                stop_annotation_live_session();
            }
            ImGui::EndDisabled();
            if (!can_use_video) {
                draw_with_optional_font(compact_font_, []() {
                    ImGui::TextWrapped("Live video annotate is not available in this build.");
                });
            }
        } else {
            const bool has_inputs = !annotate_inputs_.empty();
            ImGui::BeginDisabled(!has_inputs);
            if (ImGui::Button("Reload")) {
                load_annotation_current_frame();
            }
            if (ImGui::Button("Prev")) {
                if (annotate_current_input_index_ > 0U) {
                    --annotate_current_input_index_;
                    load_annotation_current_frame();
                }
            }
            if (ImGui::Button("Next")) {
                if (annotate_current_input_index_ + 1U < annotate_inputs_.size()) {
                    ++annotate_current_input_index_;
                    load_annotation_current_frame();
                }
            }
            ImGui::EndDisabled();
            if (has_inputs) {
                draw_with_optional_font(compact_font_, [this]() {
                    ImGui::TextWrapped("Image %zu / %zu",
                                       annotate_current_input_index_ + 1U,
                                       annotate_inputs_.size());
                });
            } else if (annotate_prepare_running_) {
                ImGui::TextUnformatted("Scanning source...");
            }
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Objects")) {
        const bool box_creation_armed =
            annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::AddBox ||
            annotate_create_drag_state_.kind == RectDragKind::Create;
        draw_section_heading("Classes");
        ImGui::Text("Loaded Classes: %zu", annotate_categories_.items.size());
        if (!annotate_categories_.items.empty()) {
            ImGui::BeginChild("annotate_classes_sidebar", ImVec2(0.0f, 96.0f), true);
            for (const AnnotationCategory& category : annotate_categories_.items) {
                ImGui::Text("%d  %s", category.id, category.name.c_str());
            }
            ImGui::EndChild();
        }
        draw_full_width_input("Add Class", annotate_pending_class_name_);
        if (ImGui::Button("Add Class")) {
            if (!annotate_pending_class_name_.empty()) {
                (void)ensure_annotation_category(annotate_categories_, annotate_pending_class_name_);
                annotate_pending_class_name_.clear();
            }
        }

        draw_section_heading("Objects");
        ImGui::BeginDisabled(!has_annotation_frame || box_creation_armed);
        if (ImGui::Button("New Box Object")) {
            if (annotate_categories_.items.empty()) {
                (void)ensure_annotation_category(annotate_categories_, "object");
            }
            AnnotationInstance instance;
            instance.instance_id = "manual-" + std::to_string(annotate_instances_.size() + 1U);
            instance.category_index = 0U;
            instance.box = AnnotationBox{};
            annotate_instances_.push_back(std::move(instance));
            annotate_selected_instance_ = annotate_instances_.size() - 1U;
            clear_annotation_geometry();
            annotate_geometry_tool_mode_ = AnnotationGeometryToolMode::AddBox;
            invalidate_annotation_preview();
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!assist_available || annotate_assist_running_ || !has_annotation_frame);
        if (ImGui::Button(annotate_assist_running_ ? "Assisting..." : "Assist Current Frame")) {
            launch_annotation_assist();
        }
        ImGui::EndDisabled();
        if (!assist_available) {
            draw_with_optional_font(compact_font_, []() {
                ImGui::TextWrapped("Manual mode is active. Full-box seeds are used until a model-backed assist mode is selected.");
            });
        }

        ImGui::BeginDisabled(selected_annotation_instance() == nullptr);
        if (ImGui::Button("Delete Selected")) {
            if (annotate_selected_instance_.has_value() &&
                *annotate_selected_instance_ < annotate_instances_.size()) {
                annotate_instances_.erase(annotate_instances_.begin() +
                                          static_cast<std::ptrdiff_t>(*annotate_selected_instance_));
                if (annotate_instances_.empty()) {
                    annotate_selected_instance_.reset();
                } else {
                    annotate_selected_instance_ =
                        std::min(*annotate_selected_instance_, annotate_instances_.size() - 1U);
                }
                clear_annotation_geometry();
                clear_rect_layer_state(annotate_canvas_layer_state_);
                invalidate_annotation_preview();
            }
        }
        ImGui::EndDisabled();

        ImGui::BeginChild("annotate_instances_sidebar", ImVec2(0.0f, 180.0f), true);
        for (std::size_t index = 0; index < annotate_instances_.size(); ++index) {
            const AnnotationInstance& instance = annotate_instances_[index];
            const std::string class_name =
                instance.category_index < annotate_categories_.items.size()
                    ? annotate_categories_.items[instance.category_index].name
                    : std::string("unassigned");
            const std::string label =
                "#" + std::to_string(index + 1U) + " " + class_name + " [" +
                annotation_seed_kind_label(instance.seed_kind) + "]";
            const bool selected = annotate_selected_instance_.has_value() && *annotate_selected_instance_ == index;
            if (ImGui::Selectable(label.c_str(), selected)) {
                annotate_selected_instance_ = index;
            }
        }
        ImGui::EndChild();

        AnnotationInstance* selected_instance = selected_annotation_instance();
        if (selected_instance != nullptr) {
            draw_section_heading("Selected Object");
            const bool original_enabled = selected_instance->enabled;
            draw_labeled_checkbox("Enabled", selected_instance->enabled);
            if (original_enabled != selected_instance->enabled) {
                invalidate_annotation_preview();
            }
            if (!annotate_categories_.items.empty()) {
                const char* preview_label =
                    annotate_categories_.items[selected_instance->category_index].name.c_str();
                draw_labeled_combo("Class", preview_label, 220.0f, [&]() {
                    for (std::size_t index = 0; index < annotate_categories_.items.size(); ++index) {
                        const bool selected = index == selected_instance->category_index;
                        if (ImGui::Selectable(annotate_categories_.items[index].name.c_str(), selected)) {
                            selected_instance->category_index = index;
                            invalidate_annotation_preview();
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                });
            }
            ImGui::Text("Seed: %s", annotation_seed_kind_label(selected_instance->seed_kind));
            ImGui::BeginDisabled(box_creation_armed);
            if (ImGui::Button("Redraw Box")) {
                if (annotate_selected_instance_.has_value()) {
                    if (!ensure_annotation_geometry()) {
                        clear_annotation_geometry();
                    } else {
                        (void)annotate_geometry_->set_instance_box(*annotate_selected_instance_, AnnotationBox{});
                        sync_annotation_instance_from_geometry(*annotate_selected_instance_);
                    }
                }
                annotate_geometry_tool_mode_ = AnnotationGeometryToolMode::AddBox;
                annotate_create_drag_state_ = {};
                invalidate_annotation_preview();
            }
            ImGui::EndDisabled();
            draw_with_optional_font(compact_font_, [selected_instance]() {
                ImGui::TextWrapped("Box: [%d, %d, %d, %d]",
                                   selected_instance->box.x1,
                                   selected_instance->box.y1,
                                   selected_instance->box.x2,
                                   selected_instance->box.y2);
            });
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Mask")) {
        AnnotationInstance* selected_instance = selected_annotation_instance();
        if (selected_instance == nullptr) {
            draw_with_optional_font(compact_font_, []() {
                ImGui::TextWrapped("Select an object to edit its mask and suppress / no-suppress HSV controls.");
            });
        } else {
            const bool box_creation_armed =
                annotate_geometry_tool_mode_ == AnnotationGeometryToolMode::AddBox ||
                annotate_create_drag_state_.kind == RectDragKind::Create;
            ImGui::Text("Seed: %s", annotation_seed_kind_label(selected_instance->seed_kind));
            draw_with_optional_font(compact_font_, [selected_instance]() {
                ImGui::TextWrapped("Box: [%d, %d, %d, %d]",
                                   selected_instance->box.x1,
                                   selected_instance->box.y1,
                                   selected_instance->box.x2,
                                   selected_instance->box.y2);
            });
            ImGui::BeginDisabled(box_creation_armed);
            if (ImGui::Button("Redraw Box")) {
                if (annotate_selected_instance_.has_value()) {
                    if (!ensure_annotation_geometry()) {
                        clear_annotation_geometry();
                    } else {
                        (void)annotate_geometry_->set_instance_box(*annotate_selected_instance_, AnnotationBox{});
                        sync_annotation_instance_from_geometry(*annotate_selected_instance_);
                    }
                }
                annotate_geometry_tool_mode_ = AnnotationGeometryToolMode::AddBox;
                annotate_create_drag_state_ = {};
                invalidate_annotation_preview();
            }
            ImGui::EndDisabled();
            draw_section_heading("Mask Tools");
            ImGui::TextWrapped("Use canvas modes for direct drag, paint, erase, and fill. Cleanup runs on the selected ROI mask.");
            ImGui::Text("Cleanup Radius: %d", annotate_cleanup_radius_);
            draw_labeled_int_input("Radius", annotate_cleanup_radius_, 70.0f);
            annotate_cleanup_radius_ = std::clamp(annotate_cleanup_radius_, 1, 32);
            if (ensure_annotation_geometry() && annotate_selected_instance_.has_value()) {
                const std::size_t selected_index = *annotate_selected_instance_;
                bool cleanup_applied = false;
                for (const AnnotationGeometryCleanupOp op : {
                         AnnotationGeometryCleanupOp::LargestComponent,
                         AnnotationGeometryCleanupOp::FillHoles,
                         AnnotationGeometryCleanupOp::Dilate,
                         AnnotationGeometryCleanupOp::Erode,
                         AnnotationGeometryCleanupOp::Open,
                         AnnotationGeometryCleanupOp::Close,
                     }) {
                    if (ImGui::Button(annotation_geometry_cleanup_label(op))) {
                        if (annotate_geometry_->cleanup_instance(selected_index, op, annotate_cleanup_radius_)) {
                            sync_annotation_instance_from_geometry(selected_index);
                            cleanup_applied = true;
                        }
                    }
                    if (op != AnnotationGeometryCleanupOp::Close) {
                        ImGui::SameLine();
                    }
                }
                if (cleanup_applied) {
                    invalidate_annotation_preview();
                }
            }
            draw_annotation_range_controls("Suppress", selected_instance->sup, selected_instance->nosup);
            draw_annotation_range_controls("No Suppress", selected_instance->nosup, selected_instance->sup);
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Save")) {
        draw_section_heading("Output");
        draw_full_width_input("Output Root", annotate_.output_dir);
        draw_full_width_input("Split", annotate_.split);
        sync_annotation_categories();
        draw_with_optional_font(compact_font_, [this]() {
            ImGui::TextWrapped("Loaded classes: %zu", annotate_categories_.items.size());
        });

        draw_section_heading("Actions");
        const bool can_save_now =
            annotate_frame_.has_value() && !annotate_resolved_instances_.empty() && !annotate_save_running_;
        ImGui::BeginDisabled(!can_save_now);
        if (ImGui::Button("Save 1")) {
            const AnnotationFrame frame_snapshot = *annotate_frame_;
            const std::vector<AnnotationResolvedInstance> resolved_instances_snapshot = annotate_resolved_instances_;
            launch_annotation_save(frame_snapshot, resolved_instances_snapshot);
            if (annotate_save_running_ && annotate_.source.kind != SourceKind::VideoStream) {
                annotate_last_saved_frame_id_ = frame_snapshot.frame_id;
                if (annotate_.source.kind == SourceKind::ImageFolder &&
                    annotate_current_input_index_ + 1U < annotate_inputs_.size()) {
                    ++annotate_current_input_index_;
                    load_annotation_current_frame();
                }
            }
        }
        ImGui::EndDisabled();
        if (live_video) {
            ImGui::BeginDisabled(!annotation_live_running());
            (void)ImGui::Button("Hold Save");
            const bool hold_button_active = ImGui::IsItemActive();
            if (!hold_button_active) {
                annotate_hold_save_blocked_ = false;
            }
            annotate_hold_save_ = hold_button_active && !annotate_hold_save_blocked_;
            ImGui::EndDisabled();
            if (annotate_hold_save_blocked_) {
                draw_with_optional_font(compact_font_, []() {
                    ImGui::TextWrapped("Release Hold Save to re-arm.");
                });
            } else if (annotate_hold_save_) {
                draw_with_optional_font(compact_font_, []() {
                    ImGui::TextWrapped("Saving newest frames while held.");
                });
            }
        } else {
            annotate_hold_save_ = false;
            annotate_hold_save_blocked_ = false;
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

} // namespace fastloader::gui
