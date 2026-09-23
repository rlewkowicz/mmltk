use crate::integration_control::{DatasetTextKind, observe_dataset_text};
pub(crate) mod progress;

use crate::fluent_theme::Element;
use crate::generated::{BenchmarkDatasetVariant, CoconutValidation, ImageResizeMode};
use crate::view::settings::{EditCadence, EditSchedule, SettingsModel};
use crate::view::shared::{card_section_divider, disclosure};
use iced::Fill;
use iced::widget::{button, checkbox, column, container, radio, row, text};

#[derive(Debug, Clone)]
pub enum Message {
    Ignore,
    BenchmarkChanged(bool),
    DatasetChanged(BenchmarkDatasetVariant),
    ValidationChanged(CoconutValidation),
    RecoverDroppedMasksChanged(bool),
    SourceChanged(String),
    CompiledDirectoryChanged(String),
    InferSplitsChanged(bool),
    TrainSplitChanged(String),
    ValidationSplitChanged(String),
    TestSplitChanged(String),
    OverwriteChanged(bool),
    CompileDimensionsChanged(bool),
    PerceptualDownscaleChanged(bool),
    ResizeModeChanged(ImageResizeMode),
    ResolutionChanged(i32),
    Browse(u64),
    Compile,
    Stop,
}

#[derive(Debug, Clone)]
pub enum Outcome {
    Ignored,
    SettingsEdited(EditSchedule),
    Browse(u64),
    Compile,
    Stop,
}

pub fn update(model: &mut SettingsModel, message: Message) -> Result<Outcome, String> {
    let cadence = EditCadence::Debounced;
    let schedule = match message {
        Message::Ignore => return Ok(Outcome::Ignored),
        Message::BenchmarkChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstraincompilebenchmarkdatasetoverride(draft, value)
        })?,
        Message::DatasetChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainbenchmarkselectiondataset(draft, value)
        })?,
        Message::ValidationChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainbenchmarkselectionvalidation(draft, value)
        })?,
        Message::RecoverDroppedMasksChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainbenchmarkselectionrecoverdroppedmasks(draft, value)
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
        Message::ResizeModeChanged(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstraincompileresizemode(draft, value)
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
    let benchmark_selection_enabled = enabled
        && !model
            .workflow
            .dataset
            .as_ref()
            .is_some_and(|state| state.active);
    let benchmark = column![
        container(
            checkbox(train.compilebenchmarkdatasetoverride)
                .label("Compile Benchmark Dataset Override")
                .on_toggle_maybe(enabled.then_some(Message::BenchmarkChanged))
                .style(crate::fluent_theme::checkbox_benchmark)
        )
        .id(super::BENCHMARK_OVERRIDE_ID),
        benchmark_choices(train, benchmark_selection_enabled),
    ];
    let fields = column![
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
    let splits = column![
        crate::view::workflow::fields::text_field(
            "Train split override",
            crate::generated::constraint_workflowstrainrequesttraincompiledpath().stable_field_id,
            &train.request.traincompiledpath,
            enabled,
            Message::TrainSplitChanged,
        ),
        crate::view::workflow::fields::text_field(
            "Validation split override",
            crate::generated::constraint_workflowstrainrequestvalcompiledpath().stable_field_id,
            &train.request.valcompiledpath,
            enabled,
            Message::ValidationSplitChanged,
        ),
        crate::view::workflow::fields::text_field(
            "Test dataset (optional)",
            crate::generated::constraint_workflowstrainrequesttestcompiledpath().stable_field_id,
            &train.request.testcompiledpath,
            enabled,
            Message::TestSplitChanged,
        ),
    ]
    .spacing(crate::view::workflow::FIELD_SPACING);
    let paths = column![
        fields,
        disclosure(
            "train.dataset.splits",
            !train.usecompileddirectorydefaults,
            container(splits)
                .padding(iced::Padding::ZERO.top(crate::view::workflow::FIELD_SPACING))
        ),
        container(crate::view::workflow::fields::toggle(
            "Overwrite",
            train.overwritecompileddataset,
            enabled,
            Message::OverwriteChanged,
        ))
        .padding(iced::Padding::ZERO.top(crate::view::workflow::FIELD_SPACING)),
    ];
    let fields = column![
        container(crate::view::workflow::fields::toggle(
            "Compile dimensions",
            train.compiledimensions,
            enabled,
            Message::CompileDimensionsChanged,
        ))
        .id(super::COMPILE_DIMENSIONS_ID),
    ]
    .spacing(crate::view::workflow::FIELD_SPACING);
    let resize_radio = |label, mode| {
        radio(label, mode, Some(train.compileresizemode), move |value| {
            if enabled {
                Message::ResizeModeChanged(value)
            } else {
                Message::Ignore
            }
        })
        .style(move |theme, status| {
            if enabled {
                iced_fluent_theme::radio::default(theme, status)
            } else {
                iced_fluent_theme::radio::disabled(theme, status)
            }
        })
    };
    let fields = fields.push(
        row![
            container(resize_radio("Stretch", ImageResizeMode::Stretch))
                .id("train.dataset.resize.stretch"),
            container(resize_radio("Letterbox", ImageResizeMode::Letterbox))
                .id("train.dataset.resize.letterbox"),
        ]
        .spacing(crate::view::workflow::FIELD_SPACING),
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
    let dimensions = column![
        fields,
        disclosure(
            "train.dataset.compile_size",
            train.compiledimensions,
            container(
                container(crate::view::workflow::fields::number_i32(
                    "Compile size",
                    train.request.resolution,
                    crate::generated::constraint_workflowstrainrequestresolution(),
                    enabled,
                    Message::ResolutionChanged,
                ))
                .id(super::COMPILE_RESOLUTION_ID),
            )
            .padding(iced::Padding::ZERO.top(crate::view::workflow::FIELD_SPACING))
        ),
    ];
    // Zero outer spacing: each rule owns precisely its two-pixel gaps.
    let fields = column![
        benchmark,
        container(card_section_divider())
            .id("train.dataset.benchmark_divider")
            .width(Fill),
        paths,
        container(card_section_divider())
            .id("train.dataset.dimensions_divider")
            .width(Fill),
        dimensions,
    ];
    let dataset = model.workflow.dataset.as_ref();
    let active = dataset.is_some_and(|state| state.active);
    let action = if active {
        button(if progress::cancelling(dataset) {
            "Cancelling…"
        } else {
            "Cancel compilation"
        })
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
            column![
                fields,
                column![
                    text(format!(
                        "Compile Size: {} x {}",
                        train.request.resolution, train.request.resolution
                    )),
                    container(progress::view(dataset)).id(super::COMPILE_PROGRESS_ID),
                    container(action.width(Fill))
                        .id(super::COMPILE_DATASET_ID)
                        .width(Fill),
                ]
                .spacing(crate::view::workflow::FIELD_SPACING)
            ]
            .spacing(crate::view::workflow::FIELD_SPACING),
        ),
    )
}

fn benchmark_radio<'a, T: Copy + Eq + 'a>(
    label: &'a str,
    id: &'static str,
    value: T,
    selected: T,
    enabled: bool,
    message: fn(T) -> Message,
) -> Element<'a, Message> {
    container(observe_dataset_text(
        id,
        DatasetTextKind::Radio,
        radio(label, value, Some(selected), move |value| {
            if enabled {
                message(value)
            } else {
                Message::Ignore
            }
        })
        .style(move |theme, status| {
            if enabled {
                iced_fluent_theme::radio::default(theme, status)
            } else {
                iced_fluent_theme::radio::disabled(theme, status)
            }
        }),
    ))
    .id(id)
    .into()
}

