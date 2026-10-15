#pragma once
#include <cstdint>
#if !defined(__CUDACC__)
#include "src/frameworks/reflection/reflection_metadata.h"
#endif
namespace mmltk::backend::data::catalog {
enum class KeypointVisibility : std::uint8_t { Unannotated = 0U, Occluded = 1U, Visible = 2U };
#if !defined(__CUDACC__)
MMLTK_REFLECT_ENUM(KeypointVisibility)
#endif
}  // namespace mmltk::backend::data::catalog
