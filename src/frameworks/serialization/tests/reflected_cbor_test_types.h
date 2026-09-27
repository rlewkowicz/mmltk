#pragma once
#include "src/frameworks/reflection/reflected_declarations.h"
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
#include "src/frameworks/serialization/reflected_cbor.h"
namespace mmltk::frameworks::serialization::test {
namespace policy = mmltk::frameworks::reflection;
namespace annotation_predicates {
template <auto Marker>
struct Markers {
 static constexpr auto is_minimum = Marker;
 static constexpr auto is_maximum = Marker;
 static constexpr auto is_finite = Marker;
 static constexpr auto is_min_bytes = Marker;
 static constexpr auto is_max_bytes = Marker;
 static constexpr auto is_max_items = Marker;
};
template <class Value, auto Marker = true>
struct ValueMarkers : Markers<Marker> {
 Value value;
};
struct MissingMarkers {
 int value;
};
struct TypeMarkers {
 using is_minimum = int;
 using is_maximum = int;
 using is_finite = int;
 using is_min_bytes = int;
 using is_max_bytes = int;
 using is_max_items = int;
 int value;
};
struct TypeValue : Markers<true> {
 using value = int;
};
class PrivateValue : public Markers<true> {
 int value;
};
struct RvalueNumber {
 operator long double() &&;
};
struct LvalueNumber {
 operator long double() &;
};
struct ExplicitNumber {
 explicit operator long double() const;
};
struct NumericOnly {
 operator long double() &&;
 operator std::size_t() && = delete;
};
struct SizeOnly {
 operator long double() && = delete;
 operator std::size_t() &&;
};
template <class Annotation, bool Numeric, bool Size>
consteval bool value_predicates_match() {
 return policy::MinimumAnnotation<Annotation> == Numeric && policy::MaximumAnnotation<Annotation> == Numeric && policy::MinBytesAnnotation<Annotation> == Size &&
        policy::MaxBytesAnnotation<Annotation> == Size && policy::MaxItemsAnnotation<Annotation> == Size;
}
static_assert(value_predicates_match<MissingMarkers, false, false>());
static_assert(value_predicates_match<Markers<true>, false, false>());
static_assert(value_predicates_match<TypeMarkers, false, false>());
static_assert(value_predicates_match<TypeValue, false, false>());
static_assert(value_predicates_match<PrivateValue, false, false>());
static_assert(value_predicates_match<ValueMarkers<int, false>, false, false>());
static_assert(value_predicates_match<ValueMarkers<int, 2>, true, true>());
static_assert(value_predicates_match<ValueMarkers<const int>, true, true>());
static_assert(value_predicates_match<ValueMarkers<ExplicitNumber>, false, false>());
static_assert(value_predicates_match<ValueMarkers<void*>, false, false>());
// Unparenthesized decltype names the declared type, even through const Annotation&.
static_assert(value_predicates_match<ValueMarkers<RvalueNumber>, true, true>());
static_assert(value_predicates_match<ValueMarkers<const RvalueNumber>, false, false>());
static_assert(value_predicates_match<ValueMarkers<RvalueNumber&>, false, false>());
static_assert(value_predicates_match<ValueMarkers<LvalueNumber>, false, false>());
static_assert(value_predicates_match<ValueMarkers<LvalueNumber&>, true, true>());
static_assert(value_predicates_match<ValueMarkers<NumericOnly>, true, false>());
static_assert(value_predicates_match<ValueMarkers<SizeOnly>, false, true>());
static_assert(policy::FiniteAnnotation<Markers<true>>);
static_assert(policy::FiniteAnnotation<Markers<1>>);
static_assert(!policy::FiniteAnnotation<Markers<false>>);
static_assert(!policy::FiniteAnnotation<Markers<0>>);
static_assert(!policy::FiniteAnnotation<MissingMarkers>);
static_assert(!policy::FiniteAnnotation<TypeMarkers>);
// Present markers that cannot form the original bool_constant remain hard errors.
// In particular, Finite's direct bool argument rejects 2; value markers use &&.
// Such hard errors cannot be instantiated in this positive compilation fixture.
template <class Kind>
struct KindMarker {
 static Kind kind;
};
struct RvalueKind {
 operator policy::PresentationKind() &&;
};
struct ExplicitKind {
 explicit operator policy::PresentationKind() const;
};
struct TypeKind {
 using kind = policy::PresentationKind;
};
struct NonstaticKind {
 policy::PresentationKind kind;
};
static_assert(policy::PresentationAnnotation<KindMarker<policy::PresentationKind>>);
static_assert(policy::PresentationAnnotation<KindMarker<RvalueKind>>);
static_assert(policy::PresentationAnnotation<NonstaticKind>);
static_assert(!policy::PresentationAnnotation<KindMarker<const RvalueKind>>);
static_assert(!policy::PresentationAnnotation<KindMarker<RvalueKind&>>);
static_assert(!policy::PresentationAnnotation<KindMarker<ExplicitKind>>);
static_assert(!policy::PresentationAnnotation<KindMarker<int>>);
static_assert(!policy::PresentationAnnotation<TypeKind>);
static_assert(!policy::PresentationAnnotation<MissingMarkers>);
static_assert(!policy::kPolicyAnnotation<policy::RuntimeDestination>);
static_assert(!policy::kPolicyAnnotation<policy::Annotation>);
static_assert(policy::kPolicyAnnotation<const policy::Minimum<int>&>);
}  // namespace annotation_predicates
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
 MMLTK_MAXIMUM(int, 1) MMLTK_MAXIMUM(int, 2) int duplicate_maximum = 0;
 MMLTK_FINITE MMLTK_FINITE double duplicate_finite = 0.0;
 MMLTK_MIN_BYTES(1U) MMLTK_MIN_BYTES(2U) std::string duplicate_min_bytes;
 MMLTK_MAX_BYTES(1U) MMLTK_MAX_BYTES(2U) std::string duplicate_max_bytes;
 MMLTK_MAX_ITEMS(1U) MMLTK_MAX_ITEMS(2U) std::vector<int> duplicate_items;
 MMLTK_PRESENTATION(policy::PresentationKind::Path) MMLTK_PRESENTATION(policy::PresentationKind::Norm) int duplicate_presentation = 0;
 MMLTK_CATALOG(BoundBase) MMLTK_CATALOG(BoundBase) int duplicate_catalog = 0;
 MMLTK_MINIMUM(int, 2) MMLTK_MAXIMUM(int, 1) int reversed_number = 0;
 MMLTK_MINIMUM(int, 0) bool boolean_number = false;
 MMLTK_MAX_BYTES(2U) int scalar_bytes = 0;
 MMLTK_MIN_BYTES(1U) MMLTK_MAX_BYTES(2U) std::vector<std::string> sequence_minimum;
 MMLTK_MAX_ITEMS(3U) std::inplace_vector<int, 2U> capacity;
};
struct DeclarationPresentation final {
 MMLTK_PRESENTATION(policy::PresentationKind::Norm) int value = 0;
};
MMLTK_REFLECT_FIELDS(DeclarationBounds)
MMLTK_REFLECT_FIELDS(InvalidDeclarationPolicies)
MMLTK_REFLECT_FIELDS(DeclarationPresentation)
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
  } else if constexpr (std::is_same_v<Member, std::string>) {
   if constexpr (Declaration::pointer == &DeclarationBounds::text) {
    result = result && std::is_same_v<Declaration, policy::MaterializedMemberDeclaration<&DeclarationBounds::text, policy::MinBytes{1U}, policy::MaxBytes{4U}>>;
   }
  }
 });
 return result;
}();
static_assert(kDeclarationAnnotationTypesMatch);
static_assert(!policy::materialized_policy_annotations_are_valid<&InvalidDeclarationPolicies::duplicate>());
static_assert(!policy::materialized_policy_annotations_are_valid<&InvalidDeclarationPolicies::finite_integer>());
static_assert(!policy::materialized_policy_annotations_are_valid<&InvalidDeclarationPolicies::reversed>());
static_assert(!policy::materialized_policy_annotations_are_valid<&InvalidDeclarationPolicies::scalar_items>());
static_assert([] consteval {
 bool all_invalid = true;
 policy::visit_materialized_members<InvalidDeclarationPolicies>([&]<class>(const auto& fact) { all_invalid = all_invalid && !fact.annotations_valid; });
 return all_invalid;
}());
static_assert(policy::reflected_policies_are_valid<DeclarationBounds>());
static_assert(policy::policy_of_member<&DeclarationBounds::fraction>() == policy::FieldConstraint{.finite = true, .has_minimum = true, .has_maximum = true, .minimum = 0.0L, .maximum = 1.0L});
static_assert(policy::policy_of_member<&DeclarationBounds::text>() == policy::FieldConstraint{.minimum_bytes = 1U, .maximum_bytes = 4U});
static_assert(policy::presentation_of_member<&DeclarationPresentation::value>() == policy::PresentationKind::Norm);
static_assert(policy::presentation_of_member<&DeclarationBounds::fraction>() == policy::PresentationKind::Default);
static_assert([] consteval {
 bool exact = false;
 policy::visit_materialized_members<DeclarationPresentation>([&]<class Declaration>(const auto&) {
  exact = std::is_same_v<Declaration, policy::MaterializedMemberDeclaration<&DeclarationPresentation::value, policy::Presentation<policy::PresentationKind::Norm>{}>>;
 });
 return exact;
}());
using OpaqueNumericPolicy = policy::MaterializedOpaqueMemberDeclaration<double, policy::Minimum<double>{0.0}, policy::Maximum<double>{1.0}, policy::Finite{}>;
static_assert(OpaqueNumericPolicy::annotations_valid);
static_assert(OpaqueNumericPolicy::constraint == policy::FieldConstraint{.finite = true, .has_minimum = true, .has_maximum = true, .minimum = 0.0L, .maximum = 1.0L});
static_assert(!policy::MaterializedOpaqueMemberDeclaration<int, policy::Minimum<int>{0}, policy::Minimum<int>{1}>::annotations_valid);
static_assert(!policy::MaterializedOpaqueMemberDeclaration<int, policy::Maximum<int>{1}, policy::Maximum<int>{2}>::annotations_valid);
static_assert(!policy::MaterializedOpaqueMemberDeclaration<double, policy::Finite{}, policy::Finite{}>::annotations_valid);
static_assert(!policy::MaterializedOpaqueMemberDeclaration<int, policy::Finite{}>::annotations_valid);
static_assert(!policy::MaterializedOpaqueMemberDeclaration<bool, policy::Minimum<int>{0}>::annotations_valid);
static_assert(!policy::MaterializedOpaqueMemberDeclaration<int, policy::Minimum<int>{2}, policy::Maximum<int>{1}>::annotations_valid);
// Opaque declaration validation intentionally checks only numeric categories.
static_assert(policy::MaterializedOpaqueMemberDeclaration<int, policy::MaxBytes{1U}, policy::MaxBytes{2U}>::annotations_valid);
static_assert(policy::MaterializedOpaqueMemberDeclaration<int, policy::Presentation<policy::PresentationKind::Path>{}, policy::Presentation<policy::PresentationKind::Norm>{}>::annotations_valid);
static_assert(policy::merge(policy::FieldConstraint{.has_minimum = true, .minimum = 1.0L, .maximum_bytes = 4U},
               policy::FieldConstraint{.finite = true, .has_minimum = true, .minimum = 2.0L, .maximum_items = 3U}) ==
              policy::FieldConstraint{.finite = true, .has_minimum = true, .minimum = 2.0L, .maximum_bytes = 4U, .maximum_items = 3U});
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
