#include "json_wire.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
namespace mmltk::frameworks::serialization {
namespace {
using Json = nlohmann::json;
Json project_json(wire::Value value) {
 return std::visit(
  [](auto&& item) -> Json {
   using T = std::remove_cvref_t<decltype(item)>;
   if constexpr (std::is_same_v<T, std::monostate>) {
    return nullptr;
   } else if constexpr (std::is_same_v<T, wire::Value::Array>) {
    Json::array_t result;
    result.reserve(item.size());
    for (auto& child : item) result.push_back(project_json(std::move(child)));
    return result;
   } else if constexpr (std::is_same_v<T, wire::Value::Object>) {
    Json::object_t result;
    for (auto& [key, child] : item) result.emplace(std::move(key), project_json(std::move(child)));
    return result;
   } else if constexpr (std::is_same_v<T, wire::Value::Bytes>) {
    std::vector<std::uint8_t> bytes(item.size());
    std::transform(item.begin(), item.end(), bytes.begin(), [](std::byte byte) { return std::to_integer<std::uint8_t>(byte); });
    return Json::binary(std::move(bytes));
   } else {
    return std::forward<decltype(item)>(item);
   }
  },
  std::move(value.storage));
}
wire::Value project_wire(Json value) {
 switch (value.type()) {
  case Json::value_t::null: return {};
  case Json::value_t::boolean: return wire::Value(value.get<bool>());
  case Json::value_t::number_integer: return wire::Value(value.get<std::int64_t>());
  case Json::value_t::number_unsigned: return wire::Value(value.get<std::uint64_t>());
  case Json::value_t::number_float: return wire::Value(value.get<double>());
  case Json::value_t::string: return wire::Value(std::move(value.get_ref<Json::string_t&>()));
  case Json::value_t::array: {
   wire::Value::Array result;
   result.reserve(value.size());
   for (auto& child : value) result.push_back(project_wire(std::move(child)));
   return wire::Value(std::move(result));
  }
  case Json::value_t::object: {
   wire::Value::Object result;
   result.reserve(value.size());
   for (auto& [key, child] : value.items()) result.emplace_back(key, project_wire(std::move(child)));
   return wire::Value(std::move(result));
  }
  default: throw std::runtime_error("JSON record contains a non-JSON value");
 }
}
}  // namespace
nlohmann::json json_from_cbor(wire::ByteSegments bytes, wire::Limits limits) {
 auto value = wire::decode(bytes, limits);
 if (!value) throw std::runtime_error("invalid CBOR JSON projection");
 return project_json(std::move(*value));
}
wire::ByteBuffer json_to_cbor(std::string_view text, wire::Limits limits) {
 if (text.size() > limits.max_bytes) throw std::runtime_error("JSON record exceeds its byte bound");
 std::size_t items = 0U;
 auto parsed = Json::parse(text, [&](int depth, Json::parse_event_t event, Json&) {
  if (event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end) return true;
  if (static_cast<std::size_t>(depth) > std::min(limits.max_depth, wire::kMaximumNestingDepth)) throw std::runtime_error("JSON record exceeds its depth bound");
  if (items == limits.max_items) throw std::runtime_error("JSON record exceeds its item bound");
  ++items;
  return true;
 });
 auto value = project_wire(std::move(parsed));
 wire::ByteBuffer bytes;
 bytes.reserve(text.size());
 if (!wire::encode(value, bytes, limits)) throw std::runtime_error("JSON record violates native wire constraints");
 return bytes;
}
}  // namespace mmltk::frameworks::serialization
