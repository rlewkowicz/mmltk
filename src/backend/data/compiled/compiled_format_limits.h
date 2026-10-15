#pragma once
#include <cstdint>
#include <cstddef>
namespace mmltk::backend::data {
inline constexpr uint32_t MAX_CLASSES = 256;
// Compilation's per-image workspace/staging budget. The observed 384x384
// Coconut + Stock maximum includes instances, mask runs and alignment.
inline constexpr std::size_t COMPILED_LABEL_CAPACITY = 121824;
// Persisted format admission is independent of the current compiler budget.
inline constexpr std::size_t COMPILED_LABEL_LIMIT = 500000;
inline constexpr std::size_t COMPILED_CATALOG_LIMIT = 32U * 1024U * 1024U;
inline constexpr std::size_t COMPILED_BLOCK_SLOTS = 64;
inline constexpr std::size_t COMPILED_METADATA_CAPACITY = 1000000000;
}  // namespace mmltk::backend::data
