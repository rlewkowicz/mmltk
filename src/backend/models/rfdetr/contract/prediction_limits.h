#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace mmltk::backend::models::rfdetr {
// Signed CUDA launch/index arithmetic is the semantic candidate boundary.
inline constexpr std::size_t kMaximumPredictionCandidates = std::numeric_limits<std::int32_t>::max();
// Each independently owned prediction tensor is bounded, including masks.
inline constexpr std::size_t kMaximumPredictionTensorBytes = 1ULL << 30U;
inline constexpr std::size_t kPredictionMaskChunkBytes = 16ULL << 20U;
inline constexpr std::size_t kMaximumPredictionEncodedBytes = 1ULL << 30U;
// Two uint32 decimal fields and their separators bound both binary runs and JSON RLE text.
inline constexpr std::size_t kMaximumPredictionMaskRuns = kMaximumPredictionEncodedBytes / 22U;
inline constexpr std::size_t kMaximumEncodedMaskPixels = std::numeric_limits<std::uint32_t>::max();
inline void validate_prediction_candidates(std::size_t requested) {
 if (requested == 0U || requested > kMaximumPredictionCandidates) throw std::invalid_argument("RF-DETR candidate limit exceeds supported signed indexing");
}
[[nodiscard]] inline std::size_t checked_prediction_extent(std::size_t count, std::size_t stride, std::size_t maximum) {
 if (count == 0U || stride == 0U || count > maximum / stride) throw std::invalid_argument("RF-DETR prediction extent exceeds supported storage");
 return count * stride;
}
}  // namespace mmltk::backend::models::rfdetr
