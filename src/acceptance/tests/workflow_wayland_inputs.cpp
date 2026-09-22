#include "workflow_wayland_inputs.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <c10/cuda/CUDAGuard.h>
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/backend/data/compiled_dataset.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/training/tests/model_state_fixture.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/controller/contracts/gui_settings.h"
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
  if (name.starts_with("transformer.enc_out_class_embed.") || (name.starts_with("transformer.enc_out_bbox_embed.") && name.find(".layers.2.") != std::string::npos))
   parameter.value().zero_();
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
WorkflowWaylandInputs::WorkflowWaylandInputs(const std::filesystem::path& root)
    // Keep real work outstanding across the trainer's one-second live-progress
    // publication interval, rather than observing only epoch-boundary records.
    : fixture_{.root_dir = root.string(), .split = "train", .width = 128, .height = 64, .num_images = 256, .background_images = 0, .pixel_evidence = true},
      weights_(root / "workflow.pt"),
      video_(root / "workflow.y4m") {
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
 config.segmentation = false;
 rfdetr::NativeRfDetrModel model{config, rfdetr::native_training_class_layout(*compiled.class_catalog())};
 prepare_caption_model(model, *compiled.class_catalog());
 rfdetr::DecodedNativeModelState checkpoint;
 checkpoint.metadata.preset_name = config.preset_name;
 checkpoint.metadata.source_kind = "wayland-workflow-fixture";
 checkpoint.metadata.source_path = weights_.string();
 checkpoint.metadata.num_classes = config.num_classes;
 checkpoint.metadata.class_layout = model.class_layout()->record();
 checkpoint.metadata.num_queries = config.num_queries;
 checkpoint.metadata.num_select = config.num_select;
 checkpoint.replace_entries(rfdetr::testsupport::clone_normalized_model_state(model));
 rfdetr::save_native_checkpoint(weights_, checkpoint);
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
 train.request.train_compiled_path = compiled;
 train.request.val_compiled_path = compiled;
 train.auto_output = false;
 train.request.output_dir = output / "training";
 std::filesystem::create_directories(train.request.output_dir);
 train.request.resolution = 64;
 train.request.num_queries = 6;
 train.request.eval_max_dets = 6;
 train.request.batch_size = 2;
 train.request.val_batch_size = 2;
 train.request.epochs = 8;
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
 validate.request.report_json_path = output / "validation.json";
 auto& export_state = settings.workflows.export_state;
 export_state.model_source = contracts::ModelSelectionSource::Custom;
 export_state.model_input = contracts::ModelArtifactInputKind::Weights;
 export_state.weights_path = weights_;
 export_state.model_resolution = 64;
 export_state.build_tensorrt = false;
 export_state.onnx_output_path = output / "cancelled-export.onnx";
 auto& predict = settings.workflows.predict;
 predict.model_source = contracts::ModelSelectionSource::Custom;
 predict.model_input = contracts::ModelArtifactInputKind::Weights;
 predict.request.weights_path = weights_;
 predict.request.resolution = 64;
 predict.request.threshold = 0.0F;
 predict.request.h2d_dataloader = true;
 predict.request.compilation_mode = rfdetr::CompilationMode::kNone;
 predict.request.output_path = output / "predictions.json";
 predict.source.kind = contracts::SourceKind::CompiledDataset;
 predict.source.compiled_path = compiled;
 predict.source.single_image_path = (std::filesystem::path{data::testsupport::dataset_dir(fixture_)} / fixture_.split / "000001.png").string();
 predict.source.video_file_path = video_.string();
}
}  // namespace mmltk::testsupport
