#pragma once
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
    [[= mmltk::frameworks::reflection::MaxBytes{1024U}]] std::string name;
    RfdetrOutputRole role = RfdetrOutputRole::Unspecified;
    auto operator<=>(const RfdetrNamedOutputRole&) const = default;
};
MMLTK_REFLECT_FIELDS(RfdetrNamedOutputRole)
}  // namespace mmltk::backend::models::rfdetr
