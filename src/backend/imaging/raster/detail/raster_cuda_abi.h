#pragma once
// The launcher invocations are the single declaration list for the typed CUDA boundary.
#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>
namespace mmltk::backend::imaging::raster::detail {
namespace draw_launch {
template <typename PixelPtrT>
struct PitchedSurface {
 PixelPtrT pixels = nullptr;
 std::size_t pitch_bytes = 0;
 int width = 0;
 int height = 0;
 [[nodiscard]] bool valid(const std::size_t channels) const noexcept { return pixels != nullptr && width > 0 && height > 0 && pitch_bytes >= static_cast<std::size_t>(width) * channels; }
};
template <typename PixelPtrT>
[[nodiscard]] inline bool is_valid(const PitchedSurface<PixelPtrT>& surface) {
 return surface.pixels != nullptr && surface.width > 0 && surface.height > 0;
}
using MutableSurfaceU8 = PitchedSurface<std::uint8_t*>;
using ConstSurfaceU8 = PitchedSurface<const std::uint8_t*>;
struct ScaleRgbaLaunch {
 ConstSurfaceU8 source;
 MutableSurfaceU8 target;
 cudaStream_t stream = nullptr;
 bool bilinear = false;
};
struct ProbeRgbaLaunch {
 ConstSurfaceU8 source;
 std::uint32_t* samples = nullptr;
 std::uint32_t coordinates[50]{};
 cudaStream_t stream = nullptr;
};
struct PackedImageU8 {
 std::uint8_t* pixels = nullptr;
 int width = 0;
 int height = 0;
};
[[nodiscard]] inline bool is_valid(const PackedImageU8& image) { return image.pixels != nullptr && image.width > 0 && image.height > 0; }
enum class RgbaTargetKindAbi : std::uint8_t {
 Pitched = 1,
 SurfaceObject = 2,
};
struct RgbaTargetViewAbi {
 RgbaTargetKindAbi kind = RgbaTargetKindAbi::Pitched;
 MutableSurfaceU8 pitched;
 cudaSurfaceObject_t surface = 0;
 int width = 0;
 int height = 0;
};
[[nodiscard]] inline bool is_valid(const RgbaTargetViewAbi& target) {
 if (target.width <= 0 || target.height <= 0) { return false; }
 if (target.kind == RgbaTargetKindAbi::Pitched) { return is_valid(target.pitched) && target.pitched.width == target.width && target.pitched.height == target.height; }
 return target.kind == RgbaTargetKindAbi::SurfaceObject && target.surface != 0;
}
[[nodiscard]] constexpr inline int normalized_positive_size(const int value) noexcept { return value > 0 ? value : 1; }
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
 int instance_count = 0;
 // Optional stream-ordered compact prefix; instance_count remains the storage bound.
 const std::int64_t* device_instance_count = nullptr;
};
struct MaskBoxLabelInputs {
 const bool* masks = nullptr;
 const float* boxes = nullptr;
 const std::uint8_t* colors = nullptr;
 const int* labels = nullptr;
 int instance_count = 0;
 // Optional stream-ordered compact prefix; instance_count remains the storage bound.
 const std::int64_t* device_instance_count = nullptr;
};
struct MaskBoxLabelRgbLaunch {
 PackedImageU8 image;
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
 bool labels = true;
 bool add_rgb_to_existing = false;
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
struct FinalizeRgbaLaunch {
 ConstSurfaceU8 clean;
 ConstSurfaceU8 semantic;
 MutableSurfaceU8 destination;
 IntRect region;
 cudaStream_t stream = nullptr;
};
struct CompositeRgbaLaunchAbi {
 RgbaTargetViewAbi base_rgba;
 ConstSurfaceU8 overlay_rgba;
 cudaStream_t stream = nullptr;
};
struct CopyBgrToRgbaPitchedLaunch {
 ConstSurfaceU8 source_bgr;
 MutableSurfaceU8 target_rgba;
 std::uint8_t alpha = 255;
 cudaStream_t stream = nullptr;
};
struct CopyBgrToRgbaLaunchAbi {
 ConstSurfaceU8 source_bgr;
 RgbaTargetViewAbi target_rgba;
 std::uint8_t alpha = 255;
 cudaStream_t stream = nullptr;
};
struct ManualMaskRgbaPitchedLaunch {
 MutableSurfaceU8 overlay_region;
 const std::uint8_t* mask = nullptr;
 RgbaColorU8 color;
 cudaStream_t stream = nullptr;
};
struct ManualMaskRunsRgbaPitchedLaunch {
 MutableSurfaceU8 overlay_region;
 const std::uint32_t* run_pairs = nullptr;
 std::uint32_t run_count = 0;
 RgbaColorU8 color;
 cudaStream_t stream = nullptr;
 IntRect clip{0, 0, 2147483647, 2147483647};
 float source_x = 0, source_y = 0, target_x = 0, target_y = 0, scale_x = 1, scale_y = 1;
};
struct BoxOutlineRgbaPitchedLaunch {
 MutableSurfaceU8 overlay;
 IntRect box;
 RgbColorU8 color;
 int thickness = 1;
 cudaStream_t stream = nullptr;
 IntRect clip{0, 0, 2147483647, 2147483647};
};
struct SelectionHandlesRgbaPitchedLaunch {
 MutableSurfaceU8 overlay;
 IntRect box;
 int handle_radius = 1;
 RgbaColorU8 color;
 cudaStream_t stream = nullptr;
 IntRect clip{0, 0, 2147483647, 2147483647};
};
struct PolylineRgbaPitchedLaunch {
 MutableSurfaceU8 overlay;
 PointBuffer points;
 bool closed = false;
 RgbColorU8 color;
 int thickness = 1;
 cudaStream_t stream = nullptr;
 IntRect clip{0, 0, 2147483647, 2147483647};
};
struct PointsRgbaPitchedLaunch {
 MutableSurfaceU8 overlay;
 PointBuffer points;
 int radius = 1;
 RgbaColorU8 color;
 cudaStream_t stream = nullptr;
 IntRect clip{0, 0, 2147483647, 2147483647};
};
struct SkeletonRgbaPitchedLaunch {
 MutableSurfaceU8 overlay;
 PointBuffer points;
 EdgeBuffer edges;
 RgbColorU8 color;
 int thickness = 1;
 cudaStream_t stream = nullptr;
 IntRect clip{0, 0, 2147483647, 2147483647};
};
}  // namespace draw_launch
#define MMLTK_RASTER_CUDA_LAUNCHER(name, type) [[nodiscard]] cudaError_t name(const draw_launch::type& launch) noexcept;
MMLTK_RASTER_CUDA_LAUNCHER(launch_build_instance_colors_from_zero_based_labels, InstanceColorBuildLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_scale_rgba, ScaleRgbaLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_probe_rgba, ProbeRgbaLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_draw_masks_boxes, MaskBoxLabelRgbLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_draw_boxes_labels_bgr_pitched, BoxLabelBgrPitchedLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_draw_masks_boxes_labels_bgr_pitched, MaskBoxLabelBgrPitchedLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_draw_analysis_overlay_rgba_pitched, AnalysisOverlayRgbaPitchedLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_composite_rgba_over_bgr_pitched, CompositeRgbaOverBgrPitchedLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_composite_rgba_over_rgba_pitched, CompositeRgbaOverRgbaPitchedLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_composite_rgba, CompositeRgbaLaunchAbi)
MMLTK_RASTER_CUDA_LAUNCHER(launch_finalize_rgba, FinalizeRgbaLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_copy_bgr_to_rgba_pitched, CopyBgrToRgbaPitchedLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_copy_bgr_to_rgba, CopyBgrToRgbaLaunchAbi)
MMLTK_RASTER_CUDA_LAUNCHER(launch_draw_manual_mask_rgba_pitched, ManualMaskRgbaPitchedLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_draw_manual_mask_runs_rgba_pitched, ManualMaskRunsRgbaPitchedLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_draw_box_outline_rgba_pitched, BoxOutlineRgbaPitchedLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_draw_selection_handles_rgba_pitched, SelectionHandlesRgbaPitchedLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_draw_polyline_rgba_pitched, PolylineRgbaPitchedLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_draw_points_rgba_pitched, PointsRgbaPitchedLaunch)
MMLTK_RASTER_CUDA_LAUNCHER(launch_draw_skeleton_rgba_pitched, SkeletonRgbaPitchedLaunch)
#undef MMLTK_RASTER_CUDA_LAUNCHER
}  // namespace mmltk::backend::imaging::raster::detail
