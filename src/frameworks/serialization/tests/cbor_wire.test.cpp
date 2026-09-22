#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include "mmltk/frameworks/reflection/materializer.h"
#include "mmltk/frameworks/reflection/member_relation.h"
#include "src/frameworks/serialization/serialization.h"
#include "src/frameworks/serialization/reflected_json.h"
#include "src/frameworks/serialization/tests/reflected_cbor_test_types.h"
#include "src/test_support/utf8_test_data.h"
namespace mmltk::frameworks::serialization::test {
// CLEANUP-IGNORE: This isolated serialization fixture mirrors an opaque private shape without depending on RF-DETR.
struct OpaqueFixtureProvider;
class[[= mmltk::frameworks::reflection::OpaqueRelationStorage{}]] OpaqueFixture final {
public:
 constexpr OpaqueFixture() noexcept = default;
 constexpr bool operator==(const OpaqueFixture&) const noexcept = default;

private:
 [[= mmltk::frameworks::reflection::Maximum<std::uint16_t>{std::uint16_t{0x07ffU}}]] std::uint16_t mask = 0U;
 friend struct mmltk::frameworks::reflection::catalog_provider_relation<OpaqueFixtureProvider>;
};
struct OpaqueFixtureRow {
 std::uint16_t value = 0U;
};
struct OpaqueFixtureOwner {
 std::uint16_t value = 0U;
 OpaqueFixture overrides;
 bool operator==(const OpaqueFixtureOwner&) const = default;
};
struct OpaqueProjectedEnvelope {
 OpaqueFixtureOwner owner;
 [[= mmltk::frameworks::reflection::MaxBytes{128U}]][[= mmltk::frameworks::reflection::MaxItems{16U}]] mmltk::frameworks::serialization::wire::FlatValue payload;
};
MMLTK_REFLECT_FIELDS(OpaqueFixture)
MMLTK_REFLECT_FIELDS(OpaqueFixtureRow)
MMLTK_REFLECT_FIELDS(OpaqueFixtureOwner)
MMLTK_REFLECT_FIELDS(OpaqueProjectedEnvelope)
}  // namespace mmltk::frameworks::serialization::test
namespace mmltk::frameworks::reflection {
template <>
struct catalog_provider_relation<mmltk::frameworks::serialization::test::OpaqueFixtureProvider>
    : StaticMemberRelation<mmltk::frameworks::serialization::test::OpaqueFixtureRow, mmltk::frameworks::serialization::test::OpaqueFixtureOwner, 1U,
       MemberRelationEntry<member_path<&mmltk::frameworks::serialization::test::OpaqueFixtureRow::value>, member_path<&mmltk::frameworks::serialization::test::OpaqueFixtureOwner::value>>> {
 using override_state_type = mmltk::frameworks::serialization::test::OpaqueFixture;
 inline static constexpr auto destination_override_state = member_path<&mmltk::frameworks::serialization::test::OpaqueFixtureOwner::overrides>;
 static constexpr void set(override_state_type& state) noexcept { state.mask = 1U; }
};
}  // namespace mmltk::frameworks::reflection
namespace {
namespace wire = mmltk::frameworks::serialization::wire;
static_assert(!std::constructible_from<wire::FlatValue, std::string>);
static_assert(!std::constructible_from<wire::FlatValue, wire::ByteBuffer>);
constexpr wire::Limits test_limits(const std::size_t max_bytes, const std::size_t max_items = 256U, const std::size_t max_depth = wire::kMaximumNestingDepth) {
 return {.max_bytes = max_bytes, .max_items = max_items, .max_depth = max_depth};
}
template <class Result>
void require_decode_error(const Result& result, const wire::ErrorCode code) {
 REQUIRE_FALSE(result.has_value());
 CHECK(result.error().code == code);
}
template <class Check>
void check_separate_splits(const wire::ByteView encoded, Check check) {
 for (std::size_t split = 0U; split <= encoded.size(); ++split) {
  CAPTURE(split);
  const auto prefix = encoded.first(split);
  const auto suffix = encoded.subspan(split);
  const wire::ByteBuffer first(prefix.begin(), prefix.end());
  const wire::ByteBuffer second(suffix.begin(), suffix.end());
  check(wire::ByteSegments{first, second});
 }
}
template <std::size_t Size>
void require_malformed_scalar(const std::array<std::byte, Size>& bytes, const wire::ErrorCode code) {
 const auto limits = test_limits(Size, Size, 4U);
 require_decode_error(wire::decode({bytes, {}}, limits), code);
 require_decode_error(wire::Reader({bytes, {}}, limits).read_flat(), code);
 require_decode_error(wire::validate_raw_item({bytes, {}}, limits), code);
 std::array<std::uint32_t, Size> scratch{};
 require_decode_error(wire::validate_raw_item_structural(bytes, limits, {.key_offsets = scratch}), code);
}
template <class Value>
wire::ByteBuffer require_reflected_round_trip(const Value& source) {
 wire::ByteBuffer encoded;
 REQUIRE(mmltk::frameworks::serialization::encode(source, encoded, test_limits(128U)).has_value());
 const auto decoded = mmltk::frameworks::serialization::decode<Value>({encoded, {}}, test_limits(128U));
 REQUIRE(decoded.has_value());
 CHECK(*decoded == source);
 return encoded;
}
[[nodiscard]] consteval bool opaque_fixture_descriptor_is_sealed() {
 bool safe = true;
 std::size_t count = 0U;
 mmltk::frameworks::reflection::visit_materialized_members<mmltk::frameworks::serialization::test::OpaqueFixture>([&]<class Declaration>(const auto& policy) {
  ++count;
  safe = safe && !requires { Declaration::pointer; } && std::same_as<typename Declaration::member_type, std::uint16_t> && policy.member_name == "mask" && policy.constraint.has_maximum &&
         policy.constraint.maximum == 2047.0;
 });
 return safe && count == 1U;
}
static_assert(opaque_fixture_descriptor_is_sealed());
struct TestCustomMaterializer final {
 template <class Owner, class Reflection>
 [[nodiscard]] consteval std::size_t operator()() const {
  return Reflection::members.size();
 }
};
template <class Owner>
concept PubliclyCustomMaterializable = requires { mmltk::frameworks::reflection::materialize<Owner>(TestCustomMaterializer{}); };
template <class Owner>
concept PublicRawMaterializationInput = requires {
 mmltk::frameworks::reflection::detail::FieldMaterializationInput<Owner>::members;
 mmltk::frameworks::reflection::detail::FieldMaterializationInput<Owner>::bases;
 mmltk::frameworks::reflection::detail::AllMemberMaterializationInput<Owner>::members;
};
template <class Owner>
concept PublicOpaqueRawMaterializationInput = requires {
 mmltk::frameworks::reflection::detail::OpaqueFieldMaterializationInput<Owner>::members;
 mmltk::frameworks::reflection::detail::OpaqueFieldMaterializationInput<Owner>::bases;
};
static_assert(PubliclyCustomMaterializable<mmltk::frameworks::serialization::test::OpaqueFixtureOwner>);
static_assert(!PubliclyCustomMaterializable<mmltk::frameworks::serialization::test::OpaqueFixture>);
static_assert(PublicRawMaterializationInput<mmltk::frameworks::serialization::test::OpaqueFixtureOwner>);
static_assert(!PublicRawMaterializationInput<mmltk::frameworks::serialization::test::OpaqueFixture>);
static_assert(!PublicOpaqueRawMaterializationInput<mmltk::frameworks::serialization::test::OpaqueFixture>);
static_assert(std::is_final_v<mmltk::frameworks::reflection::detail::FieldMaterializationInput<mmltk::frameworks::serialization::test::OpaqueFixtureOwner>>);
static_assert(std::is_final_v<mmltk::frameworks::reflection::detail::AllMemberMaterializationInput<mmltk::frameworks::serialization::test::OpaqueFixtureOwner>>);
static_assert(std::is_final_v<mmltk::frameworks::reflection::detail::OpaqueFieldMaterializationInput<mmltk::frameworks::serialization::test::OpaqueFixture>>);
static_assert(mmltk::frameworks::reflection::opaque_relation_storage_shape_is_valid<mmltk::frameworks::serialization::test::OpaqueFixture>(0x07ffU));
static_assert(!mmltk::frameworks::reflection::opaque_relation_storage_shape_is_valid<mmltk::frameworks::serialization::test::OpaqueFixture>(0x0800U));
using CborObjectLayout = mmltk::frameworks::serialization::implementation::detail::ObjectLayout;
template <class Owner, CborObjectLayout Layout>
concept PublicRawCborAppend = requires(const Owner& source, std::conditional_t<Layout == CborObjectLayout::Named, wire::Value::Object, wire::Value::Array>& object,
 std::optional<wire::EncodeError>& failure) { mmltk::frameworks::serialization::implementation::detail::append_reflected_object_fields<Layout>(source, object, failure); };
template <class Owner>
concept PublicRawCborValueDecode = requires(Owner& destination, std::span<const mmltk::frameworks::serialization::implementation::detail::NamedFieldMatch> matches, std::size_t& index,
 std::optional<wire::DecodeError>& failure) { mmltk::frameworks::serialization::implementation::detail::decode_reflected_object_fields(destination, matches, index, failure); };
template <class Owner>
concept PublicRawCborProjectedDecode =
 requires(Owner& destination, wire::Reader& reader) { mmltk::frameworks::serialization::implementation::detail::decode_projected_member(destination, reader, 0U, std::string_view{}); };
template <class Owner>
concept PublicRawCborFixedEncode =
 requires(const Owner& source, mmltk::frameworks::serialization::FixedCborEncoder& writer) { mmltk::frameworks::serialization::implementation::detail::encode_fixed_object_fields(writer, source); };
using OpaqueFixture = mmltk::frameworks::serialization::test::OpaqueFixture;
using OpaqueFixtureOwner = mmltk::frameworks::serialization::test::OpaqueFixtureOwner;
static_assert(PublicRawCborAppend<OpaqueFixtureOwner, CborObjectLayout::Named>);
static_assert(PublicRawCborAppend<OpaqueFixtureOwner, CborObjectLayout::Positional>);
static_assert(PublicRawCborValueDecode<OpaqueFixtureOwner>);
static_assert(PublicRawCborProjectedDecode<OpaqueFixtureOwner>);
static_assert(PublicRawCborFixedEncode<OpaqueFixtureOwner>);
static_assert(!PublicRawCborAppend<OpaqueFixture, CborObjectLayout::Named>);
static_assert(!PublicRawCborAppend<OpaqueFixture, CborObjectLayout::Positional>);
static_assert(!PublicRawCborValueDecode<OpaqueFixture>);
static_assert(!PublicRawCborProjectedDecode<OpaqueFixture>);
static_assert(!PublicRawCborFixedEncode<OpaqueFixture>);
[[nodiscard]] std::size_t flat_item_count(const wire::FlatValue& value) {
 std::size_t result = 0U;
 value.visit([&]<class Leaf>(const Leaf& leaf) {
  using Type = std::remove_cvref_t<Leaf>;
  if constexpr (requires(const Type& values) {
                 values.begin();
                 values.end();
                } && !std::same_as<Type, std::string> && !std::same_as<Type, wire::ByteBuffer>) {
   result = leaf.size();
  }
 });
 return result;
}
struct InheritedCborBase {
 [[= mmltk::frameworks::reflection::Minimum<std::int32_t>{1}]][[= mmltk::frameworks::reflection::Maximum<std::int32_t>{9}]] std::int32_t inherited_limit = 4;
 bool operator==(const InheritedCborBase&) const = default;
};
struct InheritedCbor final : InheritedCborBase {
 std::uint32_t derived_count = 2U;
 [[= mmltk::frameworks::reflection::Maximum<std::uint16_t>{9U}]] std::optional<std::uint16_t> optional_count;
 bool operator==(const InheritedCbor&) const = default;
};
struct DuplicateCborBase {
 std::int32_t duplicate = 1;
};
struct DuplicateCbor final : DuplicateCborBase {
 std::int32_t duplicate = 2;
};
MMLTK_REFLECT_FIELDS(InheritedCborBase)
MMLTK_REFLECT_FIELDS(InheritedCbor)
MMLTK_REFLECT_FIELDS(DuplicateCborBase)
MMLTK_REFLECT_FIELDS(DuplicateCbor)
struct ProjectedKeyEnvelope {
 InheritedCbor nested;
 std::variant<InheritedCbor> choice;
 [[= mmltk::frameworks::reflection::MaxBytes{8U}]][[= mmltk::frameworks::reflection::MaxItems{8U}]] wire::FlatValue payload;
};
MMLTK_REFLECT_FIELDS(ProjectedKeyEnvelope)
TEST_CASE("projected CBOR reports nested required members and expected variant keys", "[frameworks][serialization][reflection]") {
 namespace cbor = mmltk::frameworks::serialization;
 const auto decode_object = [](wire::Value::Object object) {
  wire::ByteBuffer encoded;
  REQUIRE(wire::encode(wire::Value(std::move(object)), encoded, test_limits(256U)));
  return cbor::decode<ProjectedKeyEnvelope>({encoded, {}}, test_limits(256U));
 };
 const auto missing = decode_object({{"nested", wire::Value(wire::Value::Object{})}});
 require_decode_error(missing, wire::ErrorCode::UnknownKey);
 CHECK(missing.error().path == "nested.inherited_limit");
 CHECK(missing.error().offset == 0U);
 const auto variant = cbor::reflected_value(std::variant<InheritedCbor>{InheritedCbor{}});
 REQUIRE(variant);
 for (const std::size_t key_index : {0U, 1U}) {
  auto fields = std::get<wire::Value::Object>(variant->storage);
  fields[key_index].first = "wrong";
  const auto wrong = decode_object({{"choice", wire::Value(std::move(fields))}});
  require_decode_error(wrong, wire::ErrorCode::TypeMismatch);
  CHECK(wrong.error().path == (key_index == 0U ? "choice.kind" : "choice.payload"));
  CHECK(wrong.error().offset == 0U);
 }
}
TEST_CASE("reflected CBOR flattens inherited members base first and enforces the complete object contract", "[frameworks][serialization][reflection][inheritance]") {
 InheritedCbor source;
 source.inherited_limit = 7;
 source.derived_count = 5U;
 const auto reflected = mmltk::frameworks::serialization::reflected_value(source);
 REQUIRE(reflected.has_value());
 const auto& object = std::get<wire::Value::Object>(reflected->storage);
 REQUIRE(object.size() == 2U);
 CHECK(object[0].first == "inherited_limit");
 CHECK(object[1].first == "derived_count");
 require_reflected_round_trip(source);
 const auto decode_wire_object = [](const wire::Value& value) {
  wire::ByteBuffer object_bytes;
  REQUIRE(wire::encode(value, object_bytes, test_limits(128U)).has_value());
  return mmltk::frameworks::serialization::decode<InheritedCbor>({object_bytes, {}}, test_limits(128U));
 };
 const wire::Value missing_inherited(wire::Value::Object{{"derived_count", wire::Value(std::uint64_t{5U})}});
 const auto missing = decode_wire_object(missing_inherited);
 REQUIRE_FALSE(missing.has_value());
 CHECK(missing.error().code == wire::ErrorCode::UnknownKey);
 CHECK(missing.error().path == "inherited_limit");
 const wire::Value unknown_member(wire::Value::Object{
  {"inherited_limit", wire::Value(std::int64_t{7})},
  {"derived_count", wire::Value(std::uint64_t{5U})},
  {"unknown", wire::Value(true)},
 });
 const auto unknown = decode_wire_object(unknown_member);
 REQUIRE_FALSE(unknown.has_value());
 CHECK(unknown.error().code == wire::ErrorCode::UnknownKey);
 CHECK(unknown.error().path == "unknown");
 const wire::Value invalid_inherited(wire::Value::Object{
  {"inherited_limit", wire::Value(std::int64_t{10})},
  {"derived_count", wire::Value(std::uint64_t{5U})},
 });
 const auto invalid = decode_wire_object(invalid_inherited);
 REQUIRE_FALSE(invalid.has_value());
 CHECK(invalid.error().code == wire::ErrorCode::LimitExceeded);
 CHECK(invalid.error().path == "inherited_limit");
 // The owned path must enforce the same contract without wire-reader guards.
 const auto check_owned = [&](wire::Value::Object fields, const bool valid, const std::optional<std::uint16_t> optional = {}) {
  InheritedCbor destination = source;
  destination.derived_count = 91U;
  destination.optional_count = 8U;
  const auto before = destination;
  const wire::Value value(std::move(fields));
  const auto result = mmltk::frameworks::serialization::decode_into(destination, value);
  REQUIRE(result.has_value() == valid);
  auto expected = source;
  expected.optional_count = optional;
  CHECK(destination == (valid ? expected : before));
  if (valid) {
   const auto decoded = decode_wire_object(value);
   REQUIRE(decoded.has_value());
   CHECK(*decoded == expected);
  }
 };
 const wire::Value::Object reverse{
  {"derived_count", wire::Value(std::uint64_t{5U})},
  {"inherited_limit", wire::Value(std::int64_t{7})},
 };
 check_owned(reverse, true);  // Omitted optional clears a previously populated destination.
 auto explicit_null = reverse;
 explicit_null.insert(explicit_null.begin(), {"optional_count", wire::Value{}});
 check_owned(std::move(explicit_null), true);
 auto optional_present = reverse;
 optional_present.emplace_back("optional_count", wire::Value(std::uint64_t{6U}));
 check_owned(optional_present, true, 6U);
 optional_present.back().second = wire::Value(std::uint64_t{10U});
 check_owned(std::move(optional_present), false);
 for (const auto& malformed : {missing_inherited, unknown_member, invalid_inherited}) { check_owned(std::get<wire::Value::Object>(malformed.storage), false); }
 auto duplicate = reverse;
 duplicate.emplace_back("inherited_limit", wire::Value(std::int64_t{7}));
 check_owned(std::move(duplicate), false);
 auto required_null = reverse;
 required_null[1].second = wire::Value{};
 check_owned(std::move(required_null), false);
 InheritedCbor invalid_source;
 invalid_source.inherited_limit = 0;
 invalid_source.derived_count = 5U;
 wire::ByteBuffer rejected_bytes;
 const auto rejected_encode = mmltk::frameworks::serialization::encode(invalid_source, rejected_bytes, test_limits(128U));
 REQUIRE_FALSE(rejected_encode.has_value());
 CHECK(rejected_encode.error().code == wire::ErrorCode::LimitExceeded);
 CHECK(rejected_encode.error().path == "inherited_limit");
 CHECK(rejected_bytes.empty());
}
TEST_CASE("named object validation retains declaration order across shuffled and conflicting fields", "[frameworks][serialization][reflection]") {
 namespace cbor = mmltk::frameworks::serialization;
 const auto check = [](wire::Value::Object fields, wire::ErrorCode code, std::string_view path) {
  InheritedCbor destination;
  destination.derived_count = 91U;
  destination.optional_count = 8U;
  const auto before = destination;
  const wire::Value input(std::move(fields));
  const auto input_before = input;
  const auto result = cbor::decode_into(destination, input);
  require_decode_error(result, code);
  CHECK(result.error().path == path);
  CHECK(result.error().offset == 0U);
  CHECK(destination == before);
  CHECK(input == input_before);
 };
 const wire::Value::Object valid{{"optional_count", wire::Value(std::uint64_t{6})}, {"derived_count", wire::Value(std::uint64_t{5})}, {"inherited_limit", wire::Value(std::uint64_t{7})}};
 for (const bool duplicate_first : {false, true}) {
  auto fields = valid;
  fields.insert(duplicate_first ? fields.begin() : fields.end(), {"derived_count", wire::Value{}});
  if (duplicate_first) {
   fields.back().second = wire::Value(std::uint64_t{10});
   // An invalid earlier base still wins over an earlier input duplicate.
   check(fields, wire::ErrorCode::LimitExceeded, "inherited_limit");
   fields.back().second = wire::Value(std::uint64_t{7});
  }
  check(fields, wire::ErrorCode::DuplicateKey, "derived_count");
  fields.erase(std::ranges::find_if(fields, [](const auto& field) { return field.first == "inherited_limit"; }));
  check(fields, wire::ErrorCode::UnknownKey, "inherited_limit");
 }
 auto fields = valid;
 fields.insert(fields.begin(), {"unknown_first", wire::Value{}});
 fields.emplace_back("unknown_last", wire::Value{});
 check(fields, wire::ErrorCode::UnknownKey, "unknown_first");
 fields[1].second = wire::Value(std::string("malformed optional"));
 check(fields, wire::ErrorCode::TypeMismatch, "optional_count");
 fields.emplace_back("optional_count", wire::Value{});
 check(fields, wire::ErrorCode::DuplicateKey, "optional_count");
 fields.emplace_back("inherited_limit", wire::Value{});
 check(fields, wire::ErrorCode::DuplicateKey, "inherited_limit");
}
TEST_CASE("opaque named values preserve ordered admission and nested error precedence", "[frameworks][serialization][opaque]") {
 namespace cbor = mmltk::frameworks::serialization;
 for (const bool known_first : {false, true}) {
  wire::Value::Object fields{{"unknown", wire::Value{}}, {"mask", wire::Value(std::uint64_t{1})}};
  if (known_first) std::ranges::reverse(fields);
  OpaqueFixtureOwner destination;
  destination.value = 73U;
  const auto before = destination;
  const wire::Value input(wire::Value::Object{{"overrides", wire::Value(std::move(fields))}, {"value", wire::Value(std::uint64_t{19})}});
  const auto result = cbor::decode_into(destination, input);
  require_decode_error(result, wire::ErrorCode::UnknownKey);
  CHECK(result.error().path == (known_first ? "overrides.unknown" : "overrides.mask"));
  CHECK(result.error().offset == 0U);
  CHECK(destination == before);
 }
}
TEST_CASE("reflected structural bounds retain inherited fields and declaration owned bytes", "[frameworks][serialization][reflection][bounds]") {
 namespace cbor = mmltk::frameworks::serialization;
 using namespace cbor::test;
 STATIC_REQUIRE(kLeafMembers == 4U);
 // One map head, inherited uint16, 24-byte text, three fixed bytes,
 // and the optional payload's null alternative. Dynamic payload bytes
 // (including their own heads) belong to the caller's aggregate budget.
 STATIC_REQUIRE(kLeafStructuralBytes == 57U);
 STATIC_REQUIRE(kLeafFullBytes == 257U);
 STATIC_REQUIRE(kFlatFullBytes == 25U);
 STATIC_REQUIRE(kFlatStructuralBytes == kFlatFullBytes);
 STATIC_REQUIRE(cbor::reflected_maximum_cbor_bytes<std::array<std::byte, 24U>>() == 26U);
 STATIC_REQUIRE(cbor::reflected_structural_cbor_bytes<wire::Value>(123U) == 123U);
 BoundLeaf leaf;
 const auto verify = [&leaf](const std::size_t payload_budget) {
  wire::ByteBuffer encoded;
  REQUIRE(cbor::encode(leaf, encoded, test_limits(kLeafFullBytes)));
  const auto measured = cbor::measure(leaf, test_limits(kLeafFullBytes));
  REQUIRE(measured);
  CHECK(*measured == encoded.size());
  CHECK(*measured <= kLeafFullBytes);
  CHECK(*measured <= cbor::reflected_structural_cbor_bytes<BoundLeaf>(payload_budget));
 };
 verify(0U);
 leaf.id = std::numeric_limits<std::uint16_t>::max();
 leaf.text.assign(24U, 'x');
 leaf.bytes.fill(std::byte{0xff});
 verify(0U);
 leaf.payload = wire::Value{};
 verify(1U);
 leaf.payload = wire::Value(wire::Value::Array{});
 verify(1U);
 leaf.payload = wire::Value(std::numeric_limits<std::uint64_t>::max());
 verify(9U);
 // MaxItems=1 keeps the unchanged full-mode dynamic bound linear:
 // each level adds one map head and a one-byte key, ending in uint64.
 for (std::size_t depth = 0U; depth < wire::kMaximumNestingDepth; ++depth) {
  if (depth + 1U == wire::kMaximumNestingDepth) verify(9U + depth * 3U);
  leaf.payload = wire::Value(wire::Value::Object{{"x", std::move(*leaf.payload)}});
 }
 const auto payload_size = cbor::measure(*leaf.payload, test_limits(kLeafFullBytes));
 REQUIRE(payload_size);
 CHECK(*payload_size == 201U);
 // The standalone payload reaches the wire nesting ceiling. The enclosing
 // reflected object consumes another level and must still reject it.
 wire::ByteBuffer rejected;
 CHECK_FALSE(cbor::encode(leaf, rejected, test_limits(kLeafFullBytes)));
 leaf.payload.reset();
 leaf.text.push_back('x');
 CHECK_FALSE(cbor::encode(leaf, rejected, test_limits(kLeafFullBytes)));
 BoundFlat flat;
 for (const wire::Value& source :
  {wire::Value{}, wire::Value(wire::Value::Array{}), wire::Value(wire::Value::Array{wire::Value(std::numeric_limits<std::uint64_t>::max()), wire::Value(std::numeric_limits<std::uint64_t>::max())})}) {
  auto projected = wire::FlatValue::from_value(source, {.max_bytes = 4U, .max_items = 2U, .max_depth = 1U});
  REQUIRE(projected);
  flat.flat = std::move(*projected);
  wire::ByteBuffer encoded;
  REQUIRE(cbor::encode(flat, encoded, test_limits(kFlatFullBytes)));
  const auto measured = cbor::measure(flat, test_limits(kFlatFullBytes));
  REQUIRE(measured);
  CHECK(*measured == encoded.size());
  CHECK(*measured <= kFlatStructuralBytes);
 }
}
TEST_CASE("structural accounting propagates through optional containers and variant envelopes", "[frameworks][serialization][reflection][bounds]") {
 namespace cbor = mmltk::frameworks::serialization;
 using namespace cbor::test;
 STATIC_REQUIRE(kContainersStructuralBytes == 495U);
 STATIC_REQUIRE(kVariantStructuralBytes == 525U);
 BoundLeaf leaf;
 leaf.id = std::numeric_limits<std::uint16_t>::max();
 leaf.text.assign(24U, 'x');
 leaf.payload = wire::Value(std::numeric_limits<std::uint64_t>::max());
 const std::array<BoundLeaf, 2U> borrowed{leaf, leaf};
 BoundContainers containers;
 const auto verify = [&containers](const std::size_t payload_budget) {
  const BoundVariant source = containers;
  const auto bound = cbor::reflected_structural_cbor_bytes<BoundVariant>(payload_budget);
  wire::ByteBuffer encoded;
  REQUIRE(cbor::encode(source, encoded, test_limits(bound)));
  const auto measured = cbor::measure(source, test_limits(bound));
  REQUIRE(measured);
  CHECK(*measured == encoded.size());
  CHECK(*measured <= bound);
 };
 verify(2U);  // Two null wire values; optional reflected values stay structural.
 containers.rows.emplace();
 verify(2U);
 containers.rows->assign(2U, leaf);
 containers.fixed = {leaf, leaf};
 containers.local.push_back(leaf);
 containers.local.push_back(leaf);
 containers.borrowed = borrowed;
 containers.values = {wire::Value(wire::Value::Array{}), wire::Value(wire::Value::Object{})};
 verify(8U * 9U + 2U);
 containers.rows->push_back(leaf);
 wire::ByteBuffer rejected;
 CHECK_FALSE(cbor::encode(containers, rejected, test_limits(1024U)));
}
TEST_CASE("schema agreed transport preserves reflected fields and named persistence", "[frameworks][serialization][reflection]") {
 namespace cbor = mmltk::frameworks::serialization;
 using namespace cbor::test;
 BoundLeaf leaf;
 leaf.id = 65535U;
 leaf.text.assign(24U, 'x');
 leaf.bytes.fill(std::byte{0xff});
 const auto named = cbor::reflected_value(leaf);
 const auto positional = cbor::reflected_transport_value(leaf);
 REQUIRE(named);
 REQUIRE(positional);
 const wire::Value expected(wire::Value::Array{wire::Value(std::uint64_t{65535U}), wire::Value(leaf.text), wire::Value(wire::ByteBuffer(3U, std::byte{0xff})), wire::Value{}});
 CHECK(*positional == expected);
 REQUIRE(std::holds_alternative<wire::Value::Object>(named->storage));
 CHECK(std::get<wire::Value::Object>(named->storage).size() == 3U);
 BoundLeaf restored;
 REQUIRE(cbor::decode_into(restored, *named));
 CHECK(restored.id == leaf.id);
 CHECK(restored.text == leaf.text);
 CHECK(restored.bytes == leaf.bytes);
 CHECK_FALSE(restored.payload);
 const std::array<BoundLeaf, 1U> borrowed{leaf};
 BoundContainers containers;
 containers.rows = std::vector<BoundLeaf>{leaf};
 containers.fixed = {leaf, std::nullopt};
 containers.local.push_back(leaf);
 containers.borrowed = borrowed;
 containers.values[0] = wire::Value(wire::Value::Object{{"kept", wire::Value(true)}});
 const auto nested = cbor::reflected_transport_value(containers);
 REQUIRE(nested);
 const auto& fields = std::get<wire::Value::Array>(nested->storage);
 REQUIRE(fields.size() == 5U);
 for (const auto index : {0U, 1U, 2U, 3U}) {
  const auto& rows = std::get<wire::Value::Array>(fields[index].storage);
  REQUIRE_FALSE(rows.empty());
  CHECK(rows.front() == expected);
 }
 CHECK(std::get<wire::Value::Array>(fields.back().storage).front() == containers.values.front());
 const auto variant = cbor::reflected_transport_value(BoundVariant{leaf});
 REQUIRE(variant);
 const auto& envelope = std::get<wire::Value::Object>(variant->storage);
 REQUIRE(envelope.size() == 2U);
 CHECK(envelope[1].second == expected);
 leaf.text.push_back('x');
 CHECK_FALSE(cbor::reflected_transport_value(leaf));
 containers.rows->resize(3U);
 CHECK_FALSE(cbor::reflected_transport_value(containers));
 const auto opaque = cbor::reflected_transport_value(OpaqueFixture{});
 REQUIRE(opaque);
 CHECK(*opaque == *cbor::reflected_value(OpaqueFixture{}));
}
TEST_CASE("reflected aggregate budgets reject arithmetic overflow", "[frameworks][serialization][reflection][bounds]") {
 namespace cbor = mmltk::frameworks::serialization;
 using cbor::test::BoundLeaf;
 constexpr auto maximum = std::numeric_limits<std::size_t>::max();
 constexpr auto last_budget = maximum - cbor::test::kLeafStructuralBytes;
 STATIC_REQUIRE(cbor::reflected_structural_cbor_bytes<BoundLeaf>(last_budget) == maximum);
 CHECK_THROWS(cbor::reflected_structural_cbor_bytes<BoundLeaf>(last_budget + 1U));
 CHECK(cbor::implementation::detail::cbor_size_multiply(0U, maximum) == 0U);
 CHECK(cbor::implementation::detail::cbor_size_multiply(maximum, 1U) == maximum);
 CHECK_THROWS(cbor::implementation::detail::cbor_size_multiply(maximum, 2U));
}
TEST_CASE("opaque relation storage remains sealed while every reflected CBOR facade preserves it", "[frameworks][serialization][reflection][opaque]") {
 using Fixture = mmltk::frameworks::serialization::test::OpaqueFixture;
 using Owner = mmltk::frameworks::serialization::test::OpaqueFixtureOwner;
 using Provider = mmltk::frameworks::serialization::test::OpaqueFixtureProvider;
 using Relation = mmltk::frameworks::reflection::catalog_provider_relation<Provider>;
 Owner source;
 source.value = 19U;
 Relation::set(source.overrides);
 const auto reflected = mmltk::frameworks::serialization::reflected_value(source);
 REQUIRE(reflected.has_value());
 const auto& owner_object = std::get<wire::Value::Object>(reflected->storage);
 REQUIRE(owner_object.size() == 2U);
 const auto& override_object = std::get<wire::Value::Object>(owner_object[1].second.storage);
 REQUIRE(override_object.size() == 1U);
 CHECK(override_object[0].first == "mask");
 CHECK(std::get<std::uint64_t>(override_object[0].second.storage) == 1U);
 const auto regular = require_reflected_round_trip(source);
 Owner decoded_into;
 REQUIRE(mmltk::frameworks::serialization::decode_into(decoded_into, *reflected).has_value());
 CHECK(decoded_into == source);
 auto reverse_owner = owner_object;
 std::ranges::reverse(reverse_owner);
 REQUIRE(mmltk::frameworks::serialization::decode_into(decoded_into, wire::Value(reverse_owner)));
 CHECK(decoded_into == source);
 for (const wire::Value::Object& fields : {
       wire::Value::Object{},
       wire::Value::Object{{"unknown", wire::Value(std::uint64_t{1U})}},
       wire::Value::Object{{"mask", wire::Value{}}},
       wire::Value::Object{{"mask", wire::Value(std::uint64_t{1U})}, {"mask", wire::Value(std::uint64_t{1U})}},
      }) {
  auto malformed = reverse_owner;
  malformed[0].second = wire::Value(fields);
  Owner destination = source;
  destination.value = 71U;
  const auto before = destination;
  const auto result = mmltk::frameworks::serialization::decode_into(destination, wire::Value(std::move(malformed)));
  REQUIRE_FALSE(result.has_value());
  CHECK(destination == before);
 }
 const wire::Value missing_override(wire::Value::Object{
  {"owner", wire::Value(wire::Value::Object{
             {"value", wire::Value(std::uint64_t{19U})},
             {"overrides", wire::Value(wire::Value::Object{})},
            })},
  {"payload", wire::Value(std::uint64_t{7U})},
 });
 wire::ByteBuffer missing_bytes;
 REQUIRE(wire::encode(missing_override, missing_bytes, test_limits(128U)));
 const auto missing_projected = mmltk::frameworks::serialization::decode<mmltk::frameworks::serialization::test::OpaqueProjectedEnvelope>({missing_bytes, {}}, test_limits(128U));
 require_decode_error(missing_projected, wire::ErrorCode::UnknownKey);
 CHECK(missing_projected.error().path == "owner.overrides.mask");
 CHECK(missing_projected.error().offset == 0U);
 std::array<std::byte, 128U> fixed{};
 const auto fixed_size = mmltk::frameworks::serialization::encode(source, fixed, test_limits(fixed.size()));
 REQUIRE(fixed_size.has_value());
 CHECK(std::ranges::equal(std::span(fixed).first(*fixed_size), regular));
 STATIC_REQUIRE(mmltk::frameworks::serialization::reflected_maximum_cbor_bytes<Fixture>() != 0U);
 const wire::Value invalid_owner(wire::Value::Object{
  {"value", wire::Value(std::uint64_t{19U})},
  {"overrides", wire::Value(wire::Value::Object{
                 {"mask", wire::Value(std::uint64_t{0x0800U})},
                })},
 });
 Owner preserved = source;
 preserved.value = 27U;
 const auto before_invalid = preserved;
 const auto invalid_regular = mmltk::frameworks::serialization::decode_into(preserved, invalid_owner);
 REQUIRE_FALSE(invalid_regular.has_value());
 CHECK(invalid_regular.error().code == wire::ErrorCode::LimitExceeded);
 CHECK(invalid_regular.error().path.ends_with("mask"));
 CHECK(preserved == before_invalid);
 const wire::Value projected(wire::Value::Object{
  {"owner", invalid_owner},
  {"payload", wire::Value(std::uint64_t{7U})},
 });
 wire::ByteBuffer projected_bytes;
 REQUIRE(wire::encode(projected, projected_bytes, test_limits(128U)).has_value());
 const auto invalid_projected = mmltk::frameworks::serialization::decode<mmltk::frameworks::serialization::test::OpaqueProjectedEnvelope>({projected_bytes, {}}, test_limits(128U));
 REQUIRE_FALSE(invalid_projected.has_value());
 CHECK(invalid_projected.error().code == wire::ErrorCode::LimitExceeded);
 CHECK(invalid_projected.error().path.ends_with("mask"));
}
TEST_CASE("reflected CBOR audit detects duplicate flattened inherited member names", "[frameworks][serialization][reflection][inheritance]") {
 STATIC_REQUIRE_FALSE(mmltk::frameworks::serialization::implementation::detail::unique_flattened_member_names<DuplicateCbor>());
}
TEST_CASE("bounded_cbor_round_trips_deterministic_maps_and_segmented_input", "[frameworks][serialization]") {
 wire::Value::Object object{{"first", wire::Value(std::uint64_t{23})}, {"bytes", wire::Value(wire::ByteBuffer{std::byte{0}, std::byte{0xff}})}, {"optional", wire::Value()}};
 wire::Value value(std::move(object));
 wire::ByteBuffer encoded;
 const auto result = wire::encode(value, encoded, test_limits(128U));
 REQUIRE(result.has_value());
 const auto measured = wire::CountingEncoder(test_limits(128U)).measure(value);
 REQUIRE(measured.has_value());
 REQUIRE(*measured == encoded.size());
 const std::size_t split = 5U;
 const auto decoded = wire::decode({std::span(encoded).first(split), std::span(encoded).subspan(split)}, {128U, 32U, 8U});
 REQUIRE(decoded.has_value());
 REQUIRE(std::get<wire::Value::Object>(decoded->storage).at(0).first == "first");
}
TEST_CASE("owned scalar payloads survive every segmented split and input retirement", "[frameworks][serialization]") {
 for (const std::size_t length : {0U, 31U, 257U}) {
  const std::string text(length, 'x');
  const wire::ByteBuffer bytes(length, std::byte{0xa5});
  for (const wire::Value& source : {wire::Value(text), wire::Value(bytes), wire::Value(std::string("a\xe2\x82\xac")), wire::Value(wire::Value::Array{}),
        wire::Value(wire::Value::Array{wire::Value(text), wire::Value(bytes), wire::Value(std::int64_t{-17})})}) {
   wire::ByteBuffer encoded;
   REQUIRE(wire::encode(source, encoded, test_limits(2048U)));
   const auto limits = test_limits(encoded.size());
   const auto expected_flat = wire::FlatValue::from_value(source, {.max_bytes = 2048U, .max_items = 256U, .max_depth = 4U});
   REQUIRE(expected_flat);
   for (std::size_t split = 0; split <= encoded.size(); ++split) {
    auto input = encoded;
    const wire::ByteSegments segments{std::span(input).first(split), std::span(input).subspan(split)};
    const auto decoded = wire::decode(segments, limits);
    const auto flat = wire::Reader(segments, limits).read_flat();
    REQUIRE(decoded);
    REQUIRE(flat);
    std::ranges::fill(input, std::byte{});
    CHECK(*decoded == source);
    CHECK(*flat == *expected_flat);
    // Const projections retain their source and return independent ownership.
    const auto projected = wire::FlatValue::from_value(*decoded, {.max_bytes = 2048U, .max_items = 256U, .max_depth = 4U});
    REQUIRE(projected);
    CHECK(*projected == *expected_flat);
    CHECK(*decoded == source);
   }
  }
 }
}
TEST_CASE("borrowed byte payloads alias separate live segments at every split", "[frameworks][serialization]") {
 for (const std::size_t count : {0U, 4U, 24U}) {
  CAPTURE(count);
  wire::ByteBuffer encoded{std::byte{0xf6}, count < 24U ? std::byte(0x40U + count) : std::byte{0x58}};
  if (count == 24U) encoded.push_back(std::byte{24U});
  const auto payload_offset = encoded.size();
  for (std::size_t index = 0U; index < count; ++index) encoded.push_back(std::byte(index * 11U));
  const auto item_end = encoded.size();
  encoded.push_back(std::byte{0xf6});
  check_separate_splits(encoded, [&](const wire::ByteSegments input) {
   struct AllocationProbe {
    mutable std::size_t calls = 0U;
   } probe;
   auto limits = test_limits(encoded.size(), 3U, 0U);
   limits.allocation_policy = {.context = &probe, .allows = [](const void* context, const wire::AllocationRequest&) noexcept {
                                ++static_cast<const AllocationProbe*>(context)->calls;
                                return false;
                               }};
   wire::Reader reader(input, limits);
   REQUIRE(reader.read_scalar_item(0U));
   const auto payload = reader.borrow_bytes_item(0U);
   REQUIRE(payload);
   CHECK(payload->size() == count);
   const auto first_count = input.first.size() > payload_offset ? std::min(count, input.first.size() - payload_offset) : count;
   CHECK(payload->first.size() == first_count);
   CHECK(payload->second.size() == count - first_count);
   const auto expected = std::span(encoded).subspan(payload_offset, count);
   CHECK(std::ranges::equal(payload->first, expected.first(first_count)));
   CHECK(std::ranges::equal(payload->second, expected.subspan(first_count)));
   if (!payload->first.empty()) {
    const auto* const backing = input.first.size() > payload_offset ? input.first.data() + payload_offset : input.second.data() + (payload_offset - input.first.size());
    CHECK(payload->first.data() == backing);
   }
   if (!payload->second.empty()) CHECK(payload->second.data() == input.second.data());
   CHECK(reader.offset() == item_end);
   CHECK(reader.items_read() == 2U);
   REQUIRE(reader.read_scalar_item(0U));
   CHECK(reader.offset() == encoded.size());
   REQUIRE(reader.finish());
   CHECK(probe.calls == 0U);
  });
 }
}
TEST_CASE("segmented text choices consume exactly one complete item on match or unknown key", "[frameworks][serialization]") {
 // Unlike owned text materialization, text choices compare exact protocol bytes.
 for (const std::string& text : {std::string{}, std::string{"ab"}, std::string{"a\xe2\x82\xac"}, std::string{"a\0b", 3U}, std::string{"\xc0\x80", 2U}}) {
  wire::ByteBuffer encoded{std::byte(0x60U + text.size())};
  for (const unsigned char character : text) encoded.push_back(std::byte(character));
  const auto item_end = encoded.size();
  encoded.push_back(std::byte{0xf6});
  const std::array<std::string_view, 3U> choices{"other", text, text};
  check_separate_splits(encoded, [&](const wire::ByteSegments input) {
   for (const std::size_t choice_count : std::array<std::size_t, 3U>{0U, 1U, choices.size()}) {
    CAPTURE(choice_count);
    auto limits = test_limits(encoded.size(), 2U, 0U);
    limits.allocation_policy.allows = [](const void*, const wire::AllocationRequest&) noexcept { return false; };
    wire::Reader reader(input, limits);
    auto scope = reader.enter_path("key");
    const auto choice = reader.read_text_choice(0U, std::span(choices).first(choice_count));
    if (choice_count == choices.size()) {
     REQUIRE(choice);
     CHECK(*choice == 1U);
    } else {
     require_decode_error(choice, wire::ErrorCode::UnknownKey);
     CHECK(choice.error().offset == item_end);
     CHECK(choice.error().path == "key");
    }
    CHECK(reader.offset() == item_end);
    CHECK(reader.items_read() == 1U);
    REQUIRE(reader.read_scalar_item(0U));
    CHECK(reader.offset() == encoded.size());
    REQUIRE(reader.finish());
   }
  });
 }
}
TEST_CASE("borrowed strings preserve admission precedence and failure cursors across separate segments", "[frameworks][serialization]") {
 struct Failure {
  wire::ByteBuffer encoded;
  wire::ErrorCode code;
  std::size_t offset;
 };
 for (const bool text : {false, true}) {
  const auto major = text ? 0x60U : 0x40U;
  const std::array failures{
   Failure{{}, wire::ErrorCode::UnexpectedEof, 0U},
   Failure{{std::byte(major + 25U), std::byte{1U}}, wire::ErrorCode::UnexpectedEof, 2U},
   Failure{{std::byte(major + 4U), std::byte{'a'}}, wire::ErrorCode::UnexpectedEof, 1U},
   Failure{{std::byte{0x99}}, wire::ErrorCode::TypeMismatch, 1U},
   Failure{{std::byte(major + 24U), std::byte{1U}, std::byte{'a'}}, wire::ErrorCode::NonMinimal, 2U},
  };
  for (const auto& failure : failures) {
   check_separate_splits(failure.encoded, [&](const wire::ByteSegments input) {
    // Combined rejections establish byte > depth > item > parsing order.
    for (unsigned rejection = 0U; rejection < 8U; ++rejection) {
     if ((rejection & 1U) != 0U && input.size() == 0U) continue;
     CAPTURE(text, rejection, failure.offset);
     auto limits = test_limits(input.size(), (rejection & 4U) != 0U ? 0U : 1U, 0U);
     if ((rejection & 1U) != 0U) --limits.max_bytes;
     limits.allocation_policy.allows = [](const void*, const wire::AllocationRequest&) noexcept { return false; };
     wire::Reader reader(input, limits);
     auto scope = reader.enter_path("payload");
     const auto expected_code = (rejection & 1U) != 0U    ? wire::ErrorCode::LimitExceeded
                                : (rejection & 2U) != 0U  ? wire::ErrorCode::DepthExceeded
                                 : (rejection & 4U) != 0U ? wire::ErrorCode::LimitExceeded
                                                          : failure.code;
     const auto expected_offset = rejection == 0U ? failure.offset : 0U;
     const auto check = [&](const auto& result) {
      require_decode_error(result, expected_code);
      CHECK(result.error().offset == expected_offset);
      CHECK(result.error().path == "payload");
     };
     const auto depth = (rejection & 2U) != 0U ? 1U : 0U;
     if (text)
      check(reader.read_text_choice(depth, {}));
     else
      check(reader.borrow_bytes_item(depth));
     CHECK(reader.offset() == expected_offset);
     CHECK(reader.items_read() == (rejection == 0U ? 1U : 0U));
    }
   });
  }
 }
}
TEST_CASE("configured maximum scalar and flat array inputs retain independent ownership", "[frameworks][serialization]") {
 constexpr wire::DynamicValueLimits admitted{.max_bytes = 65536U, .max_items = 256U, .max_depth = 1U};
 wire::Value::Array items;
 for (std::size_t index = 0U; index < admitted.max_items; ++index) {
  if (index % 2U == 0U)
   items.emplace_back(std::string(257U, 'x'));
  else
   items.emplace_back(wire::ByteBuffer(257U, std::byte{0xa5}));
 }
 for (const wire::Value& source : {wire::Value(std::string(admitted.max_bytes, 'x')), wire::Value(wire::ByteBuffer(admitted.max_bytes, std::byte{0xa5})), wire::Value(items)}) {
  const auto expected = wire::FlatValue::from_value(source, admitted);
  REQUIRE(expected);
  const bool array = std::holds_alternative<wire::Value::Array>(source.storage);
  const auto item_limit = array ? admitted.max_items + 1U : 1U;
  const auto measured = wire::CountingEncoder(test_limits(128U * 1024U, item_limit, admitted.max_depth)).measure(source);
  REQUIRE(measured);
  const auto limits = test_limits(*measured, item_limit, admitted.max_depth);
  wire::ByteBuffer encoded;
  REQUIRE(wire::encode(source, encoded, limits));
  REQUIRE(encoded.size() == limits.max_bytes);
  // A constant six splits bounds maximum-payload work to O(encoded bytes).
  for (const auto split : std::array<std::size_t, 6U>{0U, 1U, 5U, encoded.size() / 2U, encoded.size() - 1U, encoded.size()}) {
   auto input = encoded;
   const wire::ByteSegments segments{std::span(input).first(split), std::span(input).subspan(split)};
   wire::Reader reader(segments, limits);
   const auto flat = reader.read_flat();
   REQUIRE(flat);
   CHECK(reader.offset() == encoded.size());
   const auto value = wire::decode(segments, limits);
   REQUIRE(value);
   std::ranges::fill(input, std::byte{});
   CHECK(*flat == *expected);
   CHECK(*value == source);
  }
  auto insufficient = limits;
  --insufficient.max_bytes;
  const auto rejected = wire::Reader({encoded, {}}, insufficient).read_flat();
  require_decode_error(rejected, wire::ErrorCode::LimitExceeded);
  CHECK(rejected.error().offset == 0U);
  if (array) {
   insufficient = limits;
   --insufficient.max_items;
   require_decode_error(wire::Reader({encoded, {}}, insufficient).read_flat(), wire::ErrorCode::LimitExceeded);
  }
 }
 require_decode_error(wire::FlatValue::text(std::string(admitted.max_bytes + 1U, 'x'), admitted.max_bytes), wire::ErrorCode::LimitExceeded);
 require_decode_error(wire::FlatValue::bytes(wire::ByteBuffer(admitted.max_bytes + 1U), admitted.max_bytes), wire::ErrorCode::LimitExceeded);
 items.emplace_back(std::uint64_t{1U});
 require_decode_error(wire::FlatValue::from_value(wire::Value(std::move(items)), admitted), wire::ErrorCode::LimitExceeded);
}
TEST_CASE("segmented scalar failures preserve complete payload admission and offsets", "[frameworks][serialization]") {
 const std::array payload{std::byte{0x64}, std::byte{'a'}, std::byte{0xe2}, std::byte{0x82}, std::byte{0xac}};
 for (const auto head : {std::byte{0x44}, std::byte{0x64}}) {
  auto encoded = payload;
  encoded[0] = head;
  for (std::size_t size = 0U; size <= encoded.size(); ++size) {
   for (std::size_t split = 0U; split <= size; ++split) {
    const wire::ByteSegments input{std::span(encoded).first(split), std::span(encoded).subspan(split, size - split)};
    // Truncated payloads exceed both remaining input and remaining budget.
    wire::Reader reader(input, test_limits(size));
    auto scope = reader.enter_path("payload");
    const auto value = reader.read_flat();
    if (size == encoded.size()) {
     REQUIRE(value);
     CHECK(reader.offset() == size);
    } else {
     require_decode_error(value, wire::ErrorCode::UnexpectedEof);
     CHECK(value.error().offset == (size == 0U ? 0U : 1U));
     CHECK(value.error().path == "payload");
     CHECK(reader.offset() == value.error().offset);
    }
   }
  }
 }
 auto invalid = payload;
 invalid.back() = std::byte{'x'};
 for (std::size_t split = 0U; split <= invalid.size(); ++split) {
  const wire::ByteSegments input{std::span(invalid).first(split), std::span(invalid).subspan(split)};
  const auto value = wire::Reader(input, test_limits(invalid.size())).read_flat();
  require_decode_error(value, wire::ErrorCode::InvalidUtf8);
  CHECK(value.error().offset == invalid.size());
  auto limits = test_limits(invalid.size() - 1U);
  const auto budget = wire::Reader(input, limits).read_flat();
  require_decode_error(budget, wire::ErrorCode::LimitExceeded);
  CHECK(budget.error().offset == 0U);
  limits.max_bytes = invalid.size();
  limits.allocation_policy.allows = [](const void*, const wire::AllocationRequest&) noexcept { return false; };
  const auto denied = wire::Reader(input, limits).read_flat();
  require_decode_error(denied, wire::ErrorCode::LimitExceeded);
  CHECK(denied.error().offset == 1U);
 }
}
TEST_CASE("scalar allocation admission reports exact kind size path and cursor", "[frameworks][serialization]") {
 struct Observation {
  const wire::Reader* reader = nullptr;
  mutable std::size_t calls = 0U;
  mutable wire::AllocationKind kind{};
  mutable std::size_t size = 0U;
  mutable std::size_t offset = 0U;
  mutable bool path_matches = false;
 };
 for (const auto kind : {wire::AllocationKind::Text, wire::AllocationKind::Bytes}) {
  const std::array encoded{kind == wire::AllocationKind::Text ? std::byte{0x64} : std::byte{0x44}, std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, std::byte{'d'}};
  for (std::size_t split = 0U; split <= encoded.size(); ++split) {
   Observation observed;
   auto limits = test_limits(encoded.size());
   limits.allocation_policy = {.context = &observed, .allows = [](const void* context, const wire::AllocationRequest& request) noexcept {
                                const auto& state = *static_cast<const Observation*>(context);
                                ++state.calls;
                                state.kind = request.kind;
                                state.size = request.size;
                                state.offset = state.reader->offset();
                                state.path_matches = request.path.size() == 2U && request.path[0].name == "outer" && request.path[1].name == "payload";
                                return false;
                               }};
   wire::Reader reader({std::span(encoded).first(split), std::span(encoded).subspan(split)}, limits);
   observed.reader = &reader;
   auto outer = reader.enter_path("outer");
   auto inner = reader.enter_path("payload");
   const auto rejected = reader.read_flat();
   require_decode_error(rejected, wire::ErrorCode::LimitExceeded);
   CHECK(observed.calls == 1U);
   CHECK(observed.kind == kind);
   CHECK(observed.size == 4U);
   CHECK(observed.path_matches);
   CHECK(observed.offset == 1U);
   CHECK(reader.offset() == 1U);
   CHECK(rejected.error().offset == 1U);
   CHECK(rejected.error().path == "outer.payload");
  }
 }
}
TEST_CASE("UTF8 scalar limits preserve literal text across every segment split", "[frameworks][serialization]") {
 for (const std::string_view text : {
       std::string_view{},
       std::string_view{"a\0b", 3U},
       std::string_view{"\x01\x7f"},
       std::string_view{"\xc2\x80"},
       std::string_view{"\xdf\xbf"},
       std::string_view{"\xe0\xa0\x80"},
       std::string_view{"\xed\x9f\xbf"},
       std::string_view{"\xee\x80\x80"},
       std::string_view{"\xef\xbf\xbf"},
       std::string_view{"\xf0\x90\x80\x80"},
       std::string_view{"\xf4\x8f\xbf\xbf"},
      }) {
  wire::ByteBuffer expected{std::byte(0x60U + text.size())};
  for (const unsigned char byte : text) expected.push_back(std::byte{byte});
  wire::ByteBuffer encoded;
  REQUIRE(wire::encode(wire::Value(std::string{text}), encoded, test_limits(64U)).has_value());
  CHECK(encoded == expected);
  for (std::size_t split = 0U; split <= expected.size(); ++split) {
   wire::Reader reader({std::span(expected).first(split), std::span(expected).subspan(split)}, test_limits(expected.size()));
   const auto decoded = reader.read_flat();
   REQUIRE(decoded.has_value());
   CHECK(decoded->visit([&](const auto& value) {
    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, std::string>)
     return value == text;
    else
     return false;
   }));
   CHECK(reader.offset() == expected.size());
  }
 }
}
TEST_CASE("malformed UTF8 categories consume complete segmented payloads before failure", "[frameworks][serialization]") {
 for (const std::string_view malformed : mmltk::testsupport::kMalformedUtf8) {
  wire::ByteBuffer encoded{std::byte(0x60U + malformed.size())};
  for (const unsigned char byte : malformed) encoded.push_back(std::byte{byte});
  const wire::ByteBuffer retained{std::byte{0xaa}, std::byte{0xbb}};
  wire::ByteBuffer destination = retained;
  const auto rejected_encode = wire::encode(wire::Value(std::string{malformed}), destination, test_limits(64U));
  REQUIRE_FALSE(rejected_encode.has_value());
  CHECK(rejected_encode.error().code == wire::ErrorCode::InvalidUtf8);
  CHECK(destination == retained);
  for (std::size_t split = 0U; split <= encoded.size(); ++split) {
   wire::Reader reader({std::span(encoded).first(split), std::span(encoded).subspan(split)}, test_limits(encoded.size()));
   auto scope = reader.enter_path("payload");
   const auto rejected = reader.read_flat();
   require_decode_error(rejected, wire::ErrorCode::InvalidUtf8);
   CHECK(rejected.error().path == "payload");
   CHECK(rejected.error().offset == encoded.size());
   CHECK(reader.offset() == encoded.size());
  }
 }
}
TEST_CASE("CBOR byte peeks preserve offsets across empty input and segment transitions", "[frameworks][serialization]") {
 const std::array bytes{std::byte{0xf6}, std::byte{0x18}, std::byte{0x18}, std::byte{0xf6}};
 for (std::size_t split = 0U; split <= bytes.size(); ++split) {
  wire::Reader reader({std::span(bytes).first(split), std::span(bytes).subspan(split)}, test_limits(bytes.size()));
  for (const std::size_t offset : {0U, 1U, 3U}) {
   for (int repetition = 0; repetition != 2; ++repetition) {
    const auto peek = reader.next_is_null();
    REQUIRE(peek);
    CHECK(*peek == (offset != 1U));
    CHECK(reader.offset() == offset);
   }
   REQUIRE(reader.read_scalar_item(0U));
  }
  CHECK(reader.offset() == bytes.size());
  CHECK(reader.items_read() == 3U);
  require_decode_error(reader.next_is_null(), wire::ErrorCode::UnexpectedEof);
  const auto failed = reader.read_scalar_item(0U);
  require_decode_error(failed, wire::ErrorCode::UnexpectedEof);
  CHECK(failed.error().offset == bytes.size());
  CHECK(reader.offset() == bytes.size());
 }
 wire::Reader empty({}, test_limits(0U));
 auto scope = empty.enter_path("nested");
 const auto eof = empty.next_is_null();
 require_decode_error(eof, wire::ErrorCode::UnexpectedEof);
 CHECK(eof.error().offset == 0U);
 CHECK(eof.error().path == "nested");
 CHECK(empty.offset() == 0U);
 CHECK(empty.items_read() == 0U);
 wire::Reader oversized({bytes, {}}, test_limits(0U));
 const auto limited = oversized.next_is_null();
 require_decode_error(limited, wire::ErrorCode::LimitExceeded);
 CHECK(limited.error().offset == 0U);
 CHECK(oversized.offset() == 0U);
 CHECK(oversized.items_read() == 0U);
 const std::array truncated{std::byte{0x19}, std::byte{0x01}};
 wire::Reader partial({std::span(truncated).first(1U), std::span(truncated).subspan(1U)}, test_limits(truncated.size()));
 const auto failure = partial.read_scalar_item(0U);
 require_decode_error(failure, wire::ErrorCode::UnexpectedEof);
 CHECK(failure.error().offset == 2U);
 CHECK(partial.offset() == 2U);
 require_decode_error(partial.next_is_null(), wire::ErrorCode::UnexpectedEof);
 CHECK(partial.offset() == 2U);
}
TEST_CASE("dynamic_value_limits_reject_descendants_and_keys", "[frameworks][serialization]") {
 const wire::DynamicValueLimits limits{.max_bytes = 4U, .max_items = 2U, .max_depth = 16U};
 REQUIRE(!wire::dynamic_value_within_limits(wire::Value(wire::Value::Array{wire::Value(std::string{"large"})}), limits));
 REQUIRE(!wire::dynamic_value_within_limits(wire::Value(wire::Value::Array{wire::Value(wire::ByteBuffer(5U, std::byte{0x2a}))}), limits));
 REQUIRE(!wire::dynamic_value_within_limits(wire::Value(wire::Value::Array{wire::Value(wire::Value::Array{wire::Value{}, wire::Value{}, wire::Value{}})}), limits));
 REQUIRE(!wire::dynamic_value_within_limits(wire::Value(wire::Value::Object{{"large", wire::Value{true}}}), limits));
}
TEST_CASE("flat_dynamic_values_use_an_opaque_direct_reader_and_reject_recursive_containers", "[frameworks][serialization]") {
 const auto bounded_text = wire::FlatValue::text("four", 4U);
 REQUIRE(bounded_text.has_value());
 CHECK_FALSE(wire::FlatValue::text("five!", 4U).has_value());
 const std::array bounded_bytes{std::byte{0x01}, std::byte{0x02}};
 REQUIRE(wire::FlatValue::bytes(bounded_bytes, bounded_bytes.size()).has_value());
 CHECK_FALSE(wire::FlatValue::bytes(bounded_bytes, 1U).has_value());
 const wire::DynamicValueLimits direct_limits{.max_bytes = 4U, .max_items = 2U, .max_depth = 1U};
 const auto direct_array = wire::FlatValue::from_value(wire::Value(wire::Value::Array{wire::Value(std::uint64_t{7U}), wire::Value(std::string{"ok"})}), direct_limits);
 REQUIRE(direct_array.has_value());
 CHECK(flat_item_count(*direct_array) == 2U);
 CHECK_FALSE(wire::FlatValue::from_value(wire::Value(wire::Value::Array{wire::Value(wire::Value::Array{wire::Value(std::uint64_t{7U})})}), direct_limits).has_value());
 CHECK_FALSE(wire::FlatValue::from_value(wire::Value(wire::Value::Object{{"value", wire::Value(true)}}), direct_limits).has_value());
 const std::array scalar{std::byte{0x07}};
 const auto scalar_value = wire::Reader({scalar, {}}, test_limits(scalar.size(), 1U, 0U)).read_flat();
 REQUIRE(scalar_value.has_value());
 bool saw_scalar = false;
 scalar_value->visit([&]<class Leaf>(const Leaf& leaf) {
  if constexpr (std::same_as<std::remove_cvref_t<Leaf>, std::uint64_t>) { saw_scalar = leaf == 7U; }
 });
 CHECK(saw_scalar);
 const std::array flat_array{std::byte{0x82}, std::byte{0x07}, std::byte{0x62}, std::byte{'o'}, std::byte{'k'}};
 const auto decoded = wire::Reader({flat_array, {}}, test_limits(flat_array.size(), 3U, 1U)).read_flat();
 REQUIRE(decoded.has_value());
 CHECK(flat_item_count(*decoded) == 2U);
 const std::array nested_array{std::byte{0x81}, std::byte{0x81}, std::byte{0xf5}};
 const auto nested = wire::Reader({nested_array, {}}, test_limits(nested_array.size(), 3U, 2U)).read_flat();
 REQUIRE_FALSE(nested.has_value());
 CHECK(nested.error().code == wire::ErrorCode::TypeMismatch);
 const std::array object{std::byte{0xa1}, std::byte{0x61}, std::byte{'x'}, std::byte{0xf5}};
 const auto mapped = wire::Reader({object, {}}, test_limits(object.size(), 3U, 2U)).read_flat();
 REQUIRE_FALSE(mapped.has_value());
 CHECK(mapped.error().code == wire::ErrorCode::TypeMismatch);
}
TEST_CASE("flat_reader_refuses_declared_capacity_before_allocating_an_array", "[frameworks][serialization]") {
 struct AllocationProbe {
  mutable bool denied = false;
 } probe;
 const auto refuse_flat_array = [](const void* context, const wire::AllocationRequest& request) noexcept {
  const auto& state = *static_cast<const AllocationProbe*>(context);
  if (request.kind == wire::AllocationKind::Sequence && request.size == 2U) {
   state.denied = true;
   return false;
  }
  return true;
 };
 const std::array flat_array{std::byte{0x82}, std::byte{0x01}, std::byte{0x02}};
 const wire::Limits limits{
  .max_bytes = flat_array.size(),
  .max_items = 3U,
  .max_depth = 1U,
  .allocation_policy = {.context = &probe, .allows = refuse_flat_array},
 };
 const auto rejected = wire::Reader({flat_array, {}}, limits).read_flat();
 REQUIRE_FALSE(rejected.has_value());
 CHECK(rejected.error().code == wire::ErrorCode::LimitExceeded);
 CHECK(probe.denied);
 const auto capacity_rejected = wire::Reader({flat_array, {}}, test_limits(flat_array.size(), 2U, 1U)).read_flat();
 REQUIRE_FALSE(capacity_rejected.has_value());
 CHECK(capacity_rejected.error().code == wire::ErrorCode::LimitExceeded);
}
TEST_CASE("fixed_cbor_destination_is_exact_and_rejects_capacity_before_writing", "[frameworks][serialization]") {
 const wire::Value value(wire::Value::Object{
  {"sequence", wire::Value(std::uint64_t{24U})},
  {"detail", wire::Value(std::string("fixed ledger storage"))},
 });
 wire::ByteBuffer expected;
 REQUIRE(wire::encode(value, expected, test_limits(128U)).has_value());
 std::array<std::byte, 128U> fixed{};
 const auto written = wire::encode(value, fixed, test_limits(fixed.size()));
 REQUIRE(written.has_value());
 REQUIRE(*written == expected.size());
 CHECK(std::ranges::equal(std::span(fixed).first(*written), expected));
 std::array<std::byte, 128U> undersized;
 undersized.fill(std::byte{0xa5});
 const auto rejected = wire::encode(value, std::span(undersized).first(expected.size() - 1U), test_limits(expected.size()));
 REQUIRE_FALSE(rejected.has_value());
 CHECK(rejected.error().code == wire::ErrorCode::LimitExceeded);
 CHECK(std::ranges::all_of(undersized, [](const std::byte byte_value) { return byte_value == std::byte{0xa5}; }));
}
TEST_CASE("raw_array_substitution_validates_and_splices_canonical_items", "[frameworks][serialization]") {
 wire::Value root(wire::Value::Array{wire::Value{}, wire::Value{}});
 const auto* const target = std::get_if<wire::Value::Array>(&root.storage);
 REQUIRE(target != nullptr);
 wire::ByteBuffer first;
 wire::ByteBuffer second;
 REQUIRE(wire::encode(wire::Value(std::uint64_t{24U}), first, test_limits(16U)).has_value());
 REQUIRE(wire::encode(wire::Value(std::string("ok")), second, test_limits(16U)).has_value());
 const std::array items{
  wire::ByteSegments{.first = first, .second = {}},
  wire::ByteSegments{.first = std::span(second).first(1U), .second = std::span(second).subspan(1U)},
 };
 wire::ByteBuffer encoded;
 REQUIRE(wire::encode(root, wire::RawArrayItems{.target = target, .items = items}, encoded, test_limits(32U)).has_value());
 wire::ByteBuffer expected{std::byte{0x82}};
 expected.insert(expected.end(), first.begin(), first.end());
 expected.insert(expected.end(), second.begin(), second.end());
 CHECK(std::ranges::equal(encoded, expected));
 const std::array incomplete{std::byte{0x18}};
 const std::array malformed{wire::ByteSegments{.first = first, .second = {}}, wire::ByteSegments{.first = incomplete, .second = {}}};
 const wire::ByteBuffer retained{std::byte{0xaa}, std::byte{0xbb}};
 encoded = retained;
 const auto rejected = wire::encode(root, wire::RawArrayItems{.target = target, .items = malformed}, encoded, test_limits(32U));
 REQUIRE_FALSE(rejected.has_value());
 CHECK(std::ranges::equal(encoded, retained));
}
TEST_CASE("rejects_nonminimal_malformed_duplicate_and_invalid_utf8_values", "[frameworks][serialization]") {
 const std::array nonminimal{std::byte{0x18}, std::byte{0x17}};
 require_malformed_scalar(nonminimal, wire::ErrorCode::NonMinimal);
 const std::array invalid_float{std::byte{0xf9}, std::byte{0x7e}, std::byte{0x00}};
 require_malformed_scalar(invalid_float, wire::ErrorCode::InvalidFloat);
 const std::array duplicate{std::byte{0xa2}, std::byte{0x61}, std::byte{'a'}, std::byte{0}, std::byte{0x61}, std::byte{'a'}, std::byte{0}};
 // CLEANUP-IGNORE: The root duplicate-key oracle and the later nested-map oracle are separate raw CBOR schemas
 // whose similar validation sequence proves different path scopes.
 REQUIRE(!wire::validate_raw_item({duplicate, {}}, {duplicate.size(), 16U, 4U}).has_value());
 std::array<std::uint32_t, duplicate.size()> duplicate_scratch{};
 const auto structural_duplicate = wire::validate_raw_item_structural(duplicate, {duplicate.size(), duplicate.size(), 4U}, {.key_offsets = duplicate_scratch});
 REQUIRE_FALSE(structural_duplicate.has_value());
 CHECK(structural_duplicate.error().code == wire::ErrorCode::DuplicateKey);
 const std::array non_text_key{std::byte{0xa1}, std::byte{0x01}, std::byte{0x01}};
 std::array<std::uint32_t, non_text_key.size()> non_text_scratch{};
 const auto structural_non_text = wire::validate_raw_item_structural(non_text_key, {non_text_key.size(), non_text_key.size(), 2U}, {.key_offsets = non_text_scratch});
 REQUIRE_FALSE(structural_non_text.has_value());
 CHECK(structural_non_text.error().code == wire::ErrorCode::TypeMismatch);
 const std::array nested_reused_key{std::byte{0xa1}, std::byte{0x61}, std::byte{'a'}, std::byte{0xa1}, std::byte{0x61}, std::byte{'a'}, std::byte{0x01}};
 std::array<std::uint32_t, nested_reused_key.size()> nested_scratch{};
 CHECK(wire::validate_raw_item_structural(nested_reused_key, {nested_reused_key.size(), nested_reused_key.size(), 4U}, {.key_offsets = nested_scratch}).has_value());
 const std::array nested_duplicate{
  std::byte{0xa1}, std::byte{0x61}, std::byte{'o'}, std::byte{0xa2}, std::byte{0x61}, std::byte{'a'}, std::byte{0x01}, std::byte{0x61}, std::byte{'a'}, std::byte{0x02}};
 std::array<std::uint32_t, nested_duplicate.size()> nested_duplicate_scratch{};
 const auto nested_structural_duplicate = wire::validate_raw_item_structural(nested_duplicate, {nested_duplicate.size(), nested_duplicate.size(), 4U}, {.key_offsets = nested_duplicate_scratch});
 REQUIRE_FALSE(nested_structural_duplicate.has_value());
 CHECK(nested_structural_duplicate.error().code == wire::ErrorCode::DuplicateKey);
 const std::array invalid_utf8{std::byte{0x61}, std::byte{0x80}};
 require_malformed_scalar(invalid_utf8, wire::ErrorCode::InvalidUtf8);
 const std::array nested_invalid_utf8{std::byte{0xa1}, std::byte{0x65}, std::byte{'o'}, std::byte{'u'}, std::byte{'t'}, std::byte{'e'}, std::byte{'r'}, std::byte{0xa1}, std::byte{0x65},
  std::byte{'i'}, std::byte{'n'}, std::byte{'n'}, std::byte{'e'}, std::byte{'r'}, std::byte{0x61}, std::byte{0x80}};
 const auto nested_error = wire::decode({nested_invalid_utf8, {}}, {nested_invalid_utf8.size(), 16U, 4U});
 REQUIRE(!nested_error.has_value());
 REQUIRE(nested_error.error().code == wire::ErrorCode::InvalidUtf8);
 REQUIRE(nested_error.error().path == "outer.inner");
}
TEST_CASE("rejects_trailing_indefinite_tagged_and_bounded_raw_items", "[frameworks][serialization]") {
 const std::array trailing{std::byte{0}, std::byte{0}};
 require_malformed_scalar(trailing, wire::ErrorCode::TrailingData);
 const std::array indefinite{std::byte{0x9f}, std::byte{0xff}};
 require_malformed_scalar(indefinite, wire::ErrorCode::IndefiniteContainer);
 const std::array tagged{std::byte{0xc0}, std::byte{0}};
 require_malformed_scalar(tagged, wire::ErrorCode::InvalidMajorType);
 const std::array nested{std::byte{0x81}, std::byte{0x81}, std::byte{0}};
 require_decode_error(wire::decode({nested, {}}, {nested.size(), 8U, 1U}), wire::ErrorCode::DepthExceeded);
 require_decode_error(wire::decode({nested, {}}, {nested.size(), 2U, 8U}), wire::ErrorCode::LimitExceeded);
 require_decode_error(wire::decode({nested, {}}, {nested.size() - 1U, 8U, 8U}), wire::ErrorCode::LimitExceeded);
}
TEST_CASE("bounds_outbound_depth_and_item_counts_before_writing", "[frameworks][serialization]") {
 wire::Value deeply_nested(wire::Value::Array{wire::Value(wire::Value::Array{wire::Value(std::uint64_t{1U})})});
 wire::ByteBuffer destination{std::byte{0xaa}};
 const auto depth_result = wire::encode(deeply_nested, destination, test_limits(64U, 16U, 1U));
 REQUIRE(!depth_result.has_value());
 REQUIRE(depth_result.error().code == wire::ErrorCode::DepthExceeded);
 REQUIRE(destination.size() == 1U);
 REQUIRE(std::to_integer<unsigned int>(destination.front()) == 0xaaU);
 const auto measured_depth = wire::CountingEncoder(test_limits(64U, 16U, 1U)).measure(deeply_nested);
 REQUIRE(!measured_depth.has_value());
 REQUIRE(measured_depth.error().code == wire::ErrorCode::DepthExceeded);
 wire::Value too_many_items(wire::Value::Array{wire::Value(std::uint64_t{1U}), wire::Value(std::uint64_t{2U})});
 const auto item_result = wire::encode(too_many_items, destination, test_limits(64U, 2U));
 REQUIRE(!item_result.has_value());
 REQUIRE(item_result.error().code == wire::ErrorCode::LimitExceeded);
 wire::ByteBuffer appended;
 wire::Writer writer(appended, test_limits(16U, 1U));
 const std::array integer{std::byte{0x01}};
 REQUIRE(writer.append_raw_item({integer, {}}).has_value());
 REQUIRE(!writer.append_raw_item({integer, {}}).has_value());
 REQUIRE(appended.size() == 1U);
}
TEST_CASE("uses_shortest_numeric_forms_and_only_appends_complete_raw_items", "[frameworks][serialization]") {
 wire::ByteBuffer half;
 REQUIRE(wire::encode(wire::Value(1.5), half, test_limits(16U)).has_value());
 REQUIRE(half.size() == 3U);
 wire::ByteBuffer single;
 REQUIRE(wire::encode(wire::Value(1.1F), single, test_limits(16U)).has_value());
 REQUIRE(single.size() == 5U);
 wire::ByteBuffer integer;
 REQUIRE(wire::encode(wire::Value(std::uint64_t{24}), integer, test_limits(16U)).has_value());
 REQUIRE(integer.size() == 2U);
 wire::ByteBuffer destination;
 wire::Writer writer(destination, test_limits(16U));
 REQUIRE(writer.append_raw_item({integer, {}}).has_value());
 const std::array incomplete{std::byte{0x18}};
 REQUIRE(!writer.append_raw_item({incomplete, {}}).has_value());
 REQUIRE(static_cast<bool>(destination == integer));
}
TEST_CASE("direct_writer_failures_restore_reusable_destination_state", "[frameworks][serialization]") {
 const wire::ByteBuffer original{std::byte{0xaa}, std::byte{0xbb}};
 wire::ByteBuffer destination = original;
 wire::Writer writer(destination, test_limits(64U, 32U, 8U));
 const std::string invalid_utf8(1U, static_cast<char>(0x80));
 wire::Value::Object invalid_object{{"valid", wire::Value(std::uint64_t{1U})}, {"invalid", wire::Value(invalid_utf8)}};
 const auto invalid_result = writer.write(wire::Value(std::move(invalid_object)));
 REQUIRE(!invalid_result.has_value());
 REQUIRE(invalid_result.error().code == wire::ErrorCode::InvalidUtf8);
 REQUIRE(static_cast<bool>(destination == original));
 wire::Value::Object oversized_object{{"first", wire::Value(std::uint64_t{1U})}, {"second", wire::Value(std::string(60U, 'x'))}};
 const auto oversized_result = writer.write(wire::Value(std::move(oversized_object)));
 REQUIRE(!oversized_result.has_value());
 REQUIRE(oversized_result.error().code == wire::ErrorCode::LimitExceeded);
 REQUIRE(static_cast<bool>(destination == original));
 REQUIRE(writer.write(wire::Value(std::uint64_t{7U})).has_value());
 REQUIRE(destination.size() == original.size() + 1U);
 REQUIRE(std::to_integer<unsigned int>(destination.back()) == 7U);
 wire::ByteBuffer bounded_items;
 wire::Writer bounded_writer(bounded_items, test_limits(16U, 2U, 2U));
 REQUIRE(bounded_writer.write(wire::Value(std::uint64_t{1U})).has_value());
 REQUIRE(bounded_writer.write(wire::Value(std::uint64_t{2U})).has_value());
 REQUIRE(!bounded_writer.write(wire::Value(std::uint64_t{3U})).has_value());
 REQUIRE(bounded_items.size() == 2U);
}
TEST_CASE("preserves_signed_integer_boundaries_and_rejects_unrepresentable_negatives", "[frameworks][serialization]") {
 wire::ByteBuffer encoded_minimum;
 REQUIRE(wire::encode(wire::Value(std::numeric_limits<std::int64_t>::min()), encoded_minimum, test_limits(16U)).has_value());
 const auto decoded_minimum = wire::decode({encoded_minimum, {}}, {16U, 1U, 1U});
 REQUIRE(decoded_minimum.has_value());
 REQUIRE(std::get<std::int64_t>(decoded_minimum->storage) == std::numeric_limits<std::int64_t>::min());
 const std::array below_minimum{std::byte{0x3b}, std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
 const auto rejected = wire::decode({below_minimum, {}}, {below_minimum.size(), 1U, 1U});
 REQUIRE(!rejected.has_value());
 REQUIRE(rejected.error().code == wire::ErrorCode::Overflow);
}
TEST_CASE("JSON persistence preserves canonical numeric and container values", "[frameworks][serialization][json]") {
 namespace serial = mmltk::frameworks::serialization;
 const auto limits = test_limits(1024U);
 const auto expected = nlohmann::json::parse(R"({"max":18446744073709551615,"min":-9223372036854775808,"values":[null,true,0.1,1.5,-0.0,"text",{},[]]})");
 const auto bytes = serial::json_to_cbor(expected.dump(), limits);
 REQUIRE(wire::decode({bytes, {}}, limits));
 const auto actual = serial::json_from_cbor({bytes, {}}, limits);
 CHECK(actual == expected);
 CHECK(actual["max"].get<std::uint64_t>() == std::numeric_limits<std::uint64_t>::max());
 CHECK(actual["min"].get<std::int64_t>() == std::numeric_limits<std::int64_t>::min());
 CHECK(actual["values"][2].is_number_float());
 CHECK(actual["values"][2].get<double>() == 0.1);
}
TEST_CASE("JSON persistence enforces wire and reflected admission bounds", "[frameworks][serialization][json]") {
 namespace serial = mmltk::frameworks::serialization;
 using serial::test::BoundBase;
 using serial::test::BoundFlat;
 const auto limits = test_limits(128U);
 std::array<std::byte, 128U> scratch{};
 const auto text = serial::reflected_json(BoundBase{42U}, scratch, limits).dump();
 CHECK(serial::decode_reflected_json<BoundBase>(text, limits).id == 42U);
 CHECK_THROWS(serial::decode_reflected_json<BoundBase>(R"({"id":65536})", limits));
 CHECK_THROWS(serial::decode_reflected_json<BoundBase>(R"({"id":1,"extra":2})", limits));
 CHECK_THROWS(serial::decode_reflected_json<BoundFlat>(R"({"flat":"oversized"})", limits));
 CHECK_THROWS(serial::json_to_cbor("null", test_limits(3U)));
 CHECK_THROWS(serial::json_to_cbor(R"({"a":[1,2]})", test_limits(128U, 4U)));
 CHECK_NOTHROW(serial::json_to_cbor(R"({"a":[1,2]})", test_limits(128U, 5U, 2U)));
 CHECK_THROWS(serial::json_to_cbor(R"({"a":[1,2]})", test_limits(128U, 5U, 1U)));
 CHECK_THROWS(serial::json_to_cbor("1e999", limits));
 const std::array invalid{std::byte{0xff}};
 CHECK_THROWS(serial::json_from_cbor({invalid, {}}, limits));
}
}  // namespace
