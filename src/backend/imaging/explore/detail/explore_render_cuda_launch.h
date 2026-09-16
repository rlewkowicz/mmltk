#pragma once
#include <cuda_runtime_api.h>
#include "explore_render_cuda_abi.h"
namespace mmltk::backend::imaging::explore::detail {
[[nodiscard]] cudaError_t render_explore_atlas_tiles_cuda(const ExploreRenderAtlasViewAbi& view, const ExploreRenderTileBatchViewAbi& tiles,
                                                          const ExploreRenderSemanticViewAbi& semantics, const ExploreRenderScratchViewAbi& scratch,
                                                          const ExploreRenderTargetViewAbi& target, cudaStream_t stream, ExploreRenderDemand demand) noexcept;
[[nodiscard]] cudaError_t render_explore_detail_cuda(const ExploreRenderDetailViewAbi& view, const ExploreRenderSemanticViewAbi& semantics,
                                                     const ExploreRenderScratchViewAbi& scratch, const ExploreRenderTargetViewAbi& target, cudaStream_t stream,
                                                     ExploreRenderDemand demand) noexcept;
[[nodiscard]] cudaError_t count_explore_nonzero_alpha_cuda(const ExploreRenderTargetViewAbi& target, std::uint64_t* device_count, cudaStream_t stream) noexcept;
[[nodiscard]] cudaError_t checksum_explore_pixels_cuda(const ExploreRenderTargetViewAbi& target, std::uint64_t* device_checksum, cudaStream_t stream) noexcept;
[[nodiscard]] cudaError_t probe_explore_rendered_card_cuda(const ExploreRenderTargetViewAbi& clean, const ExploreRenderTargetViewAbi& semantic,
                                                           const ExploreRenderedCardProbeAbi& probe, std::uint64_t* device_counts,
                                                           cudaStream_t stream) noexcept;
[[nodiscard]] cudaError_t sample_explore_rendered_card_cuda(const ExploreRenderTargetViewAbi& clean, const ExploreRenderTargetViewAbi& semantic,
                                                            const ExploreRenderTargetViewAbi& reference, std::uint64_t* device_samples,
                                                            cudaStream_t stream) noexcept;
}  // namespace mmltk::backend::imaging::explore::detail
