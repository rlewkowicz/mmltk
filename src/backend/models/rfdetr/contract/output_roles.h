#pragma once
#include "src/frameworks/reflection/reflected_declarations.h"
#include <cstdint>
#include <cstddef>
#include <compare>
#include <string>
#include "mmltk/frameworks/reflection/materializer.h"
namespace mmltk::backend::models::rfdetr {
enum class RfdetrOutputRole : std::uint8_t { Unspecified, Logits, Boxes, Masks, Keypoints, KeypointScores, KeypointPrecision };
MMLTK_REFLECT_ENUM(RfdetrOutputRole)
inline constexpr std::size_t kMaximumNamedOutputRoles = mmltk::frameworks::reflection::enum_entries<RfdetrOutputRole>().size() - 1U;
struct RfdetrNamedOutputRole final {
 MMLTK_MAX_BYTES(1024U) std::string name;
 RfdetrOutputRole role = RfdetrOutputRole::Unspecified;
 auto operator<=>(const RfdetrNamedOutputRole&) const = default;
};
MMLTK_REFLECT_FIELDS(RfdetrNamedOutputRole)
}  // namespace mmltk::backend::models::rfdetr
