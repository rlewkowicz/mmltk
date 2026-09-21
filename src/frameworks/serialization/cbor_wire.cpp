#include "src/frameworks/serialization/cbor_wire.h"
#include "src/common/types/utf8.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
namespace mmltk::frameworks::serialization::wire {
namespace {
[[nodiscard]] bool dynamic_value_within_limits(const Value& value, const DynamicValueLimits limits, const std::size_t depth) noexcept {
 if (depth > limits.max_depth) return false;
 if (const auto* text = std::get_if<std::string>(&value.storage)) return text->size() <= limits.max_bytes;
 if (const auto* bytes = std::get_if<ByteBuffer>(&value.storage)) return bytes->size() <= limits.max_bytes;
 if (const auto* array = std::get_if<Value::Array>(&value.storage)) {
  return array->size() <= limits.max_items &&
         std::all_of(array->begin(), array->end(), [&](const Value& child) { return dynamic_value_within_limits(child, limits, depth + 1U); });
 }
 if (const auto* object = std::get_if<Value::Object>(&value.storage)) {
  return object->size() <= limits.max_items && std::all_of(object->begin(), object->end(), [&](const auto& member) {
          return member.first.size() <= limits.max_bytes && dynamic_value_within_limits(member.second, limits, depth + 1U);
         });
 }
 return true;
}
[[nodiscard]] std::uint64_t structural_key_hash(const ByteView key, const std::uintptr_t salt) noexcept {
 std::uint64_t hash = 0x9e3779b97f4a7c15ULL ^ static_cast<std::uint64_t>(salt);
 for (const std::byte value : key) {
  hash ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(value)) + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
  hash *= 0xbf58476d1ce4e5b9ULL;
 }
 hash ^= hash >> 30U;
 hash *= 0xbf58476d1ce4e5b9ULL;
 hash ^= hash >> 27U;
 hash *= 0x94d049bb133111ebULL;
 return hash ^ (hash >> 31U);
}
[[nodiscard]] std::uint16_t float_to_half(const float value) noexcept {
 const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
 const std::uint32_t sign = (bits >> 16U) & 0x8000U;
 const std::uint32_t exponent = (bits >> 23U) & 0xffU;
 std::uint32_t fraction = bits & 0x7fffffU;
 if (exponent == 0xffU) { return static_cast<std::uint16_t>(sign | 0x7c00U | (fraction == 0U ? 0U : 0x0200U)); }
 const int half_exponent = static_cast<int>(exponent) - 127 + 15;
 if (half_exponent >= 31) { return static_cast<std::uint16_t>(sign | 0x7c00U); }
 if (half_exponent <= 0) {
  if (half_exponent < -10) { return static_cast<std::uint16_t>(sign); }
  fraction = (fraction | 0x800000U) >> static_cast<unsigned>(1 - half_exponent);
  return static_cast<std::uint16_t>(sign | ((fraction + 0x1000U) >> 13U));
 }
 return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(half_exponent) << 10U) | ((fraction + 0x1000U) >> 13U));
}
[[nodiscard]] float half_to_float(const std::uint16_t half) noexcept {
 const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000U) << 16U;
 const std::uint32_t exponent = (half >> 10U) & 0x1fU;
 std::uint32_t fraction = half & 0x03ffU;
 std::uint32_t bits = sign;
 if (exponent == 0U) {
  if (fraction != 0U) {
   std::uint32_t normalized_exponent = 113U;
   while ((fraction & 0x400U) == 0U) {
    fraction <<= 1U;
    --normalized_exponent;
   }
   bits |= normalized_exponent << 23U;
   bits |= (fraction & 0x3ffU) << 13U;
  }
 } else if (exponent == 31U) {
  bits |= 0x7f800000U | (fraction << 13U);
 } else {
  bits |= (exponent + 112U) << 23U;
  bits |= fraction << 13U;
 }
 return std::bit_cast<float>(bits);
}
[[nodiscard]] EncodeError encode_error(const ErrorCode code = ErrorCode::LimitExceeded) { return {.code = code, .offset = 0U, .path = {}}; }
[[nodiscard]] bool fits_remaining(const std::size_t value, const std::size_t used, const std::size_t limit) noexcept {
 return used <= limit && value <= limit - used;
}
[[nodiscard]] bool duplicate_object_key(const Value::Object& object) {
 std::set<std::string_view, std::less<>> names;
 for (const auto& [name, ignored] : object) {
  static_cast<void>(ignored);
  if (!names.insert(name).second) { return true; }
 }
 return false;
}
[[nodiscard]] std::expected<void, EncodeError> begin_encode_item(const std::size_t depth, std::size_t& items, const Limits& limits) {
 if (depth > limits.max_depth) { return std::unexpected(encode_error(ErrorCode::DepthExceeded)); }
 if (items >= limits.max_items) { return std::unexpected(encode_error(ErrorCode::LimitExceeded)); }
 ++items;
 return {};
}
template <class Range, class Operation>
[[nodiscard]] std::expected<void, EncodeError> encode_each(const Range& range, Operation&& operation) {
 for (const auto& item : range) {
  auto result = operation(item);
  if (!result) return result;
 }
 return {};
}
[[nodiscard]] std::expected<void, EncodeError> validate_object_encoding(const Value::Object& object, const std::size_t items, const Limits& limits) {
 if (duplicate_object_key(object)) { return std::unexpected(encode_error(ErrorCode::DuplicateKey)); }
 constexpr std::size_t kMapEntriesItemCount = 2U;
 if (items > limits.max_items || object.size() > (limits.max_items - items) / kMapEntriesItemCount) {
  return std::unexpected(encode_error(ErrorCode::LimitExceeded));
 }
 return {};
}
[[nodiscard]] constexpr std::uint8_t additional_info_for_head_size(const std::size_t size) noexcept {
 switch (size) {
  case 1U: return 0U;
  case 2U: return 24U;
  case 3U: return 25U;
  case 5U: return 26U;
  case 9U: return 27U;
  default: return 31U;
 }
}
}  // namespace
std::expected<CanonicalFloatEncoding, ErrorCode> canonical_float_encoding(const double value) noexcept {
 if (!std::isfinite(value)) return std::unexpected(ErrorCode::InvalidFloat);
 const float narrowed = static_cast<float>(value);
 if (static_cast<double>(narrowed) != value) {
  return CanonicalFloatEncoding{
   .width = CanonicalFloatWidth::Double,
   .bits = std::bit_cast<std::uint64_t>(value),
  };
 }
 const std::uint16_t half = float_to_half(narrowed);
 if (half_to_float(half) == narrowed) {
  return CanonicalFloatEncoding{
   .width = CanonicalFloatWidth::Half,
   .bits = half,
  };
 }
 return CanonicalFloatEncoding{
  .width = CanonicalFloatWidth::Single,
  .bits = std::bit_cast<std::uint32_t>(narrowed),
 };
}
bool dynamic_value_within_limits(const Value& value, const DynamicValueLimits limits) noexcept {
 return limits.max_bytes != 0U && limits.max_items != 0U && dynamic_value_within_limits(value, limits, 0U);
}
FlatValue::FlatValue(const std::monostate) noexcept : storage(std::monostate{}) {}
FlatValue::FlatValue(const bool value) noexcept : storage(value) {}
FlatValue::FlatValue(const std::int64_t value) noexcept : storage(value < 0 ? Storage{value} : Storage{static_cast<std::uint64_t>(value)}) {}
FlatValue::FlatValue(const std::uint64_t value) noexcept : storage(value) {}
FlatValue::FlatValue(const double value) noexcept : storage(value) {}
FlatValue::FlatValue(std::string value) : storage(std::move(value)) {}
FlatValue::FlatValue(ByteBuffer value) : storage(std::move(value)) {}
std::expected<FlatValue, DecodeError> FlatValue::text(const std::string_view value, const std::size_t max_bytes) {
 if (max_bytes == 0U || value.size() > max_bytes) { return std::unexpected(DecodeError{.code = ErrorCode::LimitExceeded, .offset = 0U, .path = {}}); }
 return FlatValue(std::string(value));
}
std::expected<FlatValue, DecodeError> FlatValue::bytes(const ByteView value, const std::size_t max_bytes) {
 if (max_bytes == 0U || value.size() > max_bytes) { return std::unexpected(DecodeError{.code = ErrorCode::LimitExceeded, .offset = 0U, .path = {}}); }
 return FlatValue(ByteBuffer(value.begin(), value.end()));
}
std::expected<FlatValue, DecodeError> FlatValue::array(const std::span<const FlatValue> values, const DynamicValueLimits limits) {
 if (limits.max_bytes == 0U || limits.max_items == 0U || values.size() > limits.max_items || (limits.max_depth == 0U && !values.empty())) {
  return std::unexpected(DecodeError{.code = ErrorCode::LimitExceeded, .offset = 0U, .path = {}});
 }
 for (const FlatValue& value : values) {
  const bool valid_scalar = value.visit([limits]<class T>(const T& leaf) {
   if constexpr (std::is_same_v<T, Array>) {
    return false;
   } else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, ByteBuffer>) {
    return leaf.size() <= limits.max_bytes;
   } else {
    return true;
   }
  });
  if (!valid_scalar) {
   const ErrorCode code = value.visit([]<class T>(const T&) { return std::is_same_v<T, Array> ? ErrorCode::TypeMismatch : ErrorCode::LimitExceeded; });
   return std::unexpected(DecodeError{.code = code, .offset = 0U, .path = {}});
  }
 }
 Array result;
 result.reserve(values.size());
 for (const FlatValue& value : values) {
  result.push_back(value.visit([]<class T>(const T& leaf) -> Scalar {
   if constexpr (std::is_same_v<T, Array>) {
    return std::monostate{};
   } else {
    return Scalar(leaf);
   }
  }));
 }
 return FlatValue(Storage(std::move(result)));
}
std::expected<FlatValue, DecodeError> FlatValue::from_value(const Value& value, const DynamicValueLimits limits) {
 if (limits.max_bytes == 0U || limits.max_items == 0U) { return std::unexpected(DecodeError{.code = ErrorCode::LimitExceeded, .offset = 0U, .path = {}}); }
 return std::visit(
  [limits]<class T>(const T& leaf) -> std::expected<FlatValue, DecodeError> {
   if constexpr (std::is_same_v<T, std::monostate> || std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t>) {
    return FlatValue(leaf);
   } else if constexpr (std::is_same_v<T, double>) {
    return std::isfinite(leaf) ? std::expected<FlatValue, DecodeError>(FlatValue(leaf))
                               : std::unexpected(DecodeError{.code = ErrorCode::InvalidFloat, .offset = 0U, .path = {}});
   } else if constexpr (std::is_same_v<T, std::string>) {
    return FlatValue::text(leaf, limits.max_bytes);
   } else if constexpr (std::is_same_v<T, ByteBuffer>) {
    return FlatValue::bytes(leaf, limits.max_bytes);
   } else if constexpr (std::is_same_v<T, Value::Array>) {
    if (leaf.size() > limits.max_items || (limits.max_depth == 0U && !leaf.empty())) {
     return std::unexpected(DecodeError{.code = ErrorCode::LimitExceeded, .offset = 0U, .path = {}});
    }
    FlatValue::Array result;
    result.reserve(leaf.size());
    for (const Value& item : leaf) {
     auto scalar = std::visit(
      [limits]<class U>(const U& item_leaf) -> std::expected<FlatValue::Scalar, DecodeError> {
       if constexpr (std::is_same_v<U, std::monostate> || std::is_same_v<U, bool> || std::is_same_v<U, std::int64_t> || std::is_same_v<U, std::uint64_t>) {
        return FlatValue::Scalar(item_leaf);
       } else if constexpr (std::is_same_v<U, double>) {
        return std::isfinite(item_leaf) ? std::expected<FlatValue::Scalar, DecodeError>(FlatValue::Scalar(item_leaf))
                                        : std::unexpected(DecodeError{.code = ErrorCode::InvalidFloat, .offset = 0U, .path = {}});
       } else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, ByteBuffer>) {
        return item_leaf.size() <= limits.max_bytes ? std::expected<FlatValue::Scalar, DecodeError>(FlatValue::Scalar(item_leaf))
                                                    : std::unexpected(DecodeError{.code = ErrorCode::LimitExceeded, .offset = 0U, .path = {}});
       } else {
        return std::unexpected(DecodeError{.code = ErrorCode::TypeMismatch, .offset = 0U, .path = {}});
       }
      },
      item.storage);
     if (!scalar) { return std::unexpected(scalar.error()); }
     result.push_back(std::move(*scalar));
    }
    return FlatValue(Storage(std::move(result)));
   } else {
    return std::unexpected(DecodeError{.code = ErrorCode::TypeMismatch, .offset = 0U, .path = {}});
   }
  },
  value.storage);
}
bool dynamic_value_within_limits(const FlatValue& value, const DynamicValueLimits limits) noexcept {
 if (limits.max_bytes == 0U || limits.max_items == 0U) { return false; }
 return value.visit([limits]<class T>(const T& leaf) {
  if constexpr (std::is_same_v<T, FlatValue::Array>) {
   if (leaf.size() > limits.max_items || (limits.max_depth == 0U && !leaf.empty())) { return false; }
   return std::all_of(leaf.begin(), leaf.end(), [limits](const FlatValue::Scalar& scalar) {
    return std::visit(
     [limits]<class U>(const U& item) {
      if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, ByteBuffer>) { return item.size() <= limits.max_bytes; }
      return true;
     },
     scalar);
   });
  } else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, ByteBuffer>) {
   return leaf.size() <= limits.max_bytes;
  }
  return true;
 });
}
Reader::Reader(const ByteSegments bytes, const Limits limits, const StructuralValidationScratch scratch) noexcept
    : input_(bytes), limits_(limits), structural_scratch_(scratch) {}
