module;
#include <filesystem>
#include <string_view>
#include "src/backend/models/rfdetr/core/model_state.h"
module mmltk.backend.models.rfdetr.core.artifact_resolution;
namespace mmltk::backend::models::rfdetr {
ResolvedModelArtifacts resolve_model_artifacts(const std::filesystem::path& weights_path, const std::string_view preset_name, const int resolution) {
    return resolve_model_state(weights_path, preset_name, resolution).artifacts;
}
}  // namespace mmltk::backend::models::rfdetr
