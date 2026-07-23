#pragma once

#include <torch/types.h>

#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mmltk::rfdetr {

inline std::string scalar_type_name(at::ScalarType scalar_type) {
    switch (scalar_type) {
        case at::kFloat:
            return "float32";
        case at::kHalf:
            return "float16";
        case at::kBFloat16:
            return "bfloat16";
        case at::kDouble:
            return "float64";
        case at::kBool:
            return "bool";
        case at::kByte:
            return "uint8";
        case at::kChar:
            return "int8";
        case at::kShort:
            return "int16";
        case at::kInt:
            return "int32";
        case at::kLong:
            return "int64";
        default:
            throw std::runtime_error(std::format("unsupported scalar type: {}", static_cast<int>(scalar_type)));
    }
}

inline at::ScalarType scalar_type_from_name(std::string_view name) {
    if (name == "float16")
        return at::kHalf;
    if (name == "bfloat16")
        return at::kBFloat16;
    if (name == "float32")
        return at::kFloat;
    if (name == "float64")
        return at::kDouble;
    if (name == "bool")
        return at::kBool;
    if (name == "uint8")
        return at::kByte;
    if (name == "int8")
        return at::kChar;
    if (name == "int16")
        return at::kShort;
    if (name == "int32")
        return at::kInt;
    if (name == "int64")
        return at::kLong;
    throw std::runtime_error(std::format("unsupported scalar type name: {}", name));
}

}  
