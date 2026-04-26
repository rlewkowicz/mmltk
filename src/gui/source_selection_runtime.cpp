#include "source_selection.h"

#include <algorithm>
#include <string>

namespace mmltk::gui {

const char* source_kind_label(const SourceKind kind) {
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
            return "Device /dev/video" + std::to_string(std::max(0, state.device_index)) + " " +
                   std::to_string(std::max(0, state.capture_width)) + "x" +
                   std::to_string(std::max(0, state.capture_height)) + "@" +
                   std::to_string(std::max(0, state.capture_fps)) + " roi=(" + std::to_string(crop.x) + "," +
                   std::to_string(crop.y) + "," + std::to_string(crop.width) + "," + std::to_string(crop.height) + ")";
        }
    }
    return "Unknown source";
}

}  // namespace mmltk::gui
