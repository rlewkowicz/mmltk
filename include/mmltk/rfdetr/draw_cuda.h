#pragma once

#include <cuda_runtime_api.h>
#include <torch/torch.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <filesystem>
#include <optional>
#include <vector>

namespace mmltk::rfdetr {

namespace draw_launch {

template <typename PixelPtrT>
struct PitchedSurface {
    PixelPtrT pixels = nullptr;
    std::size_t pitch_bytes = 0;
    int width = 0;
    int height = 0;
};

template <typename PixelPtrT>
[[nodiscard]] inline PitchedSurface<PixelPtrT> make_pitched_surface(PixelPtrT pixels, std::size_t pitch_bytes,
                                                                    int width, int height) {
    return PitchedSurface<PixelPtrT>{pixels, pitch_bytes, width, height};
}

template <typename PixelPtrT>
[[nodiscard]] inline bool is_valid(const PitchedSurface<PixelPtrT>& surface) {
    return surface.pixels != nullptr && surface.width > 0 && surface.height > 0;
}

using MutableSurfaceU8 = PitchedSurface<std::uint8_t*>;
using ConstSurfaceU8 = PitchedSurface<const std::uint8_t*>;

struct PackedImageU8 {
    std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
};

[[nodiscard]] inline PackedImageU8 make_packed_image(std::uint8_t* pixels, int width, int height) {
    return PackedImageU8{pixels, width, height};
}

[[nodiscard]] inline bool is_valid(const PackedImageU8& image) {
    return image.pixels != nullptr && image.width > 0 && image.height > 0;
}

[[nodiscard]] constexpr inline int normalized_positive_size(const int value) noexcept {
    return value > 0 ? value : 1;
}

struct InstanceColorBuildLaunch {
    const int* labels = nullptr;
    std::size_t count = 0;
    int num_classes = 0;
    std::uint8_t* colors_rgb = nullptr;
    cudaStream_t stream = nullptr;
};

struct BoxLabelInputs {
    const float* boxes = nullptr;
    const std::uint8_t* colors = nullptr;
    const int* labels = nullptr;
    int num_instances = 0;
};

struct MaskBoxLabelInputs {
    const bool* masks = nullptr;
    const float* boxes = nullptr;
    const std::uint8_t* colors = nullptr;
    const int* labels = nullptr;
    int num_instances = 0;
};

struct MaskBoxLabelRgbLaunch {
    PackedImageU8 image;
    MaskBoxLabelInputs instances;
    float mask_alpha = 0.0f;
    int box_thickness = 1;
    cudaStream_t stream = nullptr;
};

struct MaskBoxLabelArgs {
    int width = 0;
    int height = 0;
    MaskBoxLabelInputs instances;
    float mask_alpha = 0.0f;
    int box_thickness = 1;
    cudaStream_t stream = nullptr;
};

struct RgbColorU8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

struct RgbaColorU8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

struct IntRect {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
};

struct PointBuffer {
    const int* points_xy = nullptr;
    int point_count = 0;
};

struct EdgeBuffer {
    const std::uint32_t* edge_indices = nullptr;
    int edge_count = 0;
};

struct BoxLabelBgrPitchedLaunch {
    MutableSurfaceU8 image;
    BoxLabelInputs instances;
    int box_thickness = 1;
    cudaStream_t stream = nullptr;
};

struct MaskBoxLabelBgrPitchedLaunch {
    MutableSurfaceU8 image;
    MaskBoxLabelInputs instances;
    float mask_alpha = 0.0f;
    int box_thickness = 1;
    cudaStream_t stream = nullptr;
};

struct AnalysisOverlayRgbaPitchedLaunch {
    MutableSurfaceU8 overlay;
    MaskBoxLabelInputs instances;
    std::uint8_t mask_alpha = 0;
    int box_thickness = 1;
    cudaStream_t stream = nullptr;
};

struct CompositeRgbaOverBgrPitchedLaunch {
    MutableSurfaceU8 base_bgr;
    ConstSurfaceU8 overlay_rgba;
    cudaStream_t stream = nullptr;
};

struct CompositeRgbaOverRgbaPitchedLaunch {
    MutableSurfaceU8 base_rgba;
    ConstSurfaceU8 overlay_rgba;
    cudaStream_t stream = nullptr;
};

struct CopyBgrToRgbaPitchedLaunch {
    ConstSurfaceU8 source_bgr;
    MutableSurfaceU8 target_rgba;
    std::uint8_t alpha = 255;
    cudaStream_t stream = nullptr;
};

struct ManualMaskRgbaPitchedLaunch {
    MutableSurfaceU8 overlay_region;
    const std::uint8_t* mask = nullptr;
    RgbaColorU8 color;
    cudaStream_t stream = nullptr;
};

struct BoxOutlineRgbaPitchedLaunch {
    MutableSurfaceU8 overlay;
    IntRect box;
    RgbColorU8 color;
    int thickness = 1;
    cudaStream_t stream = nullptr;
};

struct SelectionHandlesRgbaPitchedLaunch {
    MutableSurfaceU8 overlay;
    IntRect box;
    int handle_radius = 1;
    cudaStream_t stream = nullptr;
};

struct PolylineRgbaPitchedLaunch {
    MutableSurfaceU8 overlay;
    PointBuffer points;
    bool closed = false;
    RgbColorU8 color;
    int thickness = 1;
    cudaStream_t stream = nullptr;
};

struct PointsRgbaPitchedLaunch {
    MutableSurfaceU8 overlay;
    PointBuffer points;
    int radius = 1;
    RgbaColorU8 color;
    cudaStream_t stream = nullptr;
};

struct SkeletonRgbaPitchedLaunch {
    MutableSurfaceU8 overlay;
    PointBuffer points;
    EdgeBuffer edges;
    RgbColorU8 color;
    int thickness = 1;
    cudaStream_t stream = nullptr;
};

}  // namespace draw_launch

struct RenderSampleOptions {
    std::filesystem::path output_path;
    int num_classes = 6;
    float box_thickness = 1.0f;
    int label_size = 12;
    float mask_alpha = 0.5f;
};

#define MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(name, launch_type) void name(const launch_type& launch);

void draw_eval_sample_async_gpu(const torch::Tensor& image, const torch::Tensor& boxes, const torch::Tensor& labels,
                                const torch::Tensor& masks, const RenderSampleOptions& options);

void build_instance_colors_from_zero_based_labels(const int* labels, std::size_t count, int num_classes,
                                                  std::vector<std::uint8_t>* colors_rgb);

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_build_instance_colors_from_zero_based_labels,
                                 draw_launch::InstanceColorBuildLaunch)

