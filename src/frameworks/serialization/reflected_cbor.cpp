#include "src/frameworks/serialization/reflected_cbor.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include "src/frameworks/serialization/reflected_cbor_detail.h"
namespace mmltk::frameworks::serialization::implementation::detail {
wire::EncodeError encode_error(const wire::ErrorCode code) { return {.code = code, .offset = 0U, .path = {}}; }
wire::DecodeError decode_error(const wire::ErrorCode code) { return {.code = code, .offset = 0U, .path = {}}; }
void prepend_path(wire::Error& error, const std::string_view component) {
 if (error.path.empty()) {
  error.path = component;
  return;
 }
 std::string nested;
 nested.reserve(component.size() + 1U + error.path.size());
 nested.append(component);
 nested.push_back('.');
 nested.append(error.path);
 error.path = std::move(nested);
}
bool allocation_path_names_match(const DecodeAllocationLimit& limit, const wire::AllocationRequest& request) noexcept {
 if ((limit.recursive_dynamic && limit.path.size() > request.path.size()) || (!limit.recursive_dynamic && limit.path.size() != request.path.size()))
  return false;
 for (std::size_t index = 0U; index < limit.path.size(); ++index) {
  const DecodePathSelector& expected = limit.path[index];
  const wire::DecodePathElement& actual = request.path[index];
  if (expected.name != actual.name) return false;
 }
 return true;
}
bool allocation_discriminators_match(const DecodeAllocationLimit& limit, const wire::AllocationRequest& request) noexcept {
 for (std::size_t index = 0U; index < limit.path.size(); ++index) {
  const std::string& expected = limit.path[index].object_kind;
  if (!expected.empty() && expected != request.path[index].object_kind) return false;
 }
 return true;
}
bool allocation_limit_accepts(const DecodeAllocationLimit& limit, const wire::AllocationRequest& request) noexcept {
 if ((request.kind == wire::AllocationKind::Text || request.kind == wire::AllocationKind::Bytes) && limit.max_bytes && request.size > *limit.max_bytes)
  return false;
 if ((request.kind == wire::AllocationKind::Sequence || request.kind == wire::AllocationKind::Object) && limit.max_items && request.size > *limit.max_items)
  return false;
 return true;
}
bool allocation_allowed(const void* raw_context, const wire::AllocationRequest& request) noexcept {
 const auto& context = *static_cast<const DecodePolicyContext*>(raw_context);
 if (context.upstream.allows != nullptr && !context.upstream.allows(context.upstream.context, request)) return false;
 bool has_discriminator_dependent_limit = false;
 bool matched_discriminator_dependent_limit = false;
 for (const DecodeAllocationLimit& limit : context.reflected->entries) {
  if (!allocation_path_names_match(limit, request)) continue;
  const bool discriminator_dependent = std::ranges::any_of(limit.path, [](const DecodePathSelector& selector) { return !selector.object_kind.empty(); });
  has_discriminator_dependent_limit = has_discriminator_dependent_limit || discriminator_dependent;
  if (!allocation_discriminators_match(limit, request)) continue;
  matched_discriminator_dependent_limit = matched_discriminator_dependent_limit || discriminator_dependent;
  if (!allocation_limit_accepts(limit, request)) return false;
 }
 return !has_discriminator_dependent_limit || matched_discriminator_dependent_limit;
}
}  // namespace mmltk::frameworks::serialization::implementation::detail
namespace mmltk::frameworks::serialization::implementation {
FixedCborEncoder::FixedCborEncoder(const std::span<std::byte> destination) noexcept : destination_(destination) {}
bool FixedCborEncoder::unsigned_integer(const std::uint64_t value) noexcept { return head(0U, value); }
bool FixedCborEncoder::signed_integer(const std::int64_t value) noexcept {
 return value >= 0 ? head(0U, static_cast<std::uint64_t>(value)) : head(1U, static_cast<std::uint64_t>(-(value + 1)));
}
bool FixedCborEncoder::floating(const double value) noexcept {
 const auto encoding = wire::canonical_float_encoding(value);
 if (!encoding) return false;
 const std::size_t width = static_cast<std::size_t>(encoding->width);
 return put(width == 2U ? std::byte{0xf9U} : width == 4U ? std::byte{0xfaU} : std::byte{0xfbU}) && big_endian(encoding->bits, width);
}
bool FixedCborEncoder::boolean(const bool value) noexcept { return put(value ? std::byte{0xf5U} : std::byte{0xf4U}); }
bool FixedCborEncoder::null() noexcept { return put(std::byte{0xf6U}); }
bool FixedCborEncoder::text(const std::string_view value) noexcept { return head(3U, value.size()) && append(std::as_bytes(std::span{value})); }
bool FixedCborEncoder::array(const std::size_t size) noexcept { return head(4U, size); }
bool FixedCborEncoder::object(const std::size_t size) noexcept { return head(5U, size); }
bool FixedCborEncoder::append_authorized_item(const std::span<const std::byte> encoded) noexcept { return append(encoded); }
std::size_t FixedCborEncoder::size() const noexcept { return position_; }
bool FixedCborEncoder::capacity_exceeded() const noexcept { return capacity_exceeded_; }
wire::ErrorCode FixedCborEncoder::error() const noexcept { return capacity_exceeded_ ? wire::ErrorCode::LimitExceeded : wire::ErrorCode::TypeMismatch; }
bool FixedCborEncoder::put(const std::byte value) noexcept {
 if (position_ == destination_.size()) {
  capacity_exceeded_ = true;
  return false;
 }
 destination_[position_++] = value;
 return true;
}
bool FixedCborEncoder::append(const std::span<const std::byte> value) noexcept {
 if (value.size() > destination_.size() - position_) {
  capacity_exceeded_ = true;
  return false;
 }
 std::copy(value.begin(), value.end(), destination_.begin() + static_cast<std::ptrdiff_t>(position_));
 position_ += value.size();
 return true;
}
bool FixedCborEncoder::head(const std::uint8_t major, const std::uint64_t value) noexcept {
 const std::byte prefix = static_cast<std::byte>(major << 5U);
 if (value < 24U) return put(prefix | static_cast<std::byte>(value));
 if (value <= std::numeric_limits<std::uint8_t>::max()) return put(prefix | std::byte{24U}) && put(static_cast<std::byte>(value));
 if (value <= std::numeric_limits<std::uint16_t>::max()) return put(prefix | std::byte{25U}) && big_endian(value, 2U);
 if (value <= std::numeric_limits<std::uint32_t>::max()) return put(prefix | std::byte{26U}) && big_endian(value, 4U);
 return put(prefix | std::byte{27U}) && big_endian(value, 8U);
}
bool FixedCborEncoder::big_endian(const std::uint64_t value, const std::size_t bytes) noexcept {
 if (bytes > destination_.size() - position_) {
  capacity_exceeded_ = true;
  return false;
 }
 for (std::size_t index = bytes; index != 0U; --index) destination_[position_++] = static_cast<std::byte>(value >> ((index - 1U) * 8U));
 return true;
}
}  // namespace mmltk::frameworks::serialization::implementation
