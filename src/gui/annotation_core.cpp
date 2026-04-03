#include "annotation_core.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mmltk::gui {

bool annotation_box_has_area(const AnnotationBox& box);

namespace {

std::string fallback_source_name(const std::filesystem::path& path) {
    return path.filename().empty() ? path.string() : path.filename().string();
}

std::vector<std::uint8_t> bgr_to_rgb_copy(const std::vector<std::uint8_t>& pixels_bgr) {
    std::vector<std::uint8_t> pixels_rgb = pixels_bgr;
    for (std::size_t offset = 0; offset + 2U < pixels_rgb.size(); offset += 3U) {
        std::swap(pixels_rgb[offset + 0], pixels_rgb[offset + 2]);
    }
    return pixels_rgb;
}

} // namespace

void write_annotation_png(const std::filesystem::path& path,
                          const int width,
                          const int height,
                          const int channels,
                          const void* pixels,
                          const int stride_bytes) {
    if (stbi_write_png(path.c_str(), width, height, channels, pixels, stride_bytes) == 0) {
        throw std::runtime_error("failed to write PNG: " + path.string());
    }
}

AnnotationFrame load_annotation_frame(const mmltk::rfdetr::PredictImageInput& input) {
    int raw_width = 0;
    int raw_height = 0;
    int raw_channels = 0;
    stbi_uc* raw_pixels = stbi_load(input.image_path.c_str(), &raw_width, &raw_height, &raw_channels, 3);
    if (raw_pixels == nullptr) {
        throw std::runtime_error("failed to load annotation image: " + input.image_path.string());
    }
    if (raw_width <= 0 || raw_height <= 0) {
        stbi_image_free(raw_pixels);
        throw std::runtime_error("annotation image has invalid dimensions: " + input.image_path.string());
    }

    AnnotationFrame frame;
    frame.source_name = input.source_name.empty() ? fallback_source_name(input.image_path) : input.source_name;
    frame.source_path = input.image_path;
    frame.frame_id = input.image_id > 0 ? static_cast<std::uint64_t>(input.image_id) : 1U;
    frame.width = static_cast<std::uint32_t>(raw_width);
    frame.height = static_cast<std::uint32_t>(raw_height);
    frame.view_x = 0;
    frame.view_y = 0;
    frame.capture_width = frame.width;
    frame.capture_height = frame.height;
    const std::size_t byte_count =
        static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 3U;
    frame.pixels_bgr.assign(raw_pixels, raw_pixels + byte_count);
    stbi_image_free(raw_pixels);
    for (std::size_t offset = 0; offset + 2U < frame.pixels_bgr.size(); offset += 3U) {
        std::swap(frame.pixels_bgr[offset + 0], frame.pixels_bgr[offset + 2]);
    }
    return frame;
}

void write_annotation_frame_png(const std::filesystem::path& path, const AnnotationFrame& frame) {
    const std::vector<std::uint8_t> pixels_rgb = bgr_to_rgb_copy(frame.pixels_bgr);
    write_annotation_png(path,
                         static_cast<int>(frame.width),
                         static_cast<int>(frame.height),
                         3,
                         pixels_rgb.data(),
                         static_cast<int>(frame.width * 3U));
}

AnnotationHsv sample_annotation_hsv(const AnnotationFrame& frame, const int x, const int y) {
    if (x < 0 || y < 0 || x >= static_cast<int>(frame.width) || y >= static_cast<int>(frame.height)) {
        throw std::runtime_error("annotation eyedropper sample is outside the current frame");
    }
    if (frame.pixels_bgr.size() <
        static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 3U) {
        throw std::runtime_error("annotation eyedropper sample requires frame pixels");
    }
    const std::size_t pixel_index =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x);
    const std::size_t byte_offset = pixel_index * 3U;
    return annotation_bgr_to_hsv(frame.pixels_bgr[byte_offset + 0],
                                 frame.pixels_bgr[byte_offset + 1],
                                 frame.pixels_bgr[byte_offset + 2]);
}

void recenter_annotation_range(AnnotationColorRange& range, const AnnotationHsv& center) {
    range.center.hue_degrees = annotation_wrap_hue(center.hue_degrees);
    range.center.saturation = annotation_clamp_unit(center.saturation);
    range.center.value = annotation_clamp_unit(center.value);
    range.tolerance = {};
    range.sampling = false;
}

bool annotation_range_active(const AnnotationColorRange& range) {
    return range.tolerance.hue_minus_pct > 0.0f ||
           range.tolerance.hue_plus_pct > 0.0f ||
           range.tolerance.saturation_minus_pct > 0.0f ||
           range.tolerance.saturation_plus_pct > 0.0f ||
           range.tolerance.value_minus_pct > 0.0f ||
           range.tolerance.value_plus_pct > 0.0f;
}

bool annotation_box_has_area(const AnnotationBox& box) {
    return box.x2 > box.x1 && box.y2 > box.y1;
}

