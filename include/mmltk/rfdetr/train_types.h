#pragma once

#include <cstdint>

namespace mmltk::rfdetr {

enum class TrainOptimizerKind : std::uint8_t {
    AdamW = 0,
    Muon = 1,
};

struct TrainHyperparameterConfig {
    double lr = 1.0e-4;
    double lr_encoder = 1.5e-4;
    double lr_component_decay = 0.7;
    double encoder_layer_decay = 0.8;
    double momentum = 0.95;
    double weight_decay = 1.0e-4;
    double warmup_epochs = 0.0;
    double warmup_momentum = 0.0;
    double lr_min_factor = 0.0;
};

}  // namespace mmltk::rfdetr
