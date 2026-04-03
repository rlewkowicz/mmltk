#pragma once

#include <cstdint>

namespace mmltk::rfdetr {

enum class TrainOptimizerKind : std::uint8_t {
    AdamW = 0,
    Muon = 1,
};

} // namespace mmltk::rfdetr