void Reader::PathScope::release() noexcept {
 if (reader_ != nullptr) {
  reader_->path_.pop_back();
  reader_ = nullptr;
 }
}
Reader::PathScope Reader::enter_path(const std::string_view name) {
 path_.push_back({.name = name, .object_kind = {}});
 return PathScope(*this);
}
DecodeError Reader::error(const ErrorCode code) const {
 DecodeError result{.code = code, .offset = offset_, .path = {}};
 for (const DecodePathElement& element : path_) {
  if (!result.path.empty()) { result.path.push_back('.'); }
  result.path.append(element.name);
 }
 return result;
}
DecodeError Reader::contextualize(DecodeError result) const {
 if (result.path.empty()) { result.path = error(result.code).path; }
 return result;
}
std::expected<bool, DecodeError> Reader::next_is_null() const {
 if (input_.size() > limits_.max_bytes) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
 const auto next = peek_byte();
 if (!next) return std::unexpected(next.error());
 return *next == std::byte{0xf6};
}
std::expected<std::byte, DecodeError> Reader::peek_byte() const {
 if (offset_ >= input_.size()) { return std::unexpected(error(ErrorCode::UnexpectedEof)); }
 return offset_ < input_.first.size() ? input_.first[offset_] : input_.second[offset_ - input_.first.size()];
}
std::expected<std::byte, DecodeError> Reader::byte() {
 const auto result = peek_byte();
 if (!result) return std::unexpected(result.error());
 ++offset_;
 return *result;
}
std::expected<std::uint64_t, DecodeError> Reader::argument(const std::uint8_t additional) {
 if (additional < 24U) { return additional; }
 if (additional == 31U) { return std::unexpected(error(ErrorCode::IndefiniteContainer)); }
 std::size_t byte_count = 0U;
 switch (additional) {
  case 24U: byte_count = 1U; break;
  case 25U: byte_count = 2U; break;
  case 26U: byte_count = 4U; break;
  case 27U: byte_count = 8U; break;
  default: break;
 }
 if (byte_count == 0U) { return std::unexpected(error(ErrorCode::InvalidAdditionalInfo)); }
 std::uint64_t value = 0U;
 for (std::size_t index = 0U; index < byte_count; ++index) {
  auto next = byte();
  if (!next) { return std::unexpected(next.error()); }
  value = (value << 8U) | std::to_integer<std::uint8_t>(*next);
 }
 if (head_size(value) != byte_count + 1U) { return std::unexpected(error(ErrorCode::NonMinimal)); }
 return value;
}
std::expected<std::size_t, DecodeError> Reader::size_argument(const std::uint8_t additional) {
 auto encoded = argument(additional);
 if (!encoded) return std::unexpected(encoded.error());
 if (*encoded > std::numeric_limits<std::size_t>::max()) { return std::unexpected(error(ErrorCode::Overflow)); }
 return static_cast<std::size_t>(*encoded);
}
ByteSegments Reader::payload_ranges(const std::size_t count) const noexcept {
 if (offset_ >= input_.first.size()) return {.first = input_.second.subspan(offset_ - input_.first.size(), count)};
 const auto first_count = std::min(count, input_.first.size() - offset_);
 return {.first = input_.first.subspan(offset_, first_count), .second = input_.second.first(count - first_count)};
}
std::expected<ByteBuffer, DecodeError> Reader::bytes(const std::size_t count) {
 if (count > input_.size() - offset_) { return std::unexpected(error(ErrorCode::UnexpectedEof)); }
 if (count > limits_.max_bytes - offset_) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
 const auto ranges = payload_ranges(count);
 ByteBuffer result;
 result.reserve(count);
 if (!ranges.first.empty()) result.insert(result.end(), ranges.first.begin(), ranges.first.end());
 if (!ranges.second.empty()) result.insert(result.end(), ranges.second.begin(), ranges.second.end());
 offset_ += count;
 return result;
}
std::expected<std::string, DecodeError> Reader::text(const std::size_t count) {
 if (count > input_.size() - offset_) { return std::unexpected(error(ErrorCode::UnexpectedEof)); }
 if (count > limits_.max_bytes - offset_) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
 const auto ranges = payload_ranges(count);
 std::string result;
 result.reserve(count);
 if (!ranges.first.empty()) result.append(reinterpret_cast<const char*>(ranges.first.data()), ranges.first.size());
 if (!ranges.second.empty()) result.append(reinterpret_cast<const char*>(ranges.second.data()), ranges.second.size());
 offset_ += count;
 if (!mmltk::common::types::valid_utf8(result)) { return std::unexpected(error(ErrorCode::InvalidUtf8)); }
 return result;
}
bool Reader::allocation_allowed(const AllocationKind kind, const std::size_t size) const noexcept {
 if (limits_.allocation_policy.allows == nullptr) { return true; }
 return limits_.allocation_policy.allows(limits_.allocation_policy.context, AllocationRequest{.path = path_, .kind = kind, .size = size});
}
std::expected<Reader::ItemHead, DecodeError> Reader::item_head(const std::size_t depth) {
 if (input_.size() > limits_.max_bytes) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
 if (depth > limits_.max_depth) { return std::unexpected(error(ErrorCode::DepthExceeded)); }
 if (items_ >= limits_.max_items) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
 ++items_;
 auto lead = byte();
 if (!lead) { return std::unexpected(lead.error()); }
 const std::uint8_t initial = std::to_integer<std::uint8_t>(*lead);
 return ItemHead{.initial = initial, .major = static_cast<std::uint8_t>(initial >> 5U), .additional = static_cast<std::uint8_t>(initial & 31U)};
}
std::expected<FlatValue, DecodeError> Reader::read_flat_scalar(const ItemHead head, const bool apply_allocation_policy, const bool materialize) {
 if (head.major == 7U) {
  switch (head.initial) {
   case 0xf4U: return FlatValue(false);
   case 0xf5U: return FlatValue(true);
   case 0xf6U: return FlatValue();
   case 0xf9U: {
    auto first = byte();
    if (!first) { return std::unexpected(first.error()); }
    auto second = byte();
    if (!second) { return std::unexpected(second.error()); }
    const auto bits = static_cast<std::uint16_t>((std::to_integer<std::uint8_t>(*first) << 8U) | std::to_integer<std::uint8_t>(*second));
    const double value = half_to_float(bits);
    if (!std::isfinite(value)) { return std::unexpected(error(ErrorCode::InvalidFloat)); }
    return FlatValue(value);
   }
   case 0xfaU: {
    std::uint32_t bits = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
     auto next = byte();
     if (!next) { return std::unexpected(next.error()); }
     bits = (bits << 8U) | std::to_integer<std::uint8_t>(*next);
    }
    const float narrowed = std::bit_cast<float>(bits);
    if (!std::isfinite(narrowed)) { return std::unexpected(error(ErrorCode::InvalidFloat)); }
    if (half_to_float(float_to_half(narrowed)) == narrowed) { return std::unexpected(error(ErrorCode::NonMinimal)); }
    return FlatValue(static_cast<double>(narrowed));
   }
   case 0xfbU: {
    std::uint64_t bits = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
     auto next = byte();
     if (!next) { return std::unexpected(next.error()); }
     bits = (bits << 8U) | std::to_integer<std::uint8_t>(*next);
    }
    const double value = std::bit_cast<double>(bits);
    if (!std::isfinite(value)) { return std::unexpected(error(ErrorCode::InvalidFloat)); }
    if (static_cast<double>(static_cast<float>(value)) == value) { return std::unexpected(error(ErrorCode::NonMinimal)); }
    return FlatValue(value);
   }
   default: return std::unexpected(error(ErrorCode::InvalidMajorType));
  }
 }
 if (head.major == 4U || head.major == 5U) { return std::unexpected(error(ErrorCode::TypeMismatch)); }
 if (head.major != 0U && head.major != 1U && head.major != 2U && head.major != 3U) { return std::unexpected(error(ErrorCode::InvalidMajorType)); }
 auto encoded_argument = argument(head.additional);
 if (!encoded_argument) { return std::unexpected(encoded_argument.error()); }
 if (head.major == 0U) { return FlatValue(*encoded_argument); }
 if (head.major == 1U) {
  if (*encoded_argument > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) { return std::unexpected(error(ErrorCode::Overflow)); }
  return FlatValue(-1 - static_cast<std::int64_t>(*encoded_argument));
 }
 if (*encoded_argument > std::numeric_limits<std::size_t>::max()) { return std::unexpected(error(ErrorCode::Overflow)); }
 const std::size_t count = static_cast<std::size_t>(*encoded_argument);
 if (!materialize) {
  if (count > input_.size() - offset_) { return std::unexpected(error(ErrorCode::UnexpectedEof)); }
  if (count > limits_.max_bytes - offset_) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
  if (head.major == 3U) {
   const auto* data = reinterpret_cast<const char*>(input_.first.data() + offset_);
   if (!mmltk::common::types::valid_utf8(std::string_view{data, count})) { return std::unexpected(error(ErrorCode::InvalidUtf8)); }
  }
  offset_ += count;
  return FlatValue{};
 }
 const AllocationKind allocation_kind = head.major == 2U ? AllocationKind::Bytes : AllocationKind::Text;
 if (apply_allocation_policy && !allocation_allowed(allocation_kind, count)) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
 const auto as_flat = []<class T>(std::expected<T, DecodeError> value) -> std::expected<FlatValue, DecodeError> {
  return value ? std::expected<FlatValue, DecodeError>(FlatValue(std::move(*value))) : std::unexpected(value.error());
 };
 return head.major == 2U ? as_flat(bytes(count)) : as_flat(text(count));
}
std::expected<std::size_t, DecodeError> Reader::begin_container_item(const std::size_t depth, const std::uint8_t expected_major,
                                                                     const AllocationKind allocation_kind) {
 auto head = item_head(depth);
 if (!head) { return std::unexpected(head.error()); }
 if (head->major != expected_major) { return std::unexpected(error(ErrorCode::TypeMismatch)); }
 auto count = size_argument(head->additional);
 if (!count) return std::unexpected(count.error());
 if (!allocation_allowed(allocation_kind, *count)) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
 if (expected_major == 4U) {
  if (!fits_remaining(*count, items_, limits_.max_items)) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
 } else {
  constexpr std::size_t kMapEntriesItemCount = 2U;
  if (*count > (limits_.max_items - items_) / kMapEntriesItemCount) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
 }
 return *count;
}
std::expected<std::size_t, DecodeError> Reader::begin_array_item(const std::size_t depth) { return begin_container_item(depth, 4U, AllocationKind::Sequence); }
std::expected<std::size_t, DecodeError> Reader::begin_object_item(const std::size_t depth) { return begin_container_item(depth, 5U, AllocationKind::Object); }
std::expected<std::string, DecodeError> Reader::read_object_key(const std::size_t depth) {
 auto scalar = read_scalar_item(depth);
 if (!scalar) { return std::unexpected(scalar.error()); }
 auto key = std::get_if<std::string>(&scalar->storage);
 if (key == nullptr) { return std::unexpected(error(ErrorCode::TypeMismatch)); }
 return std::move(*key);
}
std::expected<ByteSegments, DecodeError> Reader::borrow_bytes_item(const std::size_t depth) { return borrow_string_item(depth, 2U); }
std::expected<ByteSegments, DecodeError> Reader::borrow_string_item(const std::size_t depth, const std::uint8_t major) {
 auto head = item_head(depth);
 if (!head) return std::unexpected(head.error());
 if (head->major != major) return std::unexpected(error(ErrorCode::TypeMismatch));
 auto count = size_argument(head->additional);
 if (!count) return std::unexpected(count.error());
 if (*count > input_.size() - offset_) return std::unexpected(error(ErrorCode::UnexpectedEof));
 if (offset_ > limits_.max_bytes || *count > limits_.max_bytes - offset_) return std::unexpected(error(ErrorCode::LimitExceeded));
 const auto result = payload_ranges(*count);
 offset_ += *count;
 return result;
}
std::expected<std::size_t, DecodeError> Reader::read_text_choice(const std::size_t depth, const std::span<const std::string_view> choices) {
 auto bytes = borrow_string_item(depth, 3U);
 if (!bytes) return std::unexpected(bytes.error());
 for (std::size_t index = 0U; index < choices.size(); ++index) {
  if (choices[index].size() != bytes->size()) continue;
  const auto expected = std::as_bytes(std::span(choices[index].data(), choices[index].size()));
  if (std::equal(bytes->first.begin(), bytes->first.end(), expected.begin()) &&
      std::equal(bytes->second.begin(), bytes->second.end(), expected.begin() + static_cast<std::ptrdiff_t>(bytes->first.size())))
   return index;
 }
 return std::unexpected(error(ErrorCode::UnknownKey));
}
std::expected<void, DecodeError> Reader::expect_text_item(const std::size_t depth, const std::string_view expected) {
 auto head = item_head(depth);
 if (!head) return std::unexpected(head.error());
 if (head->major != 3U) return std::unexpected(error(ErrorCode::TypeMismatch));
 auto count = size_argument(head->additional);
 if (!count) return std::unexpected(count.error());
 if (*count != expected.size()) return std::unexpected(error(ErrorCode::TypeMismatch));
 for (const unsigned char character : expected) {
  auto encoded = byte();
  if (!encoded) return std::unexpected(encoded.error());
  if (std::to_integer<unsigned char>(*encoded) != character) return std::unexpected(error(ErrorCode::TypeMismatch));
 }
 return {};
}
std::expected<Reader::TextRange, DecodeError> Reader::read_structural_object_key(const std::size_t depth) {
 const std::size_t item_offset = offset_;
 auto head = item_head(depth);
 if (!head) { return std::unexpected(head.error()); }
 if (head->major != 3U) { return std::unexpected(error(ErrorCode::TypeMismatch)); }
 auto count = size_argument(head->additional);
 if (!count) return std::unexpected(count.error());
 if (*count > input_.size() - offset_) { return std::unexpected(error(ErrorCode::UnexpectedEof)); }
 if (offset_ > limits_.max_bytes || *count > limits_.max_bytes - offset_) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
 const std::size_t data_offset = offset_;
 const auto* const data = reinterpret_cast<const char*>(input_.first.data() + data_offset);
 if (!mmltk::common::types::valid_utf8(std::string_view{data, *count})) { return std::unexpected(error(ErrorCode::InvalidUtf8)); }
 offset_ += *count;
 return TextRange{.item_offset = item_offset, .data_offset = data_offset, .size = *count};
}
std::expected<void, DecodeError> Reader::insert_structural_object_key(const TextRange key, const std::span<std::uint32_t> table) {
 if (table.empty() || key.item_offset >= std::numeric_limits<std::uint32_t>::max()) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
 const ByteView key_bytes = input_.first.subspan(key.data_offset, key.size);
 const std::uintptr_t salt = reinterpret_cast<std::uintptr_t>(input_.first.data()) ^ reinterpret_cast<std::uintptr_t>(table.data());
 std::size_t slot = static_cast<std::size_t>(structural_key_hash(key_bytes, salt) % table.size());
 for (std::size_t probe_count = 0U; probe_count < table.size(); ++probe_count) {
  const std::uint32_t stored = table[slot];
  if (stored == 0U) {
   table[slot] = static_cast<std::uint32_t>(key.item_offset + 1U);
   return {};
  }
  Reader previous(input_, limits_);
  previous.offset_ = static_cast<std::size_t>(stored - 1U);
  auto previous_key = previous.read_structural_object_key(0U);
  if (!previous_key) { return std::unexpected(error(ErrorCode::MalformedItem)); }
  const ByteView previous_bytes = input_.first.subspan(previous_key->data_offset, previous_key->size);
  if (std::ranges::equal(key_bytes, previous_bytes)) { return std::unexpected(error(ErrorCode::DuplicateKey)); }
  slot = slot + 1U == table.size() ? 0U : slot + 1U;
 }
 return std::unexpected(error(ErrorCode::LimitExceeded));
}
std::expected<FlatValue, DecodeError> Reader::read_scalar_item(const std::size_t depth) {
 auto head = item_head(depth);
 if (!head) { return std::unexpected(head.error()); }
 return read_flat_scalar(*head, true);
}
// CLEANUP-IGNORE: Both readers use item_head; scalar admission and bounded flat-array decoding have different bodies.
std::expected<FlatValue, DecodeError> Reader::read_flat_item(const std::size_t depth) {
 auto head = item_head(depth);
 if (!head) { return std::unexpected(head.error()); }
 if (head->major != 4U) { return read_flat_scalar(*head, true); }
 auto count = size_argument(head->additional);
 if (!count) return std::unexpected(count.error());
 if (!allocation_allowed(AllocationKind::Sequence, *count) || !fits_remaining(*count, items_, limits_.max_items)) {
  return std::unexpected(error(ErrorCode::LimitExceeded));
 }
 FlatValue::Array result;
 result.reserve(*count);
 for (std::size_t index = 0U; index < *count; ++index) {
  auto item = item_head(depth + 1U);
  if (!item) { return std::unexpected(item.error()); }
  auto scalar = read_flat_scalar(*item, true);
  if (!scalar) { return std::unexpected(scalar.error()); }
  auto flat_scalar = std::visit(
   []<class T>(T&& leaf) -> std::optional<FlatValue::Scalar> {
    if constexpr (std::is_same_v<std::remove_cvref_t<T>, FlatValue::Array>) {
     return std::nullopt;
    } else {
     return FlatValue::Scalar(std::forward<T>(leaf));
    }
   },
   std::move(scalar->storage));
  if (!flat_scalar) { return std::unexpected(error(ErrorCode::TypeMismatch)); }
  result.push_back(std::move(*flat_scalar));
 }
 return FlatValue(FlatValue::Storage(std::move(result)));
}
std::expected<FlatValue, DecodeError> Reader::read_flat() { return read_document(&Reader::read_flat_item); }
std::expected<void, DecodeError> Reader::finish() {
 if (offset_ != input_.size()) { return std::unexpected(error(ErrorCode::TrailingData)); }
 return {};
}
std::expected<Value, DecodeError> Reader::read_item(const std::size_t depth, const bool apply_allocation_policy, const bool materialize) {
 auto head = item_head(depth);
 if (!head) { return std::unexpected(head.error()); }
 if (head->major != 4U && head->major != 5U) {
  auto scalar = read_flat_scalar(*head, apply_allocation_policy, materialize);
  if (!scalar) { return std::unexpected(scalar.error()); }
  if (!materialize) { return Value{}; }
  return std::move(*scalar).visit([]<class T>(T&& leaf) -> Value {
   if constexpr (std::is_same_v<std::remove_cvref_t<T>, FlatValue::Array>) {
    return Value{};
   } else {
    return Value(std::forward<T>(leaf));
   }
  });
 }
 auto count = size_argument(head->additional);
 if (!count) return std::unexpected(count.error());
 if (head->major == 4U) {
  if (materialize && apply_allocation_policy && !allocation_allowed(AllocationKind::Sequence, *count)) {
   return std::unexpected(error(ErrorCode::LimitExceeded));
  }
  if (!fits_remaining(*count, items_, limits_.max_items)) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
  Value::Array values;
  if (materialize) { values.reserve(*count); }
  for (std::size_t index = 0U; index < *count; ++index) {
   auto value = read_item(depth + 1U, apply_allocation_policy, materialize);
   if (!value) { return std::unexpected(value.error()); }
   if (materialize) { values.push_back(std::move(*value)); }
  }
  return materialize ? Value(std::move(values)) : Value{};
 }
 if (head->major == 5U) {
  if (materialize && apply_allocation_policy && !allocation_allowed(AllocationKind::Object, *count)) {
   return std::unexpected(error(ErrorCode::LimitExceeded));
  }
  constexpr std::size_t kMapEntriesItemCount = 2U;
  if (*count > (limits_.max_items - items_) / kMapEntriesItemCount) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
  if (!materialize) {
   if (structural_scratch_cursor_ > structural_scratch_.key_offsets.size()) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
   const std::size_t available = structural_scratch_.key_offsets.size() - structural_scratch_cursor_;
   if (*count > available / kMapEntriesItemCount) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
   const std::size_t previous_cursor = structural_scratch_cursor_;
   const std::size_t table_size = *count * kMapEntriesItemCount;
   auto table = structural_scratch_.key_offsets.subspan(previous_cursor, table_size);
   std::ranges::fill(table, 0U);
   structural_scratch_cursor_ += table_size;
   for (std::size_t index = 0U; index < *count; ++index) {
    auto key = read_structural_object_key(depth + 1U);
    if (!key) { return std::unexpected(key.error()); }
    auto inserted = insert_structural_object_key(*key, table);
    if (!inserted) { return std::unexpected(inserted.error()); }
    auto value = read_item(depth + 1U, false, false);
    if (!value) { return std::unexpected(value.error()); }
   }
   structural_scratch_cursor_ = previous_cursor;
   return Value{};
  }
  Value::Object object;
  object.reserve(*count);
  // Reserve first so the string views retained by the set always point
  // at the final object-owned key storage rather than a transient copy.
  std::set<std::string_view, std::less<>> names;
  std::string_view object_kind;
  for (std::size_t index = 0U; index < *count; ++index) {
   // Object keys are allocation-bearing dynamic text too.  Apply the
   // reflected declaration budget before constructing their storage.
   auto name = read_object_key(depth + 1U);
   if (!name) { return std::unexpected(name.error()); }
   object.emplace_back(std::move(*name), Value{});
   if (!names.insert(object.back().first).second) { return std::unexpected(error(ErrorCode::DuplicateKey)); }
   path_.push_back({.name = object.back().first, .object_kind = object_kind});
   auto value = read_item(depth + 1U);
   path_.pop_back();
   if (!value) { return std::unexpected(value.error()); }
   object.back().second = std::move(*value);
   if (object.back().first == "kind") {
    if (const auto* kind = std::get_if<std::string>(&object.back().second.storage)) { object_kind = *kind; }
   }
  }
  return Value(std::move(object));
 }
 return std::unexpected(error(ErrorCode::InvalidMajorType));
}
std::expected<Value, DecodeError> Reader::read() { return read_document(&Reader::read_value_item); }
std::expected<void, DecodeError> Reader::validate_structural() {
 if (!input_.second.empty()) { return std::unexpected(error(ErrorCode::MalformedItem)); }
 if (input_.size() > limits_.max_bytes) { return std::unexpected(error(ErrorCode::LimitExceeded)); }
 if (input_.size() >= std::numeric_limits<std::uint32_t>::max() || structural_scratch_.key_offsets.size() < limits_.max_items) {
  return std::unexpected(error(ErrorCode::LimitExceeded));
 }
 structural_scratch_cursor_ = 0U;
 auto result = read_item(0U, false, false);
 if (!result) { return std::unexpected(result.error()); }
 return finish();
}
std::expected<Value, DecodeError> Reader::read_value_item(const std::size_t depth) { return read_item(depth); }
std::size_t Writer::destination_size() const noexcept { return dynamic_destination_ != nullptr ? dynamic_destination_->size() : fixed_size_; }
std::size_t Writer::bytes_written() const noexcept { return destination_size(); }
void Writer::truncate(const std::size_t size) noexcept {
 if (dynamic_destination_ != nullptr) {
  dynamic_destination_->resize(size);
 } else {
  fixed_size_ = size;
 }
}
std::expected<void, EncodeError> Writer::append(const ByteView bytes) {
 const std::size_t size = destination_size();
 if (size > limits_.max_bytes || bytes.size() > limits_.max_bytes - size) { return std::unexpected(encode_error()); }
 if (count_only_) {
  fixed_size_ += bytes.size();
 } else if (dynamic_destination_ != nullptr) {
  dynamic_destination_->insert(dynamic_destination_->end(), bytes.begin(), bytes.end());
 } else {
  if (size > fixed_destination_.size() || bytes.size() > fixed_destination_.size() - size) { return std::unexpected(encode_error()); }
  std::copy(bytes.begin(), bytes.end(), fixed_destination_.begin() + static_cast<std::ptrdiff_t>(size));
  fixed_size_ += bytes.size();
 }
 return {};
}
std::expected<void, EncodeError> Writer::put(const std::byte value) {
 const std::array bytes{value};
 auto result = append(bytes);
 if (!result) return result;
 return {};
}
std::expected<void, EncodeError> Writer::head(const std::uint8_t major, const std::uint64_t argument_value) {
 const std::size_t encoded_bytes = head_size(argument_value);
 const std::uint8_t additional = encoded_bytes == 1U ? static_cast<std::uint8_t>(argument_value) : additional_info_for_head_size(encoded_bytes);
 auto result = put(std::byte((major << 5U) | additional));
 if (!result) { return result; }
 for (std::size_t byte_index = encoded_bytes - 1U; byte_index > 0U; --byte_index) {
  result = put(std::byte((argument_value >> ((byte_index - 1U) * 8U)) & 0xffU));
  if (!result) { return result; }
 }
 return {};
}
std::expected<void, EncodeError> Writer::write_item(const Value& value, const std::size_t depth) {
 auto begun = begin_encode_item(depth, items_, limits_);
 if (!begun) return begun;
 return std::visit(
  [this, depth](const auto& item) -> std::expected<void, EncodeError> {
   using T = std::decay_t<decltype(item)>;
   if constexpr (std::is_same_v<T, std::monostate>) {
    return put(std::byte{0xf6});
   } else if constexpr (std::is_same_v<T, bool>) {
    return put(item ? std::byte{0xf5} : std::byte{0xf4});
   } else if constexpr (std::is_same_v<T, std::int64_t>) {
    return item >= 0 ? head(0U, static_cast<std::uint64_t>(item)) : head(1U, static_cast<std::uint64_t>(-(item + 1)));
   } else if constexpr (std::is_same_v<T, std::uint64_t>) {
    return head(0U, item);
   } else if constexpr (std::is_same_v<T, double>) {
    const auto encoding = canonical_float_encoding(item);
    if (!encoding) return std::unexpected(encode_error(encoding.error()));
    const std::size_t width = static_cast<std::size_t>(encoding->width);
    auto result = put(width == 2U ? std::byte{0xf9} : width == 4U ? std::byte{0xfa} : std::byte{0xfb});
    if (!result) { return result; }
    for (std::size_t index = width; index-- > 0U;) {
     result = put(std::byte((encoding->bits >> (index * 8U)) & 0xffU));
     if (!result) { return result; }
    }
    return {};
   } else if constexpr (std::is_same_v<T, std::string>) {
    if (!mmltk::common::types::valid_utf8(item)) { return std::unexpected(encode_error(ErrorCode::InvalidUtf8)); }
    auto result = head(3U, item.size());
    if (!result) { return result; }
    return append(std::as_bytes(std::span<const char>{item.data(), item.size()}));
   } else if constexpr (std::is_same_v<T, ByteBuffer>) {
    auto result = head(2U, item.size());
    if (!result) { return result; }
    return append(item);
   } else if constexpr (std::is_same_v<T, Value::Array>) {
    const bool substitute = raw_array_.target == &item;
    const std::size_t element_count = substitute ? raw_array_.items.size() : item.size();
    if (!fits_remaining(element_count, items_, limits_.max_items)) { return std::unexpected(encode_error(ErrorCode::LimitExceeded)); }
    auto result = head(4U, element_count);
    if (!result) { return result; }
    if (substitute) {
     return encode_each(raw_array_.items, [this, depth](const ByteSegments encoded) { return append_raw_item(encoded, depth + 1U); });
    }
    return encode_each(item, [this, depth](const Value& element) { return write_item(element, depth + 1U); });
   } else {
    auto valid = validate_object_encoding(item, items_, limits_);
    if (!valid) return valid;
    auto result = head(5U, item.size());
    if (!result) { return result; }
    return encode_each(item, [this, depth](const auto& entry) {
     auto encoded = write_item(Value(entry.first), depth + 1U);
     if (!encoded) return encoded;
     return write_item(entry.second, depth + 1U);
    });
   }
  },
  value.storage);
}
std::expected<void, EncodeError> Writer::write(const Value& value) {
 const std::size_t initial_size = destination_size();
 const std::size_t initial_items = items_;
 auto result = write_item(value, 0U);
 if (!result) {
  truncate(initial_size);
  items_ = initial_items;
 }
 return result;
}
std::expected<void, EncodeError> Writer::append_raw_item(const ByteSegments item) { return append_raw_item(item, 0U); }
std::expected<void, EncodeError> Writer::append_raw_item(const ByteSegments item, const std::size_t depth) {
 if (depth > limits_.max_depth) { return std::unexpected(encode_error(ErrorCode::DepthExceeded)); }
 const Limits raw_limits{
  .max_bytes = item.size(),
  .max_items = items_ <= limits_.max_items ? limits_.max_items - items_ : 0U,
  .max_depth = limits_.max_depth - depth,
 };
 Reader reader(item, raw_limits);
 auto decoded = reader.read();
 if (!decoded) { return std::unexpected(EncodeError{.code = decoded.error().code, .offset = decoded.error().offset, .path = decoded.error().path}); }
 auto appended = append(item.first);
 if (!appended) return appended;
 appended = append(item.second);
 if (!appended) {
  truncate(destination_size() - item.first.size());
  return appended;
 }
 items_ += reader.items_read();
 return {};
}
std::expected<std::size_t, EncodeError> CountingEncoder::measure(const Value& value) {
 Writer writer{limits_, raw_array_};
 auto result = writer.write(value);
 return result ? std::expected<std::size_t, EncodeError>{writer.bytes_written()} : std::unexpected(result.error());
}
std::expected<Value, DecodeError> decode(const ByteSegments bytes, const Limits limits) { return Reader(bytes, limits).read(); }
std::expected<void, DecodeError> validate_raw_item(const ByteSegments bytes, const Limits limits) {
 auto result = decode(bytes, limits);
 if (!result) { return std::unexpected(result.error()); }
 return {};
}
std::expected<void, DecodeError> validate_raw_item_structural(const ByteView bytes, const Limits limits, const StructuralValidationScratch scratch) {
 return Reader({.first = bytes}, limits, scratch).validate_structural();
}
std::expected<void, EncodeError> encode(const Value& value, ByteBuffer& destination, const Limits limits) { return encode(value, {}, destination, limits); }
std::expected<void, EncodeError> encode(const Value& value, const RawArrayItems raw_array, ByteBuffer& destination, const Limits limits) {
 auto measured = CountingEncoder(limits, raw_array).measure(value);
 if (!measured) { return std::unexpected(measured.error()); }
 destination.clear();
 if (destination.capacity() < *measured) { destination.reserve(*measured); }
 return Writer(destination, limits, raw_array).write(value);
}
std::expected<std::size_t, EncodeError> encode(const Value& value, const std::span<std::byte> destination, const Limits limits) {
 auto measured = CountingEncoder(limits).measure(value);
 if (!measured) return std::unexpected(measured.error());
 if (*measured > destination.size()) { return std::unexpected(encode_error(ErrorCode::LimitExceeded)); }
 Writer writer(destination, limits);
 auto written = writer.write(value);
 if (!written) return std::unexpected(written.error());
 return writer.bytes_written();
}
}  // namespace mmltk::frameworks::serialization::wire