inline void launch_build_instance_colors_from_zero_based_labels(const int* labels, std::size_t count, int num_classes,
                                                                std::uint8_t* colors_rgb, cudaStream_t stream) {
    launch_build_instance_colors_from_zero_based_labels(
        draw_launch::InstanceColorBuildLaunch{labels, count, num_classes, colors_rgb, stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_draw_masks_boxes, draw_launch::MaskBoxLabelRgbLaunch)

inline void launch_draw_masks_boxes(std::uint8_t* image_out, const draw_launch::MaskBoxLabelArgs& args) {
    launch_draw_masks_boxes(
        draw_launch::MaskBoxLabelRgbLaunch{draw_launch::make_packed_image(image_out, args.width, args.height),
                                           args.instances, args.mask_alpha, args.box_thickness, args.stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_draw_boxes_labels_bgr_pitched, draw_launch::BoxLabelBgrPitchedLaunch)

inline void launch_draw_boxes_labels_bgr_pitched(std::uint8_t* image_out, std::size_t pitch_bytes, int width,
                                                 int height, const float* boxes, const std::uint8_t* colors,
                                                 const int* labels, int num_instances, int box_thickness,
                                                 cudaStream_t stream) {
    launch_draw_boxes_labels_bgr_pitched(draw_launch::BoxLabelBgrPitchedLaunch{
        draw_launch::make_pitched_surface(image_out, pitch_bytes, width, height),
        draw_launch::BoxLabelInputs{boxes, colors, labels, num_instances}, box_thickness, stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_draw_masks_boxes_labels_bgr_pitched, draw_launch::MaskBoxLabelBgrPitchedLaunch)

inline void launch_draw_masks_boxes_labels_bgr_pitched(std::uint8_t* image_out, std::size_t pitch_bytes,
                                                       const draw_launch::MaskBoxLabelArgs& args) {
    launch_draw_masks_boxes_labels_bgr_pitched(draw_launch::MaskBoxLabelBgrPitchedLaunch{
        draw_launch::make_pitched_surface(image_out, pitch_bytes, args.width, args.height), args.instances,
        args.mask_alpha, args.box_thickness, args.stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_draw_analysis_overlay_rgba_pitched,
                                 draw_launch::AnalysisOverlayRgbaPitchedLaunch)

inline void launch_draw_analysis_overlay_rgba_pitched(std::uint8_t* overlay_out, std::size_t pitch_bytes, int width,
                                                      int height, const bool* masks, const float* boxes,
                                                      const std::uint8_t* colors, const int* labels, int num_instances,
                                                      std::uint8_t mask_alpha, int box_thickness, cudaStream_t stream) {
    launch_draw_analysis_overlay_rgba_pitched(draw_launch::AnalysisOverlayRgbaPitchedLaunch{
        draw_launch::make_pitched_surface(overlay_out, pitch_bytes, width, height),
        draw_launch::MaskBoxLabelInputs{masks, boxes, colors, labels, num_instances}, mask_alpha, box_thickness,
        stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_composite_rgba_over_bgr_pitched, draw_launch::CompositeRgbaOverBgrPitchedLaunch)

inline void launch_composite_rgba_over_bgr_pitched(std::uint8_t* base_bgr, std::size_t base_pitch_bytes,
                                                   const std::uint8_t* overlay_rgba, std::size_t overlay_pitch_bytes,
                                                   int width, int height, cudaStream_t stream) {
    launch_composite_rgba_over_bgr_pitched(draw_launch::CompositeRgbaOverBgrPitchedLaunch{
        draw_launch::make_pitched_surface(base_bgr, base_pitch_bytes, width, height),
        draw_launch::make_pitched_surface(overlay_rgba, overlay_pitch_bytes, width, height), stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_composite_rgba_over_rgba_pitched,
                                 draw_launch::CompositeRgbaOverRgbaPitchedLaunch)

inline void launch_composite_rgba_over_rgba_pitched(std::uint8_t* base_rgba, std::size_t base_pitch_bytes,
                                                    const std::uint8_t* overlay_rgba, std::size_t overlay_pitch_bytes,
                                                    int width, int height, cudaStream_t stream) {
    launch_composite_rgba_over_rgba_pitched(draw_launch::CompositeRgbaOverRgbaPitchedLaunch{
        draw_launch::make_pitched_surface(base_rgba, base_pitch_bytes, width, height),
        draw_launch::make_pitched_surface(overlay_rgba, overlay_pitch_bytes, width, height), stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_copy_bgr_to_rgba_pitched, draw_launch::CopyBgrToRgbaPitchedLaunch)

inline void launch_copy_bgr_to_rgba_pitched(const std::uint8_t* source_bgr, std::size_t source_pitch_bytes,
                                            std::uint8_t* target_rgba, std::size_t target_pitch_bytes, int width,
                                            int height, std::uint8_t alpha, cudaStream_t stream) {
    launch_copy_bgr_to_rgba_pitched(draw_launch::CopyBgrToRgbaPitchedLaunch{
        draw_launch::make_pitched_surface(source_bgr, source_pitch_bytes, width, height),
        draw_launch::make_pitched_surface(target_rgba, target_pitch_bytes, width, height), alpha, stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_draw_manual_mask_rgba_pitched, draw_launch::ManualMaskRgbaPitchedLaunch)

inline void launch_draw_manual_mask_rgba_pitched(std::uint8_t* overlay_region, std::size_t pitch_bytes, int width,
                                                 int height, const std::uint8_t* mask, std::uint8_t r, std::uint8_t g,
                                                 std::uint8_t b, std::uint8_t alpha, cudaStream_t stream) {
    launch_draw_manual_mask_rgba_pitched(draw_launch::ManualMaskRgbaPitchedLaunch{
        draw_launch::make_pitched_surface(overlay_region, pitch_bytes, width, height), mask,
        draw_launch::RgbaColorU8{r, g, b, alpha}, stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_draw_box_outline_rgba_pitched, draw_launch::BoxOutlineRgbaPitchedLaunch)

inline void launch_draw_box_outline_rgba_pitched(std::uint8_t* overlay_out, std::size_t pitch_bytes, int image_width,
                                                 int image_height, int box_x1, int box_y1, int box_x2, int box_y2,
                                                 std::uint8_t r, std::uint8_t g, std::uint8_t b, int thickness,
                                                 cudaStream_t stream) {
    launch_draw_box_outline_rgba_pitched(draw_launch::BoxOutlineRgbaPitchedLaunch{
        draw_launch::make_pitched_surface(overlay_out, pitch_bytes, image_width, image_height),
        draw_launch::IntRect{box_x1, box_y1, box_x2, box_y2}, draw_launch::RgbColorU8{r, g, b}, thickness, stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_draw_selection_handles_rgba_pitched,
                                 draw_launch::SelectionHandlesRgbaPitchedLaunch)

inline void launch_draw_selection_handles_rgba_pitched(std::uint8_t* overlay_out, std::size_t pitch_bytes,
                                                       int image_width, int image_height, int box_x1, int box_y1,
                                                       int box_x2, int box_y2, int handle_radius, cudaStream_t stream) {
    launch_draw_selection_handles_rgba_pitched(draw_launch::SelectionHandlesRgbaPitchedLaunch{
        draw_launch::make_pitched_surface(overlay_out, pitch_bytes, image_width, image_height),
        draw_launch::IntRect{box_x1, box_y1, box_x2, box_y2}, handle_radius, stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_draw_polyline_rgba_pitched, draw_launch::PolylineRgbaPitchedLaunch)

inline void launch_draw_polyline_rgba_pitched(std::uint8_t* overlay_out, std::size_t pitch_bytes, int image_width,
                                              int image_height, const int* points_xy, int point_count, bool closed,
                                              std::uint8_t r, std::uint8_t g, std::uint8_t b, int thickness,
                                              cudaStream_t stream) {
    launch_draw_polyline_rgba_pitched(draw_launch::PolylineRgbaPitchedLaunch{
        draw_launch::make_pitched_surface(overlay_out, pitch_bytes, image_width, image_height),
        draw_launch::PointBuffer{points_xy, point_count}, closed, draw_launch::RgbColorU8{r, g, b}, thickness, stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_draw_points_rgba_pitched, draw_launch::PointsRgbaPitchedLaunch)

inline void launch_draw_points_rgba_pitched(std::uint8_t* overlay_out, std::size_t pitch_bytes, int image_width,
                                            int image_height, const int* points_xy, int point_count, int radius,
                                            std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t alpha,
                                            cudaStream_t stream) {
    launch_draw_points_rgba_pitched(draw_launch::PointsRgbaPitchedLaunch{
        draw_launch::make_pitched_surface(overlay_out, pitch_bytes, image_width, image_height),
        draw_launch::PointBuffer{points_xy, point_count}, radius, draw_launch::RgbaColorU8{r, g, b, alpha}, stream});
}

MMLTK_DRAW_CUDA_DECLARE_LAUNCHER(launch_draw_skeleton_rgba_pitched, draw_launch::SkeletonRgbaPitchedLaunch)

inline void launch_draw_skeleton_rgba_pitched(std::uint8_t* overlay_out, std::size_t pitch_bytes, int image_width,
                                              int image_height, const int* points_xy, int point_count,
                                              const std::uint32_t* edge_indices, int edge_count, std::uint8_t r,
                                              std::uint8_t g, std::uint8_t b, int thickness, cudaStream_t stream) {
    launch_draw_skeleton_rgba_pitched(draw_launch::SkeletonRgbaPitchedLaunch{
        draw_launch::make_pitched_surface(overlay_out, pitch_bytes, image_width, image_height),
        draw_launch::PointBuffer{points_xy, point_count}, draw_launch::EdgeBuffer{edge_indices, edge_count},
        draw_launch::RgbColorU8{r, g, b}, thickness, stream});
}

void flush_eval_sample_writes();

#undef MMLTK_DRAW_CUDA_DECLARE_LAUNCHER

}  // namespace mmltk::rfdetr
