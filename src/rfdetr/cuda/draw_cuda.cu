#include "mmltk/rfdetr/draw_cuda.h"
#include "rfdetr/cuda/cuda_launch_common.h"
#include "rfdetr/draw_color_utils.h"

#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <cstdint>

namespace mmltk::rfdetr {

__constant__ unsigned char d_font5x7[10][7] = {
    {0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70},  // 0
    {0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70},  // 1
    {0x70, 0x88, 0x08, 0x30, 0x40, 0x80, 0xF8},  // 2
    {0x70, 0x88, 0x08, 0x30, 0x08, 0x88, 0x70},  // 3
    {0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10},  // 4
    {0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70},  // 5
    {0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70},  // 6
    {0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40},  // 7
    {0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70},  // 8
    {0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60}   // 9
};

inline dim3 draw_kernel_block() {
    return dim3(16, 16, 1);
}

inline dim3 draw_kernel_grid(const int width, const int height) {
    return cuda_launch::make_2d_grid(width, height, draw_kernel_block());
}

[[nodiscard]] inline bool has_valid_box_label_inputs(const draw_launch::BoxLabelInputs& inputs) {
    return inputs.boxes != nullptr && inputs.colors != nullptr && inputs.labels != nullptr && inputs.num_instances > 0;
}

[[nodiscard]] inline bool has_valid_mask_box_label_inputs(const draw_launch::MaskBoxLabelInputs& inputs) {
    return inputs.boxes != nullptr && inputs.colors != nullptr && inputs.labels != nullptr && inputs.num_instances > 0;
}

[[nodiscard]] inline bool has_valid_mask_box_label_rgb_launch(const draw_launch::MaskBoxLabelRgbLaunch& launch) {
    return draw_launch::is_valid(launch.image) && launch.instances.masks != nullptr &&
           has_valid_mask_box_label_inputs(launch.instances);
}

template <typename Surface>
__device__ bool pixel_xy_in_bounds(const Surface& surface, int& x, int& y) {
    x = blockIdx.x * blockDim.x + threadIdx.x;
    y = blockIdx.y * blockDim.y + threadIdx.y;
    return x < surface.width && y < surface.height;
}

__device__ bool load_visible_rgba_overlay(const draw_launch::ConstSurfaceU8& overlay, const int x, const int y,
                                          cuda_launch::RgbaPixelU8& pixel) {
    pixel = cuda_launch::load_rgba_pixel(overlay.pixels, overlay.pitch_bytes, x, y);
    return pixel.a != 0U;
}

template <typename Surface>
__device__ bool load_composite_rgba_overlay(const Surface& base, const draw_launch::ConstSurfaceU8& overlay, int& x,
                                            int& y, cuda_launch::RgbaPixelU8& pixel) {
    return pixel_xy_in_bounds(base, x, y) && load_visible_rgba_overlay(overlay, x, y, pixel);
}

__global__ void build_instance_colors_from_labels_kernel(const draw_launch::InstanceColorBuildLaunch launch) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    const int count = static_cast<int>(launch.count);
    if (index >= count) {
        return;
    }

    const int safe_class_count = draw_color::safe_class_count(launch.num_classes);
    int label = draw_color::normalize_label(launch.labels[index], safe_class_count);

    int rank = 0;
    for (int previous = 0; previous < index; ++previous) {
        const int previous_label = draw_color::normalize_label(launch.labels[previous], safe_class_count);
        if (previous_label == label) {
            ++rank;
        }
    }

    const int color_offset = index * 3;
    draw_color::instance_color_from_rank(label, rank, safe_class_count, launch.colors_rgb[color_offset],
                                         launch.colors_rgb[color_offset + 1], launch.colors_rgb[color_offset + 2]);
}

__device__ bool pixel_hits_box_edge(int x, int y, int x1, int y1, int x2, int y2, int box_thickness) {
    if (x < x1 - box_thickness || x > x2 + box_thickness || y < y1 - box_thickness || y > y2 + box_thickness) {
        return false;
    }
    return x < x1 || x > x2 || y < y1 || y > y2;
}

