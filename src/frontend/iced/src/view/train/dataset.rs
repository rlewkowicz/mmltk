use crate::fluent_theme::Element;
use crate::view::settings::{EditCadence, EditSchedule, SettingsModel};
use iced::Fill;
use iced::widget::{button, checkbox, column, container, text};

#[derive(Debug, Clone)]
pub enum Message {
    BenchmarkChanged(bool),
    SourceChanged(String),
    CompiledDirectoryChanged(String),
    InferSplitsChanged(bool),
    TrainSplitChanged(String),
    ValidationSplitChanged(String),
    TestSplitChanged(String),
    OverwriteChanged(bool),
    CompileDimensionsChanged(bool),
    PerceptualDownscaleChanged(bool),
    ResolutionChanged(i32),
    Browse(u64),
    Compile,
    Stop,
}

#[derive(Debug, Clone)]
pub enum Outcome {
    SettingsEdited(EditSchedule),
    Browse(u64),
    Compile,
    Stop,
}

pub fn update(model: &mut SettingsModel, message: Message) -> Result<Outcome, String> {
    let cadence = EditCadence::Debounced;
    let schedule = match message {
        Message::BenchmarkChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstraincompilebenchmarkdatasetoverride(draft, value)
        })?,
        Message::SourceChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstraindatasetsourcedir(draft, value)
        })?,
        Message::CompiledDirectoryChanged(value) => model.edit_group(cadence, |draft| {
            [
                crate::generated::edit_workflowstraincompileddatasetdir(draft, value),
                crate::generated::edit_workflowstrainusecompileddirectorydefaults(draft, true),
            ]
        })?,
        Message::InferSplitsChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainusecompileddirectorydefaults(draft, value)
        })?,
        Message::TrainSplitChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequesttraincompiledpath(draft, value)
        })?,
        Message::ValidationSplitChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestvalcompiledpath(draft, value)
        })?,
        Message::TestSplitChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequesttestcompiledpath(draft, value)
        })?,
        Message::OverwriteChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainoverwritecompileddataset(draft, value)
        })?,
        Message::PerceptualDownscaleChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstraincompileperceptualdownscale(draft, value)
        })?,
        Message::CompileDimensionsChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstraincompiledimensions(draft, value)
        })?,
        Message::ResolutionChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestresolution(draft, value)
        })?,
        Message::Browse(id) => return Ok(Outcome::Browse(id)),
        Message::Compile => return Ok(Outcome::Compile),
        Message::Stop => return Ok(Outcome::Stop),
    };
    Ok(Outcome::SettingsEdited(schedule))
}

