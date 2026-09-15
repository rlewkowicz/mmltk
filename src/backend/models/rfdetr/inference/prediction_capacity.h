#pragma once
#include <algorithm>
#include "src/backend/models/rfdetr/contract/prediction_limits.h"
namespace mmltk::backend::models::rfdetr {
struct PredictionCapacity final {
    std::size_t candidates = 0U;
    std::size_t pixels = 0U;
    // One UInt8 mask, independent of discarded top-k candidates. Chunks are selected later.
    std::size_t mask_bytes = 0U;
    [[nodiscard]] static PredictionCapacity Resolve(std::size_t requested, std::size_t batch, std::int64_t queries,
                                                    std::int64_t classes, std::uint32_t width, std::uint32_t height, bool masks) {
        validate_prediction_candidates(requested);
        if (queries <= 0 || classes <= 0 || queries > std::numeric_limits<std::int32_t>::max() || classes > std::numeric_limits<std::int32_t>::max())
            throw std::invalid_argument("RF-DETR backend candidate shape is invalid");
        const auto available = checked_prediction_extent(static_cast<std::size_t>(queries), static_cast<std::size_t>(classes),
                                                         static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()));
        static_cast<void>(checked_prediction_extent(batch, available, kMaximumPredictionTensorBytes / sizeof(float)));
        const auto count = std::min(requested, available);
        static_cast<void>(checked_prediction_extent(batch, count, kMaximumPredictionTensorBytes / (4U * sizeof(float))));
        const auto pixels = checked_prediction_extent(width, height, kMaximumEncodedMaskPixels);
        return {count, pixels, masks ? checked_prediction_extent(pixels, sizeof(std::uint8_t), kMaximumPredictionTensorBytes) : 0U};
    }
};
}
