#include "source_selection.h"
#include "file_picker.h"
#include "ui_controls.h"

#include <algorithm>
#include <vector>

#include <imgui.h>

namespace mmltk::gui {

namespace {

std::vector<SourceKind> allowed_source_kinds(bool allow_compiled_dataset,
                                             bool allow_single_image,
                                             bool allow_image_folder,
                                             bool allow_video_stream) {
    std::vector<SourceKind> kinds;
    if (allow_compiled_dataset) {
        kinds.push_back(SourceKind::CompiledDataset);
    }
    if (allow_single_image) {
        kinds.push_back(SourceKind::SingleImage);
    }
    if (allow_image_folder) {
        kinds.push_back(SourceKind::ImageFolder);
    }
    if (allow_video_stream) {
        kinds.push_back(SourceKind::VideoStream);
    }
    if (kinds.empty()) {
        kinds.push_back(SourceKind::CompiledDataset);
    }
    return kinds;
}

void draw_compiled_dataset_inputs(SourceSelectionState& state) {
    draw_full_width_input("Compiled Dataset", state.compiled_path);
    ImGui::TextWrapped("Use this when the workflow needs a compiled `.bin` dataset immediately.");
}

void draw_image_folder_inputs(SourceSelectionState& state) {
    draw_full_width_input("Image Folder", state.image_directory);
    draw_labeled_checkbox("Recursive Scan", state.recursive);
    ImGui::TextWrapped(
        "Predict will enumerate supported image files here and run the public RF-DETR raw-image workflow.");
}

bool draw_single_image_inputs(SourceSelectionState& state, bool browse_busy) {
    const bool browse_clicked = draw_file_picker_input("Image File", state.single_image_path, browse_busy);
    ImGui::TextWrapped(
        "Predict will run the public RF-DETR raw-image workflow on this image, then push the annotated result into LIVE.");
    return browse_clicked;
}

void draw_video_stream_inputs(SourceSelectionState& state) {
    draw_labeled_int_input("Device Index", state.device_index, 180.0f, 1, 8);
    draw_labeled_int_input("Capture Width", state.capture_width, 180.0f, 1, 64);
    draw_labeled_int_input("Capture Height", state.capture_height, 180.0f, 1, 64);
    draw_labeled_int_input("FPS", state.capture_fps, 180.0f, 1, 16);
    draw_labeled_int_input("V4L2 Buffers", state.v4l2_buffer_count, 180.0f, 1, 8);
    draw_labeled_int_input("Crop X", state.crop_x, 180.0f, 1, 16);
    draw_labeled_int_input("Crop Y", state.crop_y, 180.0f, 1, 16);
    draw_labeled_int_input("Crop Width", state.crop_width, 180.0f, 1, 16);
    draw_labeled_int_input("Crop Height", state.crop_height, 180.0f, 1, 16);

    ImGui::TextWrapped(
        "Crop uses width and height against the requested capture extent. "
        "If Crop X and Crop Y are both 0 and the crop is smaller than the capture size, "
        "the crop is centered automatically. A crop width or height of zero means full capture extent.");
}

} // namespace

const char* source_kind_label(SourceKind kind) {
    switch (kind) {
    case SourceKind::CompiledDataset:
        return "Compiled Dataset";
    case SourceKind::SingleImage:
        return "Single Image";
    case SourceKind::ImageFolder:
        return "Image Folder";
    case SourceKind::VideoStream:
        return "Video Stream";
    }
    return "Unknown";
}

std::string describe_source(const SourceSelectionState& state) {
    switch (state.kind) {
    case SourceKind::CompiledDataset:
        return state.compiled_path.empty() ? "No compiled dataset selected" : state.compiled_path;
    case SourceKind::SingleImage:
        return state.single_image_path.empty() ? "No single image selected" : state.single_image_path;
    case SourceKind::ImageFolder:
        return state.image_directory.empty() ? "No image folder selected" : state.image_directory;
    case SourceKind::VideoStream: {
        const ResolvedVideoCrop crop = resolve_video_crop(state);
        return "Device /dev/video" + std::to_string(std::max(0, state.device_index)) +
               " " + std::to_string(std::max(0, state.capture_width)) + "x" +
               std::to_string(std::max(0, state.capture_height)) + "@" +
               std::to_string(std::max(0, state.capture_fps)) +
               " roi=(" + std::to_string(crop.x) + "," +
               std::to_string(crop.y) + "," +
               std::to_string(crop.width) + "," +
               std::to_string(crop.height) + ")";
    }
    }
    return "Unknown source";
}

SourceSelectionUiActions draw_source_selection(SourceSelectionState& state,
                                               const char* id,
                                               bool allow_compiled_dataset,
                                               bool allow_single_image,
                                               bool allow_image_folder,
                                               bool allow_video_stream,
                                               bool single_image_browse_busy) {
    SourceSelectionUiActions actions;
    const std::vector<SourceKind> allowed =
        allowed_source_kinds(allow_compiled_dataset, allow_single_image, allow_image_folder, allow_video_stream);
    if (std::ranges::find(allowed, state.kind) == allowed.end()) {
        state.kind = allowed.front();
    }

    ImGui::PushID(id);

    draw_labeled_combo("Source Type", source_kind_label(state.kind), 220.0f, [&state, &allowed]() {
        for (SourceKind kind : allowed) {
            const bool selected = kind == state.kind;
            if (ImGui::Selectable(source_kind_label(kind), selected)) {
                state.kind = kind;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
    });

    switch (state.kind) {
    case SourceKind::CompiledDataset:
        draw_compiled_dataset_inputs(state);
        break;
    case SourceKind::SingleImage:
        actions.browse_single_image = draw_single_image_inputs(state, single_image_browse_busy);
        break;
    case SourceKind::ImageFolder:
        draw_image_folder_inputs(state);
        break;
    case SourceKind::VideoStream:
        draw_video_stream_inputs(state);
        break;
    }

    const std::string summary = describe_source(state);
    ImGui::TextWrapped("Selected Source: %s", summary.c_str());
    ImGui::PopID();
    return actions;
}

} // namespace mmltk::gui
