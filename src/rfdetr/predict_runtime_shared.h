#pragma once

#include "rfdetr/runtime.h"

#include <algorithm>
#include <cstddef>

namespace mmltk::rfdetr::predict_internal {

inline std::size_t prediction_lane_slot_count(const RuntimeSplit& split) {
    const std::size_t lane_threads = static_cast<std::size_t>(std::max(1, split.lane_threads));
    const std::size_t cpu_threads = static_cast<std::size_t>(std::max(1, split.cpu_threads));
    return std::max<std::size_t>(2, 1 + ((cpu_threads + lane_threads - 1) / lane_threads));
}

} // namespace mmltk::rfdetr::predict_internal