__device__ bool pixel_hits_label_digit(int x, int y, int x1, int y1, int label) {
    if (label < 0) {
        return false;
    }

    const int tx = x - x1;
    const int ty = y - (y1 - 16);
    if (ty < 0 || ty >= 14 || tx < 0 || tx >= 24) {
        return false;
    }

    int digits[2];
    int num_digits = 0;
    if (label == 0) {
        digits[0] = 0;
        num_digits = 1;
    } else {
        int temp = label;
        while (temp > 0 && num_digits < 2) {
            digits[num_digits++] = temp % 10;
            temp /= 10;
        }
    }

    for (int d = 0; d < num_digits; ++d) {
        const int char_idx = num_digits - 1 - d;
        const int digit = digits[d];
        const int char_start_x = char_idx * 12;
        if (tx < char_start_x || tx >= char_start_x + 10) {
            continue;
        }

        const int font_x = (tx - char_start_x) / 2;
        const int font_y = ty / 2;
        if (font_x < 5 && font_y < 7 && (d_font5x7[digit][font_y] & (0x80 >> font_x))) {
            return true;
        }
    }
    return false;
}

template <typename PixelT>
__device__ void apply_box_color(PixelT& pixel, const uint8_t* colors, int offset);

template <>
__device__ void apply_box_color<cuda_launch::RgbPixelFloat>(cuda_launch::RgbPixelFloat& pixel, const uint8_t* colors,
                                                            int offset) {
    cuda_launch::apply_rgb(&pixel, colors, offset);
}

template <>
__device__ void apply_box_color<cuda_launch::RgbaPixelU8>(cuda_launch::RgbaPixelU8& pixel, const uint8_t* colors,
                                                          int offset) {
    cuda_launch::apply_rgb(&pixel.r, &pixel.g, &pixel.b, colors, offset);
    pixel.a = 255U;
}

template <typename PixelT>
__device__ void apply_boxes_and_labels(int x, int y, const float* boxes, const uint8_t* colors, const int* labels,
                                       int num_instances, int box_thickness, PixelT& pixel) {
    for (int i = 0; i < num_instances; ++i) {
        const int x1 = static_cast<int>(boxes[i * 4 + 0]);
        const int y1 = static_cast<int>(boxes[i * 4 + 1]);
        const int x2 = static_cast<int>(boxes[i * 4 + 2]);
        const int y2 = static_cast<int>(boxes[i * 4 + 3]);
        const bool is_edge = pixel_hits_box_edge(x, y, x1, y1, x2, y2, box_thickness);
        const bool is_label = pixel_hits_label_digit(x, y, x1, y1, labels[i]);
        if (is_edge || is_label) {
            apply_box_color(pixel, colors, i * 3);
        }
    }
}

template <typename PixelT, typename InstancesT>
__device__ void blend_instance_masks(PixelT& pixel, const InstancesT& instances, const int image_width,
                                     const int image_height, const int x, const int y, const float mask_alpha) {
    const int image_area = image_width * image_height;
    const int pixel_index = y * image_width + x;
    for (int i = 0; i < instances.num_instances; ++i) {
        if (!instances.masks[i * image_area + pixel_index]) {
            continue;
        }
        pixel = cuda_launch::blend_rgb(pixel, instances.colors[i * 3], instances.colors[i * 3 + 1],
                                       instances.colors[i * 3 + 2], mask_alpha);
    }
}

template <typename LaunchT, typename PixelT>
__device__ void blend_launch_masks(PixelT& pixel, const LaunchT& launch, const int image_width, const int image_height,
                                   const int x, const int y) {
    blend_instance_masks(pixel, launch.instances, image_width, image_height, x, y, launch.mask_alpha);
}

template <typename LaunchT, typename PixelT>
__device__ void apply_launch_boxes_and_labels(const int x, const int y, const LaunchT& launch, PixelT& pixel) {
    const auto& instances = launch.instances;
    apply_boxes_and_labels(x, y, instances.boxes, instances.colors, instances.labels, instances.num_instances,
                           launch.box_thickness, pixel);
}

