#include "workflow_wayland_inputs.h"
#include "src/frameworks/serialization/reflected_json.h"
#include "src/common/io/file_digest.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <c10/cuda/CUDAGuard.h>
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/backend/data/compiled/compiled_dataset.h"
#include "src/backend/data/compiler/dataset_compiler.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/training/tests/model_state_fixture.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/controller/contracts/gui_settings.h"
#include "src/backend/models/rfdetr/core/keypoint_prediction.h"
#include "src/backend/models/rfdetr/core/keypoint_layout.h"
import mmltk.backend.models.rfdetr.model_export;
import mmltk.backend.models.rfdetr.core.dataset_limit_resolution;
namespace mmltk::testsupport {
namespace data = mmltk::backend::data;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace contracts = mmltk::controller::contracts;
namespace {
void prepare_caption_model(rfdetr::NativeRfDetrModel& model, const data::catalog::ClassCatalog& catalog) {
 // Keep the real native detector and its six queries. Its fixture checkpoint
 // must put Det captions over the synthetic GT boxes, not depend on randomly
 // initialized proposal scores or geometry. Training can still update it.
 const auto& config = model.config();
 if (!config.bbox_reparam || !config.lite_refpoint_refine) throw std::logic_error("caption fixture requires reparameterized single-step box refinement");
 const c10::cuda::CUDAGuard device(0);
 const torch::NoGradGuard no_grad;
 const mmltk::backend::ml::cuda::TorchAutocastScope precision(false, at::kFloat);
 model.to(torch::Device(torch::kCUDA, 0));
 model.eval();
 auto parameters = model.named_parameters();
 for (const auto& parameter : parameters) {
  const auto& name = parameter.key();
  if (name.starts_with("transformer.enc_out_class_embed.") || (name.starts_with("transformer.enc_out_bbox_embed.") && name.find(".layers.2.") != std::string::npos)) parameter.value().zero_();
 }
 const auto longest = std::ranges::max_element(catalog.names(), {}, [](const auto& name) { return name.size(); });
 const auto category = catalog.resolve(*longest).value();
 parameters["class_embed.weight"].zero_();
 parameters["class_embed.bias"].fill_(-8.0);
 parameters["class_embed.bias"][category].fill_(8.0);
 // Read the model's actual selected proposals instead of mirroring its feature
 // pyramid, proposal ordering, or top-k implementation in this fixture.
 const auto options = torch::TensorOptions().device(torch::kCUDA).dtype(torch::kFloat);
 const rfdetr::NestedTensor batch{torch::zeros({2, 3, config.resolution, config.resolution}, options), torch::zeros({2, config.resolution, config.resolution}, options.dtype(torch::kBool))};
 const auto proposals = model.forward(batch, false).main.pred_boxes[0];
 const auto extent = proposals.narrow(1, 2, 2);
 const auto correction = torch::cat({(0.5 - proposals.narrow(1, 0, 2)) / extent, torch::log(0.5 / extent)}, 1);
 parameters["refpoint_embed.weight"].view({-1, config.num_queries, 4}).copy_(correction.unsqueeze(0));
 const auto boxes = model.forward(batch, false).main.pred_boxes;
 if (!torch::allclose(boxes, torch::full_like(boxes, 0.5), 1e-5, 1e-5)) throw std::runtime_error("caption fixture did not produce its overlapping normalized boxes");
 model.to(torch::kCPU);
}
}  // namespace
WorkflowWaylandInputs::WorkflowWaylandInputs(const std::filesystem::path& root, bool keypoints)
    // Keep real work outstanding across the trainer's one-second live-progress
    // publication interval, rather than observing only epoch-boundary records.
    : fixture_{.root_dir = root.string(), .split = "train", .width = 128, .height = 64, .num_images = 256, .background_images = 0, .pixel_evidence = true},
      weights_(root / "workflow.pt"),
      video_(root / "workflow.y4m"),
      keypoints_(keypoints) {
 if (keypoints_) {
  fixture_.num_images = 8;
  data::testsupport::create_keypoint_dataset(fixture_);
 } else
  data::testsupport::create_synthetic_dataset(fixture_);
 const auto plan = data::DatasetCompiler::prepare({.source_dir = data::testsupport::dataset_dir(fixture_),
                                                   .output_dir = data::testsupport::compiled_dir(fixture_),
                                                   .split = fixture_.split,
                                                   .target_width = 64,
                                                   .target_height = 64,
                                                   .worker_cpus = {}},
  {fixture_.split});
 data::DatasetCompiler::compile(plan, 0U);
 const auto compiled = data::CompiledDataset::open(data::testsupport::compiled_bin_path(fixture_));
 auto config = rfdetr::native_config_from_preset(rfdetr::model_presets().front());
 config.resolution = 64;
 config.num_classes = static_cast<int>(compiled.class_catalog()->size()) + 1;
 config.num_queries = 6;
 config.num_select = 6;
 config.task = keypoints_ ? rfdetr::ModelTask::Keypoints : rfdetr::ModelTask::Detection;
 if (keypoints_) {
  config.keypoint_head = rfdetr::KeypointHeadKind::GroupPose;
  config.projector = rfdetr::ProjectorKind::DualKeypointOnly;
  config.keypoints_per_class.assign(config.num_classes, 0);
  for (std::size_t index = 0; index < compiled.class_catalog()->size(); ++index)
   config.keypoints_per_class[index] = static_cast<std::uint16_t>(compiled.class_catalog()->keypoints(index).names.size());
  config.keypoint_l1_loss_coef = config.keypoint_findable_loss_coef = config.keypoint_visible_loss_coef = config.keypoint_nll_loss_coef = 1;
 }
 rfdetr::NativeRfDetrModel model{config, rfdetr::native_training_class_layout(*compiled.class_catalog())};
 prepare_caption_model(model, *compiled.class_catalog());
 if (keypoints_) {
  const torch::NoGradGuard no_grad;
  auto parameters = model.named_parameters();
  rfdetr::KeypointPredictionPacket::select<^^rfdetr::KeypointPrediction::findable_logit>(parameters["keypoint_embed.layers.2.bias"], 0).fill_(8);
  rfdetr::KeypointPredictionPacket::select<^^rfdetr::KeypointPrediction::visible_logit>(parameters["keypoint_embed.layers.2.bias"], 0).fill_(8);
 }
 rfdetr::DecodedNativeModelState checkpoint;
 checkpoint.metadata = rfdetr::testsupport::model_checkpoint_metadata(model, "wayland-workflow-fixture", weights_.string());
 checkpoint.replace_entries(rfdetr::testsupport::clone_normalized_model_state(model));
 rfdetr::save_native_checkpoint(weights_, checkpoint);
 if (keypoints_) {
  const torch::NoGradGuard no_grad;
  auto parameters = model.named_parameters();
  rfdetr::KeypointPredictionPacket::select<^^rfdetr::KeypointPrediction::x>(parameters["keypoint_embed.layers.2.bias"], 0).add_(2);
  checkpoint.replace_entries(rfdetr::testsupport::clone_normalized_model_state(model));
  const auto alternate_weights = root / "alternate.pt";
  rfdetr::save_native_checkpoint(alternate_weights, checkpoint);
  alternate_ = root / "alternate.onnx";
  rfdetr::ExportOnnxRequest request;
  request.weights_path = alternate_weights;
  request.output_path = alternate_;
  request.preset_name = config.preset_name;
  request.resolution = config.resolution;
  rfdetr::export_onnx(request);
 }
 std::ofstream video{video_, std::ios::binary};
 video.exceptions(std::ios::badbit | std::ios::failbit);
 video << "YUV4MPEG2 W64 H64 F2:1 Ip A1:1 C420jpeg\n";
 std::array<char, 64U * 64U * 3U / 2U> pixels;
 pixels.fill(static_cast<char>(128));
 for (int frame = 0; frame < 12; ++frame) {
  std::fill_n(pixels.begin(), 64U * 64U, static_cast<char>(32 + frame * 16));
  video << "FRAME\n";
  video.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));
 }
 video.close();
}
void WorkflowWaylandInputs::Configure(contracts::GuiSettingsState& settings, const std::filesystem::path& output) const {
 const auto compiled = data::testsupport::compiled_bin_path(fixture_);
 auto& train = settings.workflows.train;
 train.model_source = contracts::ModelSelectionSource::Custom;
 train.model_input = contracts::ModelArtifactInputKind::Weights;
 train.use_compiled_directory_defaults = false;
 train.request.weights_path = weights_;
 // The browser runs from the session output root. Exercise relative admission
 // while retaining a distinct spelling for the validation input.
 train.request.train_compiled_path = std::filesystem::relative(compiled, output);
 train.request.val_compiled_path = std::filesystem::path(".") / train.request.train_compiled_path;
 train.output.automatic = false;
 train.output.directory = (output / "training").string();
 std::filesystem::create_directories(train.output.directory);
 train.request.resolution = 64;
 train.request.num_queries = 6;
 train.request.eval_max_dets = 6;
 train.request.batch_size = 2;
 train.request.val_batch_size = 2;
 train.request.epochs = keypoints_ ? 1 : 8;
 train.request.workers = 2;
 train.request.lanes = 1;
 train.request.freeze_encoder = true;
 train.request.gpu_augmentation.enabled = false;
 train.request.h2d_dataloader = true;
 train.request.compilation_mode = rfdetr::CompilationMode::kNone;
 auto& validate = settings.workflows.validate;
 validate.model_source = contracts::ModelSelectionSource::Custom;
 validate.model_input = contracts::ModelArtifactInputKind::Weights;
 validate.request.weights_path = weights_;
 validate.request.compiled_path = compiled;
 validate.request.resolution = 64;
 validate.request.batch_size = 2;
 // Match the calibrated native fixture's proposal selection and coordinates.
 validate.request.allow_fp16 = false;
 validate.request.h2d_dataloader = true;
 validate.output = {.automatic = false, .directory = (output / "validation").string()};
 auto& export_state = settings.workflows.export_state;
 export_state.model_source = contracts::ModelSelectionSource::Custom;
 export_state.model_input = contracts::ModelArtifactInputKind::Weights;
 export_state.weights_path = weights_;
 export_state.model_resolution = 64;
 export_state.build_tensorrt = false;
 export_state.output = {.automatic = false, .directory = (output / "export").string()};
 auto& predict = settings.workflows.predict;
 predict.model_source = contracts::ModelSelectionSource::Custom;
 predict.model_input = contracts::ModelArtifactInputKind::Weights;
 predict.request.weights_path = weights_;
 predict.request.resolution = 64;
 predict.request.threshold = 0.0F;
 predict.request.h2d_dataloader = true;
 predict.request.compilation_mode = rfdetr::CompilationMode::kNone;
 predict.output = {.automatic = false, .directory = (output / "prediction").string()};
 predict.source.kind = contracts::SourceKind::CompiledDataset;
 predict.source.compiled_path = compiled;
 predict.source.single_image_path = (std::filesystem::path{data::testsupport::dataset_dir(fixture_)} / fixture_.split / "000001.png").string();
 predict.source.video_file_path = video_.string();
 if (keypoints_) {
  train.dataset_source_dir = data::testsupport::dataset_dir(fixture_);
  train.compiled_dataset_dir = data::testsupport::compiled_dir(fixture_);
  settings.workflows.explore.dataset_source = contracts::ExploreDatasetSource::Train;
  settings.workflows.annotate.output_dir = (output / "annotations").string();
  predict.model_input = contracts::ModelArtifactInputKind::Onnx;
  predict.request.weights_path.clear();
  predict.request.onnx_path = output / "export" / "model.onnx";
  predict.request.allow_fp16 = false;
 }
}
void WorkflowWaylandInputs::VerifyCatalogTraining(
 const std::filesystem::path& session_root, const std::filesystem::path& output, const std::filesystem::path& checkpoint, std::string_view run_id, std::string_view preset_name) const {
 if (!keypoints_) throw std::logic_error("catalog keypoint evidence requires its retained dataset fixture");
 const auto root = std::filesystem::canonical(session_root);
 const auto inside = [&](const std::filesystem::path& path) {
  const auto absolute = std::filesystem::canonical(path.is_absolute() ? path : root / path);
  const auto relative = absolute.lexically_relative(root);
  if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("catalog training artifact escaped its acceptance root");
  return absolute;
 };
 const auto directory = inside(output), artifact = inside(checkpoint);
 const auto manifest = directory / "run.json";
 const auto bytes = std::filesystem::file_size(manifest);
 if (bytes > rfdetr::kTrainingManifestBytes) throw std::runtime_error("catalog training manifest exceeds its native budget");
 std::ifstream stream(manifest, std::ios::binary);
 std::string text(static_cast<std::size_t>(bytes), '\0');
 if (!stream.read(text.data(), static_cast<std::streamsize>(text.size()))) throw std::runtime_error("catalog training manifest read failed");
 const auto run = mmltk::frameworks::serialization::decode_reflected_json<rfdetr::TrainingRun>(text, {.max_bytes = rfdetr::kTrainingManifestBytes, .max_items = 65536, .max_depth = 32});
 const auto* preset = rfdetr::find_preset_catalog_entry(preset_name);
 if (!preset || preset->task != rfdetr::ModelTask::Keypoints || run.run_id != run_id || run.configuration.preset_name != preset_name ||
     run.configuration.resolution != static_cast<int>(preset->resolution) || run.configuration.epochs != 1 || !run.configuration.resume_path.empty() ||
     run.configuration.weights_path != run.original_weights || run.original_weights == weights_ || std::filesystem::path(run.original_weights).filename() != preset->canonical_weight_filename)
  throw std::runtime_error("catalog training did not operate the selected ordinary weights/configuration");
 const auto compiled = data::CompiledDataset::open(run.configuration.train_compiled_path.is_absolute() ? run.configuration.train_compiled_path : root / run.configuration.train_compiled_path);
 if (compiled.header().image_width != preset->resolution || compiled.header().image_height != preset->resolution)
  throw std::runtime_error("catalog training input resolution differs from the selected model");
 const auto digest = mmltk::common::io::try_file_digests(run.original_weights, true);
 if (!digest || digest->md5 != preset->canonical_weight_md5) throw std::runtime_error("catalog training original weights lost their canonical hash");
 auto expected = rfdetr::native_config_from_preset(*preset);
 auto admitted = rfdetr::resolve_model_state(run.original_weights, preset_name, expected.resolution);
 rfdetr::require_checkpoint_model_topology(expected, admitted.model_state.metadata);
 digest->snapshot.RequireUnchanged(run.original_weights);
 const auto automatic_cap = static_cast<std::size_t>(admitted.artifacts.automatic_num_queries_cap);
 admitted.model_state.release_admission();
 const auto validation = data::CompiledDataset::open(run.configuration.val_compiled_path.is_absolute() ? run.configuration.val_compiled_path : root / run.configuration.val_compiled_path);
 if (!run.configuration.test_compiled_path.empty() || validation.class_catalog()->record() != compiled.class_catalog()->record()) throw std::runtime_error("catalog fixture split schemas differ");
 const auto largest = std::max(compiled.header().max_instances_per_image, validation.header().max_instances_per_image);
 const auto queries = rfdetr::resolve_dataset_query_limit(largest, run.configuration.num_queries, automatic_cap);
 const auto& limits = run.execution.dataset_limits;
 if (limits.largest_max_instances != largest || limits.automatic_num_queries_cap != automatic_cap || limits.resolved_num_queries != queries.as_size || limits.automatic != queries.automatic)
  throw std::runtime_error("catalog training query admission differs from native dataset policy");
 const auto trained = rfdetr::decode_native_model_state(artifact);
 const auto config = rfdetr::checkpoint_configuration(trained.metadata);
 const auto layout = rfdetr::native_training_class_layout(*compiled.class_catalog());
 if (trained.metadata.class_layout != layout || run.class_layout != layout || trained.metadata.source_path != run.original_weights)
  throw std::runtime_error("completed checkpoint lost admitted dataset or original weights identity");
 rfdetr::KeypointTensorLayout(config.keypoints_per_class).require_class_layout(rfdetr::ResolvedClassLayout(layout));
 // The native query and schema owners prove these transfer-derived dimensions.
 // Every other selected architecture member is compared by native reflection.
 expected.num_classes = static_cast<int>(layout.slots.size());
 expected.keypoints_per_class = config.keypoints_per_class;
 expected.num_queries = queries.as_int;
 expected.num_select = queries.as_int;
 rfdetr::require_checkpoint_model_topology(expected, trained.metadata);
 if (config.resolution != expected.resolution || config.preset_name != preset_name || config.task != preset->task || config.num_queries != queries.as_int || config.num_select != queries.as_int)
  throw std::runtime_error("completed checkpoint lost catalog model or admitted query identity");
}
WorkflowArtifactMutation::WorkflowArtifactMutation(const std::filesystem::path& root, std::filesystem::path alternate)
    : artifact_(std::filesystem::absolute(root / "export" / "model.onnx").lexically_normal()), backup_(artifact_.string() + ".acceptance-retained"), alternate_(std::move(alternate)) {}
