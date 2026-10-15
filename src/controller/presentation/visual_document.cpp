#include "src/controller/presentation/visual_document.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <cerrno>
#include <string>
#include <system_error>
#include <sys/random.h>
#include <unistd.h>
#include "src/controller/contracts/application_boundary.h"
namespace mmltk::controller {
void VisualDocument::BindResource(contracts::WorkspaceResource resource) {
 // An immutable image document is a product identity, independent of editor
 // revisions. OS entropy scopes the monotonic meaning identity across process
 // restarts; pid additionally separates a fork inheriting the process state.
 static const std::string incarnation = [] {
  std::array<unsigned char, 16> bytes{};
  std::size_t offset = 0;
  while (offset < bytes.size()) {
   const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
   if (count < 0 && errno == EINTR) continue;
   if (count <= 0) throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "visual document incarnation");
   offset += static_cast<std::size_t>(count);
  }
  constexpr std::string_view hex = "0123456789abcdef";
  std::string result;
  result.reserve(32);
  for (const auto value : bytes) { result += hex[value >> 4]; result += hex[value & 15]; }
  return result;
 }();
 if (!resource.valid()) throw contracts::InvalidIntentError("Visual source identity is invalid");
 auto bound = contracts::WorkspaceResource::From(std::string(resource.view()) + "/snapshot-" + incarnation + "-" + std::to_string(::getpid()) + "-" + std::to_string(meaning_identity_) + "-" + std::to_string(resource.revision), resource.revision);
 if (!bound.valid()) throw contracts::InvalidIntentError("Visual source identity exceeds document capacity");
 scene.document = bound;
}
std::vector<mmltk::backend::data::catalog::KeypointAnnotation> VisualSkeleton::Annotations() const {
 using Visibility = mmltk::backend::data::catalog::KeypointVisibility;
 if (joints.size() != schema.names.size()) throw contracts::InvalidIntentError("Visual joints disagree with their schema");
 std::vector<mmltk::backend::data::catalog::KeypointAnnotation> result;
 result.reserve(joints.size());
 for (const auto& joint : joints) result.push_back({joint.point.x, joint.point.y, joint.visibility.value_or(joint.confidence >= 0.5F ? Visibility::Visible : Visibility::Unannotated)});
 return result;
}
void VisualDocument::SetCatalog(const mmltk::backend::data::catalog::ClassCatalog& catalog) {
 auto record = catalog.record();
 scene.categories = std::move(record.names);
 scene.keypoint_schemas = record.keypoints ? std::move(*record.keypoints) : std::vector<mmltk::backend::data::catalog::ClassKeypointSchema>{};
}
void VisualDocument::AppendSourceObject(contracts::AnnotationObject object, std::array<float, 4> bounds, std::span<const mmltk::backend::data::catalog::KeypointAnnotation> joints) {
 if (!joints.empty()) {
  if (object.category >= scene.keypoint_schemas.size()) throw contracts::InvalidIntentError("Annotation joint schema is unavailable");
  const auto& schema = scene.keypoint_schemas[object.category];
  if (schema.names.size() != joints.size()) throw contracts::InvalidIntentError("Annotation joints disagree with their class schema");
  object.shape = contracts::AnnotationShape::Skeleton;
  for (std::size_t index = 0; index < joints.size(); ++index) object.skeleton_nodes.push_back({schema.names[index], {joints[index].x, joints[index].y}, joints[index].visibility});
  for (const auto& edge : schema.skeleton) object.skeleton_edges.push_back({edge.first, edge.second});
 }
 mask_bounds.push_back(bounds);
 if (object.shape != contracts::AnnotationShape::Skeleton) object.shape = object.mask.present ? contracts::AnnotationShape::Mask : contracts::AnnotationShape::Box;
 scene.objects.push_back(std::move(object));
}
std::uint64_t VisualDocument::NextIdentity() {
 static std::atomic_uint64_t next{1U};
 const auto identity = next.fetch_add(1U, std::memory_order_relaxed);
 if (identity == 0U || identity == std::numeric_limits<std::uint64_t>::max()) std::terminate();
 return identity;
}
namespace {
// Inspect canonical field declarations once at compile time. Nonspatial trees
// (notably potentially large RLE storage) generate no runtime traversal.
template <class T>
consteval bool contains_annotation_point() {
 if constexpr (std::is_same_v<T, contracts::AnnotationPoint>) {
  return true;
 } else if constexpr (std::ranges::range<T>) {
  return contains_annotation_point<std::ranges::range_value_t<T>>();
 } else if constexpr (requires { materialized_field_policies(std::type_identity<T>{}); }) {
  bool found = false;
  mmltk::frameworks::reflection::visit_materialized_members<T>([&]<class Declaration>(const auto&) {
   using Field = std::remove_cvref_t<decltype(std::declval<T&>().*Declaration::pointer)>;
   found = found || contains_annotation_point<Field>();
  });
  return found;
 } else {
  return false;
 }
}
struct PointProjection final {
 float scale_x = 1.0F;
 float scale_y = 1.0F;
 VisualRegion crop{};
 VisualExtent target{};
 void Apply(contracts::AnnotationPoint& point) const {
  point.x *= scale_x;
  point.y *= scale_y;
  if (!point.finite()) throw contracts::InvalidIntentError("Annotation coordinate is not finite");
  if (crop.valid()) {
   point.x = std::clamp(point.x - static_cast<float>(crop.x), 0.0F, static_cast<float>(crop.width));
   point.y = std::clamp(point.y - static_cast<float>(crop.y), 0.0F, static_cast<float>(crop.height));
   if (target.valid()) {
    point.x *= static_cast<float>(target.width) / static_cast<float>(crop.width);
    point.y *= static_cast<float>(target.height) / static_cast<float>(crop.height);
   }
  }
 }
};
template <class T>
void project_spatial_members(T& value, const PointProjection projection) {
 if constexpr (std::is_same_v<T, contracts::AnnotationPoint>) {
  projection.Apply(value);
 } else if constexpr (contains_annotation_point<T>()) {
  if constexpr (std::ranges::range<T>) {
   for (auto& item : value) project_spatial_members(item, projection);
  } else {
   mmltk::frameworks::reflection::visit_materialized_members<T>([&]<class Declaration>(const auto&) { project_spatial_members(value.*Declaration::pointer, projection); });
  }
 }
}
}  // namespace
contracts::AnnotationSceneContent materialize_visual_document(const VisualDocument& document, const VisualExtent extent, VisualRegion crop, VisualExtent target) {
 if (!crop.valid()) crop = {.width = extent.width, .height = extent.height};
 if (!extent.valid() || crop.x > extent.width || crop.y > extent.height || crop.width > extent.width - crop.x || crop.height > extent.height - crop.y ||
     crop.width > std::numeric_limits<std::uint16_t>::max() || crop.height > std::numeric_limits<std::uint16_t>::max())
  throw contracts::InvalidIntentError("Annotation crop is outside the image extent");
 if (!target.valid()) target = {crop.width, crop.height};
 if (target.width > std::numeric_limits<std::uint16_t>::max() || target.height > std::numeric_limits<std::uint16_t>::max())
  throw contracts::InvalidIntentError("Annotation target exceeds the document extent");
 auto scene = document.scene;
 scene.frame_width = static_cast<std::uint16_t>(target.width);
 scene.frame_height = static_cast<std::uint16_t>(target.height);
 scene.frame_ready = true;
 if (scene.document.view().find("/snapshot-") != std::string_view::npos) {
  // Two equal-sized crops of one retained source still refer to different
  // pixels. Derived scaling keeps the source resource; this import binds its
  // actual sampling geometry without changing the producer's source facts.
  scene.document = contracts::WorkspaceResource::From(std::string(scene.document.view()) + "/crop-" + std::to_string(extent.width) + "-" + std::to_string(extent.height) + "-" + std::to_string(crop.x) + "-" + std::to_string(crop.y) + "-" + std::to_string(crop.width) + "-" + std::to_string(crop.height) + "-" + std::to_string(target.width) + "-" + std::to_string(target.height), scene.document.revision);
  if (!scene.document.valid()) throw contracts::InvalidIntentError("Annotation source geometry identity exceeds document capacity");
 }
 project_spatial_members(scene.objects, {.crop = crop, .target = target});
 // A clipped joint has no annotation in this crop. Keep its stable slot/name;
 // clamping its position alone must not invent a visible boundary joint.
 for (std::size_t index = 0; index < scene.objects.size(); ++index) {
  auto& object = scene.objects[index];
  const auto& source = document.scene.objects[index];
  for (std::size_t joint = 0; joint < object.skeleton_nodes.size(); ++joint) {
   const auto point = source.skeleton_nodes[joint].point;
   if (point.x < crop.x || point.y < crop.y || point.x > crop.x + crop.width || point.y > crop.y + crop.height)
    object.skeleton_nodes[joint].visibility = mmltk::backend::data::catalog::KeypointVisibility::Unannotated;
  }
 }
 std::size_t total_runs = 0U;
 for (std::size_t index = 0U; index < scene.objects.size(); ++index) {
  auto& object = scene.objects[index];
  if (!object.mask.present) continue;
  object.mask.runs.clear();
  if (index >= document.mask_bounds.size()) throw contracts::InvalidIntentError("Annotation mask bounds are unavailable");
  const auto& bounds = document.mask_bounds[index];
  if (!std::ranges::all_of(bounds, [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; }) || bounds[0] > bounds[2] || bounds[1] > bounds[3])
   throw contracts::InvalidIntentError("Annotation mask bounds are invalid");
  if (bounds[0] == bounds[2] || bounds[1] == bounds[3]) continue;
  if (!document.mask_contains) throw contracts::InvalidIntentError("Annotation mask support is unavailable");
  const auto coordinate = [](std::uint32_t pixel, std::uint32_t offset, std::uint32_t size, std::uint32_t output, std::uint32_t full) {
   return (static_cast<float>(offset) + std::floor(static_cast<float>(pixel) * static_cast<float>(size) / static_cast<float>(output)) + 0.5F) / static_cast<float>(full);
  };
  // Binary search the actual nearest-sampling coordinates. This includes
  // ties at closed support edges and avoids inverse-rounding omissions.
  const auto interval = [&](float low, float high, std::uint32_t offset, std::uint32_t size, std::uint32_t output, std::uint32_t full) {
   const auto pixels = std::views::iota(0U, output);
   const auto first = std::ranges::lower_bound(pixels, low, {}, [&](auto pixel) { return coordinate(pixel, offset, size, output, full); });
   const auto last = std::ranges::upper_bound(pixels, high, {}, [&](auto pixel) { return coordinate(pixel, offset, size, output, full); });
   return std::pair{static_cast<std::uint32_t>(first - pixels.begin()), static_cast<std::uint32_t>(last - pixels.begin())};
  };
  const auto [first_x, last_x] = interval(bounds[0], bounds[2], crop.x, crop.width, target.width, extent.width);
  const auto [first_y, last_y] = interval(bounds[1], bounds[3], crop.y, crop.height, target.height, extent.height);
  for (auto y = first_y; y < last_y; ++y) {
   const auto normalized_y = coordinate(y, crop.y, crop.height, target.height, extent.height);
   auto run_start = last_x;
   const auto append = [&](std::uint32_t end) {
    if (total_runs == contracts::kAnnotationMaskRunCapacity) throw contracts::InvalidIntentError("Annotation import exceeds the document mask-run capacity");
    object.mask.runs.push_back({static_cast<std::uint16_t>(y), static_cast<std::uint16_t>(run_start), static_cast<std::uint16_t>(end - 1U)});
    ++total_runs;
   };
   for (auto x = first_x; x < last_x; ++x) {
    const bool supported = document.mask_contains(index, coordinate(x, crop.x, crop.width, target.width, extent.width), normalized_y);
    if (supported && run_start == last_x) run_start = x;
    if (!supported && run_start != last_x) {
     append(x);
     run_start = last_x;
    }
   }
   if (run_start != last_x) append(last_x);
  }
 }
 if (!scene.valid()) throw contracts::InvalidIntentError("Annotation import exceeds the document geometry or catalog capacity");
 return scene;
}
std::shared_ptr<const VisualDocument> scale_visual_document(const std::shared_ptr<const VisualDocument>& source, const VisualExtent source_extent, const VisualExtent target_extent) {
 if (!source || !source_extent.valid() || !target_extent.valid()) throw contracts::InvalidIntentError("Visual document scale is invalid");
 auto target = std::make_shared<VisualDocument>(*source);
 project_spatial_members(target->scene.objects,
  {.scale_x = static_cast<float>(target_extent.width) / static_cast<float>(source_extent.width), .scale_y = static_cast<float>(target_extent.height) / static_cast<float>(source_extent.height)});
 // The scene is a lightweight semantic projection until import chooses a
 // checked editable extent. Its normalized mask predicate remains valid.
 target->scene.frame_ready = false;
 return target;
}
}  // namespace mmltk::controller