template <typename OverlayT, typename ColorT>
__device__ bool store_segment_hit_rgba_pixel(const OverlayT& overlay, const int x, const int y, const float px,
                                             const float py, const float ax, const float ay, const float bx,
                                             const float by, const float max_distance_sq, const ColorT& color) {
    if (cuda_launch::point_to_segment_distance_sq(px, py, ax, ay, bx, by) > max_distance_sq) {
        return false;
    }
    cuda_launch::store_rgba_pixel(overlay.pixels, overlay.pitch_bytes, x, y,
                                  cuda_launch::RgbaPixelU8{color.r, color.g, color.b, 255U});
    return true;
}

// Colors blending kernel
__global__ void draw_masks_and_boxes_kernel(const draw_launch::MaskBoxLabelRgbLaunch launch) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const auto& image = launch.image;

    if (x >= image.width || y >= image.height) {
        return;
    }

    auto pixel = cuda_launch::load_rgb_pixel(image.pixels, static_cast<std::size_t>(image.width) * 3U, x, y);

    blend_launch_masks(pixel, launch, image.width, image.height, x, y);
    apply_launch_boxes_and_labels(x, y, launch, pixel);

    cuda_launch::store_rgb_pixel(image.pixels, static_cast<std::size_t>(image.width) * 3U, x, y, pixel);
}

__global__ void draw_boxes_labels_bgr_pitched_kernel(const draw_launch::BoxLabelBgrPitchedLaunch launch) {
    const auto& image = launch.image;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= image.width || y >= image.height) {
        return;
    }

    auto pixel = cuda_launch::load_bgr_pixel(image.pixels, image.pitch_bytes, x, y);

    apply_launch_boxes_and_labels(x, y, launch, pixel);

    cuda_launch::store_bgr_pixel(image.pixels, image.pitch_bytes, x, y, pixel);
}

__global__ void draw_masks_boxes_labels_bgr_pitched_kernel(const draw_launch::MaskBoxLabelBgrPitchedLaunch launch) {
    const auto& image = launch.image;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= image.width || y >= image.height) {
        return;
    }

    auto pixel = cuda_launch::load_bgr_pixel(image.pixels, image.pitch_bytes, x, y);

    blend_launch_masks(pixel, launch, image.width, image.height, x, y);
    apply_launch_boxes_and_labels(x, y, launch, pixel);

    cuda_launch::store_bgr_pixel(image.pixels, image.pitch_bytes, x, y, pixel);
}

__global__ void draw_analysis_overlay_rgba_pitched_kernel(const draw_launch::AnalysisOverlayRgbaPitchedLaunch launch) {
    const auto& overlay = launch.overlay;
    const auto& instances = launch.instances;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= overlay.width || y >= overlay.height) {
        return;
    }

    cuda_launch::RgbaPixelU8 pixel{};

    if (instances.masks != nullptr) {
        for (int i = 0; i < instances.num_instances; ++i) {
            if (!instances.masks[i * overlay.width * overlay.height + y * overlay.width + x]) {
                continue;
            }
            cuda_launch::apply_rgb(&pixel.r, &pixel.g, &pixel.b, instances.colors, i * 3);
            pixel.a = launch.mask_alpha;
        }
    }

    apply_launch_boxes_and_labels(x, y, launch, pixel);

    cuda_launch::store_rgba_pixel(overlay.pixels, overlay.pitch_bytes, x, y, pixel);
}

__global__ void composite_rgba_over_bgr_pitched_kernel(const draw_launch::CompositeRgbaOverBgrPitchedLaunch launch) {
    const auto& base_bgr = launch.base_bgr;
    const auto& overlay_rgba = launch.overlay_rgba;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= base_bgr.width || y >= base_bgr.height) {
        return;
    }

    cuda_launch::RgbaPixelU8 overlay_pixel{};
    if (!load_visible_rgba_overlay(overlay_rgba, x, y, overlay_pixel)) {
        return;
    }

    const auto base_pixel = cuda_launch::load_bgr_pixel(base_bgr.pixels, base_bgr.pitch_bytes, x, y);
    cuda_launch::store_bgr_pixel(base_bgr.pixels, base_bgr.pitch_bytes, x, y,
                                 cuda_launch::composite_rgba_over_bgr(base_pixel, overlay_pixel));
}

