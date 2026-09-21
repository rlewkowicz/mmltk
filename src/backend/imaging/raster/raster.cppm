module;
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "detail/raster_cuda_abi.h"
export module mmltk.backend.imaging.raster;
export namespace mmltk::backend::imaging::raster {
template <class Pixel>
struct PitchedView {
    Pixel* pixels = nullptr;
    std::size_t pitch_bytes = 0U;
    int width = 0;
    int height = 0;
    [[nodiscard]] bool valid(const std::size_t channels) const noexcept {
        return pixels != nullptr && width > 0 && height > 0 && pitch_bytes >= static_cast<std::size_t>(width) * channels;
    }
};
using MutableBytes = PitchedView<std::uint8_t>;
using ConstBytes = PitchedView<const std::uint8_t>;
// Exactly 25 RGBA samples, with explicit source pixel coordinates. Destination
// storage belongs to the caller and is reusable across asynchronous launches.
[[nodiscard]] std::int32_t probe_rgba(ConstBytes, std::uint32_t* samples, std::span<const std::uint32_t, 50> coordinates, std::uintptr_t stream) noexcept;
[[nodiscard]] std::int32_t scale_rgba(ConstBytes, MutableBytes, std::uintptr_t stream, bool bilinear = false) noexcept;
struct PackedImage {
    std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
};
enum class RgbaTargetKind : std::uint8_t { Pitched = 1, SurfaceObject = 2 };
struct RgbaTargetView {
    RgbaTargetKind kind = RgbaTargetKind::Pitched;
    MutableBytes pitched{};
    std::uintptr_t surface = 0U;
    int width = 0;
    int height = 0;
};
[[nodiscard]] inline RgbaTargetView pitched_rgba_target(std::uint8_t* pixels, const std::size_t pitch_bytes, const int width, const int height) noexcept {
    return {RgbaTargetKind::Pitched, {pixels, pitch_bytes, width, height}, 0U, width, height};
}
[[nodiscard]] inline RgbaTargetView surface_rgba_target(const std::uintptr_t surface, const int width, const int height) noexcept {
    return {RgbaTargetKind::SurfaceObject, {}, surface, width, height};
}
using RgbColor = detail::draw_launch::RgbColorU8;
using RgbaColor = detail::draw_launch::RgbaColorU8;
using IntRect = detail::draw_launch::IntRect;
using PointBuffer = detail::draw_launch::PointBuffer;
using EdgeBuffer = detail::draw_launch::EdgeBuffer;
using BoxLabelInputs = detail::draw_launch::BoxLabelInputs;
using MaskBoxLabelInputs = detail::draw_launch::MaskBoxLabelInputs;
struct NativeStream {
    std::uintptr_t value = 0U;
    NativeStream() noexcept = default;
    NativeStream(void* native) noexcept : value(reinterpret_cast<std::uintptr_t>(native)) {}
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0U; }
};
struct CategoryColorWork final {
    const int* labels = nullptr;
    std::size_t count = 0U;
    int category_count = 0;
    std::uint8_t* colors_rgb = nullptr;
    NativeStream stream{};
};
struct MaskBoxLabelRgbWork final {
    PackedImage image{};
    MaskBoxLabelInputs instances{};
    float mask_alpha = 0.0F;
    int box_thickness = 1;
    NativeStream stream{};
};
struct BoxLabelBgrWork final {
    MutableBytes image{};
    BoxLabelInputs instances{};
    int box_thickness = 1;
    NativeStream stream{};
};
struct MaskBoxLabelBgrWork final {
    MutableBytes image{};
    MaskBoxLabelInputs instances{};
    float mask_alpha = 0.0F;
    int box_thickness = 1;
    NativeStream stream{};
};
struct InstanceOverlayRgbaWork final {
    MutableBytes overlay{};
    BoxLabelInputs instances{};
    const bool* masks = nullptr;
    std::uint8_t mask_alpha = 0U;
    int box_thickness = 1;
    NativeStream stream{};
    bool labels = true;
    bool add_rgb_to_existing = false;
};
struct CompositeRgbaOverBgrWork final {
    MutableBytes base_bgr{};
    ConstBytes overlay_rgba{};
    NativeStream stream{};
};
struct CompositeRgbaWork final {
    RgbaTargetView base_rgba{};
    ConstBytes overlay_rgba{};
    NativeStream stream{};
};
// Coordinates are half-open and clipped to the image. Empty coverage writes nothing.
struct FinalizeRgbaWork final {
    ConstBytes clean{};
    ConstBytes semantic{};
    MutableBytes destination{};
    std::span<const IntRect> regions{};
    bool full_image = true;
    NativeStream stream{};
};
struct CopyBgrToRgbaWork final {
    ConstBytes source_bgr{};
    RgbaTargetView target_rgba{};
    std::uint8_t alpha = 255U;
    NativeStream stream{};
};
struct MaskRgbaWork final {
    MutableBytes overlay{};
    const std::uint8_t* mask = nullptr;
    RgbaColor color{};
    NativeStream stream{};
};
struct MaskRunsRgbaWork final {
    MutableBytes overlay{};
    const std::uint32_t* run_pairs = nullptr;
    std::uint32_t run_count = 0U;
    RgbaColor color{};
    NativeStream stream{};
    IntRect clip{0, 0, 2147483647, 2147483647};
    float source_x = 0, source_y = 0, target_x = 0, target_y = 0, scale_x = 1, scale_y = 1;
};
struct BoxOutlineRgbaWork final {
    MutableBytes overlay{};
    IntRect box{};
    RgbColor color{};
    int thickness = 1;
    NativeStream stream{};
    IntRect clip{0, 0, 2147483647, 2147483647};
};
struct SelectionHandlesRgbaWork final {
    MutableBytes overlay{};
    IntRect box{};
    int handle_radius = 1;
    RgbaColor color{};
    NativeStream stream{};
    IntRect clip{0, 0, 2147483647, 2147483647};
};
struct PolylineRgbaWork final {
    MutableBytes overlay{};
    PointBuffer points{};
    bool closed = false;
    RgbColor color{};
    int thickness = 1;
    NativeStream stream{};
    IntRect clip{0, 0, 2147483647, 2147483647};
};
struct PointsRgbaWork final {
    MutableBytes overlay{};
    PointBuffer points{};
    int radius = 1;
    RgbaColor color{};
    NativeStream stream{};
    IntRect clip{0, 0, 2147483647, 2147483647};
};
struct SkeletonRgbaWork final {
    MutableBytes overlay{};
    PointBuffer points{};
    EdgeBuffer edges{};
    RgbColor color{};
    int thickness = 1;
    NativeStream stream{};
    IntRect clip{0, 0, 2147483647, 2147483647};
};
struct BoolMaskPackWork final {
    const bool* masks = nullptr;
    std::uint8_t* packed_masks = nullptr;
    std::int64_t mask_count = 0;
    std::int64_t pixels_per_mask = 0;
    std::int64_t bytes_per_mask = 0;
    NativeStream stream{};
};
[[nodiscard]] std::vector<std::uint8_t> category_colors(std::span<const int> labels, int category_count);
[[nodiscard]] std::int32_t build_category_colors_cuda(const CategoryColorWork& work) noexcept;
[[nodiscard]] std::int32_t raster_mask_boxes_rgb(const MaskBoxLabelRgbWork& work) noexcept;
[[nodiscard]] std::int32_t raster_boxes_bgr(const BoxLabelBgrWork& work) noexcept;
[[nodiscard]] std::int32_t raster_mask_boxes_bgr(const MaskBoxLabelBgrWork& work) noexcept;
[[nodiscard]] std::int32_t raster_instance_overlay_rgba(const InstanceOverlayRgbaWork& work) noexcept;
[[nodiscard]] std::int32_t composite_rgba_over_bgr(const CompositeRgbaOverBgrWork& work) noexcept;
[[nodiscard]] std::int32_t composite_rgba(const CompositeRgbaWork& work) noexcept;
[[nodiscard]] std::int32_t finalize_rgba(const FinalizeRgbaWork& work) noexcept;
[[nodiscard]] std::int32_t copy_bgr_to_rgba(const CopyBgrToRgbaWork& work) noexcept;
[[nodiscard]] std::int32_t raster_mask_rgba(const MaskRgbaWork& work) noexcept;
[[nodiscard]] std::int32_t raster_mask_runs_rgba(const MaskRunsRgbaWork& work) noexcept;
[[nodiscard]] std::int32_t raster_box_outline_rgba(const BoxOutlineRgbaWork& work) noexcept;
[[nodiscard]] std::int32_t raster_selection_handles_rgba(const SelectionHandlesRgbaWork& work) noexcept;
[[nodiscard]] std::int32_t raster_polyline_rgba(const PolylineRgbaWork& work) noexcept;
[[nodiscard]] std::int32_t raster_points_rgba(const PointsRgbaWork& work) noexcept;
[[nodiscard]] std::int32_t raster_skeleton_rgba(const SkeletonRgbaWork& work) noexcept;
[[nodiscard]] std::int32_t pack_bool_masks(const BoolMaskPackWork& work) noexcept;
}  // namespace mmltk::backend::imaging::raster
