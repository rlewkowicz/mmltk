#include "source_runtime.h"

#include "rfdetr/backends_internal.h"
#include "string_utils.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace mmltk::gui {

namespace fs = std::filesystem;
using mmltk::rfdetr::PredictImageInput;

namespace {

bool has_supported_image_extension(const fs::path& path) {
    const std::string extension = strings::to_lower(path.extension().string());
    return extension == ".jpg" || extension == ".jpeg" || extension == ".png" || extension == ".bmp" ||
           extension == ".tga" || extension == ".webp" || extension == ".ppm" || extension == ".pgm";
}

std::vector<fs::path> collect_image_paths(const fs::path& root, bool recursive) {
    std::vector<fs::path> images;
    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && has_supported_image_extension(entry.path())) {
                images.push_back(entry.path());
            }
        }
    } else {
        for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.is_regular_file() && has_supported_image_extension(entry.path())) {
                images.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(images);
    return images;
}

std::string relative_source_name(const fs::path& root, const fs::path& path) {
    std::error_code error;
    const fs::path relative = fs::relative(path, root, error);
    return error ? path.string() : relative.string();
}

std::vector<PredictImageInput> make_folder_predict_inputs(const SourceSelectionState& state) {
    const fs::path root(state.image_directory);
    std::vector<fs::path> image_paths = collect_image_paths(root, state.recursive);
    std::vector<PredictImageInput> inputs;
    inputs.reserve(image_paths.size());
    for (const fs::path& image_path : image_paths) {
        PredictImageInput input;
        input.image_path = image_path;
        input.source_name = relative_source_name(root, image_path);
        inputs.push_back(std::move(input));
    }
    return inputs;
}

PredictImageInput make_single_image_predict_input(const SourceSelectionState& state) {
    PredictImageInput input;
    input.image_path = state.single_image_path;
    input.source_name = fs::path(state.single_image_path).filename().string();
    return input;
}

}  

std::string validate_video_stream_source(const SourceSelectionState& state) {
    if (state.kind != SourceKind::VideoStream) {
        return "Video-stream source required.";
    }
    if (state.capture_width <= 0 || state.capture_height <= 0 || state.capture_fps <= 0) {
        return "Video device capture width, height, and fps must be greater than zero.";
    }
    if (state.v4l2_buffer_count <= 0) {
        return "Video device V4L2 buffer count must be greater than zero.";
    }
    if (state.crop_x < 0 || state.crop_y < 0 || state.crop_width < 0 || state.crop_height < 0) {
        return "Video device crop coordinates and dimensions must not be negative.";
    }

    const ResolvedVideoCrop crop = resolve_video_crop(state);
    if (crop.width <= 0 || crop.height <= 0) {
        return "Video device crop width and height must resolve to positive values.";
    }
    if (crop.width > state.capture_width) {
        return "Video device crop width exceeds the requested capture width.";
    }
    if (crop.height > state.capture_height) {
        return "Video device crop height exceeds the requested capture height.";
    }
    if (crop.x < 0 || crop.y < 0 || crop.x + crop.width > state.capture_width ||
        crop.y + crop.height > state.capture_height) {
        return "Video device crop must resolve within the requested capture extent.";
    }

    const fs::path device_path = "/dev/video" + std::to_string(std::max(0, state.device_index));
    std::error_code exists_error;
    if (!fs::exists(device_path, exists_error)) {
        return exists_error ? "Unable to inspect video device: " + device_path.string() + ": " + exists_error.message()
                            : "Video device does not exist: " + device_path.string();
    }
    std::error_code type_error;
    if (!fs::is_character_file(device_path, type_error)) {
        return type_error ? "Unable to inspect video device type: " + device_path.string() + ": " + type_error.message()
                          : "Video source is not a character device: " + device_path.string();
    }
    if (::access(device_path.c_str(), R_OK | W_OK) != 0) {
        const int access_errno = errno;
        return "Video device must be readable and writable: " + device_path.string() + ": " +
               std::strerror(access_errno);
    }
    return {};
}

std::string validate_predict_source(const SourceSelectionState& state) {
    switch (state.kind) {
        case SourceKind::CompiledDataset:
            if (state.compiled_path.empty()) {
                return "Predict requires a compiled dataset path.";
            }
            if (!fs::exists(state.compiled_path)) {
                return "Compiled dataset path does not exist: " + state.compiled_path;
            }
            return {};
        case SourceKind::SingleImage:
            if (state.single_image_path.empty()) {
                return "Predict requires a single image path.";
            }
            if (!fs::exists(state.single_image_path)) {
                return "Single image path does not exist: " + state.single_image_path;
            }
            if (!fs::is_regular_file(state.single_image_path)) {
                return "Single image source must point at a file: " + state.single_image_path;
            }
            if (!has_supported_image_extension(fs::path(state.single_image_path))) {
                return "Single image source is not a supported image file: " + state.single_image_path;
            }
            return {};
        case SourceKind::ImageFolder:
            if (state.image_directory.empty()) {
                return "Predict requires an image folder path.";
            }
            if (!fs::exists(state.image_directory)) {
                return "Image folder does not exist: " + state.image_directory;
            }
            if (!fs::is_directory(state.image_directory)) {
                return "Image folder source must point at a directory: " + state.image_directory;
            }
            return {};
        case SourceKind::VideoStream:
            return validate_video_stream_source(state);
    }
    return "Unsupported prediction source.";
}

PreparedPredictSource prepare_predict_source(const SourceSelectionState& state) {
    const std::string validation_error = validate_predict_source(state);
    if (!validation_error.empty()) {
        throw std::runtime_error(validation_error);
    }

    PreparedPredictSource prepared;
    switch (state.kind) {
        case SourceKind::CompiledDataset:
            return prepared;
        case SourceKind::SingleImage:
            prepared.image_inputs.push_back(make_single_image_predict_input(state));
            return prepared;
        case SourceKind::ImageFolder:
            prepared.image_inputs = make_folder_predict_inputs(state);
            if (prepared.image_inputs.empty()) {
                throw std::runtime_error("Image folder does not contain supported image files: " +
                                         state.image_directory);
            }
            return prepared;
        case SourceKind::VideoStream:
            throw std::runtime_error("Video device predict uses the RF-DETR live session, not prepare_predict_source.");
    }
    throw std::runtime_error("Unsupported prediction source.");
}

}  
