#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/services/artifact_store.h"
namespace mmltk::controller::services {
std::vector<std::string> build_train_command_arguments(const mmltk::backend::models::rfdetr::TrainRequest& request, std::string_view fallback_preset_name = {});
// Launch source admission occurs before worker process creation. Workers never acquire.
void prepare_train_encoder(mmltk::backend::models::rfdetr::TrainRequest&, const ArtifactStore&, const ArtifactCancellationToken&);
std::filesystem::path resolve_sibling_mmltk_cli(const std::filesystem::path& executable_path);
}  // namespace mmltk::controller::services
