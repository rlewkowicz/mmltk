#pragma once
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/models/rfdetr/contract/encoder_pretraining.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/common/io/file_digest.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::backend::models::rfdetr::testsupport {
inline void require_reusable_encoder_request(const TrainRequest& request) {
 CHECK(request.from_scratch);
 CHECK(request.encoder_weights_path.empty());
}
// Optional absence alone skips. Explicit evidence always admits every original
// asset before any CLI/product path is invoked; this helper never acquires files.
[[nodiscard]] inline std::optional<std::filesystem::path> encoder_training_evidence() {
 const auto* configured = std::getenv("MMLTK_RFDETR_ENCODER_PRETRAINING_DIR");
 if (!configured) return std::nullopt;
 REQUIRE(*configured != '\0');
 const auto root = std::filesystem::canonical(configured);
 REQUIRE(std::filesystem::is_regular_file(root / "manifest.json"));
 for (const auto& asset : kEncoderPretrainingAssets) {
  const auto path = root / "assets" / (std::string(mmltk::frameworks::reflection::enum_name(asset.backbone)) + ".safetensors");
  CAPTURE(path);
  const auto file = mmltk::common::io::try_file_digests(path, false);
  REQUIRE(file);
  REQUIRE(file->snapshot.bytes == asset.file_bytes);
  REQUIRE(mmltk::common::io::sha256_hex(file->sha256) == asset.sha256);
  file->snapshot.RequireUnchanged(path);
 }
 return root;
}
[[nodiscard]] inline TrainRequest encoder_training_request(const std::filesystem::path& root, const std::filesystem::path& evidence, std::string_view preset_name = "rf-detr-atto") {
 const auto* preset = find_preset_catalog_entry(preset_name);
 REQUIRE(preset);
 const auto config = native_config_from_preset(*preset);
 mmltk::backend::data::testsupport::FixtureSpec spec{
  .root_dir = root.string(), .width = config.patch_size * config.num_windows * 2, .height = config.patch_size * config.num_windows * 2, .num_images = 4, .background_images = 0
 };
 mmltk::backend::data::testsupport::create_compiled_fixture(spec, config.has_keypoints());
 TrainRequest request;
 request.preset_name = preset_name;
 request.from_scratch = true;
 request.encoder_weights_path = root / "source.safetensors";
 std::filesystem::create_symlink(evidence / "assets" / (std::string(mmltk::frameworks::reflection::enum_name(config.backbone.kind)) + ".safetensors"), request.encoder_weights_path);
 request.train_compiled_path = request.val_compiled_path = request.test_compiled_path = mmltk::backend::data::testsupport::compiled_bin_path(spec);
 request.output_dir = root / "initial";
 request.num_queries = 8;
 request.batch_size = 1;
 request.val_batch_size = 2;
 request.lanes = 2;
 request.workers = 4;
 request.epochs = 1;
 request.compilation_mode = CompilationMode::kNone;
 request.amp = request.fused_optimizer = request.progress_bar = false;
 request.use_ema = true;
 request.validation_loss = true;
 request.freeze_encoder = true;
 return request;
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