pub fn view<'a>(
    train: Option<&'a crate::generated::TrainViewState>,
    model: &'a crate::view_model::ApplicationModel,
    enabled: bool,
    settled: bool,
) -> Element<'a, Message> {
    let Some(train) = train else {
        return crate::view::shared::card(
            "Dataset",
            "Compile and configure typed dataset facts.",
            text("Dataset settings unavailable"),
        );
    };
    let fields = column![
        container(
            checkbox(train.compilebenchmarkdatasetoverride)
                .label("Compile Benchmark Dataset Override")
                .on_toggle_maybe(enabled.then_some(Message::BenchmarkChanged))
                .style(crate::fluent_theme::checkbox_benchmark)
        )
        .id(super::BENCHMARK_OVERRIDE_ID),
        container(crate::view::workflow::fields::text_field(
            "Dataset source",
            crate::generated::constraint_workflowstraindatasetsourcedir().stable_field_id,
            &train.datasetsourcedir,
            enabled && !train.compilebenchmarkdatasetoverride,
            Message::SourceChanged,
        ))
        .id(super::DATASET_SOURCE_ID),
        container(
            button("Browse dataset source")
                .on_press_maybe(
                    (enabled && !train.compilebenchmarkdatasetoverride).then_some(Message::Browse(
                        crate::generated::constraint_workflowstraindatasetsourcedir()
                            .stable_field_id,
                    ))
                )
                .style(crate::fluent_theme::button_primary)
        )
        .id(super::DATASET_BROWSE_ID),
        container(crate::view::workflow::fields::text_field(
            "Compiled output",
            crate::generated::constraint_workflowstraincompileddatasetdir().stable_field_id,
            &train.compileddatasetdir,
            enabled,
            Message::CompiledDirectoryChanged,
        ))
        .id(super::COMPILED_DIRECTORY_ID),
        button("Browse compiled output")
            .on_press_maybe(enabled.then_some(Message::Browse(
                crate::generated::constraint_workflowstraincompileddatasetdir().stable_field_id,
            )))
            .style(crate::fluent_theme::button_primary),
        crate::view::workflow::fields::toggle(
            "Infer train/validation splits",
            train.usecompileddirectorydefaults,
            enabled,
            Message::InferSplitsChanged,
        ),
    ]
    .spacing(crate::view::workflow::FIELD_SPACING);
    let fields = if train.usecompileddirectorydefaults {
        fields
    } else {
        fields
            .push(crate::view::workflow::fields::text_field(
                "Train split override",
                crate::generated::constraint_workflowstrainrequesttraincompiledpath()
                    .stable_field_id,
                &train.request.traincompiledpath,
                enabled,
                Message::TrainSplitChanged,
            ))
            .push(crate::view::workflow::fields::text_field(
                "Validation split override",
                crate::generated::constraint_workflowstrainrequestvalcompiledpath().stable_field_id,
                &train.request.valcompiledpath,
                enabled,
                Message::ValidationSplitChanged,
            ))
            .push(crate::view::workflow::fields::text_field(
                "Test dataset (optional)",
                crate::generated::constraint_workflowstrainrequesttestcompiledpath()
                    .stable_field_id,
                &train.request.testcompiledpath,
                enabled,
                Message::TestSplitChanged,
            ))
    };
    let fields = fields
        .push(crate::view::workflow::fields::toggle(
            "Overwrite",
            train.overwritecompileddataset,
            enabled,
            Message::OverwriteChanged,
        ))
        .push(
            container(crate::view::workflow::fields::toggle(
                "Compile dimensions",
                train.compiledimensions,
                enabled,
                Message::CompileDimensionsChanged,
            ))
            .id(super::COMPILE_DIMENSIONS_ID),
        );
    let fields = fields.push(
        container(crate::view::workflow::fields::toggle(
            "Perceptual downscaling",
            train.compileperceptualdownscale,
            enabled,
            Message::PerceptualDownscaleChanged,
        ))
        .id(
            crate::generated::constraint_workflowstraincompileperceptualdownscale()
                .stable_field_id
                .to_string(),
        ),
    );
    let fields = if train.compiledimensions {
        fields.push(
            container(crate::view::workflow::fields::number_i32(
                "Compile size",
                train.request.resolution,
                crate::generated::constraint_workflowstrainrequestresolution(),
                enabled,
                Message::ResolutionChanged,
            ))
            .id(super::COMPILE_RESOLUTION_ID),
        )
    } else {
        fields
    };
    let dataset = model.workflow.dataset.as_ref();
    let active = dataset.is_some_and(|state| state.active);
    let action = if active {
        button("Cancel compilation")
            .on_press_maybe(model.dataset_stop_available().then_some(Message::Stop))
    } else {
        button(if train.compilebenchmarkdatasetoverride {
            "Compile Benchmark Dataset"
        } else {
            "Compile Dataset"
        })
        .on_press_maybe((settled && model.dataset_compile_available()).then_some(Message::Compile))
        .style(crate::fluent_theme::button_primary)
    };
    crate::view::shared::identified(
        super::DATASET_CARD_ID,
        crate::view::shared::card(
            "Dataset",
            "Compile and configure typed dataset facts.",
            fields
                .push(text(format!(
                    "Compile Size: {} x {}",
                    train.request.resolution, train.request.resolution
                )))
                .push(
                    container(crate::view::workflow::progress::artifact(dataset))
                        .id(super::COMPILE_PROGRESS_ID),
                )
                .push(
                    container(action.width(Fill))
                        .id(super::COMPILE_DATASET_ID)
                        .width(Fill),
                ),
        ),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view::settings::installed_settings_model;

    #[test]
    fn inference_toggles_retain_optional_test_path() {
        let mut model = installed_settings_model();
        update(
            &mut model,
            Message::TestSplitChanged("/retained/test.bin".into()),
        )
        .unwrap();
        for inferred in [false, true, false] {
            update(&mut model, Message::InferSplitsChanged(inferred)).unwrap();
            let train = &model.draft.as_ref().unwrap().workflows.train;
            assert_eq!(train.usecompileddirectorydefaults, inferred);
            assert_eq!(train.request.testcompiledpath, "/retained/test.bin");
        }
    }

    #[test]
    fn test_selection_and_clear_preserve_inference_and_other_inputs() {
        let mut model = installed_settings_model();
        let before = model.draft.as_ref().unwrap().workflows.train.clone();
        for path in ["/independent/test.bin", ""] {
            assert!(matches!(
                update(&mut model, Message::TestSplitChanged(path.into())),
                Ok(Outcome::SettingsEdited(EditSchedule::Debounce(_)))
            ));
            let train = &model.draft.as_ref().unwrap().workflows.train;
            assert_eq!(train.request.testcompiledpath, path);
            assert_eq!(
                train.usecompileddirectorydefaults,
                before.usecompileddirectorydefaults
            );
            assert_eq!(
                train.request.traincompiledpath,
                before.request.traincompiledpath
            );
            assert_eq!(
                train.request.valcompiledpath,
                before.request.valcompiledpath
            );
        }
    }

    #[test]
    fn dataset_controls_retain_wayland_acceptance_identities() {
        assert_eq!(super::super::DATASET_SOURCE_ID, "train.dataset.source");
        assert_eq!(
            super::super::COMPILED_DIRECTORY_ID,
            "train.dataset.compiled_directory"
        );
        assert_eq!(
            super::super::COMPILE_RESOLUTION_ID,
            "train.dataset.resolution"
        );
        assert_eq!(
            super::super::COMPILE_DIMENSIONS_ID,
            "train.dataset.compile_dimensions"
        );
        let source = crate::generated::constraint_workflowstraindatasetsourcedir().stable_field_id;
        let compiled =
            crate::generated::constraint_workflowstraincompileddatasetdir().stable_field_id;
        assert!(
            crate::generated::FILE_DIALOGS
                .iter()
                .any(|dialog| dialog.stable_field_id == source)
        );
        assert!(
            crate::generated::FILE_DIALOGS
                .iter()
                .any(|dialog| dialog.stable_field_id == compiled)
        );
    }

    #[test]
    fn compile_dimensions_precedes_the_typed_resolution_edit() {
        let mut model = installed_settings_model();
        update(&mut model, Message::CompileDimensionsChanged(true)).unwrap();
        assert!(
            model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .compiledimensions
        );

        update(&mut model, Message::ResolutionChanged(640)).unwrap();
        assert_eq!(
            model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .request
                .resolution,
            640
        );
    }

    #[test]
    fn benchmark_override_keeps_its_typed_debounced_edit() {
        let mut model = installed_settings_model();
        let original = model
            .draft
            .as_ref()
            .unwrap()
            .workflows
            .train
            .compilebenchmarkdatasetoverride;
        assert!(matches!(
            update(&mut model, Message::BenchmarkChanged(!original)),
            Ok(Outcome::SettingsEdited(EditSchedule::Debounce(_)))
        ));
        assert_eq!(
            model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .compilebenchmarkdatasetoverride,
            !original
        );
    }

    #[test]
    fn browse_messages_use_generated_field_identities() {
        let mut model = installed_settings_model();
        let source = crate::generated::constraint_workflowstraindatasetsourcedir().stable_field_id;
        let compiled =
            crate::generated::constraint_workflowstraincompileddatasetdir().stable_field_id;
        assert!(matches!(
            update(&mut model, Message::Browse(source)),
            Ok(Outcome::Browse(id)) if id == source
        ));
        assert!(matches!(
            update(&mut model, Message::Browse(compiled)),
            Ok(Outcome::Browse(id)) if id == compiled
        ));
    }
}
