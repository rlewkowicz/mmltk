// Compiled only against the unchanged 1c13fec8 application and its fixtures.
// The historical compiler, trainer, serializers and optimizer produce every
// product byte. This driver owns inputs and provenance observations only.
#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/data/compiled/compiled_dataset.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/training/train.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/backend/models/rfdetr/training/detail/training_session_checkpoint.h"
#include "src/backend/models/rfdetr/training/tests/model_state_fixture.h"
#include "historical_identity_wrap.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>
namespace rf = mmltk::backend::models::rfdetr;
namespace data = mmltk::backend::data;
namespace {
std::mutex identity_mutex;
std::string original_signature;
bool signature_mismatch = false;
std::atomic_bool signature_observation_failed = false;
}  // namespace
// The linker symbol is derived from the historical archive, not handwritten.
decltype(rf::training_dataset_identity) original_identity asm(MMLTK_BASELINE_REAL);
decltype(rf::training_dataset_identity) observed_identity asm(MMLTK_BASELINE_WRAP);
std::string observed_identity(const data::FileHeader& header, const mmltk::common::io::FileSnapshot& file, std::string_view signature) {
 auto result = original_identity(header, file, signature);
 try {
  std::lock_guard lock(identity_mutex);
  if (original_signature.empty())
   original_signature = signature;
  else
   signature_mismatch |= original_signature != signature;
 } catch (...) { signature_observation_failed.store(true, std::memory_order_relaxed); }
 return result;
}
TEST_CASE("Original application produces immutable continuation evidence", "[historical_producer]") {
 const auto* destination = std::getenv("MMLTK_RFDETR_BASELINE_OUTPUT");
 REQUIRE(destination != nullptr);
 const auto root = std::filesystem::canonical(destination);
 REQUIRE_FALSE(std::filesystem::exists(root / "historical.json"));
 data::testsupport::FixtureSpec inputs{.root_dir = (root / "inputs").string(), .width = 64, .height = 64, .num_images = 4, .background_images = 1};
 data::testsupport::create_synthetic_dataset(inputs);
 data::testsupport::compile_existing_fixture(inputs);
 const auto dataset_path = data::testsupport::compiled_bin_path(inputs);
 const auto dataset = data::CompiledDataset::open(dataset_path);
 REQUIRE(dataset.header().version == 10);
 auto config = rf::native_config_from_preset(*rf::find_preset_catalog_entry("rf-detr-nano"));
 config.resolution = 64;
 config.num_queries = config.num_select = 8;
 config.num_classes = static_cast<int>(dataset.class_catalog()->size()) + 1;
 const auto seed_path = root / "inputs/seed.pt";
 {
  torch::manual_seed(731);
  rf::NativeRfDetrModel model(config, rf::native_training_class_layout(*dataset.class_catalog()));
  rf::DecodedNativeModelState seed;
  seed.metadata = rf::testsupport::model_checkpoint_metadata(model, "historical-baseline-input", seed_path.string());
  seed.replace_entries(rf::testsupport::clone_normalized_model_state(model, true));
  rf::save_native_checkpoint(seed_path, seed);
 }
 rf::TrainRequest request;
 request.preset_name = "rf-detr-nano";
 request.resolution = config.resolution;
 request.weights_path = seed_path;
 request.train_compiled_path = request.val_compiled_path = request.test_compiled_path = dataset_path;
 request.output_dir = root / "original";
 request.epochs = 1;
 request.num_queries = config.num_queries;
 request.batch_size = 1;
 request.val_batch_size = 2;
 request.lanes = 1;
 request.workers = 4;
 request.compilation_mode = rf::CompilationMode::kNone;
 request.amp = request.fused_optimizer = request.progress_bar = false;
 request.use_ema = request.validation_loss = request.freeze_encoder = true;
 const auto original = rf::run_training(request);
 REQUIRE(original.selected);
 REQUIRE(original.test_summary);
 rf::TrainingSessionAdmission saved(original.checkpoint_path);
 REQUIRE(std::ranges::any_of(saved.manifest().models, [](const auto& value) { return value.best.has_value(); }));
 request.weights_path.clear();
 request.resume_path = original.checkpoint_path;
 request.output_dir = root / "continued";
 request.epochs = 2;
 const auto continued = rf::run_training(request);
 REQUIRE(continued.selected);
 REQUIRE(continued.test_summary);
 REQUIRE_FALSE(original_signature.empty());
 REQUIRE_FALSE(signature_mismatch);
 REQUIRE_FALSE(signature_observation_failed.load(std::memory_order_relaxed));
 std::filesystem::create_directories(root / "source_headers");
 for (const auto* name : {"configuration", "validation"}) {
  std::ofstream header(root / "source_headers" / name, std::ios::binary);
  header.write(reinterpret_cast<const char*>(&dataset.header()), sizeof(data::FileHeader));
  REQUIRE(header.good());
 }
 nlohmann::json record{
  {"session", original.checkpoint_path.string()}, {"continued_session", continued.checkpoint_path.string()}, {"continued_selection", (request.output_dir / "selected.json").string()},
  {"configuration_signature", original_signature}, {"source_headers", {{"configuration", "source_headers/configuration"}, {"validation", "source_headers/validation"}}}
 };
 std::ofstream output(root / "historical.json");
 output << record.dump(2) << '\n';
 REQUIRE(output.good());
 saved.require_unchanged();
}
