// SPDX-License-Identifier: MIT
// Öztireli/Gross (2015) perceptual downscaling; provenance in detail/perceptual_downscale_math.h.
#include "src/backend/data/image_resize_cuda.h"
#include "src/backend/data/detail/perceptual_downscale_views.h"
#include "src/backend/data/detail/perceptual_downscale_math.h"
#include "src/backend/data/detail/perceptual_downscale_completion.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_authority.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/cuda_high_water_allocation.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
namespace mmltk::backend::data {
namespace {
using namespace perceptual;
constexpr unsigned threads = 256;
constexpr std::size_t workspace_limit = std::size_t{256} * 1024U * 1024U;
void require_cuda(cudaError_t status) {
    if (status != cudaSuccess) throw std::runtime_error(std::string("perceptual CUDA operation failed: ") + cudaGetErrorString(status));
}
unsigned blocks(std::size_t count) { return static_cast<unsigned>(std::min<std::size_t>((count - 1) / threads + 1, 65535)); }
struct Workspace {
    TransferTable* transfer;
    Footprint* x;
    Footprint* y;
    Moment* moments;
    Coefficient* coefficients;
    float* alpha;
};
std::size_t workspace_bytes(std::uint32_t width, std::uint32_t height, bool alpha) {
    const auto axis_count = common::math::checked_add<std::size_t>(width, height, "perceptual image offset overflow");
    const auto axes = common::math::checked_multiply(axis_count, sizeof(Footprint), "perceptual image extent overflow");
    const auto pixels = common::math::checked_multiply<std::size_t>(width, height, "perceptual image extent overflow");
    const auto planes =
        common::math::checked_multiply(pixels, sizeof(Moment) + sizeof(Coefficient) + (alpha ? sizeof(float) : 0), "perceptual image extent overflow");
    if (axes > workspace_limit - sizeof(TransferTable) || planes > workspace_limit - sizeof(TransferTable) - axes)
        throw std::length_error("perceptual CUDA workspace exceeds 256 MiB");
    return sizeof(TransferTable) + axes + planes;
}
Workspace bind_workspace(void* storage, std::uint32_t width, std::uint32_t height, bool alpha) {
    const auto pixels = std::size_t(width) * height;
    Workspace result{};
    result.transfer = static_cast<TransferTable*>(storage);
    result.x = reinterpret_cast<Footprint*>(result.transfer + 1);
    result.y = result.x + width;
    result.moments = reinterpret_cast<Moment*>(result.y + height);
    result.coefficients = reinterpret_cast<Coefficient*>(result.moments + pixels);
    result.alpha = alpha ? reinterpret_cast<float*>(result.coefficients + pixels) : nullptr;
    return result;
}
__global__ void prepare_kernel(Workspace work, std::uint32_t sw, std::uint32_t sh, std::uint32_t dw, std::uint32_t dh) {
    for (std::size_t i = blockIdx.x * std::size_t(blockDim.x) + threadIdx.x; i < static_cast<std::size_t>(max(max(dw, dh), 256U));
         i += std::size_t(blockDim.x) * gridDim.x) {
        if (i < 256) work.transfer->linear[i] = decode(float(i) * (1.0F / 255.0F));
        if (i < dw) work.x[i] = footprint(sw, dw, static_cast<std::uint32_t>(i));
        if (i < dh) work.y[i] = footprint(sh, dh, static_cast<std::uint32_t>(i));
    }
}
template <RgbPixelFormat Format, bool Integer>
__device__ void accumulate(RgbConstImageView source, const Workspace& work, Footprint fx, Footprint fy, std::size_t offset, std::size_t step,
                           MomentAccumulator& sum, float& alpha) {
    const std::size_t width = std::size_t(fx.end) - fx.first;
    const std::size_t count = width * (std::size_t(fy.end) - fy.first);
    float alpha_error = 0;
    for (std::size_t j = offset; j < count; j += step) {
        const auto x = fx.first + static_cast<std::uint32_t>(j % width), y = fy.first + static_cast<std::uint32_t>(j / width);
        const float weight = Integer ? 1.0F : fx.weight(x) * fy.weight(y);
        float coverage = 1;
        const Color color = load<Format>(source, x, y, *work.transfer, coverage);
        sum.add(color, weight);
        if constexpr (Format == RgbPixelFormat::RGBA8) compensated_add(weight * coverage, alpha, alpha_error);
    }
    if constexpr (Format == RgbPixelFormat::RGBA8) alpha = sum.weight > 0 ? (alpha - alpha_error) / sum.weight : 0;
}
template <RgbPixelFormat Format>
__device__ void write_moment(Workspace work, std::size_t i, Moment sum, float alpha) {
    work.moments[i] = sum;
    if constexpr (Format == RgbPixelFormat::RGBA8) work.alpha[i] = unit(alpha);
}
template <RgbPixelFormat Format, bool Integer>
__global__ void small_moments(RgbConstImageView source, Workspace work, std::uint32_t width, std::size_t count) {
    for (std::size_t i = blockIdx.x * std::size_t(blockDim.x) + threadIdx.x; i < count; i += std::size_t(blockDim.x) * gridDim.x) {
        const auto fx = work.x[i % width], fy = work.y[i / width];
        MomentAccumulator sum;
        float alpha = 0;
        accumulate<Format, Integer>(source, work, fx, fy, 0, 1, sum, alpha);
        write_moment<Format>(work, i, sum.finish(), alpha);
    }
}
template <RgbPixelFormat Format, bool Integer>
__global__ void large_moments(RgbConstImageView source, Workspace work, std::uint32_t width, std::size_t count) {
    // One cooperative block per footprint. Source accesses are adjacent across
    // lanes even for a million-pixel footprint; the reduction uses fixed shared
    // storage and log2(256) steps, not a single-thread source-sized loop.
    constexpr unsigned components = Format == RgbPixelFormat::RGBA8 ? 8 : 7;
    __shared__ float partial[components][threads];
    for (std::size_t i = blockIdx.x; i < count; i += gridDim.x) {
        const auto fx = work.x[i % width], fy = work.y[i / width];
        MomentAccumulator accumulator;
        float alpha = 0;
        accumulate<Format, Integer>(source, work, fx, fy, threadIdx.x, blockDim.x, accumulator, alpha);
        Moment sum = accumulator.finish();
        for (int k = 0; k < 3; ++k) {
            partial[k][threadIdx.x] = sum.mean[k];
            partial[k + 3][threadIdx.x] = sum.variance[k];
        }
        partial[6][threadIdx.x] = accumulator.weight;
        if constexpr (Format == RgbPixelFormat::RGBA8) partial[7][threadIdx.x] = alpha;
        __syncthreads();
        for (unsigned stride = threads / 2; stride; stride /= 2) {
            if (threadIdx.x < stride) {
                const auto lane = threadIdx.x, other = lane + stride;
                Moment a, b;
                for (int k = 0; k < 3; ++k) {
                    a.mean[k] = partial[k][lane];
                    a.variance[k] = partial[k + 3][lane];
                    b.mean[k] = partial[k][other];
                    b.variance[k] = partial[k + 3][other];
                }
                const float aw = partial[6][lane], bw = partial[6][other];
                const auto combined = merge(a, aw, b, bw);
                for (int k = 0; k < 3; ++k) {
                    partial[k][lane] = combined.mean[k];
                    partial[k + 3][lane] = combined.variance[k];
                }
                partial[6][lane] = aw + bw;
                if constexpr (Format == RgbPixelFormat::RGBA8)
                    partial[7][lane] = aw + bw > 0 ? (partial[7][lane] * aw + partial[7][other] * bw) / (aw + bw) : 0;
            }
            __syncthreads();
        }
        if (threadIdx.x == 0) {
            for (int k = 0; k < 3; ++k) {
                sum.mean[k] = partial[k][0];
                sum.variance[k] = partial[k + 3][0];
            }
            if constexpr (Format == RgbPixelFormat::RGBA8) alpha = partial[7][0];
            write_moment<Format>(work, i, sum, alpha);
        }
        __syncthreads();
    }
}
__global__ void coefficients_kernel(Workspace work, std::uint32_t width, std::uint32_t height, std::size_t tiles) {
    // Compact 16x16 tiles plus one right/bottom halo. Every patch ratio/sqrt
    // is computed exactly once and then reused by all four reconstructions.
    __shared__ Moment tile[17 * 17];
    const std::size_t columns = (std::size_t(width) + 15) / 16;
    for (std::size_t index = blockIdx.x; index < tiles; index += gridDim.x) {
        const auto ox = (index % columns) * 16, oy = (index / columns) * 16;
        for (unsigned j = threadIdx.x; j < 17 * 17; j += threads) {
            const auto px = ox + j % 17, py = oy + j / 17;
            const auto x = px < width ? px : std::size_t(width) - 1, y = py < height ? py : std::size_t(height) - 1;
            tile[j] = work.moments[y * width + x];
        }
        __syncthreads();
        const unsigned tx = threadIdx.x % 16, ty = threadIdx.x / 16;
        if (ox + tx < width && oy + ty < height) {
            const auto j = ty * 17 + tx;
            work.coefficients[(oy + ty) * width + ox + tx] = patch(tile[j], tile[j + 1], tile[j + 17], tile[j + 18]);
        }
        __syncthreads();
    }
}
template <RgbPixelFormat Format>
__global__ void output_kernel(Workspace work, RgbMutableImageView output, std::size_t count) {
    const auto width = output.layout.width;
    for (std::size_t i = blockIdx.x * std::size_t(blockDim.x) + threadIdx.x; i < count; i += std::size_t(blockDim.x) * gridDim.x) {
        const auto x = static_cast<std::uint32_t>(i % width), y = static_cast<std::uint32_t>(i / width);
        const auto left = x ? i - 1 : i, up = y ? i - width : i, corner = y ? left - width : left;
        float alpha = 1;
        if constexpr (Format == RgbPixelFormat::RGBA8) alpha = work.alpha[i];
        store<Format>(output, x, y,
                      reconstruct(work.moments[i], work.coefficients[i], work.coefficients[left], work.coefficients[up], work.coefficients[corner]), alpha,
                      *work.transfer);
    }
}
template <RgbPixelFormat Format, bool Integer>
void launch(RgbConstImageView source, RgbMutableImageView output, Workspace work, cudaStream_t stream) {
    const std::size_t count = std::size_t(output.layout.width) * output.layout.height;
    // A fractional footprint can intersect ceil(ratio)+1 source pixels.
    const std::size_t fw = (std::size_t(source.layout.width) + output.layout.width - 1) / output.layout.width + 1;
    const std::size_t fh = (std::size_t(source.layout.height) + output.layout.height - 1) / output.layout.height + 1;
    if (fw <= 64 && fh <= 64 / fw)
        small_moments<Format, Integer><<<blocks(count), threads, 0, stream>>>(source, work, output.layout.width, count);
    else
        large_moments<Format, Integer>
            <<<static_cast<unsigned>(std::min<std::size_t>(count, 65535)), threads, 0, stream>>>(source, work, output.layout.width, count);
    require_cuda(cudaGetLastError());
    const auto tiles = ((std::size_t(output.layout.width) + 15) / 16) * ((std::size_t(output.layout.height) + 15) / 16);
    coefficients_kernel<<<static_cast<unsigned>(std::min<std::size_t>(tiles, 65535)), threads, 0, stream>>>(work, output.layout.width, output.layout.height,
                                                                                                            tiles);
    require_cuda(cudaGetLastError());
    output_kernel<Format><<<blocks(count), threads, 0, stream>>>(work, output, count);
    require_cuda(cudaGetLastError());
}
template <RgbPixelFormat Format>
void dispatch(RgbConstImageView source, RgbMutableImageView output, Workspace work, cudaStream_t stream) {
    if (source.layout.width % output.layout.width == 0 && source.layout.height % output.layout.height == 0)
        launch<Format, true>(source, output, work, stream);
    else
        launch<Format, false>(source, output, work, stream);
}
}  // namespace
struct GpuPerceptualDownscaler::Impl {
    frameworks::gpu::DeviceContext context;
    CUcontext native_context = nullptr;
    frameworks::gpu::TerminalCudaRetirementAuthority& retirement;
    frameworks::gpu::TerminalCudaRetirementLease lease;
    frameworks::gpu::CudaHighWaterAllocation<void*> storage;
    perceptual::CudaDownscaleCompletion completion;
    std::size_t capacity = 0;
    std::uint32_t sw = 0, sh = 0, dw = 0, dh = 0;
    bool alpha = false;
    Impl(frameworks::gpu::DeviceContext owner, frameworks::gpu::TerminalCudaRetirementAuthority& authority)
        : context(std::move(owner)), retirement(authority), lease(frameworks::gpu::ReserveTerminalCudaLease(authority)) {
        CUcontext previous = nullptr;
        if (cuCtxGetCurrent(&previous) != CUDA_SUCCESS) throw std::runtime_error("cannot inspect perceptual CUDA caller context");
        try {
            context.Bind();
            if (cuCtxGetCurrent(&native_context) != CUDA_SUCCESS || !native_context) throw std::runtime_error("cannot inspect perceptual CUDA owning context");
        } catch (...) {
            (void)cuCtxSetCurrent(previous);
            throw;
        }
        if (cuCtxSetCurrent(previous) != CUDA_SUCCESS) throw std::runtime_error("cannot restore perceptual CUDA caller context");
    }
    static void retire_owner(std::shared_ptr<Impl>& owner, cudaError_t failure) noexcept {
        auto lease = std::move(owner->lease);
        std::move(lease).Install(frameworks::gpu::TerminalCudaCustody::Share(std::move(owner)), failure);
    }
    static void admit(std::shared_ptr<Impl>& owner, DriverAdmission result) {
        if (result.physical_failure()) {
            // cuda_error.h establishes CUDA's shared Driver/Runtime identities.
            const auto status = static_cast<cudaError_t>(result.status);
            retire_owner(owner, status);
            throw frameworks::gpu::CudaError(status, "perceptual CUDA admission");
        }
        if (result.caller_rejection()) throw std::invalid_argument("perceptual CUDA context, mapped span, or stream is invalid");
    }
    DriverAdmission check_context() const {
        CUcontext current = nullptr;
        const auto status = cuCtxGetCurrent(&current);
        return {status, current == native_context, AdmissionQuery::Context};
    }
    void check_admission() const {
        if (!retirement.admission_open()) throw std::runtime_error("perceptual CUDA admission is closed");
    }
    DriverAdmission check_span(const void* pointer, std::size_t extent) const {
        CUcontext allocation_context = nullptr;
        unsigned memory_type = 0;
        CUdeviceptr base = 0;
        std::size_t bytes = 0;
        const auto address = static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(pointer));
        CUpointer_attribute attributes[]{CU_POINTER_ATTRIBUTE_CONTEXT, CU_POINTER_ATTRIBUTE_MEMORY_TYPE};
        void* results[]{static_cast<void*>(&allocation_context), &memory_type};
        const auto pointer_status = cuPointerGetAttributes(2, attributes, results, address);
        if (pointer_status != CUDA_SUCCESS || memory_type != CU_MEMORYTYPE_DEVICE || allocation_context != native_context)
            return {pointer_status, false, AdmissionQuery::Pointer};
        // RANGE_START_ADDR/RANGE_SIZE can include unmapped reserved VA.
        const auto range_status = cuMemGetAddressRange(&base, &bytes, address);
        return {range_status,
                range_status == CUDA_SUCCESS && address >= base && address - base <= bytes && extent <= bytes - static_cast<std::size_t>(address - base),
                AdmissionQuery::Range};
    }
    DriverAdmission check_stream(cudaStream_t stream) const {
        if (!stream) return {};  // Exact current context proves null-stream identity.
        CUcontext stream_context = nullptr;
        const auto status = cuStreamGetCtx(reinterpret_cast<CUstream>(stream), &stream_context);
        return {status, stream_context == native_context, AdmissionQuery::Stream};
    }
    cudaError_t release() noexcept {
        auto status = completion.settle(true);
        if (status != cudaSuccess) return status;
        const auto released = storage.ReleaseAll([](void* pointer) noexcept { return cudaFree(pointer); });
        if (!released.released()) return released.failure;
        return completion.release();
    }
};
GpuPerceptualDownscaler::GpuPerceptualDownscaler(frameworks::gpu::DeviceContext context, frameworks::gpu::TerminalCudaRetirementAuthority& authority)
    : impl_(std::make_shared<Impl>(std::move(context), authority)) {
    Impl::admit(impl_, impl_->check_context());
    impl_->check_admission();
}
void GpuPerceptualDownscaler::retire(cudaError_t failure) noexcept { Impl::retire_owner(impl_, failure); }
GpuPerceptualDownscaler::~GpuPerceptualDownscaler() noexcept {
    if (!impl_) return;
    if (cuCtxPushCurrent(impl_->native_context) != CUDA_SUCCESS) {
        retire(cudaErrorContextIsDestroyed);
        return;
    }
    auto status = impl_->release();
    CUcontext popped = nullptr;
    if (cuCtxPopCurrent(&popped) != CUDA_SUCCESS || popped != impl_->native_context) status = cudaErrorUnknown;
    if (status != cudaSuccess) retire(status);
}
void GpuPerceptualDownscaler::finish() {
    if (!impl_) throw std::runtime_error("perceptual CUDA owner has terminal custody");
    Impl::admit(impl_, impl_->check_context());
    const auto status = impl_->completion.settle(true);
    if (status != cudaSuccess) {
        retire(status);
        require_cuda(status);
    }
}
void GpuPerceptualDownscaler::downscale(RgbConstImageView source, RgbMutableImageView destination, cudaStream_t stream,
                                        std::shared_ptr<const void> source_custody, std::shared_ptr<const void> destination_custody) {
    if (!impl_) throw std::runtime_error("perceptual CUDA owner has terminal custody");
    const auto validated = validate_pair(source, destination);
    const bool identity = validated.identity;
    if (!source_custody || !destination_custody) throw std::invalid_argument("perceptual CUDA views require exact resource custody");
    Impl::admit(impl_, impl_->check_context());
    impl_->check_admission();
    const auto settled = impl_->completion.settle(false);
    if (settled != cudaSuccess) {
        retire(settled);
        require_cuda(settled);
    }
    Impl::admit(impl_, impl_->check_span(source.data, validated.source_extent));
    Impl::admit(impl_, impl_->check_span(destination.data, validated.destination_extent));
    Impl::admit(impl_, impl_->check_stream(stream));
    const bool alpha = source.layout.format == RgbPixelFormat::RGBA8;
    const auto required = identity ? 0 : workspace_bytes(destination.layout.width, destination.layout.height, alpha);
    auto& slot = [&]() -> perceptual::CudaDownscaleCompletion::Submission& {
        try {
            return impl_->completion.reserve(stream, std::move(source_custody), std::move(destination_custody));
        } catch (const perceptual::CompletionAllocationFailure& failure) {
            if (failure.physical_failure()) retire(failure.status());
            throw;
        }
    }();
    try {
        require_cuda(impl_->completion.order(slot));
        if (identity) {
            if (source.data != destination.data) {
                const auto geometry = identity_geometry(source.layout);
                for (unsigned plane = 0; plane < geometry.planes; ++plane)
                    require_cuda(cudaMemcpy2DAsync(static_cast<std::uint8_t*>(destination.data) + plane * destination.layout.plane_stride_bytes,
                                                   destination.layout.row_stride_bytes,
                                                   static_cast<const std::uint8_t*>(source.data) + plane * source.layout.plane_stride_bytes,
                                                   source.layout.row_stride_bytes, geometry.row_bytes, source.layout.height, cudaMemcpyDeviceToDevice, stream));
            }
        } else {
            const bool grow = required > impl_->capacity;
            require_cuda(impl_->storage.RetryPending([&](void* p) noexcept { return cudaFreeAsync(p, stream); }).failure);
            if (grow) {
                require_cuda(impl_->storage.AllocateCandidate([&](void*& p) noexcept { return cudaMallocAsync(&p, required, stream); }).failure);
                require_cuda(impl_->storage.PromoteCandidate([&](void* p) noexcept { return cudaFreeAsync(p, stream); }).failure);
                impl_->capacity = required;
            }
            const Workspace work = bind_workspace(impl_->storage.active(), destination.layout.width, destination.layout.height, alpha);
            if (grow || impl_->sw != source.layout.width || impl_->sh != source.layout.height || impl_->dw != destination.layout.width ||
                impl_->dh != destination.layout.height || impl_->alpha != alpha) {
                prepare_kernel<<<blocks(std::max<std::size_t>({destination.layout.width, destination.layout.height, 256})), threads, 0, stream>>>(
                    work, source.layout.width, source.layout.height, destination.layout.width, destination.layout.height);
                require_cuda(cudaGetLastError());
                impl_->sw = source.layout.width;
                impl_->sh = source.layout.height;
                impl_->dw = destination.layout.width;
                impl_->dh = destination.layout.height;
                impl_->alpha = alpha;
            }
            switch (source.layout.format) {
                case RgbPixelFormat::RGB8: dispatch<RgbPixelFormat::RGB8>(source, destination, work, stream); break;
                case RgbPixelFormat::RGBA8: dispatch<RgbPixelFormat::RGBA8>(source, destination, work, stream); break;
                case RgbPixelFormat::PlanarUnitSrgbF32: dispatch<RgbPixelFormat::PlanarUnitSrgbF32>(source, destination, work, stream); break;
            }
        }
        require_cuda(impl_->completion.record(slot));
    } catch (...) {
        const auto original = std::current_exception();
        // Include every operation that might have reached this exact stream.
        // If the event cannot be recorded, settle the stream before releasing
        // any borrowed view. Unproved settlement quarantines the whole owner.
        (void)impl_->completion.record(slot);
        const auto status = impl_->completion.settle(true);
        if (status != cudaSuccess)
            retire(status);
        else {
            impl_->sw = 0;
            impl_->completion.forget_order();
        }
        std::rethrow_exception(original);
    }
}
}  // namespace mmltk::backend::data
