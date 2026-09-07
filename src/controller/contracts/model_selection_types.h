#pragma once

#include <cstdint>

#include "mmltk/frameworks/reflection/materializer.h"
#include "src/backend/models/catalog/artifacts.h"

namespace mmltk::controller::contracts {

using mmltk::backend::models::catalog::ModelArtifactInputKind;

enum class ModelSelectionSource : std::uint8_t {
    Canonical = 0,
    Custom = 1,
};

MMLTK_REFLECT_ENUM(ModelSelectionSource)

}  // namespace mmltk::controller::contracts
