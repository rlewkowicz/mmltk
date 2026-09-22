#pragma once
#include <array>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
namespace mmltk::frameworks::serialization {
template <typename T>
 requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
[[nodiscard]] T decode_json_integer_exact(const nlohmann::json& value) {
 using JsonSigned = nlohmann::json::number_integer_t;
 using JsonUnsigned = nlohmann::json::number_unsigned_t;
 if (value.is_number_unsigned()) {
  const JsonUnsigned source = value.get_ref<const JsonUnsigned&>();
  if constexpr (std::is_signed_v<T>) {
   if constexpr (std::numeric_limits<T>::digits < std::numeric_limits<JsonUnsigned>::digits) {
    if (source > static_cast<JsonUnsigned>(std::numeric_limits<T>::max())) { throw std::runtime_error("JSON unsigned integer is outside the signed target range"); }
   }
  } else if constexpr (std::numeric_limits<T>::digits < std::numeric_limits<JsonUnsigned>::digits) {
   if (source > static_cast<JsonUnsigned>(std::numeric_limits<T>::max())) { throw std::runtime_error("JSON unsigned integer is outside the target range"); }
  }
  return static_cast<T>(source);
 }
 if (value.is_number_integer()) {
  const JsonSigned source = value.get_ref<const JsonSigned&>();
  if constexpr (std::is_signed_v<T>) {
   if constexpr (std::numeric_limits<T>::digits < std::numeric_limits<JsonSigned>::digits) {
    if (source < static_cast<JsonSigned>(std::numeric_limits<T>::min()) || source > static_cast<JsonSigned>(std::numeric_limits<T>::max())) {
     throw std::runtime_error("JSON signed integer is outside the target range");
    }
   }
  } else {
   if (source < 0) throw std::runtime_error("JSON negative integer cannot decode as unsigned");
   using UnsignedJsonSigned = std::make_unsigned_t<JsonSigned>;
   const UnsignedJsonSigned nonnegative = static_cast<UnsignedJsonSigned>(source);
   if constexpr (std::numeric_limits<T>::digits < std::numeric_limits<UnsignedJsonSigned>::digits) {
    if (nonnegative > static_cast<UnsignedJsonSigned>(std::numeric_limits<T>::max())) { throw std::runtime_error("JSON signed integer is outside the unsigned target range"); }
   }
  }
  return static_cast<T>(source);
 }
 throw std::runtime_error("integral field must decode from an integer JSON number");
}
template <typename T>
 requires std::is_floating_point_v<T>
[[nodiscard]] T decode_json_floating_exact(const nlohmann::json& value) {
 long double source = 0.0L;
 if (value.is_number_unsigned()) {
  source = static_cast<long double>(value.get_ref<const nlohmann::json::number_unsigned_t&>());
 } else if (value.is_number_integer()) {
  source = static_cast<long double>(value.get_ref<const nlohmann::json::number_integer_t&>());
 } else if (value.is_number_float()) {
  const auto stored = value.get_ref<const nlohmann::json::number_float_t&>();
  if (!std::isfinite(stored)) throw std::runtime_error("floating JSON field must be finite");
  source = static_cast<long double>(stored);
 } else {
  throw std::runtime_error("floating field must decode from a JSON number");
 }
 if (source < static_cast<long double>(std::numeric_limits<T>::lowest()) || source > static_cast<long double>(std::numeric_limits<T>::max())) {
  throw std::runtime_error("floating JSON number is outside the target range");
 }
 const T decoded = static_cast<T>(source);
 if (!std::isfinite(decoded)) throw std::runtime_error("floating JSON field must be finite");
 return decoded;
}
template <typename T>
struct JsonOptional : std::false_type {};
template <typename T>
struct JsonOptional<std::optional<T>> : std::true_type {};
template <typename T>
struct JsonVector : std::false_type {};
template <typename T, typename Allocator>
struct JsonVector<std::vector<T, Allocator>> : std::true_type {};
template <typename T>
struct JsonArray : std::false_type {};
template <typename T, std::size_t Size>
struct JsonArray<std::array<T, Size>> : std::true_type {};
template <typename T>
void decode_json_value_exact(const nlohmann::json& value, T& out) {
 using U = std::remove_cvref_t<T>;
 if constexpr (std::is_same_v<U, bool>) {
  if (!value.is_boolean()) throw std::runtime_error("boolean field must decode from a JSON boolean");
  out = value.template get<bool>();
 } else if constexpr (std::is_integral_v<U>) {
  out = decode_json_integer_exact<U>(value);
 } else if constexpr (std::is_floating_point_v<U>) {
  out = decode_json_floating_exact<U>(value);
 } else if constexpr (JsonOptional<U>::value) {
  if (value.is_null()) {
   out.reset();
  } else {
   typename U::value_type decoded{};
   decode_json_value_exact(value, decoded);
   out = std::move(decoded);
  }
 } else if constexpr (JsonVector<U>::value) {
  if (!value.is_array()) throw std::runtime_error("sequence field must decode from a JSON array");
  U decoded;
  decoded.reserve(value.size());
  for (const auto& item : value) {
   typename U::value_type element{};
   decode_json_value_exact(item, element);
   decoded.push_back(std::move(element));
  }
  out = std::move(decoded);
 } else if constexpr (JsonArray<U>::value) {
  if (!value.is_array() || value.size() != std::tuple_size_v<U>) { throw std::runtime_error("fixed sequence field has the wrong JSON extent"); }
  U decoded{};
  for (std::size_t index = 0U; index < decoded.size(); ++index) { decode_json_value_exact(value.at(index), decoded[index]); }
  out = std::move(decoded);
 } else {
  U decoded = out;
  value.get_to(decoded);
  out = std::move(decoded);
 }
}
}  // namespace mmltk::frameworks::serialization
