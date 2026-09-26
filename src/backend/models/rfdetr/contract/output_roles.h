#pragma once
#include "src/frameworks/reflection/declaration_annotations.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include <cstdint>
#include <compare>
#include <string>
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "mmltk/frameworks/reflection/materializer.h"
namespace mmltk::backend::models::rfdetr {
enum class RfdetrOutputRole : std::uint8_t { Unspecified, Logits, Boxes, Masks };
MMLTK_REFLECT_ENUM(RfdetrOutputRole)
struct RfdetrNamedOutputRole final {
 MMLTK_MAX_BYTES(1024U) std::string name;
 RfdetrOutputRole role = RfdetrOutputRole::Unspecified;
 auto operator<=>(const RfdetrNamedOutputRole&) const = default;
};
MMLTK_REFLECT_FIELDS(RfdetrNamedOutputRole)
}  // namespace mmltk::backend::models::rfdetr
