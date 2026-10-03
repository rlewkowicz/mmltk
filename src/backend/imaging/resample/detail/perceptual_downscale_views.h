// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include "src/backend/imaging/resample/image_resize.h"
namespace mmltk::backend::imaging::resample::perceptual {
// Requires validated positive, non-enlarging extents. Unchanged geometry uses
// integer filtering; each checked entrypoint owns its identity-copy policy.
[[nodiscard]] RgbResizePath classify_geometry(std::uint32_t source_width, std::uint32_t source_height, std::uint32_t destination_width, std::uint32_t destination_height) noexcept;
struct IdentityCopyGeometry {
 std::size_t row_bytes;
 unsigned planes;
};
IdentityCopyGeometry identity_geometry(const RgbImageLayout& layout);
// Logical byte spans only. Device allocation/context admission and copy
// execution remain with the respective CPU/CUDA owners.
std::size_t validate_view(const void* pointer, const RgbImageLayout& layout);
struct ValidatedResize {
 std::size_t source_extent, destination_extent;
 bool identity;
 explicit operator bool() const noexcept { return identity; }
};
ValidatedResize validate_pair(RgbConstImageView source, RgbMutableImageView destination, bool quantized_planar = false);
}  // namespace mmltk::backend::imaging::resample::perceptual