fn benchmark_choices(
    train: &crate::generated::TrainViewState,
    enabled: bool,
) -> Element<'_, Message> {
    let mut children = column![
        container(observe_dataset_text(
            super::RECOVER_DROPPED_MASKS_ID,
            DatasetTextKind::Checkbox,
            checkbox(train.benchmarkselection.recoverdroppedmasks)
                .label("Recover dropped masks from original annotations")
                .text_size(12)
                .on_toggle_maybe(enabled.then_some(Message::RecoverDroppedMasksChanged))
        ),)
        .id(super::RECOVER_DROPPED_MASKS_ID),
    ]
    .spacing(crate::view::workflow::FIELD_SPACING);
    for (label, description, id, description_id, value) in [
        (
            "Coconut validation",
            "COCO val2017 and Objects365 validation with Coconut annotations.",
            super::COCONUT_VALIDATION_ID,
            "train.dataset.validation.coconut.description",
            CoconutValidation::Coconut,
        ),
        (
            "Stock validation",
            "COCO val2017 with stock instance annotations.",
            super::STOCK_VALIDATION_ID,
            "train.dataset.validation.stock.description",
            CoconutValidation::Stock,
        ),
        (
            "Coconut stock",
            "COCO val2017 with Coconut enhanced annotations.",
            super::COCONUT_STOCK_ID,
            "train.dataset.validation.coconut_stock.description",
            CoconutValidation::CoconutStock,
        ),
    ] {
        children = children.push(
            column![
                benchmark_radio(
                    label,
                    id,
                    value,
                    train.benchmarkselection.validation,
                    enabled,
                    Message::ValidationChanged
                ),
                container(observe_dataset_text(
                    description_id,
                    DatasetTextKind::Description,
                    text(description).size(12)
                ))
                .padding(iced::Padding::ZERO.left(24)),
            ]
            .spacing(2),
        );
    }
    let recipes = column![
        benchmark_radio(
            "Coco custom",
            super::BENCHMARK_CUSTOM_ID,
            BenchmarkDatasetVariant::CocoCustom,
            train.benchmarkselection.dataset,
            enabled,
            Message::DatasetChanged
        ),
        benchmark_radio(
            "Coconut",
            super::BENCHMARK_COCONUT_ID,
            BenchmarkDatasetVariant::Coconut,
            train.benchmarkselection.dataset,
            enabled,
            Message::DatasetChanged
        ),
    ]
    .spacing(crate::view::workflow::FIELD_SPACING);
    disclosure(
        "train.dataset.benchmark_choices",
        train.compilebenchmarkdatasetoverride,
        container(column![
            recipes,
            disclosure(
                "train.dataset.coconut_options",
                train.benchmarkselection.dataset == BenchmarkDatasetVariant::Coconut,
                container(children).padding(
                    iced::Padding::ZERO
                        .left(16)
                        .top(crate::view::workflow::FIELD_SPACING)
                )
            ),
        ])
        .padding(iced::Padding::ZERO.top(crate::view::workflow::FIELD_SPACING)),
    )
    .into()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view::settings::installed_settings_model;

    #[test]
    fn active_compilation_disables_only_new_selection_radios() {
        use iced::advanced::{Layout, Shell, layout, renderer::Headless, widget};
        use iced::{Event, Point, Rectangle, Size, mouse};
        struct Bounds(std::collections::BTreeMap<String, Rectangle>);
        impl widget::Operation for Bounds {
            fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn widget::Operation)) {
                operate(self);
            }
            fn container(&mut self, id: Option<&widget::Id>, bounds: Rectangle) {
                for control in [
                    super::super::BENCHMARK_CUSTOM_ID,
                    super::super::BENCHMARK_COCONUT_ID,
                    super::super::COCONUT_VALIDATION_ID,
                    super::super::STOCK_VALIDATION_ID,
                    super::super::COCONUT_STOCK_ID,
                    super::super::RECOVER_DROPPED_MASKS_ID,
                    "train.dataset.resize.letterbox",
                ] {
                    if id == Some(&widget::Id::from(control)) {
                        self.0.insert(control.to_owned(), bounds);
                    }
                }
            }
        }
        let renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
            Default::default(),
            Some("wgpu"),
        ))
        .expect("dataset interaction requires the container renderer");
        let mut model = crate::view_model::test_support::bootstrapped();
        model.workflow.dataset.as_mut().unwrap().active = true;
        let settings = installed_settings_model();
        let mut train = settings.draft.as_ref().unwrap().workflows.train.clone();
        train.compilebenchmarkdatasetoverride = true;
        train.benchmarkselection.dataset = BenchmarkDatasetVariant::Coconut;
        let mut element = view(Some(&train), &model, true, true);
        let mut tree = widget::Tree::new(&element);
        tree.diff(&mut element);
        let size = Size::new(1000.0, 4000.0);
        let node =
            element
                .as_widget_mut()
                .layout(&mut tree, &renderer, &layout::Limits::new(size, size));
        let mut bounds = Bounds(Default::default());
        element
            .as_widget_mut()
            .operate(&mut tree, Layout::new(&node), &renderer, &mut bounds);
        assert_eq!(bounds.0.len(), 7);
        for (control, rectangle) in &bounds.0 {
            let mut messages = Vec::new();
            let mut shell = Shell::new(
                &iced::window::Headless,
                iced_runtime::core::shell::Waker::new(|| {}),
                &mut messages,
            );
            element.as_widget_mut().update(
                &mut tree,
                &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
                Layout::new(&node),
                mouse::Cursor::Available(Point::new(rectangle.x + 8.0, rectangle.center_y())),
                &renderer,
                &mut shell,
                &Rectangle::new(Point::ORIGIN, size),
            );
            if control == super::super::RECOVER_DROPPED_MASKS_ID {
                assert!(messages.is_empty());
                continue;
            }
            assert_eq!(messages.len(), 1);
            if control == "train.dataset.resize.letterbox" {
                assert!(matches!(
                    messages[0],
                    Message::ResizeModeChanged(ImageResizeMode::Letterbox)
                ));
            } else {
                assert!(matches!(messages[0], Message::Ignore));
            }
        }
    }

    #[test]
    fn benchmark_choices_retain_hidden_settings_and_emit_only_debounced_edits() {
        let mut model = installed_settings_model();
        let train = &model.draft.as_ref().unwrap().workflows.train;
        assert_eq!(
            train.benchmarkselection.dataset,
            BenchmarkDatasetVariant::CocoCustom
        );
        assert_eq!(
            train.benchmarkselection.validation,
            CoconutValidation::Coconut
        );
        assert!(!train.benchmarkselection.recoverdroppedmasks);
        assert!(matches!(
            update(&mut model, Message::RecoverDroppedMasksChanged(true)),
            Ok(Outcome::SettingsEdited(EditSchedule::Debounce(_)))
        ));
        for dataset in [
            BenchmarkDatasetVariant::Coconut,
            BenchmarkDatasetVariant::CocoCustom,
        ] {
            assert!(matches!(
                update(&mut model, Message::DatasetChanged(dataset)),
                Ok(Outcome::SettingsEdited(EditSchedule::Debounce(_)))
            ));
            for validation in [
                CoconutValidation::Stock,
                CoconutValidation::CoconutStock,
                CoconutValidation::Coconut,
            ] {
                assert!(matches!(
                    update(&mut model, Message::ValidationChanged(validation)),
                    Ok(Outcome::SettingsEdited(EditSchedule::Debounce(_)))
                ));
                for enabled in [true, false, true] {
                    update(&mut model, Message::BenchmarkChanged(enabled)).unwrap();
                    let train = &model.draft.as_ref().unwrap().workflows.train;
                    assert_eq!(train.benchmarkselection.dataset, dataset);
                    assert_eq!(train.benchmarkselection.validation, validation);
                    assert!(train.benchmarkselection.recoverdroppedmasks);
                }
            }
        }
        let before = model.draft.clone();
        assert!(matches!(
            update(&mut model, Message::Ignore),
            Ok(Outcome::Ignored)
        ));
        assert_eq!(model.draft, before);
        assert!(matches!(
            update(&mut model, Message::Compile),
            Ok(Outcome::Compile)
        ));
        assert_eq!(model.draft, before);
    }

    #[test]
    fn benchmark_operations_expose_only_visible_controls() {
        use iced::advanced::{Layout, layout, renderer::Headless, widget};
        use iced::{Rectangle, Size};
        struct Controls(usize);
        impl widget::Operation for Controls {
            fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn widget::Operation)) {
                operate(self);
            }
            fn container(&mut self, id: Option<&widget::Id>, bounds: Rectangle) {
                if bounds.height > 0.0
                    && [
                        super::super::BENCHMARK_CUSTOM_ID,
                        super::super::BENCHMARK_COCONUT_ID,
                        super::super::RECOVER_DROPPED_MASKS_ID,
                        super::super::COCONUT_VALIDATION_ID,
                        super::super::STOCK_VALIDATION_ID,
                        super::super::COCONUT_STOCK_ID,
                    ]
                    .iter()
                    .any(|control| id == Some(&widget::Id::from(*control)))
                {
                    self.0 += 1;
                }
            }
        }
        let renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
            Default::default(),
            Some("wgpu"),
        ))
        .expect("dataset visibility requires the container renderer");
        let settings = installed_settings_model();
        let mut train = settings.draft.as_ref().unwrap().workflows.train.clone();
        for (override_enabled, variant, count) in [
            (false, BenchmarkDatasetVariant::CocoCustom, 0),
            (true, BenchmarkDatasetVariant::CocoCustom, 2),
            (true, BenchmarkDatasetVariant::Coconut, 6),
            (false, BenchmarkDatasetVariant::Coconut, 0),
        ] {
            train.compilebenchmarkdatasetoverride = override_enabled;
            train.benchmarkselection.dataset = variant;
            for enabled in [true, false] {
                let mut element = benchmark_choices(&train, enabled);
                let mut tree = widget::Tree::new(&element);
                tree.diff(&mut element);
                let node = element.as_widget_mut().layout(
                    &mut tree,
                    &renderer,
                    &layout::Limits::new(Size::ZERO, Size::new(200.0, 4000.0)),
                );
                let mut controls = Controls(0);
                element.as_widget_mut().operate(
                    &mut tree,
                    Layout::new(&node),
                    &renderer,
                    &mut controls,
                );
                assert_eq!(controls.0, count);
            }
        }
    }

    #[test]
    fn resize_radios_settle_independently_of_perceptual_choice() {
        let mut model = installed_settings_model();
        assert_eq!(
            model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .compileresizemode,
            ImageResizeMode::Stretch
        );
        update(&mut model, Message::PerceptualDownscaleChanged(true)).unwrap();
        for mode in [ImageResizeMode::Letterbox, ImageResizeMode::Stretch] {
            assert!(matches!(
                update(&mut model, Message::ResizeModeChanged(mode)),
                Ok(Outcome::SettingsEdited(EditSchedule::Debounce(_)))
            ));
            let train = &model.draft.as_ref().unwrap().workflows.train;
            assert_eq!(train.compileresizemode, mode);
            assert!(train.compileperceptualdownscale);
        }
    }

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
