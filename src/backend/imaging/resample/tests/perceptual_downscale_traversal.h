// SPDX-License-Identifier: MIT
#pragma once
#include "src/backend/imaging/resample/image_resize.h"
#include <cuda_runtime_api.h>
#include <cstdint>
namespace mmltk::backend::imaging::resample::test_perceptual {
// Mean, variance, weight and final alpha, before reconstruction. Two adjacent
// records per destination footprint: retained strided, then serial traversal.
struct TraversalMoments {
    std::uint32_t bits[8];
};
cudaError_t compare_traversal(RgbConstImageView source, std::uint32_t width, std::uint32_t height,
                              TraversalMoments* results, cudaStream_t stream);
}  // namespace mmltk::backend::imaging::resample::test_perceptual
