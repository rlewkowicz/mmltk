#pragma once

#include <filesystem>
#include <vector>

#include "model_technical.h"

namespace mmltk::backend::models::rfdetr::detail {

struct ModelStateTechnicalOwner {
    std::vector<NormalizedModelStateEntry> entries;
};

}  // namespace mmltk::backend::models::rfdetr::detail
