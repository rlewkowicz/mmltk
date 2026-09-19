#pragma once
#include <cstdint>
#include "mmltk/frameworks/reflection/member_relation.h"
#include "src/controller/contracts/visual_source.h"
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
struct VisualFrame final {
    PresentationSourceIdentity source{};
    VisualExtent extent{};
    std::uint64_t revision = 0U;
    VisualRegion content{};
    std::uint64_t clean_revision = 0U;
    VisualExtent source_extent{};
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
[[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView borrow_matching_visual_product(const VisualFrame&,
                                                                                                  mmltk::frameworks::gpu::BorrowedImageProductReadView);
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
struct VisualCleanContentRelation final
    : mmltk::frameworks::reflection::StaticMemberRelation<
          VisualFrame, VisualCleanContentIdentity, 4U,
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
    if (identity.*VisualCleanContentRelation::zero_fallback_destination == 0U)
        identity.*VisualCleanContentRelation::zero_fallback_destination = frame.*VisualCleanContentRelation::zero_fallback_source;
    return identity;
}
}  // namespace mmltk::controller
