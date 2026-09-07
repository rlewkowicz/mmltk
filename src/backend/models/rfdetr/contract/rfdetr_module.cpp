#include <filesystem>
#include <string_view>

#include "src/backend/models/rfdetr/contract/contract.h"
#include "src/backend/models/rfdetr/contract/model_config.h"

namespace mmltk::backend::models::rfdetr {

std::string_view infer_artifact_preset(const std::filesystem::path& path) {
    const auto* preset = infer_model_preset_from_path(path);
    return preset == nullptr ? std::string_view{} : preset->preset_name;
}

}  // namespace mmltk::backend::models::rfdetr