__global__ void composite_rgba_over_rgba_pitched_kernel(const draw_launch::CompositeRgbaOverRgbaPitchedLaunch launch) {
    const auto& base_rgba = launch.base_rgba;
    const auto& overlay_rgba = launch.overlay_rgba;
    int x = 0;
    int y = 0;
    cuda_launch::RgbaPixelU8 overlay_pixel{};
    if (!load_composite_rgba_overlay(base_rgba, overlay_rgba, x, y, overlay_pixel)) {
        return;
    }

    const auto base_pixel = cuda_launch::load_rgba_pixel(base_rgba.pixels, base_rgba.pitch_bytes, x, y);
    cuda_launch::store_rgba_pixel(base_rgba.pixels, base_rgba.pitch_bytes, x, y,
                                  cuda_launch::composite_rgba_over_rgba(base_pixel, overlay_pixel));
}

__global__ void composite_rgba_over_workspace_rgba_kernel(
    const draw_launch::CompositeRgbaOverWorkspaceRgbaLaunch launch) {
    const auto& base_rgba = launch.base_rgba;
    const auto& overlay_rgba = launch.overlay_rgba;
    int x = 0;
    int y = 0;
    cuda_launch::RgbaPixelU8 overlay_pixel{};
    if (!load_composite_rgba_overlay(base_rgba, overlay_rgba, x, y, overlay_pixel)) {
        return;
    }

    uchar4 raw_base{};
    surf2Dread(&raw_base, base_rgba.surface, x * static_cast<int>(sizeof(uchar4)), y);
    const auto base_pixel = cuda_launch::RgbaPixelU8{raw_base.x, raw_base.y, raw_base.z, raw_base.w};
    const auto composited = cuda_launch::composite_rgba_over_rgba(base_pixel, overlay_pixel);
    surf2Dwrite(make_uchar4(composited.r, composited.g, composited.b, composited.a), base_rgba.surface,
                x * static_cast<int>(sizeof(uchar4)), y);
}

__global__ void copy_bgr_to_rgba_pitched_kernel(const draw_launch::CopyBgrToRgbaPitchedLaunch launch) {
    const auto& source_bgr = launch.source_bgr;
    const auto& target_rgba = launch.target_rgba;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= source_bgr.width || y >= source_bgr.height) {
        return;
    }

    const auto source_pixel = cuda_launch::load_bgr_pixel(source_bgr.pixels, source_bgr.pitch_bytes, x, y);
    cuda_launch::store_rgba_pixel(target_rgba.pixels, target_rgba.pitch_bytes, x, y,
                                  cuda_launch::RgbaPixelU8{
                                      cuda_launch::clamp_to_u8(source_pixel.r),
                                      cuda_launch::clamp_to_u8(source_pixel.g),
                                      cuda_launch::clamp_to_u8(source_pixel.b),
                                      launch.alpha,
                                  });
}

__global__ void copy_bgr_to_workspace_rgba_kernel(const draw_launch::CopyBgrToWorkspaceRgbaLaunch launch) {
    const auto& source_bgr = launch.source_bgr;
    const auto& target_rgba = launch.target_rgba;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= source_bgr.width || y >= source_bgr.height) {
        return;
    }

    const auto source_pixel = cuda_launch::load_bgr_pixel(source_bgr.pixels, source_bgr.pitch_bytes, x, y);
    surf2Dwrite(make_uchar4(cuda_launch::clamp_to_u8(source_pixel.r), cuda_launch::clamp_to_u8(source_pixel.g),
                            cuda_launch::clamp_to_u8(source_pixel.b), launch.alpha),
                target_rgba.surface, x * static_cast<int>(sizeof(uchar4)), y);
}

__device__ uchar4 synthetic_workspace_pattern_pixel(const int x, const int y, const std::uint64_t seed) {
    const auto low = static_cast<unsigned int>(seed & 0xffU);
    const auto mid = static_cast<unsigned int>((seed >> 8U) & 0xffU);
    const auto high = static_cast<unsigned int>((seed >> 16U) & 0xffU);
    return make_uchar4(static_cast<unsigned char>((static_cast<unsigned int>(x) * 17U + low) & 0xffU),
                       static_cast<unsigned char>((static_cast<unsigned int>(y) * 31U + mid) & 0xffU),
                       static_cast<unsigned char>(
                           ((static_cast<unsigned int>(x) ^ static_cast<unsigned int>(y)) * 13U + high) & 0xffU),
                       255U);
}