WorkflowArtifactMutation::~WorkflowArtifactMutation() { Restore(); }
void WorkflowArtifactMutation::Restore() noexcept {
 if (!held_) return;
 std::error_code error;
 if (companion_) {
  std::filesystem::rename(backup_.string() + ".classes.json", artifact_.string() + ".classes.json", error);
  if (error) {
   std::fprintf(stderr, "acceptance artifact companion restore failed: %s\n", error.message().c_str());
   return;
  }
  companion_ = false;
  replacement_companion_ = false;
 } else if (replacement_companion_) {
  std::filesystem::remove(artifact_.string() + ".classes.json", error);
  if (error) {
   std::fprintf(stderr, "acceptance replacement companion removal failed: %s\n", error.message().c_str());
   return;
  }
  replacement_companion_ = false;
 }
 std::filesystem::rename(backup_, artifact_, error);
 if (error) {
  std::fprintf(stderr, "acceptance artifact restore failed: %s\n", error.message().c_str());
  return;
 }
 held_ = false;
}
void WorkflowArtifactMutation::Apply(std::uint32_t index) {
 if (index != next_) throw std::logic_error("acceptance artifact mutation order");
 if (index == 1) {
  Restore();
  if (held_) throw std::runtime_error("acceptance artifact restore incomplete");
 } else if (index == 0 || index == 2) {
  if (held_ || !std::filesystem::is_regular_file(artifact_) || std::filesystem::exists(backup_)) throw std::logic_error("acceptance exported artifact is unavailable");
  const bool has_companion = std::filesystem::exists(artifact_.string() + ".classes.json");
  std::filesystem::rename(artifact_, backup_);
  held_ = true;
  if (has_companion) {
   std::filesystem::rename(artifact_.string() + ".classes.json", backup_.string() + ".classes.json");
   companion_ = true;
  }
  if (index == 2) {
   std::filesystem::copy_file(alternate_, artifact_);
   if (std::filesystem::exists(alternate_.string() + ".classes.json")) {
    replacement_companion_ = true;
    std::filesystem::copy_file(alternate_.string() + ".classes.json", artifact_.string() + ".classes.json");
   }
  }
 } else
  throw std::logic_error("unexpected acceptance artifact mutation");
 ++next_;
}
}  // namespace mmltk::testsupport
