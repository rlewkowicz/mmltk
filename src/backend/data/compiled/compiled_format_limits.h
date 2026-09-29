#pragma once
#include <cstdint>
#include <cstddef>
namespace mmltk::backend::data {
inline constexpr uint32_t MAX_CLASSES = 256;
inline constexpr std::size_t COMPILED_LABEL_CAPACITY = 500000;
inline constexpr std::size_t COMPILED_BLOCK_SLOTS = 64;
inline constexpr std::size_t COMPILED_METADATA_CAPACITY = 1000000000;
}  // namespace mmltk::backend::data
