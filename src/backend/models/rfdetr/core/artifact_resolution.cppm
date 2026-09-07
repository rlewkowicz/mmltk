module;
#include <filesystem>
#include <string_view>

#include "src/backend/models/rfdetr/contract/artifacts.h"

export module mmltk.backend.models.rfdetr.core.artifact_resolution;

export namespace mmltk::backend::models::rfdetr {

[[nodiscard]] ResolvedModelArtifacts resolve_model_artifacts(const std::filesystem::path& weights_path, std::string_view preset_name,
                                                             int resolution);

}  // namespace mmltk::backend::models::rfdetr
