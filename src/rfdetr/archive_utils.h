#pragma once

#include <torch/serialize.h>
#include <ATen/core/ivalue.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mmltk::rfdetr {

inline std::string archive_entry_name(std::size_t index) {
    return std::format("entry_{:06}", index);
}

inline std::string archive_entry_name(const char* prefix, std::size_t index) {
    return std::format("{}_{:06}", prefix, index);
}

inline void write_string(torch::serialize::OutputArchive& archive, const char* key, std::string_view value) {
    archive.write(key, c10::IValue(std::string(value)));
}

inline void write_int(torch::serialize::OutputArchive& archive, const char* key, int64_t value) {
    archive.write(key, c10::IValue(value));
}

inline void write_bool(torch::serialize::OutputArchive& archive, const char* key, bool value) {
    archive.write(key, c10::IValue(value));
}

inline void write_double(torch::serialize::OutputArchive& archive, const char* key, double value) {
    archive.write(key, c10::IValue(value));
}

inline void write_optional_bool(torch::serialize::OutputArchive& archive, const char* key,
                                const std::optional<bool>& value) {
    if (value.has_value())
        write_bool(archive, key, *value);
}

inline void write_optional_int(torch::serialize::OutputArchive& archive, const char* key,
                               const std::optional<int64_t>& value) {
    if (value.has_value())
        write_int(archive, key, *value);
}

inline void write_optional_double(torch::serialize::OutputArchive& archive, const char* key,
                                  const std::optional<double>& value) {
    if (value.has_value())
        write_double(archive, key, *value);
}

inline std::optional<std::string> read_optional_string(torch::serialize::InputArchive& archive, const char* key) {
    c10::IValue value;
    if (!archive.try_read(key, value))
        return std::nullopt;
    if (!value.isString())
        throw std::runtime_error(std::format("archive key is not a string: {}", key));
    return std::string(value.toStringRef());
}

inline std::optional<int64_t> read_optional_int(torch::serialize::InputArchive& archive, const char* key) {
    c10::IValue value;
    if (!archive.try_read(key, value))
        return std::nullopt;
    if (!value.isInt())
        throw std::runtime_error(std::format("archive key is not an int: {}", key));
    return value.toInt();
}

inline std::optional<double> read_optional_double(torch::serialize::InputArchive& archive, const char* key) {
    c10::IValue value;
    if (!archive.try_read(key, value))
        return std::nullopt;
    if (!value.isDouble() && !value.isInt())
        throw std::runtime_error(std::format("archive key is not numeric: {}", key));
    return value.isDouble() ? value.toDouble() : static_cast<double>(value.toInt());
}

inline std::optional<bool> read_optional_bool(torch::serialize::InputArchive& archive, const char* key) {
    c10::IValue value;
    if (!archive.try_read(key, value))
        return std::nullopt;
    if (!value.isBool())
        throw std::runtime_error(std::format("archive key is not a bool: {}", key));
    return value.toBool();
}

inline std::string require_string(torch::serialize::InputArchive& archive, const char* key) {
    auto value = read_optional_string(archive, key);
    if (!value.has_value())
        throw std::runtime_error(std::format("archive is missing key: {}", key));
    return *value;
}

inline int64_t require_int(torch::serialize::InputArchive& archive, const char* key) {
    c10::IValue value;
    archive.read(key, value);
    if (!value.isInt())
        throw std::runtime_error(std::format("archive key is not an int: {}", key));
    return value.toInt();
}

inline double require_double(torch::serialize::InputArchive& archive, const char* key) {
    c10::IValue value;
    archive.read(key, value);
    if (value.isDouble())
        return value.toDouble();
    if (value.isInt())
        return static_cast<double>(value.toInt());
    throw std::runtime_error(std::format("archive key is not a number: {}", key));
}

inline torch::Tensor require_tensor(torch::serialize::InputArchive& archive, const char* key) {
    torch::Tensor tensor;
    archive.read(key, tensor);
    if (!tensor.defined())
        throw std::runtime_error(std::format("archive tensor is undefined: {}", key));
    return tensor;
}

}  // namespace mmltk::rfdetr
