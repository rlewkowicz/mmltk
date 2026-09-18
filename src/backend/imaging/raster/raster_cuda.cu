#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <cstdint>
#include <algorithm>
#include "src/frameworks/gpu/cuda_launch.cuh"
#include "detail/raster_math.cuh"
#include "class_palette.h"
#include "detail/raster_cuda_abi.h"
namespace mmltk::backend::imaging::raster::detail {
namespace raster_math = mmltk::backend::imaging::raster::math;
namespace cuda_launch = mmltk::frameworks::gpu::launch;
__global__ void probe_rgba_kernel(const draw_launch::ProbeRgbaLaunch launch) {
    const auto index = static_cast<std::size_t>(threadIdx.x);
    if (index >= 25U) return;
    const auto x = launch.coordinates[index * 2U];
    const auto y = launch.coordinates[index * 2U + 1U];
    const auto* pixel = launch.source.pixels + static_cast<std::size_t>(y) * launch.source.pitch_bytes + static_cast<std::size_t>(x) * 4U;
    launch.samples[index] = static_cast<std::uint32_t>(pixel[0]) | (static_cast<std::uint32_t>(pixel[1]) << 8U) |
                            (static_cast<std::uint32_t>(pixel[2]) << 16U) | (static_cast<std::uint32_t>(pixel[3]) << 24U);
}
cudaError_t launch_probe_rgba(const draw_launch::ProbeRgbaLaunch& launch) noexcept {
    probe_rgba_kernel<<<1, 32, 0, launch.stream>>>(launch);
    return cudaGetLastError();
}
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
inline dim3 draw_kernel_block() { return dim3(16, 16, 1); }
inline dim3 draw_kernel_grid(const int width, const int height) { return cuda_launch::make_2d_grid(width, height, draw_kernel_block()); }
__device__ inline int global_thread_x() { return static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x); }
__device__ inline int global_thread_y() { return static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y); }
[[nodiscard]] inline bool has_valid_box_label_inputs(const draw_launch::BoxLabelInputs& inputs) {
    return inputs.boxes != nullptr && inputs.colors != nullptr && inputs.labels != nullptr && inputs.instance_count > 0;
}
[[nodiscard]] inline bool has_valid_mask_box_label_inputs(const draw_launch::MaskBoxLabelInputs& inputs) {
    return inputs.boxes != nullptr && inputs.colors != nullptr && inputs.labels != nullptr && inputs.instance_count > 0;
}
[[nodiscard]] inline bool has_valid_mask_box_label_rgb_launch(const draw_launch::MaskBoxLabelRgbLaunch& launch) {
    return draw_launch::is_valid(launch.image) && launch.instances.masks != nullptr && has_valid_mask_box_label_inputs(launch.instances);
}
template <typename Surface>
__device__ bool pixel_xy_in_bounds(const Surface& surface, int& x, int& y) {
    x = global_thread_x();
    y = global_thread_y();
    return x < surface.width && y < surface.height;
}
__device__ bool load_visible_rgba_overlay(const draw_launch::ConstSurfaceU8& overlay, const int x, const int y, raster_math::RgbaPixelU8& pixel) {
    pixel = raster_math::load_rgba_pixel(overlay.pixels, overlay.pitch_bytes, x, y);
    return pixel.a != 0U;
}
template <typename Surface>
__device__ bool load_composite_rgba_overlay(const Surface& base, const draw_launch::ConstSurfaceU8& overlay, int& x, int& y, raster_math::RgbaPixelU8& pixel) {
    return pixel_xy_in_bounds(base, x, y) && load_visible_rgba_overlay(overlay, x, y, pixel);
}
// Prologue shared by the RGBA-over-RGBA composite kernels: this thread's pixel coordinates plus the
// overlay sample, with `visible` false when the thread is out of bounds or the overlay is clear.
struct CompositeRgbaOverlaySample {
    int x = 0;
    int y = 0;
    raster_math::RgbaPixelU8 overlay_pixel{};
    bool visible = false;
};
template <typename Launch>
__device__ CompositeRgbaOverlaySample load_composite_rgba_overlay_sample(const Launch& launch) {
    CompositeRgbaOverlaySample sample;
    sample.visible = load_composite_rgba_overlay(launch.base_rgba, launch.overlay_rgba, sample.x, sample.y, sample.overlay_pixel);
    return sample;
}
__global__ void build_instance_colors_from_labels_kernel(const draw_launch::InstanceColorBuildLaunch launch) {
    const int index = global_thread_x();
    const int count = static_cast<int>(launch.count);
    if (index >= count) { return; }
    const int safe_class_count = color::safe_class_count(launch.num_classes);
    int label = color::normalize_label(launch.labels[index], safe_class_count);
    const int color_offset = index * 3;
    color::class_color(label, safe_class_count, launch.colors_rgb[color_offset], launch.colors_rgb[color_offset + 1], launch.colors_rgb[color_offset + 2]);
}
__global__ void scale_rgba_kernel(const draw_launch::ScaleRgbaLaunch launch) {
    const auto x = global_thread_x();
    const auto y = global_thread_y();
    if (x >= launch.target.width || y >= launch.target.height) return;
    const auto sx = static_cast<int>(static_cast<std::uint64_t>(x) * launch.source.width / launch.target.width);
    const auto sy = static_cast<int>(static_cast<std::uint64_t>(y) * launch.source.height / launch.target.height);
    const auto pixel = raster_math::load_rgba_pixel(launch.source.pixels, launch.source.pitch_bytes, sx, sy);
    raster_math::store_rgba_pixel(launch.target.pixels, launch.target.pitch_bytes, x, y, pixel);
}
cudaError_t launch_scale_rgba(const draw_launch::ScaleRgbaLaunch& launch) noexcept {
    if (!launch.source.valid(4U) || !launch.target.valid(4U)) return cudaErrorInvalidValue;
    scale_rgba_kernel<<<draw_kernel_grid(launch.target.width, launch.target.height), draw_kernel_block(), 0, launch.stream>>>(launch);
    return cudaGetLastError();
}
__device__ bool pixel_hits_box_edge(int x, int y, int x1, int y1, int x2, int y2, int box_thickness) {
    if (x < x1 - box_thickness || x > x2 + box_thickness || y < y1 - box_thickness || y > y2 + box_thickness) { return false; }
    return x < x1 || x > x2 || y < y1 || y > y2;
}
__device__ bool pixel_hits_label_digit(int x, int y, int x1, int y1, int label) {
    if (label < 0) { return false; }
    const int tx = x - x1;
    const int ty = y - (y1 - 16);
    if (ty < 0 || ty >= 14 || tx < 0 || tx >= 24) { return false; }
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
        if (tx < char_start_x || tx >= char_start_x + 10) { continue; }
        const int font_x = (tx - char_start_x) / 2;
        const int font_y = ty / 2;
        if (font_x < 5 && font_y < 7 && (d_font5x7[digit][font_y] & (0x80 >> font_x))) { return true; }
    }
    return false;
}
template <typename PixelT>
__device__ void apply_box_color(PixelT& pixel, const uint8_t* colors, int offset);
template <>
__device__ void apply_box_color<raster_math::RgbPixelFloat>(raster_math::RgbPixelFloat& pixel, const uint8_t* colors, int offset) {
    raster_math::apply_rgb(&pixel, colors, offset);
}
template <>
__device__ void apply_box_color<raster_math::RgbaPixelU8>(raster_math::RgbaPixelU8& pixel, const uint8_t* colors, int offset) {
    raster_math::apply_rgb(&pixel.r, &pixel.g, &pixel.b, colors, offset);
    pixel.a = 255U;
}
template <typename PixelT>
__device__ void apply_boxes_and_labels(int x, int y, const float* boxes, const uint8_t* colors, const int* labels, int num_instances, int box_thickness,
                                       PixelT& pixel, bool labels_enabled = true) {
    if (box_thickness <= 0 && !labels_enabled) return;
    for (int i = 0; i < num_instances; ++i) {
        const int x1 = static_cast<int>(boxes[i * 4 + 0]);
        const int y1 = static_cast<int>(boxes[i * 4 + 1]);
        const int x2 = static_cast<int>(boxes[i * 4 + 2]);
        const int y2 = static_cast<int>(boxes[i * 4 + 3]);
        const bool is_edge = box_thickness > 0 && pixel_hits_box_edge(x, y, x1, y1, x2, y2, box_thickness);
        const bool is_label = labels_enabled && pixel_hits_label_digit(x, y, x1, y1, labels[i]);
        if (is_edge || is_label) { apply_box_color(pixel, colors, i * 3); }
    }
}
template <typename PixelT, typename InstancesT>
__device__ void blend_instance_masks(PixelT& pixel, const InstancesT& instances, const int image_width, const int image_height, const int x, const int y,
                                     const float mask_alpha) {
    const int image_area = image_width * image_height;
    const int pixel_index = y * image_width + x;
    for (int i = 0; i < instances.instance_count; ++i) {
        if (!instances.masks[i * image_area + pixel_index]) { continue; }
        pixel = raster_math::blend_rgb(pixel, instances.colors[i * 3], instances.colors[i * 3 + 1], instances.colors[i * 3 + 2], mask_alpha);
    }
}
template <typename LaunchT, typename PixelT>
__device__ void blend_launch_masks(PixelT& pixel, const LaunchT& launch, const int image_width, const int image_height, const int x, const int y) {
    blend_instance_masks(pixel, launch.instances, image_width, image_height, x, y, launch.mask_alpha);
}
template <typename LaunchT, typename PixelT>
__device__ void apply_launch_boxes_and_labels(const int x, const int y, const LaunchT& launch, PixelT& pixel) {
    const auto& instances = launch.instances;
    apply_boxes_and_labels(x, y, instances.boxes, instances.colors, instances.labels, instances.instance_count, launch.box_thickness, pixel);
}
template <typename OverlayT, typename ColorT>
__device__ bool store_segment_hit_rgba_pixel(const OverlayT& overlay, const int x, const int y, const float px, const float py, const float ax, const float ay,
                                             const float bx, const float by, const float max_distance_sq, const ColorT& color) {
    if (raster_math::point_to_segment_distance_sq(px, py, ax, ay, bx, by) > max_distance_sq) { return false; }
    raster_math::store_rgba_pixel(overlay.pixels, overlay.pitch_bytes, x, y, raster_math::RgbaPixelU8{color.r, color.g, color.b, 255U});
    return true;
}
// Colors blending kernel
__global__ void draw_masks_and_boxes_kernel(const draw_launch::MaskBoxLabelRgbLaunch launch) {
    const int x = global_thread_x();
    const int y = global_thread_y();
    const auto& image = launch.image;
    if (x >= image.width || y >= image.height) { return; }
    auto pixel = raster_math::load_rgb_pixel<raster_math::RgbByteOrder::Rgb>(image.pixels, static_cast<std::size_t>(image.width) * 3U, x, y);
    blend_launch_masks(pixel, launch, image.width, image.height, x, y);
    apply_launch_boxes_and_labels(x, y, launch, pixel);
    raster_math::store_rgb_pixel<raster_math::RgbByteOrder::Rgb>(image.pixels, static_cast<std::size_t>(image.width) * 3U, x, y, pixel);
}
__global__ void draw_boxes_labels_bgr_pitched_kernel(const draw_launch::BoxLabelBgrPitchedLaunch launch) {
    const auto& image = launch.image;
    const int x = global_thread_x();
    const int y = global_thread_y();
    if (x >= image.width || y >= image.height) { return; }
    auto pixel = raster_math::load_rgb_pixel<raster_math::RgbByteOrder::Bgr>(image.pixels, image.pitch_bytes, x, y);
    apply_launch_boxes_and_labels(x, y, launch, pixel);
    raster_math::store_rgb_pixel<raster_math::RgbByteOrder::Bgr>(image.pixels, image.pitch_bytes, x, y, pixel);
}
__global__ void draw_masks_boxes_labels_bgr_pitched_kernel(const draw_launch::MaskBoxLabelBgrPitchedLaunch launch) {
    const auto& image = launch.image;
    const int x = global_thread_x();
    const int y = global_thread_y();
    if (x >= image.width || y >= image.height) { return; }
    auto pixel = raster_math::load_rgb_pixel<raster_math::RgbByteOrder::Bgr>(image.pixels, image.pitch_bytes, x, y);
    blend_launch_masks(pixel, launch, image.width, image.height, x, y);
    apply_launch_boxes_and_labels(x, y, launch, pixel);
    raster_math::store_rgb_pixel<raster_math::RgbByteOrder::Bgr>(image.pixels, image.pitch_bytes, x, y, pixel);
}
__global__ void draw_analysis_overlay_rgba_pitched_kernel(const draw_launch::AnalysisOverlayRgbaPitchedLaunch launch) {
    const auto& overlay = launch.overlay;
    const auto& instances = launch.instances;
    const int x = global_thread_x();
    const int y = global_thread_y();
    if (x >= overlay.width || y >= overlay.height) { return; }
    raster_math::RgbaPixelU8 pixel{};
    if (instances.masks != nullptr) {
        for (int i = 0; i < instances.instance_count; ++i) {
            if (!instances.masks[i * overlay.width * overlay.height + y * overlay.width + x]) { continue; }
            raster_math::apply_rgb(&pixel.r, &pixel.g, &pixel.b, instances.colors, i * 3);
            pixel.a = launch.mask_alpha;
        }
    }
    apply_boxes_and_labels(x, y, instances.boxes, instances.colors, instances.labels, instances.instance_count, launch.box_thickness, pixel, launch.labels);
    if (launch.add_rgb_to_existing) {
        const auto existing = raster_math::load_rgba_pixel(overlay.pixels, overlay.pitch_bytes, x, y);
        pixel = raster_math::add_layer_rgb(existing, pixel);
    }
    raster_math::store_rgba_pixel(overlay.pixels, overlay.pitch_bytes, x, y, pixel);
}
__global__ void composite_rgba_over_bgr_pitched_kernel(const draw_launch::CompositeRgbaOverBgrPitchedLaunch launch) {
    const auto& base_bgr = launch.base_bgr;
    const auto& overlay_rgba = launch.overlay_rgba;
    const int x = global_thread_x();
    const int y = global_thread_y();
    if (x >= base_bgr.width || y >= base_bgr.height) { return; }
    raster_math::RgbaPixelU8 overlay_pixel{};
    if (!load_visible_rgba_overlay(overlay_rgba, x, y, overlay_pixel)) { return; }
    const auto base_pixel = raster_math::load_rgb_pixel<raster_math::RgbByteOrder::Bgr>(base_bgr.pixels, base_bgr.pitch_bytes, x, y);
    raster_math::store_rgb_pixel<raster_math::RgbByteOrder::Bgr>(base_bgr.pixels, base_bgr.pitch_bytes, x, y,
                                                                 raster_math::composite_rgba_over_bgr(base_pixel, overlay_pixel));
}
__global__ void finalize_rgba_kernel(const draw_launch::FinalizeRgbaLaunch launch) {
    const auto dx = blockIdx.x * blockDim.x + threadIdx.x;
    const auto dy = blockIdx.y * blockDim.y + threadIdx.y;
    if (dx >= static_cast<unsigned>(launch.region.x2 - launch.region.x1) || dy >= static_cast<unsigned>(launch.region.y2 - launch.region.y1)) return;
    const int x = launch.region.x1 + static_cast<int>(dx);
    const int y = launch.region.y1 + static_cast<int>(dy);
    auto pixel = raster_math::load_rgba_pixel(launch.clean.pixels, launch.clean.pitch_bytes, x, y);
    if (launch.semantic.pixels) {
        const auto overlay = raster_math::load_rgba_pixel(launch.semantic.pixels, launch.semantic.pitch_bytes, x, y);
        if (overlay.a != 0U) pixel = raster_math::composite_rgba_over_rgba(pixel, overlay);
    }
    raster_math::store_rgba_pixel(launch.destination.pixels, launch.destination.pitch_bytes, x, y, pixel);
}
cudaError_t launch_finalize_rgba(const draw_launch::FinalizeRgbaLaunch& launch) noexcept {
    if (!launch.clean.valid(4U) || !launch.destination.valid(4U) || launch.region.x1 < 0 || launch.region.y1 < 0 || launch.region.x2 <= launch.region.x1 ||
        launch.region.y2 <= launch.region.y1 || launch.region.x2 > launch.clean.width || launch.region.y2 > launch.clean.height ||
        launch.destination.width != launch.clean.width || launch.destination.height != launch.clean.height ||
        (launch.semantic.pixels &&
         (!launch.semantic.valid(4U) || launch.semantic.width != launch.clean.width || launch.semantic.height != launch.clean.height)))
        return cudaErrorInvalidValue;
    finalize_rgba_kernel<<<draw_kernel_grid(launch.region.x2 - launch.region.x1, launch.region.y2 - launch.region.y1), draw_kernel_block(), 0, launch.stream>>>(
        launch);
    return cudaGetLastError();
}
__global__ void composite_rgba_over_rgba_pitched_kernel(const draw_launch::CompositeRgbaOverRgbaPitchedLaunch launch) {
    const CompositeRgbaOverlaySample sample = load_composite_rgba_overlay_sample(launch);
    if (!sample.visible) { return; }
    const auto& base_rgba = launch.base_rgba;
    const auto base_pixel = raster_math::load_rgba_pixel(base_rgba.pixels, base_rgba.pitch_bytes, sample.x, sample.y);
    raster_math::store_rgba_pixel(base_rgba.pixels, base_rgba.pitch_bytes, sample.x, sample.y,
                                  raster_math::composite_rgba_over_rgba(base_pixel, sample.overlay_pixel));
}
__device__ raster_math::RgbaPixelU8 surface_read_rgba(const cudaSurfaceObject_t surface, const int x, const int y) {
    uchar4 raw{};
    surf2Dread(&raw, surface, x * static_cast<int>(sizeof(uchar4)), y);
    return raster_math::RgbaPixelU8{raw.x, raw.y, raw.z, raw.w};
}
__device__ void surface_write_rgba(const cudaSurfaceObject_t surface, const int x, const int y, const raster_math::RgbaPixelU8& pixel) {
    surf2Dwrite(make_uchar4(pixel.r, pixel.g, pixel.b, pixel.a), surface, x * static_cast<int>(sizeof(uchar4)), y);
}
__global__ void composite_rgba_kernel(const draw_launch::CompositeRgbaLaunchAbi launch) {
    const CompositeRgbaOverlaySample sample = load_composite_rgba_overlay_sample(launch);
    if (!sample.visible) { return; }
    const cudaSurfaceObject_t surface = launch.base_rgba.surface;
    const auto base_pixel = surface_read_rgba(surface, sample.x, sample.y);
    surface_write_rgba(surface, sample.x, sample.y, raster_math::composite_rgba_over_rgba(base_pixel, sample.overlay_pixel));
}
// Loads the bounds-checked BGR source pixel for the copy kernels and converts it to clamped RGBA with the
// requested alpha. Returns false (without writing) for threads outside the source image.
template <typename SourceBgr, typename Alpha>
__device__ bool load_bgr_as_rgba(const SourceBgr& source_bgr, const Alpha alpha, int& x, int& y, raster_math::RgbaPixelU8& pixel) {
    x = global_thread_x();
    y = global_thread_y();
    if (x >= source_bgr.width || y >= source_bgr.height) { return false; }
    const auto source_pixel = raster_math::load_rgb_pixel<raster_math::RgbByteOrder::Bgr>(source_bgr.pixels, source_bgr.pitch_bytes, x, y);
    pixel = raster_math::RgbaPixelU8{
        raster_math::clamp_to_u8(source_pixel.r),
        raster_math::clamp_to_u8(source_pixel.g),
        raster_math::clamp_to_u8(source_pixel.b),
        alpha,
    };
    return true;
}
__global__ void copy_bgr_to_rgba_pitched_kernel(const draw_launch::CopyBgrToRgbaPitchedLaunch launch) {
    const auto& target_rgba = launch.target_rgba;
    int x = 0;
    int y = 0;
    raster_math::RgbaPixelU8 pixel{};
    if (!load_bgr_as_rgba(launch.source_bgr, launch.alpha, x, y, pixel)) { return; }
    raster_math::store_rgba_pixel(target_rgba.pixels, target_rgba.pitch_bytes, x, y, pixel);
}
__global__ void copy_bgr_to_rgba_kernel(const draw_launch::CopyBgrToRgbaLaunchAbi launch) {
    int x = 0;
    int y = 0;
    raster_math::RgbaPixelU8 pixel{};
    if (!load_bgr_as_rgba(launch.source_bgr, launch.alpha, x, y, pixel)) { return; }
    surface_write_rgba(launch.target_rgba.surface, x, y, pixel);
}
__global__ void draw_manual_mask_rgba_pitched_kernel(const draw_launch::ManualMaskRgbaPitchedLaunch launch) {
    const auto& overlay_region = launch.overlay_region;
    const int x = global_thread_x();
    const int y = global_thread_y();
    if (x >= overlay_region.width || y >= overlay_region.height) { return; }
    if (launch.mask[y * overlay_region.width + x] == 0U) { return; }
    raster_math::store_rgba_pixel(overlay_region.pixels, overlay_region.pitch_bytes, x, y,
                                  raster_math::RgbaPixelU8{launch.color.r, launch.color.g, launch.color.b, launch.color.a});
}
__global__ void draw_manual_mask_runs_rgba_pitched_kernel(const draw_launch::ManualMaskRunsRgbaPitchedLaunch launch) {
    const std::uint32_t run_index = blockIdx.x;
    if (run_index >= launch.run_count) { return; }
    const std::uint64_t pixel_count = static_cast<std::uint64_t>(launch.overlay_region.width) * static_cast<std::uint64_t>(launch.overlay_region.height);
    const std::size_t pair_index = static_cast<std::size_t>(run_index) * 2U;
    const std::uint64_t start = launch.run_pairs[pair_index];
    const std::uint64_t length = launch.run_pairs[pair_index + 1U];
    if (start >= pixel_count) { return; }
    if (launch.scale_x != 1 || launch.scale_y != 1 || launch.source_x != launch.target_x || launch.source_y != launch.target_y) {
        if (launch.scale_x <= 0 || launch.scale_y <= 0 || length == 0) return;
        const auto width = static_cast<std::uint64_t>(launch.overlay_region.width);
        // NOLINTNEXTLINE(bugprone-integer-division): A linear pixel index selects an integer row before geometric projection.
        const float x = static_cast<float>(start % width), y = static_cast<float>(start / width);
        const auto project = [](float value, float source, float target, float scale) {
            // Match the separate float operations used when materializing the
            // saved intervals; contraction can cross a floor/ceil boundary.
            return __fadd_rn(target, __fmul_rn(__fsub_rn(value, source), scale));
        };
        const int first = max(max(0, launch.clip.x1), static_cast<int>(floorf(project(x, launch.source_x, launch.target_x, launch.scale_x))));
        const int last = min(min(launch.overlay_region.width, launch.clip.x2),
                             static_cast<int>(ceilf(project(x + static_cast<float>(length), launch.source_x, launch.target_x, launch.scale_x))));
        const int top = max(max(0, launch.clip.y1), static_cast<int>(floorf(project(y, launch.source_y, launch.target_y, launch.scale_y))));
        const int bottom =
            min(min(launch.overlay_region.height, launch.clip.y2), static_cast<int>(ceilf(project(y + 1, launch.source_y, launch.target_y, launch.scale_y))));
        if (first >= last || top >= bottom) return;
        const auto count = static_cast<std::uint64_t>(last - first) * static_cast<std::uint64_t>(bottom - top);
        for (auto pixel = static_cast<std::uint64_t>(threadIdx.x); pixel < count; pixel += blockDim.x) {
            const auto px = first + static_cast<int>(pixel % static_cast<std::uint64_t>(last - first));
            const auto py = top + static_cast<int>(pixel / static_cast<std::uint64_t>(last - first));
            // Scaling can map disjoint base intervals onto the same destination
            // pixel. A single atomic RGBA store preserves canonical union
            // coverage without a conflicting write or transformed-run upload.
            const auto color = static_cast<unsigned>(launch.color.r) | (static_cast<unsigned>(launch.color.g) << 8U) |
                               (static_cast<unsigned>(launch.color.b) << 16U) | (static_cast<unsigned>(launch.color.a) << 24U);
            auto* destination = reinterpret_cast<unsigned*>(launch.overlay_region.pixels + static_cast<std::size_t>(py) * launch.overlay_region.pitch_bytes +
                                                            static_cast<std::size_t>(px) * 4U);
            atomicExch(destination, color);
        }
        return;
    }
    const std::uint64_t unclamped_end = start > UINT64_MAX - length ? UINT64_MAX : start + length;
    const std::uint64_t end = unclamped_end < pixel_count ? unclamped_end : pixel_count;
    for (std::uint64_t pixel = start + threadIdx.x; pixel < end; pixel += blockDim.x) {
        const int x = static_cast<int>(pixel % static_cast<std::uint64_t>(launch.overlay_region.width));
        const int y = static_cast<int>(pixel / static_cast<std::uint64_t>(launch.overlay_region.width));
        if (x < launch.clip.x1 || x >= launch.clip.x2 || y < launch.clip.y1 || y >= launch.clip.y2) continue;
        raster_math::store_rgba_pixel(launch.overlay_region.pixels, launch.overlay_region.pitch_bytes, x, y,
                                      raster_math::RgbaPixelU8{launch.color.r, launch.color.g, launch.color.b, launch.color.a});
    }
}
__global__ void draw_box_outline_rgba_pitched_kernel(const draw_launch::BoxOutlineRgbaPitchedLaunch launch) {
    const auto& overlay = launch.overlay;
    const auto& box = launch.box;
    const int x = launch.clip.x1 + global_thread_x();
    const int y = launch.clip.y1 + global_thread_y();
    if (x >= launch.clip.x2 || y >= launch.clip.y2 || x >= overlay.width || y >= overlay.height) { return; }
    if (!pixel_hits_box_edge(x, y, box.x1, box.y1, box.x2 - 1, box.y2 - 1, launch.thickness)) { return; }
    raster_math::store_rgba_pixel(overlay.pixels, overlay.pitch_bytes, x, y, raster_math::RgbaPixelU8{launch.color.r, launch.color.g, launch.color.b, 255U});
}
__global__ void draw_selection_handles_rgba_pitched_kernel(const draw_launch::SelectionHandlesRgbaPitchedLaunch launch) {
    const auto& overlay = launch.overlay;
    const auto& box = launch.box;
    const int x = launch.clip.x1 + global_thread_x();
    const int y = launch.clip.y1 + global_thread_y();
    if (x >= launch.clip.x2 || y >= launch.clip.y2 || x >= overlay.width || y >= overlay.height) { return; }
    const int corners_x[4] = {box.x1, box.x2 - 1, box.x1, box.x2 - 1};
    const int corners_y[4] = {box.y1, box.y1, box.y2 - 1, box.y2 - 1};
    for (int i = 0; i < 4; ++i) {
        if (x < corners_x[i] - launch.handle_radius || x > corners_x[i] + launch.handle_radius || y < corners_y[i] - launch.handle_radius ||
            y > corners_y[i] + launch.handle_radius) {
            continue;
        }
        raster_math::store_rgba_pixel(overlay.pixels, overlay.pitch_bytes, x, y,
                                      raster_math::RgbaPixelU8{launch.color.r, launch.color.g, launch.color.b, launch.color.a});
        return;
    }
}
__global__ void draw_polyline_rgba_pitched_kernel(const draw_launch::PolylineRgbaPitchedLaunch launch) {
    const auto& overlay = launch.overlay;
    const auto& points = launch.points;
    const int x = launch.clip.x1 + global_thread_x();
    const int y = launch.clip.y1 + global_thread_y();
    if (x >= launch.clip.x2 || y >= launch.clip.y2 || x >= overlay.width || y >= overlay.height || points.points_xy == nullptr || points.point_count < 2) {
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
        if (store_segment_hit_rgba_pixel(overlay, x, y, px, py, ax, ay, bx, by, max_distance_sq, launch.color)) { return; }
    }
}
__global__ void draw_points_rgba_pitched_kernel(const draw_launch::PointsRgbaPitchedLaunch launch) {
    const auto& overlay = launch.overlay;
    const auto& points = launch.points;
    const int x = launch.clip.x1 + global_thread_x();
    const int y = launch.clip.y1 + global_thread_y();
    if (x >= launch.clip.x2 || y >= launch.clip.y2 || x >= overlay.width || y >= overlay.height || points.points_xy == nullptr || points.point_count <= 0) {
        return;
    }
    const float px = static_cast<float>(x) + 0.5f;
    const float py = static_cast<float>(y) + 0.5f;
    const float max_distance_sq = static_cast<float>(launch.radius * launch.radius);
    for (int point_index = 0; point_index < points.point_count; ++point_index) {
        const int xy_index = point_index * 2;
        const float qx = static_cast<float>(points.points_xy[xy_index + 0]);
        const float qy = static_cast<float>(points.points_xy[xy_index + 1]);
        if (raster_math::point_distance_sq(px, py, qx, qy) > max_distance_sq) { continue; }
        raster_math::store_rgba_pixel(overlay.pixels, overlay.pitch_bytes, x, y,
                                      raster_math::RgbaPixelU8{launch.color.r, launch.color.g, launch.color.b, launch.color.a});
        return;
    }
}
__global__ void draw_skeleton_rgba_pitched_kernel(const draw_launch::SkeletonRgbaPitchedLaunch launch) {
    const auto& overlay = launch.overlay;
    const auto& points = launch.points;
    const auto& edges = launch.edges;
    const int x = launch.clip.x1 + global_thread_x();
    const int y = launch.clip.y1 + global_thread_y();
    if (x >= launch.clip.x2 || y >= launch.clip.y2 || x >= overlay.width || y >= overlay.height || points.points_xy == nullptr ||
        edges.edge_indices == nullptr || points.point_count <= 0 || edges.edge_count <= 0) {
        return;
    }
    const float px = static_cast<float>(x) + 0.5f;
    const float py = static_cast<float>(y) + 0.5f;
    const float max_distance_sq = fmaxf(1.0f, static_cast<float>(launch.thickness * launch.thickness));
    for (int edge_index = 0; edge_index < edges.edge_count; ++edge_index) {
        const int pair_index = edge_index * 2;
        const std::uint32_t source_index = edges.edge_indices[pair_index + 0];
        const std::uint32_t target_index = edges.edge_indices[pair_index + 1];
        if (source_index >= static_cast<std::uint32_t>(points.point_count) || target_index >= static_cast<std::uint32_t>(points.point_count)) { continue; }
        const int source_xy_index = static_cast<int>(source_index) * 2;
        const int target_xy_index = static_cast<int>(target_index) * 2;
        const float ax = static_cast<float>(points.points_xy[source_xy_index + 0]);
        const float ay = static_cast<float>(points.points_xy[source_xy_index + 1]);
        const float bx = static_cast<float>(points.points_xy[target_xy_index + 0]);
        const float by = static_cast<float>(points.points_xy[target_xy_index + 1]);
        if (store_segment_hit_rgba_pixel(overlay, x, y, px, py, ax, ay, bx, by, max_distance_sq, launch.color)) { return; }
    }
}
#define MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(name, launch_type) cudaError_t name(const launch_type& launch) noexcept
#define MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(name, launch_type, surface_member, size_member, valid_expression, kernel) \
    MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(name, launch_type) {                                                                             \
        if (!(valid_expression)) { return cudaErrorInvalidValue; }                                                                   \
        auto normalized_launch = launch;                                                                                             \
        normalized_launch.size_member = draw_launch::normalized_positive_size(normalized_launch.size_member);                        \
        const dim3 block = draw_kernel_block();                                                                                      \
        const dim3 grid = draw_kernel_grid(normalized_launch.surface_member.width, normalized_launch.surface_member.height);         \
        kernel<<<grid, block, 0, normalized_launch.stream>>>(normalized_launch);                                                     \
        return cudaGetLastError();                                                                                                   \
    }
MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(launch_draw_masks_boxes, draw_launch::MaskBoxLabelRgbLaunch, image, box_thickness,
                                                   has_valid_mask_box_label_rgb_launch(launch), draw_masks_and_boxes_kernel)
MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(launch_draw_boxes_labels_bgr_pitched, draw_launch::BoxLabelBgrPitchedLaunch, image, box_thickness,
                                                   draw_launch::is_valid(launch.image) && has_valid_box_label_inputs(launch.instances),
                                                   draw_boxes_labels_bgr_pitched_kernel)
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_build_instance_colors_from_zero_based_labels, draw_launch::InstanceColorBuildLaunch) {
    if (launch.labels == nullptr || launch.colors_rgb == nullptr || launch.count == 0U) { return cudaErrorInvalidValue; }
    const int safe_count = static_cast<int>(launch.count);
    constexpr int threads = 128;
    const int blocks = cuda_launch::linear_blocks_for(safe_count, threads);
    build_instance_colors_from_labels_kernel<<<blocks, threads, 0, launch.stream>>>(launch);
    return cudaGetLastError();
}
MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER(launch_draw_masks_boxes_labels_bgr_pitched, draw_launch::MaskBoxLabelBgrPitchedLaunch, image, box_thickness,
                                                   draw_launch::is_valid(launch.image) && launch.instances.masks != nullptr &&
                                                       has_valid_mask_box_label_inputs(launch.instances),
                                                   draw_masks_boxes_labels_bgr_pitched_kernel)
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_draw_analysis_overlay_rgba_pitched, draw_launch::AnalysisOverlayRgbaPitchedLaunch) {
    if (!draw_launch::is_valid(launch.overlay) || !has_valid_mask_box_label_inputs(launch.instances) || launch.box_thickness < 0) return cudaErrorInvalidValue;
    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.overlay.width, launch.overlay.height);
    draw_analysis_overlay_rgba_pitched_kernel<<<grid, block, 0, launch.stream>>>(launch);
    return cudaGetLastError();
}
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_composite_rgba_over_bgr_pitched, draw_launch::CompositeRgbaOverBgrPitchedLaunch) {
    if (!draw_launch::is_valid(launch.base_bgr) || !draw_launch::is_valid(launch.overlay_rgba)) { return cudaErrorInvalidValue; }
    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.base_bgr.width, launch.base_bgr.height);
    composite_rgba_over_bgr_pitched_kernel<<<grid, block, 0, launch.stream>>>(launch);
    return cudaGetLastError();
}
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_composite_rgba_over_rgba_pitched, draw_launch::CompositeRgbaOverRgbaPitchedLaunch) {
    if (!draw_launch::is_valid(launch.base_rgba) || !draw_launch::is_valid(launch.overlay_rgba)) { return cudaErrorInvalidValue; }
    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.base_rgba.width, launch.base_rgba.height);
    composite_rgba_over_rgba_pitched_kernel<<<grid, block, 0, launch.stream>>>(launch);
    return cudaGetLastError();
}
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_composite_rgba, draw_launch::CompositeRgbaLaunchAbi) {
    if (!draw_launch::is_valid(launch.base_rgba) || !draw_launch::is_valid(launch.overlay_rgba)) { return cudaErrorInvalidValue; }
    if (launch.base_rgba.width != launch.overlay_rgba.width || launch.base_rgba.height != launch.overlay_rgba.height) { return cudaErrorInvalidValue; }
    if (launch.base_rgba.kind == draw_launch::RgbaTargetKindAbi::Pitched) {
        return launch_composite_rgba_over_rgba_pitched(
            draw_launch::CompositeRgbaOverRgbaPitchedLaunch{launch.base_rgba.pitched, launch.overlay_rgba, launch.stream});
    }
    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.base_rgba.width, launch.base_rgba.height);
    composite_rgba_kernel<<<grid, block, 0, launch.stream>>>(launch);
    return cudaGetLastError();
}
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_copy_bgr_to_rgba_pitched, draw_launch::CopyBgrToRgbaPitchedLaunch) {
    if (!draw_launch::is_valid(launch.source_bgr) || !draw_launch::is_valid(launch.target_rgba)) { return cudaErrorInvalidValue; }
    if (launch.source_bgr.width != launch.target_rgba.width || launch.source_bgr.height != launch.target_rgba.height) { return cudaErrorInvalidValue; }
    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.source_bgr.width, launch.source_bgr.height);
    copy_bgr_to_rgba_pitched_kernel<<<grid, block, 0, launch.stream>>>(launch);
    return cudaGetLastError();
}
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_copy_bgr_to_rgba, draw_launch::CopyBgrToRgbaLaunchAbi) {
    if (!draw_launch::is_valid(launch.source_bgr) || !draw_launch::is_valid(launch.target_rgba)) { return cudaErrorInvalidValue; }
    if (launch.source_bgr.width != launch.target_rgba.width || launch.source_bgr.height != launch.target_rgba.height) { return cudaErrorInvalidValue; }
    if (launch.target_rgba.kind == draw_launch::RgbaTargetKindAbi::Pitched) {
        return launch_copy_bgr_to_rgba_pitched(
            draw_launch::CopyBgrToRgbaPitchedLaunch{launch.source_bgr, launch.target_rgba.pitched, launch.alpha, launch.stream});
    }
    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.source_bgr.width, launch.source_bgr.height);
    copy_bgr_to_rgba_kernel<<<grid, block, 0, launch.stream>>>(launch);
    return cudaGetLastError();
}
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_draw_manual_mask_rgba_pitched, draw_launch::ManualMaskRgbaPitchedLaunch) {
    if (!draw_launch::is_valid(launch.overlay_region) || launch.mask == nullptr) { return cudaErrorInvalidValue; }
    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.overlay_region.width, launch.overlay_region.height);
    draw_manual_mask_rgba_pitched_kernel<<<grid, block, 0, launch.stream>>>(launch);
    return cudaGetLastError();
}
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_draw_manual_mask_runs_rgba_pitched, draw_launch::ManualMaskRunsRgbaPitchedLaunch) {
    if (!draw_launch::is_valid(launch.overlay_region) || launch.run_pairs == nullptr || launch.run_count == 0U) { return cudaErrorInvalidValue; }
    constexpr unsigned int kThreadsPerRun = 256U;
    draw_manual_mask_runs_rgba_pitched_kernel<<<launch.run_count, kThreadsPerRun, 0, launch.stream>>>(launch);
    return cudaGetLastError();
}
template <auto Kernel, class Launch>
cudaError_t launch_editor_shape(Launch& launch, int& size) noexcept {
    if (!draw_launch::is_valid(launch.overlay)) return cudaErrorInvalidValue;
    if constexpr (requires { launch.points; }) {
        const int minimum = requires { launch.closed; } ? 2 : 1;
        if (!launch.points.points_xy || launch.points.point_count < minimum) return cudaErrorInvalidValue;
    }
    if constexpr (requires { launch.edges; })
        if (!launch.edges.edge_indices || launch.edges.edge_count <= 0) return cudaErrorInvalidValue;
    launch.clip.x1 = std::max(0, launch.clip.x1);
    launch.clip.y1 = std::max(0, launch.clip.y1);
    launch.clip.x2 = std::min(launch.overlay.width, launch.clip.x2);
    launch.clip.y2 = std::min(launch.overlay.height, launch.clip.y2);
    if (launch.clip.x1 >= launch.clip.x2 || launch.clip.y1 >= launch.clip.y2) return cudaSuccess;
    size = draw_launch::normalized_positive_size(size);
    const dim3 block = draw_kernel_block();
    const dim3 grid = draw_kernel_grid(launch.clip.x2 - launch.clip.x1, launch.clip.y2 - launch.clip.y1);
    void* arguments[] = {&launch};
    return cudaLaunchKernel(reinterpret_cast<const void*>(Kernel), grid, block, arguments, 0U, launch.stream);
}
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_draw_box_outline_rgba_pitched, draw_launch::BoxOutlineRgbaPitchedLaunch) {
    auto normalized = launch;
    return launch_editor_shape<draw_box_outline_rgba_pitched_kernel>(normalized, normalized.thickness);
}
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_draw_selection_handles_rgba_pitched, draw_launch::SelectionHandlesRgbaPitchedLaunch) {
    auto normalized = launch;
    return launch_editor_shape<draw_selection_handles_rgba_pitched_kernel>(normalized, normalized.handle_radius);
}
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_draw_polyline_rgba_pitched, draw_launch::PolylineRgbaPitchedLaunch) {
    auto normalized = launch;
    return launch_editor_shape<draw_polyline_rgba_pitched_kernel>(normalized, normalized.thickness);
}
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_draw_points_rgba_pitched, draw_launch::PointsRgbaPitchedLaunch) {
    auto normalized = launch;
    return launch_editor_shape<draw_points_rgba_pitched_kernel>(normalized, normalized.radius);
}
MMLTK_DRAW_CUDA_DEFINE_LAUNCHER(launch_draw_skeleton_rgba_pitched, draw_launch::SkeletonRgbaPitchedLaunch) {
    auto normalized = launch;
    return launch_editor_shape<draw_skeleton_rgba_pitched_kernel>(normalized, normalized.thickness);
}
#undef MMLTK_DRAW_CUDA_DEFINE_NORMALIZED_SURFACE_LAUNCHER
#undef MMLTK_DRAW_CUDA_DEFINE_LAUNCHER
}  // namespace mmltk::backend::imaging::raster::detail
