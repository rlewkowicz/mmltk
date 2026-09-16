#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
namespace mmltk::controller::services {
std::vector<std::string> build_train_command_arguments(const mmltk::backend::models::rfdetr::TrainRequest& request, std::string_view fallback_preset_name = {});
std::filesystem::path resolve_sibling_mmltk_cli(const std::filesystem::path& executable_path);
}  // namespace mmltk::controller::services
