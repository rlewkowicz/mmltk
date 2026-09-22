#pragma once
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <torch/types.h>
#include <ATen/core/ivalue.h>
#include <torch/serialize.h>
namespace mmltk::backend::ml::serialization {
inline std::string archive_entry_name(std::size_t index) { return std::format("entry_{:06}", index); }
inline std::string archive_entry_name(const char* prefix, std::size_t index) { return std::format("{}_{:06}", prefix, index); }
inline void write_string(torch::serialize::OutputArchive& archive, const char* key, std::string_view value) { archive.write(key, c10::IValue(std::string(value))); }
inline void write_int(torch::serialize::OutputArchive& archive, const char* key, int64_t value) { archive.write(key, c10::IValue(value)); }
inline void write_bool(torch::serialize::OutputArchive& archive, const char* key, bool value) { archive.write(key, c10::IValue(value)); }
inline void write_double(torch::serialize::OutputArchive& archive, const char* key, double value) { archive.write(key, c10::IValue(value)); }
inline void write_optional_bool(torch::serialize::OutputArchive& archive, const char* key, const std::optional<bool>& value) {
 if (value.has_value()) write_bool(archive, key, *value);
}
inline void write_optional_int(torch::serialize::OutputArchive& archive, const char* key, const std::optional<int64_t>& value) {
 if (value.has_value()) write_int(archive, key, *value);
}
inline void write_optional_double(torch::serialize::OutputArchive& archive, const char* key, const std::optional<double>& value) {
 if (value.has_value()) write_double(archive, key, *value);
}
template <typename Value>
inline std::optional<Value> read_optional_value(torch::serialize::InputArchive& archive, const char* key) {
 static_assert(std::is_same_v<Value, int64_t> || std::is_same_v<Value, bool> || std::is_same_v<Value, double> || std::is_same_v<Value, std::string>);
 c10::IValue value;
 if (!archive.try_read(key, value)) return std::nullopt;
 const bool matches = [&] {
  if constexpr (std::is_same_v<Value, int64_t>) {
   return value.isInt();
  } else if constexpr (std::is_same_v<Value, bool>) {
   return value.isBool();
  } else if constexpr (std::is_same_v<Value, std::string>) {
   return value.isString();
  } else {
   return value.isDouble() || value.isInt();
  }
 }();
 constexpr std::string_view expected_type = [] {
  if constexpr (std::is_same_v<Value, int64_t>) {
   return std::string_view{"an int"};
  } else if constexpr (std::is_same_v<Value, bool>) {
   return std::string_view{"a bool"};
  } else if constexpr (std::is_same_v<Value, std::string>) {
   return std::string_view{"a string"};
  } else {
   return std::string_view{"numeric"};
  }
 }();
 if (!matches) throw std::runtime_error(std::format("archive key is not {}: {}", expected_type, key));
 if constexpr (std::is_same_v<Value, int64_t>) {
  return value.toInt();
 } else if constexpr (std::is_same_v<Value, bool>) {
  return value.toBool();
 } else if constexpr (std::is_same_v<Value, std::string>) {
  return std::string(value.toStringRef());
 } else {
  return value.isDouble() ? value.toDouble() : static_cast<double>(value.toInt());
 }
}
inline std::string require_string(torch::serialize::InputArchive& archive, const char* key) {
 auto value = read_optional_value<std::string>(archive, key);
 if (!value.has_value()) throw std::runtime_error(std::format("archive is missing key: {}", key));
 return *value;
}
inline int64_t require_int(torch::serialize::InputArchive& archive, const char* key) {
 c10::IValue value;
 archive.read(key, value);
 if (!value.isInt()) throw std::runtime_error(std::format("archive key is not an int: {}", key));
 return value.toInt();
}
inline double require_double(torch::serialize::InputArchive& archive, const char* key) {
 c10::IValue value;
 archive.read(key, value);
 if (value.isDouble()) return value.toDouble();
 if (value.isInt()) return static_cast<double>(value.toInt());
 throw std::runtime_error(std::format("archive key is not a number: {}", key));
}
inline torch::Tensor require_tensor(torch::serialize::InputArchive& archive, const char* key) {
 torch::Tensor tensor;
 archive.read(key, tensor);
 if (!tensor.defined()) throw std::runtime_error(std::format("archive tensor is undefined: {}", key));
 return tensor;
}
}  // namespace mmltk::backend::ml::serialization
