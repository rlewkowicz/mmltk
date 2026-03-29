#pragma once

#include <string>

namespace fastloader::gui {

enum class SourceKind : int {
    CompiledDataset = 0,
    SingleImage = 1,
    ImageFolder = 2,
    VideoStream = 3,
};

struct SourceSelectionState {
    SourceKind kind = SourceKind::CompiledDataset;
    std::string compiled_path;
    std::string single_image_path;
    std::string image_directory;
    bool recursive = false;
    int device_index = 0;
    int capture_width = 1920;
    int capture_height = 1080;
    int capture_fps = 120;
    int v4l2_buffer_count = 1;
    int crop_x = 0;
    int crop_y = 0;
    int crop_width = 0;
    int crop_height = 0;
};

struct ResolvedVideoCrop {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct SourceSelectionUiActions {
    bool browse_single_image = false;
};

const char* source_kind_label(SourceKind kind);
ResolvedVideoCrop resolve_video_crop(const SourceSelectionState& state);
std::string describe_source(const SourceSelectionState& state);
SourceSelectionUiActions draw_source_selection(SourceSelectionState& state,
                                               const char* id,
                                               bool allow_compiled_dataset = true,
                                               bool allow_single_image = false,
                                               bool allow_image_folder = true,
                                               bool allow_video_stream = true,
                                               bool single_image_browse_busy = false);

} // namespace fastloader::gui
