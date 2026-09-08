#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <type_traits>

#include "mmltk/frameworks/reflection/materializer.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"

namespace mmltk::controller {

struct BrowserContentSession final {
    std::uint64_t value;
};

enum class PresentationSourceKind : std::uint8_t {
    None[[= BrowserContentSession{0U}]],
    Explore[[= BrowserContentSession{1U}]],
    Annotation[[= BrowserContentSession{2U}]],
    Predict[[= BrowserContentSession{3U}]],
    Live[[= BrowserContentSession{4U}]],
    Upscale[[= BrowserContentSession{5U}]],
};

struct PresentationSourceMetadata final {
    PresentationSourceKind kind;
    std::uint64_t session;
};

struct PresentationSourceMaterializer final {
    template <class Owner, class Input>
    [[nodiscard]] consteval auto operator()() const {
        std::array<PresentationSourceMetadata, Input::enumerators.size()> result{};
        std::size_t index = 0U;
        template for (constexpr auto enumerator : Input::enumerators) {
            std::size_t count = 0U;
            std::uint64_t session = 0U;
            template for (constexpr auto annotation : std::define_static_array(std::meta::annotations_of(enumerator))) {
                if constexpr (std::same_as<std::remove_cvref_t<typename[:std::meta::type_of(annotation):]>, BrowserContentSession>) {
                    session = std::meta::extract<BrowserContentSession>(annotation).value;
                    ++count;
                }
            }
            if (count != 1U) throw "each visual source requires one browser content session";
            const auto kind = std::meta::extract<Owner>(enumerator);
            if (static_cast<std::size_t>(kind) != index) throw "visual source kinds require dense values starting at zero";
            if ((kind == Owner::None) != (session == 0U)) throw "only the empty visual source has a zero session";
            for (std::size_t prior = 0U; prior < index; ++prior)
                if (result[prior].session == session) throw "visual source sessions must be unique";
            result[index++] = {kind, session};
        }
        return result;
    }
};

inline constexpr auto presentation_source_metadata =
    mmltk::frameworks::reflection::materialize<PresentationSourceKind>(PresentationSourceMaterializer{});

[[nodiscard]] constexpr std::uint64_t presentation_source_session(const PresentationSourceKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < presentation_source_metadata.size() ? presentation_source_metadata[index].session : 0U;
}

struct PresentationSourceIdentity final {
    PresentationSourceKind kind = PresentationSourceKind::None;
    std::uint32_t instance = 0U;

    [[nodiscard]] constexpr bool valid() const noexcept { return presentation_source_session(kind) != 0U && instance != 0U; }
    constexpr auto operator<=>(const PresentationSourceIdentity&) const = default;
};

MMLTK_REFLECT_ENUM(PresentationSourceKind)
MMLTK_REFLECT_FIELDS(PresentationSourceIdentity)

}  // namespace mmltk::controller
