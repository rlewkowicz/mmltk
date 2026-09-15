// SPDX-License-Identifier: MIT
// Öztireli/Gross (2015) perceptual downscaling; provenance in perceptual_downscale_math.h.
#include "src/backend/data/detail/perceptual_downscale.h"

#include <algorithm>
#include <cstring>
#include <immintrin.h>
#include <limits>
#include <stdexcept>

namespace mmltk::backend::data::perceptual {
std::size_t checked_product(std::size_t a, std::size_t b) {
    if (b && a > std::numeric_limits<std::size_t>::max() / b) throw std::overflow_error("perceptual image extent overflow");
    return a * b;
}
namespace {
std::size_t checked_add(std::size_t a, std::size_t b) {
    if (b > std::numeric_limits<std::size_t>::max() - a) throw std::overflow_error("perceptual image offset overflow");
    return a + b;
}
std::size_t row_bytes(const RgbImageLayout& l) {
    switch(l.format) {
        case RgbPixelFormat::RGB8: return checked_product(l.width, 3);
        case RgbPixelFormat::RGBA8:
        case RgbPixelFormat::PlanarUnitSrgbF32: return checked_product(l.width, 4);
    }
    throw std::invalid_argument("unknown perceptual image format");
}
void vector_add(__m256 value,__m256& sum,__m256& error) {
    const auto adjusted=_mm256_sub_ps(value,error),next=_mm256_add_ps(sum,adjusted);
    error=_mm256_sub_ps(_mm256_sub_ps(next,sum),adjusted);sum=next;
}
// Eight neighboring output cells traverse their contiguous integer source
// rectangle together. Even 2x2 reductions use eight active SIMD lanes. Byte
// gathers never load a fourth byte beyond an RGB pixel or cross row padding.
template<RgbPixelFormat Format>
void integer_moments8(RgbConstImageView source,const TransferTable& transfer,
                      std::uint32_t first_x,Footprint fy,std::uint32_t step,Moment* output,float* alpha_output) {
    __m256 means[3]{},variances[3]{},mean_errors[3]{},variance_errors[3]{};
    __m256 alpha_sum=_mm256_setzero_ps(),alpha_error=_mm256_setzero_ps();
    std::uint64_t samples=0;
    for(std::uint32_t y=fy.first;y<fy.end;++y) for(std::uint32_t dx=0;dx<step;++dx) {
        __m256 rgb[3];
        __m256 alpha=_mm256_set1_ps(1);
        const auto* row=static_cast<const std::uint8_t*>(source.data)+std::size_t(y)*source.layout.row_stride_bytes;
        constexpr unsigned channels=Format==RgbPixelFormat::RGBA8 ? 4:3;
        for(unsigned k=0;k<3;++k) {
            if constexpr(Format==RgbPixelFormat::PlanarUnitSrgbF32) {
                alignas(32) float linear[8];
                const auto* plane=reinterpret_cast<const float*>(row+k*source.layout.plane_stride_bytes);
                for(unsigned lane=0;lane<8;++lane) linear[lane]=decode(unit(plane[first_x+lane*step+dx]));
                rgb[k]=_mm256_load_ps(linear);
            } else {
                alignas(32) int indices[8];
                for(unsigned lane=0;lane<8;++lane) indices[lane]=row[std::size_t(first_x+lane*step+dx)*channels+k];
                rgb[k]=_mm256_i32gather_ps(transfer.linear,_mm256_load_si256(reinterpret_cast<const __m256i*>(indices)),4);
            }
        }
        if constexpr(Format==RgbPixelFormat::RGBA8) {
            alignas(32) int values[8];
            for(unsigned lane=0;lane<8;++lane) values[lane]=row[std::size_t(first_x+lane*step+dx)*4+3];
            alpha=_mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_load_si256(reinterpret_cast<const __m256i*>(values))),_mm256_set1_ps(1.0F/255));
            for(auto& channel:rgb) channel=_mm256_mul_ps(channel,alpha);
            vector_add(alpha,alpha_sum,alpha_error);
        }
        using C=ColorCoefficients;
        const auto channel=[&](float r,float g,float b) {
            return _mm256_fmadd_ps(rgb[0],_mm256_set1_ps(r),
                    _mm256_fmadd_ps(rgb[1],_mm256_set1_ps(g),_mm256_mul_ps(rgb[2],_mm256_set1_ps(b))));
        };
        const __m256 colors[]{channel(C::yr,C::yg,C::yb),channel(C::cbr,C::cbg,C::cbb),channel(C::crr,C::crg,C::crb)};
        const auto inverse=_mm256_set1_ps(1.0F/static_cast<float>(++samples));
        for(int k=0;k<3;++k) {
            const auto delta=_mm256_sub_ps(colors[k],means[k]);
            vector_add(_mm256_mul_ps(delta,inverse),means[k],mean_errors[k]);
            vector_add(_mm256_mul_ps(delta,_mm256_sub_ps(colors[k],means[k])),variances[k],variance_errors[k]);
        }
    }
    const auto inverse=_mm256_set1_ps(1.0F/static_cast<float>(samples));
    alignas(32) float means_out[8],variances_out[8];
    for(int k=0;k<3;++k) {
        _mm256_store_ps(means_out,means[k]);
        _mm256_store_ps(variances_out,_mm256_max_ps(_mm256_setzero_ps(),_mm256_mul_ps(_mm256_sub_ps(variances[k],variance_errors[k]),inverse)));
        for(unsigned lane=0;lane<8;++lane) {output[lane].mean[k]=means_out[lane];output[lane].variance[k]=variances_out[lane];}
    }
    if constexpr(Format==RgbPixelFormat::RGBA8)
        _mm256_storeu_ps(alpha_output,_mm256_mul_ps(_mm256_sub_ps(alpha_sum,alpha_error),inverse));
}
}
std::size_t validate_view(const void* pointer, const RgbImageLayout& l) {
    if (!pointer || !l.width || !l.height) throw std::invalid_argument("perceptual image requires storage and positive dimensions");
    const std::size_t row = row_bytes(l);
    if (l.row_stride_bytes < row) throw std::invalid_argument("perceptual image row stride is too small");
    std::size_t extent = checked_add(checked_product(l.height-1,l.row_stride_bytes),row);
    if (l.format == RgbPixelFormat::PlanarUnitSrgbF32) {
        if (reinterpret_cast<std::uintptr_t>(pointer) % alignof(float) || l.row_stride_bytes % sizeof(float) ||
            l.plane_stride_bytes % sizeof(float) || l.plane_stride_bytes < extent)
            throw std::invalid_argument("perceptual float planes overlap or are unaligned");
        extent = checked_add(checked_product(2,l.plane_stride_bytes),extent);
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
void copy_identity(RgbConstImageView source, RgbMutableImageView destination) {
    if (source.data == destination.data) return;
    const unsigned planes = source.layout.format == RgbPixelFormat::PlanarUnitSrgbF32 ? 3 : 1;
    const std::size_t bytes = row_bytes(source.layout);
    for (unsigned plane=0; plane<planes; ++plane)
        for (std::uint32_t y=0; y<source.layout.height; ++y)
            std::memcpy(static_cast<std::uint8_t*>(destination.data)+plane*destination.layout.plane_stride_bytes+y*destination.layout.row_stride_bytes,
                        static_cast<const std::uint8_t*>(source.data)+plane*source.layout.plane_stride_bytes+y*source.layout.row_stride_bytes,bytes);
}
CpuDownscaler::CpuDownscaler() {
    for (unsigned i=0; i<256; ++i) transfer_.linear[i] = decode(float(i)*(1.0F/255.0F));
}
void CpuDownscaler::prepare(const RgbImageLayout& source, const RgbImageLayout& destination) {
    const auto width = destination.width, height = destination.height;
    (void)checked_product(width,sizeof(Moment)*2 + sizeof(Coefficient)*2 + sizeof(float)*2);
    if (source_width_ != source.width || source_height_ != source.height || width_ != width || height_ != height) {
        x_.resize(width); y_.resize(height);
        for (std::uint32_t x=0; x<width; ++x) x_[x] = footprint(source.width,width,x);
        for (std::uint32_t y=0; y<height; ++y) y_[y] = footprint(source.height,height,y);
        for (auto& row : moments_) row.resize(width);
        for (auto& row : coefficients_) row.resize(width);
        source_width_=source.width; source_height_=source.height; width_=width; height_=height;
    }
    if (source.format == RgbPixelFormat::RGBA8) for (auto& row : alpha_) row.resize(width);
}
template<RgbPixelFormat Format, bool Integer>
void CpuDownscaler::execute(RgbConstImageView source, RgbMutableImageView destination) {
    const auto width = destination.layout.width, height = destination.layout.height;
    auto fill_row = [&](std::uint32_t y) {
        const Footprint fy = y_[y];
        auto& row = moments_[y%2];
        std::uint32_t x=0;
        if constexpr(Integer) {
            for(;width-x>=8;x+=8) {
                float* alpha=nullptr;
                if constexpr(Format==RgbPixelFormat::RGBA8) alpha=alpha_[y%2].data()+x;
                integer_moments8<Format>(source,transfer_,x_[x].first,fy,source.layout.width/width,row.data()+x,alpha);
            }
        }
        for (; x<width; ++x) {
            const Footprint fx=x_[x];
            MomentAccumulator sum;
            float coverage = 0,coverage_error=0;
            for (std::uint32_t sy=fy.first; sy<fy.end; ++sy) {
                const float wy = Integer ? 1.0F : fy.weight(sy);
                for (std::uint32_t sx=fx.first; sx<fx.end; ++sx) {
                    const float weight = Integer ? 1.0F : wy*fx.weight(sx);
                    float alpha = 1.0F;
                    const Color color=load<Format>(source,sx,sy,transfer_,alpha);
                    sum.add(color,weight);
                    if constexpr (Format == RgbPixelFormat::RGBA8) compensated_add(weight*alpha,coverage,coverage_error);
                }
            }
            row[x]=sum.finish();
            if constexpr (Format == RgbPixelFormat::RGBA8) alpha_[y%2][x]=unit((coverage-coverage_error)/sum.weight);
        }
    };
    fill_row(0);
    for (std::uint32_t y=0; y<height; ++y) {
        if (y+1<height) fill_row(y+1);
        const auto& row=moments_[y%2];
        const auto& next=moments_[(y+1<height ? y+1 : y)%2];
        auto& current=coefficients_[y%2];
        for (std::uint32_t x=0; x+1<width; ++x) current[x]=patch(row[x],row[x+1],next[x],next[x+1]);
        current[width-1]=patch(row[width-1],row[width-1],next[width-1],next[width-1]);
        const auto& previous=coefficients_[(y ? y-1 : y)%2];
        for (std::uint32_t x=0; x<width; ++x) {
            const auto left=x ? x-1 : 0;
            float alpha=1;
            if constexpr (Format == RgbPixelFormat::RGBA8) alpha=alpha_[y%2][x];
            store<Format>(destination,x,y,reconstruct(row[x],current[x],current[left],previous[x],previous[left]),alpha,transfer_);
        }
    }
}
void CpuDownscaler::run(RgbConstImageView source, RgbMutableImageView destination) {
    if (validate_pair(source,destination)) { copy_identity(source,destination); return; }
    prepare(source.layout,destination.layout);
    const bool integer = source.layout.width % destination.layout.width == 0 && source.layout.height % destination.layout.height == 0;
    // One format/geometry dispatch per image, never per source pixel.
    switch(source.layout.format) {
        case RgbPixelFormat::RGB8:
            if (integer) execute<RgbPixelFormat::RGB8,true>(source,destination);
            else execute<RgbPixelFormat::RGB8,false>(source,destination);
            break;
        case RgbPixelFormat::RGBA8:
            if (integer) execute<RgbPixelFormat::RGBA8,true>(source,destination);
            else execute<RgbPixelFormat::RGBA8,false>(source,destination);
            break;
        case RgbPixelFormat::PlanarUnitSrgbF32:
            if (integer) execute<RgbPixelFormat::PlanarUnitSrgbF32,true>(source,destination);
            else execute<RgbPixelFormat::PlanarUnitSrgbF32,false>(source,destination);
            break;
    }
}
} // namespace mmltk::backend::data::perceptual
