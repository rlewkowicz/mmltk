module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include "detail/mask_pack_cuda_abi.h"
#include "detail/raster_color.h"
#include "detail/raster_cuda_abi.h"

module mmltk.backend.imaging.raster;

namespace mmltk::backend::imaging::raster {
std::int32_t probe_rgba(const ConstBytes source, std::uint32_t* samples, const std::span<const std::uint32_t, 50> coordinates,
                        const std::uintptr_t stream) noexcept {
    if (!source.valid(4U) || samples == nullptr) return cudaErrorInvalidValue;
    detail::draw_launch::ProbeRgbaLaunch launch{
        .source = {source.pixels, source.pitch_bytes, source.width, source.height},
        .samples = samples,
        .stream = reinterpret_cast<cudaStream_t>(stream),
    };
    for (std::size_t index = 0U; index < coordinates.size(); ++index) {
        if (coordinates[index] >= static_cast<std::uint32_t>(index % 2U == 0U ? source.width : source.height)) return cudaErrorInvalidValue;
        launch.coordinates[index] = coordinates[index];
    }
    return detail::launch_probe_rgba(launch);
}

namespace {
namespace launch = detail::draw_launch;

template <typename Pixel>
[[nodiscard]] launch::PitchedSurface<Pixel*> as_launch_surface(const PitchedView<Pixel>& value) noexcept {
    return {value.pixels, value.pitch_bytes, value.width, value.height};
}

[[nodiscard]] launch::PackedImageU8 as_launch_image(const PackedImage& value) noexcept { return {value.pixels, value.width, value.height}; }

[[nodiscard]] launch::RgbaTargetViewAbi as_launch_target(const RgbaTargetView& value) noexcept {
    return {.kind = value.kind == RgbaTargetKind::Pitched ? launch::RgbaTargetKindAbi::Pitched : launch::RgbaTargetKindAbi::SurfaceObject,
            .pitched = as_launch_surface(value.pitched),
            .surface = static_cast<cudaSurfaceObject_t>(value.surface),
            .width = value.width,
            .height = value.height};
}

[[nodiscard]] cudaStream_t as_stream(const NativeStream value) noexcept { return reinterpret_cast<cudaStream_t>(value.value); }

[[nodiscard]] bool packed_image_valid(const PackedImage& image) noexcept {
    return image.pixels != nullptr && image.width > 0 && image.height > 0;
}

[[nodiscard]] bool target_valid(const RgbaTargetView& target) noexcept {
    if (target.width <= 0 || target.height <= 0) return false;
    if (target.kind == RgbaTargetKind::Pitched) {
        return target.pitched.valid(4U) && target.pitched.width == target.width && target.pitched.height == target.height;
    }
    return target.kind == RgbaTargetKind::SurfaceObject && target.surface != 0U;
}

[[nodiscard]] bool box_inputs_valid(const BoxLabelInputs& inputs) noexcept {
    return inputs.instance_count >= 0 && inputs.instance_count <= std::numeric_limits<int>::max() / 4 &&
           (inputs.instance_count == 0 || (inputs.boxes != nullptr && inputs.colors != nullptr && inputs.labels != nullptr));
}

[[nodiscard]] bool mask_inputs_valid(const MaskBoxLabelInputs& inputs) noexcept {
    return inputs.masks != nullptr && box_inputs_valid({inputs.boxes, inputs.colors, inputs.labels, inputs.instance_count});
}

[[nodiscard]] bool indexed_instances_fit(const int instance_count, const int width, const int height) noexcept {
    if (instance_count <= 0 || width <= 0 || height <= 0 || instance_count > std::numeric_limits<int>::max() / 4) { return false; }
    const std::int64_t pixels = static_cast<std::int64_t>(width) * height;
    return pixels <= std::numeric_limits<int>::max() && instance_count <= std::numeric_limits<int>::max() / pixels;
}

[[nodiscard]] bool squared_extent_fits(const int value) noexcept {
    constexpr int kMaxSquaredExtent = 46340;
    return value > 0 && value <= kMaxSquaredExtent;
}

[[nodiscard]] bool same_extent(const int left_width, const int left_height, const int right_width, const int right_height) noexcept {
    return left_width == right_width && left_height == right_height;
}

[[nodiscard]] bool writable_rgba(const MutableBytes& overlay, const NativeStream stream) noexcept {
    return overlay.valid(4U) && static_cast<bool>(stream);
}

[[nodiscard]] bool drawable_box(const MutableBytes& overlay, const IntRect& box, const int extent, const NativeStream stream) noexcept {
    return writable_rgba(overlay, stream) && box.x2 > box.x1 && box.y2 > box.y1 && squared_extent_fits(extent);
}

[[nodiscard]] bool drawable_points(const MutableBytes& overlay, const PointBuffer& points, const int minimum_count, const int extent,
                                   const NativeStream stream) noexcept {
    return overlay.valid(4U) && points.points_xy != nullptr && points.point_count >= minimum_count &&
           points.point_count <= std::numeric_limits<int>::max() / 2 && squared_extent_fits(extent) && static_cast<bool>(stream);
}

}  // namespace

std::vector<std::uint8_t> category_colors(const std::span<const int> labels, const int category_count) {
    const int safe_count = detail::color::safe_class_count(category_count);
    std::vector<std::uint8_t> colors(labels.size() * 3U, 0U);
    std::vector<std::uint8_t> palette(static_cast<std::size_t>(safe_count) * 3U);
    for (int label = 0; label < safe_count; ++label)
        detail::color::class_color(label, safe_count, palette[label * 3U], palette[label * 3U + 1U], palette[label * 3U + 2U]);
    for (std::size_t index = 0; index < labels.size(); ++index) {
        const int label = detail::color::normalize_label(labels[index], safe_count);
        for (std::size_t channel = 0U; channel != 3U; ++channel)
            colors[index * 3U + channel] = palette[static_cast<std::size_t>(label) * 3U + channel];
    }
    return colors;
}
std::int32_t scale_rgba_nearest(const ConstBytes source, const MutableBytes target, const std::uintptr_t stream) noexcept {
    if (!source.valid(4U) || !target.valid(4U) || stream == 0U) return cudaErrorInvalidValue;
    return detail::launch_scale_rgba({as_launch_surface(source), as_launch_surface(target), reinterpret_cast<cudaStream_t>(stream)});
}

std::int32_t build_category_colors_cuda(const CategoryColorWork& work) noexcept {
    if (work.count == 0U) return cudaSuccess;
    if (work.labels == nullptr || work.colors_rgb == nullptr || work.category_count <= 0 || !work.stream ||
        work.count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return cudaErrorInvalidValue;
    }
    return detail::launch_build_instance_colors_from_zero_based_labels(
        {work.labels, work.count, work.category_count, work.colors_rgb, as_stream(work.stream)});
}

std::int32_t raster_mask_boxes_rgb(const MaskBoxLabelRgbWork& work) noexcept {
    if (work.instances.instance_count == 0) return cudaSuccess;
    if (!packed_image_valid(work.image) || !mask_inputs_valid(work.instances) || work.mask_alpha < 0.0F || work.mask_alpha > 1.0F ||
        work.box_thickness <= 0 || !indexed_instances_fit(work.instances.instance_count, work.image.width, work.image.height) ||
        !work.stream) {
        return cudaErrorInvalidValue;
    }
    return detail::launch_draw_masks_boxes(
        {as_launch_image(work.image), work.instances, work.mask_alpha, work.box_thickness, as_stream(work.stream)});
}

std::int32_t raster_boxes_bgr(const BoxLabelBgrWork& work) noexcept {
    if (work.instances.instance_count == 0) return cudaSuccess;
    if (!work.image.valid(3U) || !box_inputs_valid(work.instances) || work.box_thickness <= 0 || !work.stream) {
        return cudaErrorInvalidValue;
    }
    return detail::launch_draw_boxes_labels_bgr_pitched(
        {as_launch_surface(work.image), work.instances, work.box_thickness, as_stream(work.stream)});
}

std::int32_t raster_mask_boxes_bgr(const MaskBoxLabelBgrWork& work) noexcept {
    if (work.instances.instance_count == 0) return cudaSuccess;
    if (!work.image.valid(3U) || !mask_inputs_valid(work.instances) || work.mask_alpha < 0.0F || work.mask_alpha > 1.0F ||
        work.box_thickness <= 0 || !work.stream) {
        return cudaErrorInvalidValue;
    }
    // CLEANUP-IGNORE: BGR mask rasterization has an indexed-capacity precondition absent from the RGB launch wrapper.
    if (!indexed_instances_fit(work.instances.instance_count, work.image.width, work.image.height)) { return cudaErrorInvalidValue; }
    return detail::launch_draw_masks_boxes_labels_bgr_pitched(
        {as_launch_surface(work.image), work.instances, work.mask_alpha, work.box_thickness, as_stream(work.stream)});
}

std::int32_t raster_instance_overlay_rgba(const InstanceOverlayRgbaWork& work) noexcept {
    if (work.instances.instance_count == 0) return cudaSuccess;
    if (!work.overlay.valid(4U) || !box_inputs_valid(work.instances) || work.box_thickness <= 0 ||
        !indexed_instances_fit(work.instances.instance_count, work.overlay.width, work.overlay.height) || !work.stream) {
        return cudaErrorInvalidValue;
    }
    const MaskBoxLabelInputs launch_instances{work.masks, work.instances.boxes, work.instances.colors, work.instances.labels,
                                              work.instances.instance_count};
    return detail::launch_draw_analysis_overlay_rgba_pitched(
        {as_launch_surface(work.overlay), launch_instances, work.mask_alpha, work.box_thickness, as_stream(work.stream)});
}

std::int32_t composite_rgba_over_bgr(const CompositeRgbaOverBgrWork& work) noexcept {
    if (!work.base_bgr.valid(3U) || !work.overlay_rgba.valid(4U) ||
        !same_extent(work.base_bgr.width, work.base_bgr.height, work.overlay_rgba.width, work.overlay_rgba.height) || !work.stream) {
        return cudaErrorInvalidValue;
    }
    return detail::launch_composite_rgba_over_bgr_pitched(
        {as_launch_surface(work.base_bgr), as_launch_surface(work.overlay_rgba), as_stream(work.stream)});
}

std::int32_t composite_rgba(const CompositeRgbaWork& work) noexcept {
    if (!target_valid(work.base_rgba) || !work.overlay_rgba.valid(4U) ||
        !same_extent(work.base_rgba.width, work.base_rgba.height, work.overlay_rgba.width, work.overlay_rgba.height) || !work.stream) {
        return cudaErrorInvalidValue;
    }
    return detail::launch_composite_rgba({as_launch_target(work.base_rgba), as_launch_surface(work.overlay_rgba), as_stream(work.stream)});
}

std::int32_t finalize_rgba(const FinalizeRgbaWork& work) noexcept {
    if (!work.clean.valid(4U) || !work.destination.valid(4U) || !work.stream ||
        !same_extent(work.clean.width, work.clean.height, work.destination.width, work.destination.height) ||
        (work.semantic.pixels &&
         (!work.semantic.valid(4U) || !same_extent(work.clean.width, work.clean.height, work.semantic.width, work.semantic.height))))
        return cudaErrorInvalidValue;
    const auto submit = [&](IntRect region) {
        region.x1 = std::clamp(region.x1, 0, work.clean.width);
        region.y1 = std::clamp(region.y1, 0, work.clean.height);
        region.x2 = std::clamp(region.x2, 0, work.clean.width);
        region.y2 = std::clamp(region.y2, 0, work.clean.height);
        if (region.x2 <= region.x1 || region.y2 <= region.y1) return cudaSuccess;
        return detail::launch_finalize_rgba({as_launch_surface(work.clean), as_launch_surface(work.semantic),
                                             as_launch_surface(work.destination), region, as_stream(work.stream)});
    };
    if (work.full_image) return submit({0, 0, work.clean.width, work.clean.height});
    for (const auto region : work.regions) {
        const auto status = submit(region);
        if (status != cudaSuccess) return status;
    }
    return cudaSuccess;
}

std::int32_t copy_bgr_to_rgba(const CopyBgrToRgbaWork& work) noexcept {
    if (!work.source_bgr.valid(3U) || !target_valid(work.target_rgba) ||
        !same_extent(work.source_bgr.width, work.source_bgr.height, work.target_rgba.width, work.target_rgba.height) || !work.stream) {
        return cudaErrorInvalidValue;
    }
    return detail::launch_copy_bgr_to_rgba(
        {as_launch_surface(work.source_bgr), as_launch_target(work.target_rgba), work.alpha, as_stream(work.stream)});
}

std::int32_t raster_mask_rgba(const MaskRgbaWork& work) noexcept {
    if (!writable_rgba(work.overlay, work.stream) || work.mask == nullptr ||
        !indexed_instances_fit(1, work.overlay.width, work.overlay.height)) {
        return cudaErrorInvalidValue;
    }
    return detail::launch_draw_manual_mask_rgba_pitched({as_launch_surface(work.overlay), work.mask, work.color, as_stream(work.stream)});
}

std::int32_t raster_mask_runs_rgba(const MaskRunsRgbaWork& work) noexcept {
    // CLEANUP-IGNORE: RLE-mask validation and launch semantics are independent from category-color generation.
    if (!writable_rgba(work.overlay, work.stream) || work.run_pairs == nullptr || work.run_count == 0U ||
        work.run_count > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return cudaErrorInvalidValue;
    }
    if (!std::isfinite(work.source_x) || !std::isfinite(work.source_y) || !std::isfinite(work.target_x) || !std::isfinite(work.target_y) ||
        !std::isfinite(work.scale_x) || !std::isfinite(work.scale_y) || work.scale_x < 0 || work.scale_y < 0)
        return cudaErrorInvalidValue;
    if ((work.scale_x != 1 || work.scale_y != 1 || work.source_x != work.target_x || work.source_y != work.target_y) &&
        (work.overlay.pitch_bytes % alignof(std::uint32_t) != 0U ||
         reinterpret_cast<std::uintptr_t>(work.overlay.pixels) % alignof(std::uint32_t) != 0U))
        return cudaErrorInvalidValue;
    return detail::launch_draw_manual_mask_runs_rgba_pitched({as_launch_surface(work.overlay), work.run_pairs, work.run_count, work.color,
                                                              as_stream(work.stream), work.clip, work.source_x, work.source_y,
                                                              work.target_x, work.target_y, work.scale_x, work.scale_y});
}

std::int32_t raster_box_outline_rgba(const BoxOutlineRgbaWork& work) noexcept {
    if (!drawable_box(work.overlay, work.box, work.thickness, work.stream)) { return cudaErrorInvalidValue; }
    return detail::launch_draw_box_outline_rgba_pitched(
        {as_launch_surface(work.overlay), work.box, work.color, work.thickness, as_stream(work.stream), work.clip});
}

std::int32_t raster_selection_handles_rgba(const SelectionHandlesRgbaWork& work) noexcept {
    if (!drawable_box(work.overlay, work.box, work.handle_radius, work.stream)) { return cudaErrorInvalidValue; }
    return detail::launch_draw_selection_handles_rgba_pitched(
        {as_launch_surface(work.overlay), work.box, work.handle_radius, work.color, as_stream(work.stream), work.clip});
}

std::int32_t raster_polyline_rgba(const PolylineRgbaWork& work) noexcept {
    if (!drawable_points(work.overlay, work.points, 2, work.thickness, work.stream)) { return cudaErrorInvalidValue; }
    return detail::launch_draw_polyline_rgba_pitched(
        {as_launch_surface(work.overlay), work.points, work.closed, work.color, work.thickness, as_stream(work.stream), work.clip});
}

std::int32_t raster_points_rgba(const PointsRgbaWork& work) noexcept {
    // CLEANUP-IGNORE: Point and polyline entry points preserve distinct typed work contracts and CUDA launches.
    if (!drawable_points(work.overlay, work.points, 1, work.radius, work.stream)) { return cudaErrorInvalidValue; }
    return detail::launch_draw_points_rgba_pitched(
        {as_launch_surface(work.overlay), work.points, work.radius, work.color, as_stream(work.stream), work.clip});
}

std::int32_t raster_skeleton_rgba(const SkeletonRgbaWork& work) noexcept {
    if (!drawable_points(work.overlay, work.points, 1, work.thickness, work.stream) || work.edges.edge_indices == nullptr ||
        work.edges.edge_count <= 0 || work.edges.edge_count > std::numeric_limits<int>::max() / 2) {
        return cudaErrorInvalidValue;
    }
    return detail::launch_draw_skeleton_rgba_pitched(
        {as_launch_surface(work.overlay), work.points, work.edges, work.color, work.thickness, as_stream(work.stream), work.clip});
}

std::int32_t pack_bool_masks(const BoolMaskPackWork& work) noexcept {
    if (work.mask_count == 0) return cudaSuccess;
    if (work.masks == nullptr || work.packed_masks == nullptr || work.mask_count < 0 || work.pixels_per_mask <= 0 ||
        work.bytes_per_mask <= 0 || !work.stream || work.pixels_per_mask > std::numeric_limits<std::int64_t>::max() - 7 ||
        work.bytes_per_mask != (work.pixels_per_mask + 7) / 8 ||
        work.mask_count > std::numeric_limits<std::int64_t>::max() / work.bytes_per_mask ||
        work.mask_count > std::numeric_limits<std::int64_t>::max() / work.pixels_per_mask ||
        work.mask_count * work.bytes_per_mask > static_cast<std::int64_t>(std::numeric_limits<int>::max()) * 256) {
        return cudaErrorInvalidValue;
    }
    return detail::launch_pack_bool_masks_cuda(
        {work.masks, work.packed_masks, work.mask_count, work.pixels_per_mask, work.bytes_per_mask, as_stream(work.stream)});
}

}  // namespace mmltk::backend::imaging::raster
