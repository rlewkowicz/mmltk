#pragma once

#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"

#include <cstddef>
#include <compare>
#include <string>
#include <type_traits>

#include "mmltk/frameworks/reflection/materializer.h"

namespace mmltk::controller::contracts {

inline constexpr std::size_t kArtifactCatalogCapacity = 256U;
inline constexpr std::size_t kArtifactClassNameCapacity = 256U;

struct ArtifactClassName final {
    [[= mmltk::frameworks::reflection::MaxBytes{kArtifactClassNameCapacity}]] std::string value;

    [[nodiscard]] bool valid() const noexcept {
        return !value.empty() && value.size() <= kArtifactClassNameCapacity && value.find('\0') == std::string::npos;
    }
    auto operator<=>(const ArtifactClassName&) const = default;
};

MMLTK_REFLECT_FIELDS(ArtifactClassName)

}  // namespace mmltk::controller::contracts