AnnotationBox normalize_annotation_box(AnnotationBox box, const std::uint32_t width, const std::uint32_t height) {
    const int max_width = static_cast<int>(width);
    const int max_height = static_cast<int>(height);
    box.x1 = std::clamp(box.x1, 0, max_width);
    box.x2 = std::clamp(box.x2, 0, max_width);
    box.y1 = std::clamp(box.y1, 0, max_height);
    box.y2 = std::clamp(box.y2, 0, max_height);
    if (box.x2 < box.x1) {
        std::swap(box.x1, box.x2);
    }
    if (box.y2 < box.y1) {
        std::swap(box.y1, box.y2);
    }
    return box;
}

std::uint32_t annotation_frame_capture_width(const AnnotationFrame& frame) {
    return frame.capture_width > 0 ? frame.capture_width : frame.width;
}

std::uint32_t annotation_frame_capture_height(const AnnotationFrame& frame) {
    return frame.capture_height > 0 ? frame.capture_height : frame.height;
}

AnnotationBox annotation_frame_view_box(const AnnotationFrame& frame) {
    const std::uint32_t capture_width = annotation_frame_capture_width(frame);
    const std::uint32_t capture_height = annotation_frame_capture_height(frame);
    return normalize_annotation_box(
        AnnotationBox{
            static_cast<int>(frame.view_x),
            static_cast<int>(frame.view_y),
            static_cast<int>(frame.view_x + frame.width),
            static_cast<int>(frame.view_y + frame.height),
        },
        capture_width,
        capture_height);
}

AnnotationBox annotation_box_to_frame(const AnnotationFrame& frame, const AnnotationBox& capture_box) {
    const AnnotationBox view_box = annotation_frame_view_box(frame);
    const AnnotationBox normalized =
        normalize_annotation_box(capture_box, annotation_frame_capture_width(frame), annotation_frame_capture_height(frame));
    const AnnotationBox overlap = annotation_intersect_boxes(normalized, view_box);
    if (!annotation_box_has_area(overlap)) {
        return {};
    }
    return AnnotationBox{
        overlap.x1 - view_box.x1,
        overlap.y1 - view_box.y1,
        overlap.x2 - view_box.x1,
        overlap.y2 - view_box.y1,
    };
}

AnnotationBox annotation_box_from_frame(const AnnotationFrame& frame, const AnnotationBox& frame_box) {
    const AnnotationBox view_box = annotation_frame_view_box(frame);
    const AnnotationBox normalized = normalize_annotation_box(frame_box, frame.width, frame.height);
    return normalize_annotation_box(
        AnnotationBox{
            normalized.x1 + view_box.x1,
            normalized.y1 + view_box.y1,
            normalized.x2 + view_box.x1,
            normalized.y2 + view_box.y1,
        },
        annotation_frame_capture_width(frame),
        annotation_frame_capture_height(frame));
}

AnnotationMaskRegion annotation_mask_region_from_frame(const AnnotationFrame& frame) {
    const AnnotationBox view_box = annotation_frame_view_box(frame);
    return AnnotationMaskRegion{
        static_cast<std::uint32_t>(view_box.x1),
        static_cast<std::uint32_t>(view_box.y1),
        frame.width,
        frame.height,
    };
}

AnnotationFrame extract_annotation_frame_region(const AnnotationFrame& frame, const AnnotationBox& capture_box) {
    const AnnotationBox requested =
        normalize_annotation_box(capture_box, annotation_frame_capture_width(frame), annotation_frame_capture_height(frame));
    const AnnotationBox view_box = annotation_frame_view_box(frame);
    const AnnotationBox overlap = annotation_intersect_boxes(requested, view_box);
    if (!annotation_box_has_area(requested) ||
        overlap.x1 != requested.x1 ||
        overlap.y1 != requested.y1 ||
        overlap.x2 != requested.x2 ||
        overlap.y2 != requested.y2) {
        throw std::runtime_error("requested annotation crop is outside the current frame view");
    }

    AnnotationFrame extracted = frame;
    extracted.view_x = static_cast<std::uint32_t>(requested.x1);
    extracted.view_y = static_cast<std::uint32_t>(requested.y1);
    extracted.width = static_cast<std::uint32_t>(requested.x2 - requested.x1);
    extracted.height = static_cast<std::uint32_t>(requested.y2 - requested.y1);
    extracted.capture_width = annotation_frame_capture_width(frame);
    extracted.capture_height = annotation_frame_capture_height(frame);
    extracted.pixels_bgr.assign(static_cast<std::size_t>(extracted.width) * static_cast<std::size_t>(extracted.height) * 3U,
                                0U);

    const AnnotationBox local = annotation_box_to_frame(frame, requested);
    for (std::uint32_t row = 0; row < extracted.height; ++row) {
        const std::size_t src_offset =
            (static_cast<std::size_t>(local.y1) + static_cast<std::size_t>(row)) * static_cast<std::size_t>(frame.width) * 3U +
            static_cast<std::size_t>(local.x1) * 3U;
        const std::size_t dst_offset = static_cast<std::size_t>(row) * static_cast<std::size_t>(extracted.width) * 3U;
        std::copy_n(frame.pixels_bgr.begin() + static_cast<std::ptrdiff_t>(src_offset),
                    static_cast<std::size_t>(extracted.width) * 3U,
                    extracted.pixels_bgr.begin() + static_cast<std::ptrdiff_t>(dst_offset));
    }
    return extracted;
}

