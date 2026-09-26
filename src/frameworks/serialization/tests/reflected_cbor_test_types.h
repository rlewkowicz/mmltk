#pragma once
#include "src/frameworks/reflection/declaration_annotations.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <inplace_vector>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
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
 MMLTK_MAX_BYTES(24U) std::string text;
 std::array<std::byte, 3U> bytes{};
 MMLTK_MAX_BYTES(1U) MMLTK_MAX_ITEMS(1U) std::optional<wire::Value> payload;
};
struct BoundContainers final {
 MMLTK_MAX_ITEMS(2U) std::optional<std::vector<BoundLeaf>> rows;
 std::array<std::optional<BoundLeaf>, 2U> fixed;
 std::inplace_vector<BoundLeaf, 2U> local;
 MMLTK_MAX_ITEMS(2U) std::span<const BoundLeaf> borrowed;
 // Unannotated dynamic leaves use the enclosing aggregate admission budget.
 std::array<wire::Value, 2U> values;
};
struct BoundFlat final {
 MMLTK_MAX_BYTES(4U) MMLTK_MAX_ITEMS(2U) wire::FlatValue flat;
};
struct BoundPaths final {
 MMLTK_MAX_BYTES(4U) MMLTK_MAX_ITEMS(2U) std::vector<std::filesystem::path> paths;
};
struct BoundTextContainers final {
 MMLTK_MAX_BYTES(4U) std::array<std::string, 2U> fixed;
 MMLTK_MAX_BYTES(4U) std::inplace_vector<std::string, 2U> local;
 MMLTK_MAX_BYTES(4U) MMLTK_MAX_ITEMS(2U) std::span<const std::string> borrowed;
};
MMLTK_REFLECT_FIELDS(BoundBase)
MMLTK_REFLECT_FIELDS(BoundLeaf)
MMLTK_REFLECT_FIELDS(BoundContainers)
MMLTK_REFLECT_FIELDS(BoundFlat)
MMLTK_REFLECT_FIELDS(BoundPaths)
MMLTK_REFLECT_FIELDS(BoundTextContainers)
using BoundVariant = std::variant<BoundLeaf, BoundContainers>;
// Authoring syntax exercises the same canonical policies as the serializer's
// existing nested-container fixtures, without defining an alternate schema.
struct DeclarationBounds final {
 MMLTK_MIN_BYTES(1U) MMLTK_MAX_BYTES(4U) std::string text = "a";
 MMLTK_MAX_BYTES(4U) MMLTK_MAX_ITEMS(std::integral_constant<std::size_t, 2U>::value) std::optional<std::vector<std::filesystem::path>> paths;
 MMLTK_MINIMUM(double, 0.0) MMLTK_MAXIMUM(double, 1.0) MMLTK_FINITE double fraction = 0.5;
 MMLTK_MAX_PATH_BYTES std::filesystem::path directory;
 MMLTK_MAX_PATH_BYTES std::string original;
 bool operator==(const DeclarationBounds&) const = default;
};
struct InvalidDeclarationPolicies final {
 MMLTK_MINIMUM(int, 0) MMLTK_MINIMUM(int, 1) int duplicate = 1;
 MMLTK_FINITE int finite_integer = 0;
 MMLTK_MIN_BYTES(5U) MMLTK_MAX_BYTES(4U) std::string reversed;
 MMLTK_MAX_ITEMS(2U) int scalar_items = 0;
};
MMLTK_REFLECT_FIELDS(DeclarationBounds)
MMLTK_REFLECT_FIELDS(InvalidDeclarationPolicies)
// Compare annotation *types and values*, not just a projection that could hide
// a changed typed bound. The single reflected declaration owns field identity.
inline constexpr bool kDeclarationAnnotationTypesMatch = [] consteval {
 bool result = true;
 policy::visit_materialized_members<DeclarationBounds>([&]<class Declaration>(const auto&) {
  using Member = typename Declaration::member_type;
  if constexpr (std::is_same_v<Member, double>) {
   result = result && std::is_same_v<Declaration, policy::MaterializedMemberDeclaration<&DeclarationBounds::fraction, policy::Minimum<double>{0.0}, policy::Maximum<double>{1.0}, policy::Finite{}>>;
  } else if constexpr (std::is_same_v<Member, std::optional<std::vector<std::filesystem::path>>>) {
   result = result && std::is_same_v<Declaration, policy::MaterializedMemberDeclaration<&DeclarationBounds::paths, policy::MaxBytes{4U}, policy::MaxItems{2U}>>;
  }
 });
 return result;
}();
static_assert(kDeclarationAnnotationTypesMatch);
static_assert(!policy::materialized_policy_annotations_are_valid<&InvalidDeclarationPolicies::duplicate>());
static_assert(!policy::materialized_policy_annotations_are_valid<&InvalidDeclarationPolicies::finite_integer>());
static_assert(!policy::materialized_policy_annotations_are_valid<&InvalidDeclarationPolicies::reversed>());
static_assert(!policy::materialized_policy_annotations_are_valid<&InvalidDeclarationPolicies::scalar_items>());
static_assert(policy::policy_of_member<&DeclarationBounds::directory>() == policy::policy_of_member<&DeclarationBounds::original>());
// Materialize each query beside its ordinary canonical declaration.
inline constexpr auto kLeafFullBytes = reflected_maximum_cbor_bytes<BoundLeaf>();
inline constexpr auto kLeafStructuralBytes = reflected_structural_cbor_bytes<BoundLeaf>();
inline constexpr auto kLeafMembers = reflected_cbor_member_count<BoundLeaf>();
inline constexpr auto kContainersStructuralBytes = reflected_structural_cbor_bytes<BoundContainers>();
inline constexpr auto kVariantStructuralBytes = reflected_structural_cbor_bytes<BoundVariant>();
inline constexpr auto kFlatFullBytes = reflected_maximum_cbor_bytes<BoundFlat>();
inline constexpr auto kFlatStructuralBytes = reflected_structural_cbor_bytes<BoundFlat>();
inline constexpr auto kPathsFullBytes = reflected_maximum_cbor_bytes<BoundPaths>();
inline constexpr auto kTextContainersFullBytes = reflected_maximum_cbor_bytes<BoundTextContainers>();
}  // namespace mmltk::frameworks::serialization::test
