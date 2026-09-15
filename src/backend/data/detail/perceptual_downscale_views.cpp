// SPDX-License-Identifier: MIT
#include "src/backend/data/detail/perceptual_downscale_views.h"
#include "src/common/math/checked_arithmetic.h"

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace mmltk::backend::data::perceptual {
IdentityCopyGeometry identity_geometry(const RgbImageLayout& layout) {
    switch (layout.format) {
        case RgbPixelFormat::RGB8:
            return {common::math::checked_multiply<std::size_t>(layout.width,3,"perceptual image extent overflow"),1};
        case RgbPixelFormat::RGBA8:
            return {common::math::checked_multiply<std::size_t>(layout.width,4,"perceptual image extent overflow"),1};
        case RgbPixelFormat::PlanarUnitSrgbF32:
            return {common::math::checked_multiply<std::size_t>(layout.width,sizeof(float),"perceptual image extent overflow"),3};
    }
    throw std::invalid_argument("unknown perceptual image format");
}
std::size_t validate_view(const void* pointer, const RgbImageLayout& l) {
    if (!pointer || !l.width || !l.height) throw std::invalid_argument("perceptual image requires storage and positive dimensions");
    const std::size_t row = identity_geometry(l).row_bytes;
    if (l.row_stride_bytes < row) throw std::invalid_argument("perceptual image row stride is too small");
    const auto preceding_rows = common::math::checked_multiply<std::size_t>(
        l.height-1,l.row_stride_bytes,"perceptual image extent overflow");
    std::size_t extent = common::math::checked_add(preceding_rows,row,"perceptual image offset overflow");
    if (l.format == RgbPixelFormat::PlanarUnitSrgbF32) {
        if (reinterpret_cast<std::uintptr_t>(pointer) % alignof(float) || l.row_stride_bytes % sizeof(float) ||
            l.plane_stride_bytes % sizeof(float) || l.plane_stride_bytes < extent)
            throw std::invalid_argument("perceptual float planes overlap or are unaligned");
        const auto preceding_planes = common::math::checked_multiply<std::size_t>(
            2,l.plane_stride_bytes,"perceptual image extent overflow");
        extent = common::math::checked_add(preceding_planes,extent,"perceptual image offset overflow");
    } else if (l.plane_stride_bytes) throw std::invalid_argument("packed perceptual image has a plane stride");
    if (extent > l.capacity_bytes) throw std::invalid_argument("perceptual image storage is too small");
    if (extent > std::numeric_limits<std::uintptr_t>::max() - reinterpret_cast<std::uintptr_t>(pointer))
        throw std::overflow_error("perceptual image address overflow");
    return extent;
}
ValidatedResize validate_pair(RgbConstImageView source, RgbMutableImageView destination) {
    const auto source_bytes = validate_view(source.data,source.layout), destination_bytes = validate_view(destination.data,destination.layout);
    if (source.layout.format != destination.layout.format) throw std::invalid_argument("perceptual image formats must match");
    if (source.layout.width < destination.layout.width || source.layout.height < destination.layout.height)
        throw std::invalid_argument("perceptual resampling cannot enlarge images");
    const bool identity = source.layout.width == destination.layout.width && source.layout.height == destination.layout.height;
    const auto a = reinterpret_cast<std::uintptr_t>(source.data), b = reinterpret_cast<std::uintptr_t>(destination.data);
    if (a < b + destination_bytes && b < a + source_bytes &&
        !(identity && a == b && source.layout.row_stride_bytes == destination.layout.row_stride_bytes &&
          source.layout.plane_stride_bytes == destination.layout.plane_stride_bytes))
        throw std::invalid_argument("perceptual image input/output storage overlaps");
    return {source_bytes,destination_bytes,identity};
}
} // namespace mmltk::backend::data::perceptual
