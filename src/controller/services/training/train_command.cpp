#include "src/controller/services/training/train_command.h"
#include "src/pch_std.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/encoder_pretraining.h"
#include "src/common/io/file_digest.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
namespace mmltk::controller::services {
std::vector<std::string> build_train_command_arguments(const mmltk::backend::models::rfdetr::TrainRequest& request, const std::string_view fallback_preset_name) {
 auto effective = request;
 if (effective.preset_name.empty()) effective.preset_name = fallback_preset_name;
 return {"rfdetr", "train", "--request-json", mmltk::backend::models::rfdetr::encode_train_request_json(effective)};
}
void prepare_train_encoder(mmltk::backend::models::rfdetr::TrainRequest& request, const ArtifactStore& store, const ArtifactCancellationToken& cancellation) {
 namespace rfdetr = mmltk::backend::models::rfdetr;
 rfdetr::validate_train_request(request);
 if (!request.from_scratch) return;
 if (request.distributed_worker) {
  if (request.encoder_weights_path.empty()) throw std::invalid_argument("scratch worker requires a resolved encoder path");
  return;
 }
 if (cancellation.cancelled()) throw std::runtime_error("encoder acquisition cancelled");
 const auto* preset = rfdetr::find_preset_catalog_entry(request.preset_name);
 if (!preset) throw std::invalid_argument("scratch training requires a known preset");
 const auto& asset = *rfdetr::find_encoder_pretraining_asset(preset->backbone.kind);
 if (request.encoder_weights_path.empty()) {
  request.encoder_weights_path = store.canonical_encoder_weight_path(asset.backbone, cancellation);
 } else {
  const auto digest = mmltk::common::io::try_file_digests(request.encoder_weights_path, false, [&] { return cancellation.cancelled(); });
  if (!digest || digest->snapshot.bytes != asset.file_bytes || mmltk::common::io::sha256_hex(digest->sha256) != asset.sha256)
   throw std::invalid_argument("resolved scratch encoder does not match the preset asset");
  digest->snapshot.RequireUnchanged(request.encoder_weights_path);
 }
 if (cancellation.cancelled()) throw std::runtime_error("encoder acquisition cancelled");
 request.encoder_weights_path = std::filesystem::absolute(request.encoder_weights_path);
}
std::filesystem::path resolve_sibling_mmltk_cli(const std::filesystem::path& executable_path) {
 const std::filesystem::path cli_path = executable_path.parent_path() / "mmltk";
 if (!std::filesystem::exists(cli_path)) { throw std::runtime_error("failed to locate sibling mmltk next to mmltk-browser-host: " + cli_path.string()); }
 return cli_path;
}
}  // namespace mmltk::controller::services
