#pragma once
#include <cstdint>
#include <concepts>
#include <limits>
#include <meta>
#include <optional>
#include "mmltk/frameworks/reflection/member_relation.h"
#include "src/controller/contracts/visual_source.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::frameworks::gpu {
class BorrowedImageProductReadView;
}
namespace mmltk::controller {
struct VisualExtent final {
 std::uint32_t width = 0U;
 std::uint32_t height = 0U;
 [[nodiscard]] constexpr bool valid() const noexcept { return width != 0U && height != 0U; }
 // CLEANUP-IGNORE: Visual geometry has controller sampling semantics, independent of model-analysis regions.
 bool operator==(const VisualExtent&) const = default;
};
struct VisualRegion final {
 std::uint32_t x = 0U;
 std::uint32_t y = 0U;
 std::uint32_t width = 0U;
 std::uint32_t height = 0U;
 [[nodiscard]] constexpr bool valid() const noexcept { return width != 0U && height != 0U; }
 bool operator==(const VisualRegion&) const = default;
};
// Pixel geometry only: provenance, normalized support and product policy stay with their owners.
template <class Geometry, class Visitor>
 requires(std::same_as<Geometry, VisualExtent> || std::same_as<Geometry, VisualRegion>)
constexpr void visit_visual_geometry_members(Visitor&& visitor) {
 template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^Geometry, std::meta::access_context::unchecked()))) {
  static_assert(std::same_as<typename[:std::meta::type_of(member):], std::uint32_t>);
  visitor.template operator()<member>();
 }
}
template <class Geometry>
 requires(std::same_as<Geometry, VisualExtent> || std::same_as<Geometry, VisualRegion>)
[[nodiscard]] constexpr std::optional<Geometry> checked_visual_scale(const Geometry& source, const std::uint32_t scale) {
 Geometry result{};
 bool valid = true;
 visit_visual_geometry_members<Geometry>([&]<std::meta::info Member>() {
  if (scale != 0U && source.[:Member:] > std::numeric_limits<std::uint32_t>::max() / scale)
   valid = false;
  else
   result.[:Member:] = source.[:Member:] * scale;
 });
 if (!valid) return std::nullopt;
 return result;
}
struct VisualFrame final {
 PresentationSourceIdentity source{};
 VisualExtent extent{};
 std::uint64_t revision = 0U;
 VisualRegion content{};
 std::uint64_t clean_revision = 0U;
 VisualExtent source_extent{};
 // Compiled resize provenance; a rounded Letterbox may occupy the full canvas.
 // Unclassified products leave this absent instead of inferring a policy.
 std::optional<mmltk::backend::imaging::resample::ImageResizeMode> resize_mode{};
 bool operator==(const VisualFrame&) const = default;
 [[nodiscard]] constexpr bool valid() const noexcept { return source.valid() && extent.valid() && revision != 0U; }
};
struct VisualSourceObservation final {
 VisualFrame frame{};
 std::uint64_t snapshot_revision = 0U;
 bool operator==(const VisualSourceObservation&) const = default;
 [[nodiscard]] constexpr bool valid() const noexcept { return frame.valid() && snapshot_revision != 0U; }
};
struct VisualCleanContentIdentity final {
 PresentationSourceIdentity source{};
 VisualExtent extent{};
 VisualRegion content{};
 std::uint64_t revision = 0U;
 bool operator==(const VisualCleanContentIdentity&) const = default;
};
[[nodiscard]] bool visual_product_matches_frame(const VisualFrame&, const mmltk::frameworks::gpu::BorrowedImageProductReadView&) noexcept;
[[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView borrow_matching_visual_product(const VisualFrame&, mmltk::frameworks::gpu::BorrowedImageProductReadView);
[[nodiscard]] constexpr VisualFrame visual_frame(const PresentationSourceIdentity source, const VisualExtent extent, const std::uint64_t revision) noexcept {
 return {
  .source = source,
  .extent = extent,
  .revision = revision,
 };
}
MMLTK_REFLECT_FIELDS(VisualExtent)
MMLTK_REFLECT_FIELDS(VisualRegion)
MMLTK_REFLECT_FIELDS(VisualFrame)
MMLTK_REFLECT_FIELDS(VisualSourceObservation)
MMLTK_REFLECT_FIELDS(VisualCleanContentIdentity)
struct VisualCleanContentRelation final : mmltk::frameworks::reflection::StaticMemberRelation<VisualFrame, VisualCleanContentIdentity, 4U,
                                           mmltk::frameworks::reflection::MemberRelationEntry<&VisualFrame::source, &VisualCleanContentIdentity::source>,
                                           mmltk::frameworks::reflection::MemberRelationEntry<&VisualFrame::extent, &VisualCleanContentIdentity::extent>,
                                           mmltk::frameworks::reflection::MemberRelationEntry<&VisualFrame::content, &VisualCleanContentIdentity::content>,
                                           mmltk::frameworks::reflection::MemberRelationEntry<&VisualFrame::clean_revision, &VisualCleanContentIdentity::revision>> {
 static constexpr auto zero_fallback_source = &VisualFrame::revision;
 static constexpr auto zero_fallback_destination = &VisualCleanContentIdentity::revision;
};
static_assert(VisualCleanContentRelation::valid());
[[nodiscard]] constexpr VisualCleanContentIdentity visual_clean_content_identity(const VisualFrame& frame) {
 VisualCleanContentIdentity identity;
 VisualCleanContentRelation::Project(frame, identity);
 if (identity.*VisualCleanContentRelation::zero_fallback_destination == 0U) identity.*VisualCleanContentRelation::zero_fallback_destination = frame.*VisualCleanContentRelation::zero_fallback_source;
 return identity;
}
}  // namespace mmltk::controller
