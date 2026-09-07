#pragma once

#include <cstdint>

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
    bool operator==(const VisualFrame&) const = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return source.valid() && extent.valid() && revision != 0U; }
};
struct VisualSourceObservation final {
    VisualFrame frame{};
    std::uint64_t snapshot_revision = 0U;
    bool operator==(const VisualSourceObservation&) const = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return frame.valid() && snapshot_revision != 0U; }
};
[[nodiscard]] bool visual_product_matches_frame(const VisualFrame&, const mmltk::frameworks::gpu::BorrowedImageProductReadView&) noexcept;
[[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView borrow_matching_visual_product(
    const VisualFrame&, mmltk::frameworks::gpu::BorrowedImageProductReadView);

[[nodiscard]] constexpr VisualFrame visual_frame(const PresentationSourceIdentity source, const VisualExtent extent,
                                                 const std::uint64_t revision) noexcept {
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

}  // namespace mmltk::controller
