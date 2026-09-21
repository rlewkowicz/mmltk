module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
export module mmltk.backend.imaging.annotation.core;
export import mmltk.backend.imaging.annotation.common;
export namespace mmltk::backend::imaging::annotation {
using AnnotationShapeType = ShapeType;
using AnnotationPoint = Point;
using AnnotationSplineHandleMode = SplineHandleMode;
using AnnotationSplineHandle = SplineHandle;
using AnnotationSplineKnot = SplineKnot;
using AnnotationBoxShape = BoxShape;
using AnnotationDeferredMaskRun = MaskRun;
using AnnotationDeferredMask = DeferredMask;
using AnnotationMaskShape = MaskShape;
using AnnotationSplineShape = SplineShape;
using AnnotationPointShape = PointShape;
using AnnotationSkeletonNode = SkeletonNode;
using AnnotationSkeletonEdge = SkeletonEdge;
using AnnotationSkeletonShape = SkeletonShape;
using AnnotationShapeVariant = Shape;
using AnnotationObject = Object;
struct AnnotationResolvedObject {
 std::size_t object_index = 0;
 std::size_t category_index = 0;
 std::string class_name;
 AnnotationShapeType shape_type = AnnotationShapeType::Box;
 AnnotationBox bbox{};
 std::vector<std::uint8_t> mask;
 std::string mask_rle;
 std::vector<AnnotationPoint> points_xy;
 std::vector<std::uint8_t> crop_rgba;
 std::uint32_t crop_width = 0;
 std::uint32_t crop_height = 0;
};
[[nodiscard]] bool materialize_annotation_mask(AnnotationMaskShape* shape);
[[nodiscard]] bool annotation_deferred_mask_valid(const AnnotationMaskShape& shape) noexcept;
[[nodiscard]] std::optional<std::vector<std::uint8_t>> project_annotation_mask_pixels(const AnnotationMaskShape& shape);
[[nodiscard]] std::string next_annotation_object_id(std::size_t object_count);
const char* annotation_spline_handle_mode_name(AnnotationSplineHandleMode mode) noexcept;
AnnotationSplineHandleMode annotation_spline_handle_mode_from_name(std::string_view value);
AnnotationShapeType annotation_shape_type(const AnnotationShapeVariant& shape);
const char* annotation_shape_type_name(AnnotationShapeType shape_type);
const char* annotation_object_shape_label(const AnnotationObject& object);
std::optional<AnnotationBox> annotation_object_bbox(const AnnotationObject& object);
std::optional<AnnotationBox> annotation_object_display_box(const AnnotationObject& object);
std::vector<AnnotationPoint> annotation_object_points(const AnnotationObject& object);
AnnotationMaskRegion mask_region_from_box(const AnnotationBox& box);
AnnotationBox box_from_mask_region(const AnnotationMaskRegion& region);
const AnnotationMaskShape* annotation_object_mask_shape(const AnnotationObject& object);
AnnotationMaskShape* annotation_object_mask_shape(AnnotationObject* object);
bool annotation_object_supports_mask_editing(const AnnotationObject& object);
bool translate_annotation_object(AnnotationObject* object, int dx, int dy, std::uint32_t capture_width, std::uint32_t capture_height,
                                 bool clip_to_bounds = false);
bool resize_annotation_object_to_box(AnnotationObject* object, const AnnotationBox& box, std::uint32_t capture_width, std::uint32_t capture_height);
std::optional<AnnotationPoint> annotation_spline_segment_point(const AnnotationSplineShape& spline, std::size_t segment_index, float t);
std::vector<AnnotationPoint> sample_annotation_spline_points(const AnnotationSplineShape& spline, int samples_per_segment = 32);
enum class AnnotationHandleRole : std::uint8_t {
 None = 0,
 Point = 1,
 SplineKnot = 2,
 SplineInHandle = 3,
 SplineOutHandle = 4,
 SkeletonNode = 5,
};
struct AnnotationHandleId {
 std::size_t object_index = 0;
 std::size_t element_index = 0;
 AnnotationHandleRole role = AnnotationHandleRole::None;
 [[nodiscard]] bool operator==(const AnnotationHandleId&) const noexcept = default;
};
struct AnnotationSceneView {
 std::uint64_t generation = 0U;
 std::span<const AnnotationObject> objects{};
 [[nodiscard]] std::size_t size() const noexcept { return objects.size(); }
 [[nodiscard]] const AnnotationObject* object(const std::size_t index) const noexcept { return index < objects.size() ? &objects[index] : nullptr; }
};
struct AnnotationVisibleGeometry {
 std::vector<AnnotationPoint> frame_points;
 std::vector<AnnotationSkeletonEdge> edges;
};
struct AnnotationVisibleObject {
 std::size_t index = 0;
 AnnotationBox capture_box{};
 AnnotationBox frame_box{};
 bool fully_visible = false;
 AnnotationVisibleGeometry geometry{};
};
struct AnnotationProjectedBox {
 std::optional<AnnotationBox> frame_box;
 bool fully_visible = false;
};
struct AnnotationEditableHandle {
 AnnotationHandleId id{};
 std::size_t category_index = 0;
 AnnotationPoint capture_point{};
 AnnotationPoint frame_point{};
 AnnotationPoint tether_frame_point{};
 bool has_tether = false;
 bool materialized = true;
};
struct AnnotationProjectedScene {
 std::uint64_t document_generation = 0;
 std::optional<std::size_t> selected_object_index;
 std::vector<AnnotationVisibleObject> visible_objects;
 std::vector<AnnotationEditableHandle> editable_handles;
};
struct AnnotationProjectedSceneLookup {
 const AnnotationProjectedScene* projected_scene = nullptr;
 std::size_t object_count = 0;
 mutable std::size_t visible_object_cursor = 0;
 mutable std::size_t visible_geometry_cursor = 0;
 [[nodiscard]] const AnnotationVisibleObject* visible_object(std::size_t index) const noexcept;
 [[nodiscard]] const AnnotationVisibleGeometry* visible_geometry(std::size_t index) const noexcept;
};
struct AnnotationCategorySkeletonEdge {
 std::size_t source_index = 0;
 std::size_t target_index = 0;
};
struct AnnotationCategory {
 AnnotationCategory() = default;
 AnnotationCategory(int id_in, std::string name_in) : id(id_in), name(std::move(name_in)) {}
 int id = 1;
 std::string name;
 std::vector<std::string> keypoints;
 std::vector<AnnotationCategorySkeletonEdge> skeleton_edges;
};
struct AnnotationCategories {
 std::string dataset_name = "annotation-dataset";
 std::vector<AnnotationCategory> items;
};
struct AnnotationSaveConfig {
 std::filesystem::path output_root;
 std::string split = "train";
};
struct AnnotationSaveResult {
 std::filesystem::path scene_image_path;
 std::filesystem::path scene_jsonl_path;
 std::vector<std::filesystem::path> entity_paths;
 std::uint32_t scene_index = 0;
};
struct AnnotationImageInput {
 std::filesystem::path image_path;
 std::string source_name;
 std::int64_t image_id = 0;
};
struct AnnotationEncodedMask {
 std::uint32_t width = 0U;
 std::uint32_t height = 0U;
 std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
};
[[nodiscard]] inline float annotation_clamp_unit(const float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }
[[nodiscard]] inline float annotation_wrap_hue(const float hue_degrees) noexcept {
 constexpr float kHueRange = 360.0f;
 float wrapped = std::fmod(hue_degrees, kHueRange);
 if (wrapped < 0.0f) { wrapped += kHueRange; }
 return wrapped;
}
[[nodiscard]] inline AnnotationHsv annotation_bgr_to_hsv(const std::uint8_t b, const std::uint8_t g, const std::uint8_t r) noexcept {
 const float bf = static_cast<float>(b) / 255.0f;
 const float gf = static_cast<float>(g) / 255.0f;
 const float rf = static_cast<float>(r) / 255.0f;
 const float maximum = std::max({rf, gf, bf});
 const float minimum = std::min({rf, gf, bf});
 const float delta = maximum - minimum;
 AnnotationHsv hsv;
 hsv.value = maximum;
 hsv.saturation = maximum <= 0.0f ? 0.0f : delta / maximum;
 if (delta <= 0.0f) {
  hsv.hue_degrees = 0.0f;
  return hsv;
 }
 if (maximum == rf) {
  hsv.hue_degrees = 60.0f * std::fmod(((gf - bf) / delta), 6.0f);
 } else if (maximum == gf) {
  hsv.hue_degrees = 60.0f * (((bf - rf) / delta) + 2.0f);
 } else {
  hsv.hue_degrees = 60.0f * (((rf - gf) / delta) + 4.0f);
 }
 hsv.hue_degrees = annotation_wrap_hue(hsv.hue_degrees);
 return hsv;
}
AnnotationFrame load_annotation_frame(const AnnotationImageInput& input);
void write_annotation_frame_png(const std::filesystem::path& path, const AnnotationFrame& frame);
AnnotationHsv sample_annotation_hsv(const AnnotationFrame& frame, int x, int y);
void recenter_annotation_range(AnnotationColorRange& range, const AnnotationHsv& center);
bool annotation_range_active(const AnnotationColorRange& range);
bool annotation_box_has_area(const AnnotationBox& box);
[[nodiscard]] inline AnnotationBox annotation_intersect_boxes(const AnnotationBox& lhs, const AnnotationBox& rhs) noexcept {
 const AnnotationBox overlap{
  std::max(lhs.x1, rhs.x1),
  std::max(lhs.y1, rhs.y1),
  std::min(lhs.x2, rhs.x2),
  std::min(lhs.y2, rhs.y2),
 };
 if (!annotation_box_has_area(overlap)) { return {}; }
 return overlap;
}
AnnotationBox normalize_annotation_box(AnnotationBox box, std::uint32_t width, std::uint32_t height);
// Capture regions are described as origin plus extent throughout the live and mask paths, while
// AnnotationBox stores two corners; converting between the two lives here so each region type does
// not restate the arithmetic.
template <typename Coordinate, typename Extent>
[[nodiscard]] constexpr AnnotationBox annotation_box_from_origin_extent(const Coordinate x, const Coordinate y, const Extent width,
                                                                        const Extent height) noexcept {
 return AnnotationBox{
  static_cast<int>(x),
  static_cast<int>(y),
  static_cast<int>(x + width),
  static_cast<int>(y + height),
 };
}
std::uint32_t annotation_frame_capture_width(const AnnotationFrame& frame);
std::uint32_t annotation_frame_capture_height(const AnnotationFrame& frame);
AnnotationBox annotation_frame_view_box(const AnnotationFrame& frame);
AnnotationBox annotation_box_to_frame(const AnnotationFrame& frame, const AnnotationBox& capture_box);
AnnotationBox annotation_box_from_frame(const AnnotationFrame& frame, const AnnotationBox& frame_box);
[[nodiscard]] inline AnnotationPoint annotation_capture_point_to_frame_unclipped(const AnnotationFrame& frame, const AnnotationPoint& point) noexcept {
 return AnnotationPoint{
  point.x - static_cast<float>(frame.view_x),
  point.y - static_cast<float>(frame.view_y),
 };
}
[[nodiscard]] inline AnnotationPoint annotation_frame_point_to_capture_unclipped(const AnnotationPoint& point, const std::uint32_t view_x,
                                                                                 const std::uint32_t view_y) noexcept {
 return AnnotationPoint{
  point.x + static_cast<float>(view_x),
  point.y + static_cast<float>(view_y),
 };
}
AnnotationMaskRegion annotation_mask_region_from_frame(const AnnotationFrame& frame);
AnnotationFrame extract_annotation_frame_region(const AnnotationFrame& frame, const AnnotationBox& capture_box);
std::optional<AnnotationBox> annotation_bbox_from_mask(const std::vector<std::uint8_t>& mask, std::uint32_t width, std::uint32_t height);
std::vector<std::uint8_t> decode_annotation_prediction_mask(const AnnotationEncodedMask& mask, std::uint32_t width, std::uint32_t height);
std::vector<std::uint8_t> decode_annotation_mask_rle(std::string_view encoded_mask, std::uint32_t width, std::uint32_t height);
std::string encode_annotation_mask_rle(const std::vector<std::uint8_t>& mask);
[[nodiscard]] AnnotationProjectedSceneLookup make_annotation_projected_scene_lookup(const AnnotationProjectedScene* projected_scene, std::size_t object_count);
std::vector<AnnotationResolvedObject> resolve_annotation_objects(const AnnotationFrame& frame, const AnnotationCategories& categories,
                                                                 const std::vector<AnnotationObject>& objects, bool live_mode,
                                                                 const AnnotationProjectedScene* projected_scene = nullptr);
AnnotationCategories load_annotation_categories(const std::filesystem::path& output_root);
std::size_t ensure_annotation_category(AnnotationCategories& categories, const std::string& class_name);
void write_annotation_categories(const std::filesystem::path& output_root, const AnnotationCategories& categories);
std::vector<AnnotationObject> load_annotation_scene_objects(const std::filesystem::path& scene_jsonl_path, AnnotationCategories* categories);
std::optional<std::vector<AnnotationObject>> load_saved_annotation_scene_for_frame(const std::filesystem::path& output_root, const AnnotationFrame& frame,
                                                                                   AnnotationCategories* categories);
void write_annotation_png(const std::filesystem::path& path, int width, int height, int channels, const void* pixels, int stride_bytes);
AnnotationSaveResult save_annotation_scene(const AnnotationSaveConfig& config, const AnnotationFrame& frame, AnnotationCategories& categories,
                                           const std::vector<AnnotationObject>& objects, bool live_mode,
                                           const AnnotationProjectedScene* projected_scene = nullptr);
}  // namespace mmltk::backend::imaging::annotation
