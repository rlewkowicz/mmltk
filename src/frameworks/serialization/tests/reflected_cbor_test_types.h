#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <inplace_vector>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/serialization/reflected_cbor.h"
namespace mmltk::frameworks::serialization::test {
namespace policy = mmltk::frameworks::reflection;
struct BoundBase {
    std::uint16_t id = 0U;
};
struct BoundLeaf final : BoundBase {
    [[= policy::MaxBytes{24U}]] std::string text;
    std::array<std::byte, 3U> bytes{};
    [[= policy::MaxBytes{1U}]][[= policy::MaxItems{1U}]] std::optional<wire::Value> payload;
};
struct BoundContainers final {
    [[= policy::MaxItems{2U}]] std::optional<std::vector<BoundLeaf>> rows;
    std::array<std::optional<BoundLeaf>, 2U> fixed;
    std::inplace_vector<BoundLeaf, 2U> local;
    [[= policy::MaxItems{2U}]] std::span<const BoundLeaf> borrowed;
    // Unannotated dynamic leaves use the enclosing aggregate admission budget.
    std::array<wire::Value, 2U> values;
};
struct BoundFlat final {
    [[= policy::MaxBytes{4U}]][[= policy::MaxItems{2U}]] wire::FlatValue flat;
};
MMLTK_REFLECT_FIELDS(BoundBase)
MMLTK_REFLECT_FIELDS(BoundLeaf)
MMLTK_REFLECT_FIELDS(BoundContainers)
MMLTK_REFLECT_FIELDS(BoundFlat)
using BoundVariant = std::variant<BoundLeaf, BoundContainers>;
// Materialize each query beside its ordinary canonical declaration.
inline constexpr auto kLeafFullBytes = reflected_maximum_cbor_bytes<BoundLeaf>();
inline constexpr auto kLeafStructuralBytes = reflected_structural_cbor_bytes<BoundLeaf>();
inline constexpr auto kLeafMembers = reflected_cbor_member_count<BoundLeaf>();
inline constexpr auto kContainersStructuralBytes = reflected_structural_cbor_bytes<BoundContainers>();
inline constexpr auto kVariantStructuralBytes = reflected_structural_cbor_bytes<BoundVariant>();
inline constexpr auto kFlatFullBytes = reflected_maximum_cbor_bytes<BoundFlat>();
inline constexpr auto kFlatStructuralBytes = reflected_structural_cbor_bytes<BoundFlat>();
}  // namespace mmltk::frameworks::serialization::test