__global__ void fill_workspace_rgba_pattern_kernel(const draw_launch::FillWorkspaceRgbaPatternLaunch launch) {
    const auto& target_rgba = launch.target_rgba;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= target_rgba.width || y >= target_rgba.height) {
        return;
    }

    const uchar4 pixel = synthetic_workspace_pattern_pixel(x, y, launch.seed);
    if (target_rgba.kind == draw_launch::WorkspaceRgbaTargetKind::Pitched) {
        cuda_launch::store_rgba_pixel(target_rgba.pitched.pixels, target_rgba.pitched.pitch_bytes, x, y,
                                      cuda_launch::RgbaPixelU8{pixel.x, pixel.y, pixel.z, pixel.w});
        return;
    }
    surf2Dwrite(pixel, target_rgba.surface, x * static_cast<int>(sizeof(uchar4)), y);
}

__global__ void draw_manual_mask_rgba_pitched_kernel(const draw_launch::ManualMaskRgbaPitchedLaunch launch) {
    const auto& overlay_region = launch.overlay_region;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= overlay_region.width || y >= overlay_region.height) {
        return;
    }
    if (launch.mask[y * overlay_region.width + x] == 0U) {
        return;
    }

    cuda_launch::store_rgba_pixel(
        overlay_region.pixels, overlay_region.pitch_bytes, x, y,
        cuda_launch::RgbaPixelU8{launch.color.r, launch.color.g, launch.color.b, launch.color.a});
}

__global__ void draw_box_outline_rgba_pitched_kernel(const draw_launch::BoxOutlineRgbaPitchedLaunch launch) {
    const auto& overlay = launch.overlay;
    const auto& box = launch.box;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= overlay.width || y >= overlay.height) {
        return;
    }
    if (!pixel_hits_box_edge(x, y, box.x1, box.y1, box.x2 - 1, box.y2 - 1, launch.thickness)) {
        return;
    }

    cuda_launch::store_rgba_pixel(overlay.pixels, overlay.pitch_bytes, x, y,
                                  cuda_launch::RgbaPixelU8{launch.color.r, launch.color.g, launch.color.b, 255U});
}

__global__ void draw_selection_handles_rgba_pitched_kernel(
    const draw_launch::SelectionHandlesRgbaPitchedLaunch launch) {
    const auto& overlay = launch.overlay;
    const auto& box = launch.box;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= overlay.width || y >= overlay.height) {
        return;
    }

    const int corners_x[4] = {box.x1, box.x2 - 1, box.x1, box.x2 - 1};
    const int corners_y[4] = {box.y1, box.y1, box.y2 - 1, box.y2 - 1};
    for (int i = 0; i < 4; ++i) {
        if (x < corners_x[i] - launch.handle_radius || x > corners_x[i] + launch.handle_radius ||
            y < corners_y[i] - launch.handle_radius || y > corners_y[i] + launch.handle_radius) {
            continue;
        }
        cuda_launch::store_rgba_pixel(overlay.pixels, overlay.pitch_bytes, x, y,
                                      cuda_launch::RgbaPixelU8{255U, 220U, 96U, 240U});
        return;
    }
}

