#pragma once
#include <filesystem>
#include <catch2/catch_test_macros.hpp>
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/controller/services/settings_store.h"
#include "src/controller/services/settings_location.h"
namespace mmltk::controller::test_support {
[[nodiscard]] inline services::SettingsLocation install_settings(const std::filesystem::path& root) {
 auto settings = contracts::default_gui_settings_state();
 settings.workflows.train.dataset_source_dir = root / "source";
 settings.workflows.train.compiled_dataset_dir = root / "compiled";
 settings.workflows.train.request.train_compiled_path = root / "train.bin";
 settings.workflows.train.request.val_compiled_path = root / "val.bin";
 settings.workflows.train.request.test_compiled_path.clear();
 settings.workflows.train.request.weights_path = root / "weights.pt";
 settings.workflows.train.request.output_dir = root / "training";
 settings.workflows.train.model_source = contracts::ModelSelectionSource::Custom;
 settings.workflows.train.model_input = contracts::ModelArtifactInputKind::Weights;
 settings.workflows.train.remote_container_image = "mmltk-test";
 settings.workflows.train.remote_launch_template = "mmltk-test";
 settings.workflows.validate.request.compiled_path = root / "val.bin";
 settings.workflows.validate.request.weights_path = root / "weights.pt";
 settings.workflows.validate.model_source = contracts::ModelSelectionSource::Custom;
 settings.workflows.validate.model_input = contracts::ModelArtifactInputKind::Weights;
 settings.workflows.predict.source.compiled_path = (root / "train.bin").string();
 settings.workflows.predict.request.output_path = root / "prediction.json";
 settings.workflows.predict.request.weights_path = root / "weights.pt";
 settings.workflows.predict.model_source = contracts::ModelSelectionSource::Custom;
 settings.workflows.predict.model_input = contracts::ModelArtifactInputKind::Weights;
 settings.workflows.export_state.weights_path = root / "weights.pt";
 settings.workflows.export_state.onnx_input_path = root / "model-input.onnx";
 settings.workflows.export_state.onnx_output_path = root / "model-output.onnx";
 settings.workflows.export_state.model_source = contracts::ModelSelectionSource::Custom;
 settings.workflows.export_state.model_input = contracts::ModelArtifactInputKind::Onnx;
 REQUIRE(contracts::gui_settings_valid(settings));
 const services::SettingsLocation location{(root / "settings.json").string()};
 REQUIRE(services::SettingsStore::save(location.value(), settings, 1U).succeeded());
 return location;
}
}  // namespace mmltk::controller::test_support
