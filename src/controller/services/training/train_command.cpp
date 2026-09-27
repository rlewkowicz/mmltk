#include "src/controller/services/training/train_command.h"
#include "src/pch_std.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
namespace mmltk::controller::services {
std::vector<std::string> build_train_command_arguments(const mmltk::backend::models::rfdetr::TrainRequest& request, const std::string_view fallback_preset_name) {
 auto effective = request;
 if (effective.preset_name.empty()) effective.preset_name = fallback_preset_name;
 return {"rfdetr", "train", "--request-json", mmltk::backend::models::rfdetr::encode_train_request_json(effective)};
}
std::filesystem::path resolve_sibling_mmltk_cli(const std::filesystem::path& executable_path) {
 const std::filesystem::path cli_path = executable_path.parent_path() / "mmltk";
 if (!std::filesystem::exists(cli_path)) { throw std::runtime_error("failed to locate sibling mmltk next to mmltk-browser-host: " + cli_path.string()); }
 return cli_path;
}
}  // namespace mmltk::controller::services