__global__ void draw_polyline_rgba_pitched_kernel(const draw_launch::PolylineRgbaPitchedLaunch launch) {
    const auto& overlay = launch.overlay;
    const auto& points = launch.points;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= overlay.width || y >= overlay.height || points.points_xy == nullptr || points.point_count < 2) {
        return;
    }

    const int segment_count = launch.closed ? points.point_count : points.point_count - 1;
    const float px = static_cast<float>(x) + 0.5f;
    const float py = static_cast<float>(y) + 0.5f;
    const float max_distance_sq = fmaxf(1.0f, static_cast<float>(launch.thickness * launch.thickness));
    for (int segment_index = 0; segment_index < segment_count; ++segment_index) {
        const int start_index = segment_index * 2;
        const int end_point_index = ((segment_index + 1) % points.point_count) * 2;
        const float ax = static_cast<float>(points.points_xy[start_index + 0]);
        const float ay = static_cast<float>(points.points_xy[start_index + 1]);
        const float bx = static_cast<float>(points.points_xy[end_point_index + 0]);
        const float by = static_cast<float>(points.points_xy[end_point_index + 1]);
        if (store_segment_hit_rgba_pixel(overlay, x, y, px, py, ax, ay, bx, by, max_distance_sq, launch.color)) {
            return;
        }
    }
}

__global__ void draw_points_rgba_pitched_kernel(const draw_launch::PointsRgbaPitchedLaunch launch) {
    const auto& overlay = launch.overlay;
    const auto& points = launch.points;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= overlay.width || y >= overlay.height || points.points_xy == nullptr || points.point_count <= 0) {
        return;
    }

    const float px = static_cast<float>(x) + 0.5f;
    const float py = static_cast<float>(y) + 0.5f;
    const float max_distance_sq = static_cast<float>(launch.radius * launch.radius);
    for (int point_index = 0; point_index < points.point_count; ++point_index) {
        const int xy_index = point_index * 2;
        const float qx = static_cast<float>(points.points_xy[xy_index + 0]);
        const float qy = static_cast<float>(points.points_xy[xy_index + 1]);
        if (cuda_launch::point_distance_sq(px, py, qx, qy) > max_distance_sq) {
            continue;
        }
        cuda_launch::store_rgba_pixel(
            overlay.pixels, overlay.pitch_bytes, x, y,
            cuda_launch::RgbaPixelU8{launch.color.r, launch.color.g, launch.color.b, launch.color.a});
        return;
    }
}

__global__ void draw_skeleton_rgba_pitched_kernel(const draw_launch::SkeletonRgbaPitchedLaunch launch) {
    const auto& overlay = launch.overlay;
    const auto& points = launch.points;
    const auto& edges = launch.edges;
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= overlay.width || y >= overlay.height || points.points_xy == nullptr || edges.edge_indices == nullptr ||
        points.point_count <= 0 || edges.edge_count <= 0) {
        return;
    }

    const float px = static_cast<float>(x) + 0.5f;
    const float py = static_cast<float>(y) + 0.5f;
    const float max_distance_sq = fmaxf(1.0f, static_cast<float>(launch.thickness * launch.thickness));
    for (int edge_index = 0; edge_index < edges.edge_count; ++edge_index) {
        const int pair_index = edge_index * 2;
        const std::uint32_t source_index = edges.edge_indices[pair_index + 0];
        const std::uint32_t target_index = edges.edge_indices[pair_index + 1];
        if (source_index >= static_cast<std::uint32_t>(points.point_count) ||
            target_index >= static_cast<std::uint32_t>(points.point_count)) {
            continue;
        }
        const int source_xy_index = static_cast<int>(source_index) * 2;
        const int target_xy_index = static_cast<int>(target_index) * 2;
        const float ax = static_cast<float>(points.points_xy[source_xy_index + 0]);
        const float ay = static_cast<float>(points.points_xy[source_xy_index + 1]);
        const float bx = static_cast<float>(points.points_xy[target_xy_index + 0]);
        const float by = static_cast<float>(points.points_xy[target_xy_index + 1]);
        if (store_segment_hit_rgba_pixel(overlay, x, y, px, py, ax, ay, bx, by, max_distance_sq, launch.color)) {
            return;
        }
    }
}

#define MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(name, launch_type) void name(const launch_type& launch)

#define MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(name, launch_type, surface_member, size_member,     \
                                                           valid_expression, kernel)                           \
    MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(name, launch_type) {                                                       \
        if (!(valid_expression)) {                                                                             \
            return;                                                                                            \
        }                                                                                                      \
        auto normalized_launch = launch;                                                                       \
        normalized_launch.size_member = draw_launch::normalized_positive_size(normalized_launch.size_member);  \
        const dim3 block = draw_kernel_block();                                                                \
        const dim3 grid =                                                                                      \
            draw_kernel_grid(normalized_launch.surface_member.width, normalized_launch.surface_member.height); \
        kernel<<<grid, block, 0, normalized_launch.stream>>>(normalized_launch);                               \
    }

MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(launch_draw_masks_boxes, draw_launch::MaskBoxLabelRgbLaunch, image,
                                                   box_thickness, has_valid_mask_box_label_rgb_launch(launch),
                                                   draw_masks_and_boxes_kernel)

MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(launch_draw_boxes_labels_bgr_pitched,
                                                   draw_launch::BoxLabelBgrPitchedLaunch, image, box_thickness,
                                                   draw_launch::is_valid(launch.image) &&
                                                       has_valid_box_label_inputs(launch.instances),
                                                   draw_boxes_labels_bgr_pitched_kernel)

MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_build_instance_colors_from_zero_based_labels,
                                draw_launch::InstanceColorBuildLaunch) {
    if (launch.labels == nullptr || launch.colors_rgb == nullptr || launch.count == 0U) {
        return;
    }

    const int safe_count = static_cast<int>(launch.count);
    constexpr int threads = 128;
    const int blocks = cuda_launch::linear_blocks_for(safe_count, threads);
    build_instance_colors_from_labels_kernel<<<blocks, threads, 0, launch.stream>>>(launch);
}

MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(launch_draw_masks_boxes_labels_bgr_pitched,
                                                   draw_launch::MaskBoxLabelBgrPitchedLaunch, image, box_thickness,
                                                   draw_launch::is_valid(launch.image) &&
                                                       launch.instances.masks != nullptr &&
                                                       has_valid_mask_box_label_inputs(launch.instances),
                                                   draw_masks_boxes_labels_bgr_pitched_kernel)

MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(launch_draw_analysis_overlay_rgba_pitched,
                                                   draw_launch::AnalysisOverlayRgbaPitchedLaunch, overlay,
                                                   box_thickness,
                                                   draw_launch::is_valid(launch.overlay) &&
                                                       has_valid_mask_box_label_inputs(launch.instances),
                                                   draw_analysis_overlay_rgba_pitched_kernel)

MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_composite_rgba_over_bgr_pitched,
                                draw_launch::CompositeRgbaOverBgrPitchedLaunch) {
    if (!draw_launch::is_valid(launch.base_bgr) || !draw_launch::is_valid(launch.overlay_rgba)) {
        return;
    }

    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.base_bgr.width, launch.base_bgr.height);
    composite_rgba_over_bgr_pitched_kernel<<<grid, block, 0, launch.stream>>>(launch);
}

MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_composite_rgba_over_rgba_pitched,
                                draw_launch::CompositeRgbaOverRgbaPitchedLaunch) {
    if (!draw_launch::is_valid(launch.base_rgba) || !draw_launch::is_valid(launch.overlay_rgba)) {
        return;
    }

    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.base_rgba.width, launch.base_rgba.height);
    composite_rgba_over_rgba_pitched_kernel<<<grid, block, 0, launch.stream>>>(launch);
}

MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_composite_rgba_over_workspace_rgba,
                                draw_launch::CompositeRgbaOverWorkspaceRgbaLaunch) {
    if (!draw_launch::is_valid(launch.base_rgba) || !draw_launch::is_valid(launch.overlay_rgba)) {
        return;
    }
    if (launch.base_rgba.width != launch.overlay_rgba.width || launch.base_rgba.height != launch.overlay_rgba.height) {
        return;
    }
    if (launch.base_rgba.kind == draw_launch::WorkspaceRgbaTargetKind::Pitched) {
        launch_composite_rgba_over_rgba_pitched(draw_launch::CompositeRgbaOverRgbaPitchedLaunch{
            launch.base_rgba.pitched, launch.overlay_rgba, launch.stream});
        return;
    }

    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.base_rgba.width, launch.base_rgba.height);
    composite_rgba_over_workspace_rgba_kernel<<<grid, block, 0, launch.stream>>>(launch);
}

MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_copy_bgr_to_rgba_pitched, draw_launch::CopyBgrToRgbaPitchedLaunch) {
    if (!draw_launch::is_valid(launch.source_bgr) || !draw_launch::is_valid(launch.target_rgba)) {
        return;
    }
    if (launch.source_bgr.width != launch.target_rgba.width || launch.source_bgr.height != launch.target_rgba.height) {
        return;
    }

    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.source_bgr.width, launch.source_bgr.height);
    copy_bgr_to_rgba_pitched_kernel<<<grid, block, 0, launch.stream>>>(launch);
}

MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_copy_bgr_to_workspace_rgba, draw_launch::CopyBgrToWorkspaceRgbaLaunch) {
    if (!draw_launch::is_valid(launch.source_bgr) || !draw_launch::is_valid(launch.target_rgba)) {
        return;
    }
    if (launch.source_bgr.width != launch.target_rgba.width || launch.source_bgr.height != launch.target_rgba.height) {
        return;
    }
    if (launch.target_rgba.kind == draw_launch::WorkspaceRgbaTargetKind::Pitched) {
        launch_copy_bgr_to_rgba_pitched(draw_launch::CopyBgrToRgbaPitchedLaunch{
            launch.source_bgr, launch.target_rgba.pitched, launch.alpha, launch.stream});
        return;
    }

    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.source_bgr.width, launch.source_bgr.height);
    copy_bgr_to_workspace_rgba_kernel<<<grid, block, 0, launch.stream>>>(launch);
}

MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_fill_workspace_rgba_pattern, draw_launch::FillWorkspaceRgbaPatternLaunch) {
    if (!draw_launch::is_valid(launch.target_rgba)) {
        return;
    }

    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.target_rgba.width, launch.target_rgba.height);
    fill_workspace_rgba_pattern_kernel<<<grid, block, 0, launch.stream>>>(launch);
}

MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_draw_manual_mask_rgba_pitched, draw_launch::ManualMaskRgbaPitchedLaunch) {
    if (!draw_launch::is_valid(launch.overlay_region) || launch.mask == nullptr) {
        return;
    }

    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.overlay_region.width, launch.overlay_region.height);
    draw_manual_mask_rgba_pitched_kernel<<<grid, block, 0, launch.stream>>>(launch);
}

MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(launch_draw_box_outline_rgba_pitched,
                                                   draw_launch::BoxOutlineRgbaPitchedLaunch, overlay, thickness,
                                                   draw_launch::is_valid(launch.overlay),
                                                   draw_box_outline_rgba_pitched_kernel)

MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(launch_draw_selection_handles_rgba_pitched,
                                                   draw_launch::SelectionHandlesRgbaPitchedLaunch, overlay,
                                                   handle_radius, draw_launch::is_valid(launch.overlay),
                                                   draw_selection_handles_rgba_pitched_kernel)

MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(launch_draw_polyline_rgba_pitched,
                                                   draw_launch::PolylineRgbaPitchedLaunch, overlay, thickness,
                                                   draw_launch::is_valid(launch.overlay) &&
                                                       launch.points.points_xy != nullptr &&
                                                       launch.points.point_count >= 2,
                                                   draw_polyline_rgba_pitched_kernel)

MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(launch_draw_points_rgba_pitched,
                                                   draw_launch::PointsRgbaPitchedLaunch, overlay, radius,
                                                   draw_launch::is_valid(launch.overlay) &&
                                                       launch.points.points_xy != nullptr &&
                                                       launch.points.point_count > 0,
                                                   draw_points_rgba_pitched_kernel)

MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(launch_draw_skeleton_rgba_pitched,
                                                   draw_launch::SkeletonRgbaPitchedLaunch, overlay, thickness,
                                                   draw_launch::is_valid(launch.overlay) &&
                                                       launch.points.points_xy != nullptr &&
                                                       launch.edges.edge_indices != nullptr &&
                                                       launch.points.point_count > 0 && launch.edges.edge_count > 0,
                                                   draw_skeleton_rgba_pitched_kernel)

#undef MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER
#undef MMLTK_DRAW_CUDA_DEFINE_LAUNCHER

}  // namespace mmltk::rfdetr
