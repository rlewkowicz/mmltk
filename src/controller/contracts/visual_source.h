#pragma once

#include <compare>
#include <cstdint>
#include <type_traits>

#include "mmltk/frameworks/reflection/materializer.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"

namespace mmltk::controller {

enum class PresentationSourceKind : std::uint8_t {
    None,
    Explore,
    Annotation,
    Predict,
    Live,
    Upscale,
};

[[nodiscard]] constexpr std::uint64_t presentation_source_session(const PresentationSourceKind kind) noexcept {
    switch (kind) {
        case PresentationSourceKind::Explore:
            return 1U;
        case PresentationSourceKind::Annotation:
            return 2U;
        case PresentationSourceKind::Predict:
            return 3U;
        case PresentationSourceKind::Live:
            return 4U;
        case PresentationSourceKind::Upscale:
            return 5U;
        case PresentationSourceKind::None:
            return 0U;
    }
    return 0U;
}

struct PresentationSourceIdentity final {
    PresentationSourceKind kind = PresentationSourceKind::None;
    std::uint32_t instance = 0U;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return kind > PresentationSourceKind::None && kind <= PresentationSourceKind::Upscale && instance != 0U;
    }
    constexpr auto operator<=>(const PresentationSourceIdentity&) const = default;
};

MMLTK_REFLECT_ENUM(PresentationSourceKind)
MMLTK_REFLECT_FIELDS(PresentationSourceIdentity)

}  // namespace mmltk::controller
