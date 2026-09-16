// SPDX-License-Identifier: MIT
// Öztireli/Gross (2015) perceptual downscaling; algorithm provenance in detail/perceptual_downscale_math.h.
#pragma once
#include <cuda_runtime_api.h>
#include <memory>
#include "src/backend/data/image_resize.h"
namespace mmltk::frameworks::gpu {
class DeviceContext;
class TerminalCudaRetirementAuthority;
}  // namespace mmltk::frameworks::gpu
namespace mmltk::backend::data {
// Single issuing thread, with the supplied DeviceContext bound. Pixels and
// producer dependencies are supplied by the caller. Both custody arguments
// retain the exact storage AND its context through completion, including
// submission failure. A caller may use an aliasing shared_ptr for a view.
// Caller streams remain alive until finish/destruction. Cross-stream calls
// are ordered after this owner's preceding use, without a CPU wait.
class GpuPerceptualDownscaler final {
   public:
    explicit GpuPerceptualDownscaler(frameworks::gpu::DeviceContext context, frameworks::gpu::TerminalCudaRetirementAuthority& retirement);
    ~GpuPerceptualDownscaler() noexcept;
    GpuPerceptualDownscaler(const GpuPerceptualDownscaler&) = delete;
    GpuPerceptualDownscaler& operator=(const GpuPerceptualDownscaler&) = delete;
    // Same checked/color contract as RgbImageResizer::downscale; complete
    // device spans must fit one driver-reported mapped allocation in the
    // retained context. Non-null streams must belong to that exact context.
    // Up to 16 distinct outstanding custody pairs; repeated
    // same-stream views of one owning aggregate share its completion. Pressure
    // refuses before submission. finish provides explicit bounded backpressure.
    // Workspace admission is bounded to 256 MiB (512 MiB during replacement).
    void downscale(RgbConstImageView source, RgbMutableImageView destination, cudaStream_t stream, std::shared_ptr<const void> source_custody,
                   std::shared_ptr<const void> destination_custody);
    // Settlement stays available after the shared authority closes admission.
    void finish();

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    void retire(cudaError_t failure) noexcept;
};
}  // namespace mmltk::backend::data
