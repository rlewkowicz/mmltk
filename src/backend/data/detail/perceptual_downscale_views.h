// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include "src/backend/data/image_resize.h"

namespace mmltk::backend::data::perceptual {
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
ValidatedResize validate_pair(RgbConstImageView source, RgbMutableImageView destination);
} // namespace mmltk::backend::data::perceptual