std::optional<AnnotationBox> annotation_bbox_from_mask(const std::vector<std::uint8_t>& mask,
                                                       const std::uint32_t width,
                                                       const std::uint32_t height) {
    if (mask.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) ||
        width == 0U || height == 0U) {
        return std::nullopt;
    }

    int min_x = static_cast<int>(width);
    int min_y = static_cast<int>(height);
    int max_x = -1;
    int max_y = -1;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t row_offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (std::uint32_t x = 0; x < width; ++x) {
            if (mask[row_offset + static_cast<std::size_t>(x)] == 0U) {
                continue;
            }
            min_x = std::min(min_x, static_cast<int>(x));
            min_y = std::min(min_y, static_cast<int>(y));
            max_x = std::max(max_x, static_cast<int>(x));
            max_y = std::max(max_y, static_cast<int>(y));
        }
    }
    if (max_x < min_x || max_y < min_y) {
        return std::nullopt;
    }
    return AnnotationBox{
        min_x,
        min_y,
        max_x + 1,
        max_y + 1,
    };
}

std::vector<std::uint8_t> decode_annotation_prediction_mask(const mmltk::rfdetr::EncodedMask& mask,
                                                            const std::uint32_t width,
                                                            const std::uint32_t height) {
    std::vector<std::uint8_t> dense(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    if (mask.width != width || mask.height != height) {
        return dense;
    }
    const std::size_t pixel_count = dense.size();
    for (const auto& run : mask.runs) {
        const std::size_t start = std::min<std::size_t>(run.first, pixel_count);
        const std::size_t end = std::min<std::size_t>(start + run.second, pixel_count);
        std::fill(dense.begin() + static_cast<std::ptrdiff_t>(start),
                  dense.begin() + static_cast<std::ptrdiff_t>(end),
                  1U);
    }
    return dense;
}

std::vector<std::uint8_t> decode_annotation_mask_rle(const std::string_view encoded_mask,
                                                     const std::uint32_t width,
                                                     const std::uint32_t height) {
    std::vector<std::uint8_t> dense(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    if (dense.empty() || encoded_mask.empty()) {
        return dense;
    }

    const auto parse_size = [](const std::string_view token, std::size_t* value) {
        if (value == nullptr || token.empty()) {
            return false;
        }
        const char* begin = token.data();
        const char* end = token.data() + token.size();
        const auto [ptr, error] = std::from_chars(begin, end, *value);
        return error == std::errc{} && ptr == end;
    };

    std::size_t cursor = 0U;
    while (cursor < encoded_mask.size()) {
        while (cursor < encoded_mask.size() && std::isspace(static_cast<unsigned char>(encoded_mask[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= encoded_mask.size()) {
            break;
        }
        const std::size_t token_start = cursor;
        while (cursor < encoded_mask.size() && std::isspace(static_cast<unsigned char>(encoded_mask[cursor])) == 0) {
            ++cursor;
        }
        const std::string_view token = encoded_mask.substr(token_start, cursor - token_start);
        const std::size_t separator = token.find(':');
        if (separator == std::string_view::npos) {
            continue;
        }
        std::size_t run_start = 0U;
        std::size_t run_length = 0U;
        if (!parse_size(token.substr(0U, separator), &run_start) ||
            !parse_size(token.substr(separator + 1U), &run_length)) {
            continue;
        }
        const std::size_t bounded_start = std::min(run_start, dense.size());
        const std::size_t bounded_end = std::min(bounded_start + run_length, dense.size());
        std::fill(dense.begin() + static_cast<std::ptrdiff_t>(bounded_start),
                  dense.begin() + static_cast<std::ptrdiff_t>(bounded_end),
                  1U);
    }

    return dense;
}

std::string encode_annotation_mask_rle(const std::vector<std::uint8_t>& mask) {
    std::string encoded;
    std::size_t cursor = 0;
    while (cursor < mask.size()) {
        while (cursor < mask.size() && mask[cursor] == 0U) {
            ++cursor;
        }
        if (cursor >= mask.size()) {
            break;
        }
        const std::size_t start = cursor;
        while (cursor < mask.size() && mask[cursor] != 0U) {
            ++cursor;
        }
        if (!encoded.empty()) {
            encoded.push_back(' ');
        }
        encoded += std::to_string(start);
        encoded.push_back(':');
        encoded += std::to_string(cursor - start);
    }
    return encoded;
}

} // namespace mmltk::gui
