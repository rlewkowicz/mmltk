#include "workflow_wayland_inputs.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
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
WorkflowWaylandInputs::WorkflowWaylandInputs(const std::filesystem::path& root)
    : fixture_{.root_dir = root.string(), .split = "train", .width = 64, .height = 64, .num_images = 24, .background_images = 0, .pixel_evidence = true},
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
    train.request.output_dir = output / "training";
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
    validate.request.h2d_dataloader = true;
    validate.request.report_json_path = output / "validation.json";
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
