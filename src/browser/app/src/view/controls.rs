use crate::app::{App, string_at, value_at};
use crate::generated::{
    FileDialogId, ModelPreflightStatus, SettingsPath, SettingsValueKind, SourceKind,
    WeightArtifactStatus, Workflow,
};
use crate::message::{Action, Message};
use crate::model::{DraftEntry, DraftValue, source_kind_label};
use iced::widget::{
    Column, Grid, button, checkbox, column, container, pick_list, progress_bar, row, rule, space,
    text, text_input,
};
use iced::{Border, Color, Element, Fill, Font, Length, Padding, Shadow, Vector};
use iced_aw::ContextMenu;
use iced_aw::helpers::{card, number_input, selection_list_with};
use serde_json::Value;
use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct NumericChoice {
    value: i32,
    label: &'static str,
}

impl fmt::Display for NumericChoice {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.label)
    }
}

const EXECUTION_TARGETS: [NumericChoice; 2] = [
    NumericChoice {
        value: 0,
        label: "Local GPUs",
    },
    NumericChoice {
        value: 1,
        label: "Remote Vast instance",
    },
];
const TRAIN_INPUTS: [NumericChoice; 2] = [
    NumericChoice {
        value: 0,
        label: "Canonical weights",
    },
    NumericChoice {
        value: 1,
        label: "Resume checkpoint",
    },
];
const OPTIMIZERS: [NumericChoice; 2] = [
    NumericChoice {
        value: 0,
        label: "AdamW",
    },
    NumericChoice {
        value: 1,
        label: "Muon",
    },
];
const COMPILE_MODES: [NumericChoice; 3] = [
    NumericChoice {
        value: 0,
        label: "Disabled",
    },
    NumericChoice {
        value: 1,
        label: "Selective",
    },
    NumericChoice {
        value: 2,
        label: "Full",
    },
];
const GPU_AUGMENTATION_FIELDS: [(&str, SettingsPath); 19] = [
    (
        "Geometry probability",
        SettingsPath::WorkflowsTrainAugmentationGeometryProbability,
    ),
    (
        "Geometry strength min",
        SettingsPath::WorkflowsTrainAugmentationGeometryMinStrength,
    ),
    (
        "Geometry strength max",
        SettingsPath::WorkflowsTrainAugmentationGeometryMaxStrength,
    ),
    (
        "Resize probability",
        SettingsPath::WorkflowsTrainAugmentationResizeProbability,
    ),
    (
        "Resize strength min",
        SettingsPath::WorkflowsTrainAugmentationResizeMinStrength,
    ),
    (
        "Resize strength max",
        SettingsPath::WorkflowsTrainAugmentationResizeMaxStrength,
    ),
    (
        "Color probability",
        SettingsPath::WorkflowsTrainAugmentationColorProbability,
    ),
    (
        "Color strength min",
        SettingsPath::WorkflowsTrainAugmentationColorMinStrength,
    ),
    (
        "Color strength max",
        SettingsPath::WorkflowsTrainAugmentationColorMaxStrength,
    ),
    (
        "Noise probability",
        SettingsPath::WorkflowsTrainAugmentationNoiseProbability,
    ),
    (
        "Noise strength min",
        SettingsPath::WorkflowsTrainAugmentationNoiseMinStrength,
    ),
    (
        "Noise strength max",
        SettingsPath::WorkflowsTrainAugmentationNoiseMaxStrength,
    ),
    (
        "Blur probability",
        SettingsPath::WorkflowsTrainAugmentationBlurProbability,
    ),
    (
        "Blur strength min",
        SettingsPath::WorkflowsTrainAugmentationBlurMinStrength,
    ),
    (
        "Blur strength max",
        SettingsPath::WorkflowsTrainAugmentationBlurMaxStrength,
    ),
    (
        "Occlusion probability",
        SettingsPath::WorkflowsTrainAugmentationOcclusionProbability,
    ),
    (
        "Occlusion strength min",
        SettingsPath::WorkflowsTrainAugmentationOcclusionMinStrength,
    ),
    (
        "Occlusion strength max",
        SettingsPath::WorkflowsTrainAugmentationOcclusionMaxStrength,
    ),
    (
        "Copy-paste probability",
        SettingsPath::WorkflowsTrainAugmentationCopyPasteProbability,
    ),
];

pub fn view(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    container(setup_view(app))
        .padding(Padding {
            right: shell.card_padding,
            bottom: shell.card_padding,
            left: shell.card_padding,
            ..Padding::ZERO
        })
        .width(Length::FillPortion(super::SIDEBAR_PORTION))
        .height(Length::Shrink)
        .style(iced::widget::container::secondary)
        .into()
}

fn setup_view(app: &App) -> Element<'_, Message> {
    let mut content = Column::new().spacing(app.drafts.shell_style().section_spacing);
    content = content.push(model_card(app));
    content = match app.selected_workflow {
        Workflow::Train => content.push(train_dataset_card(app)),
        Workflow::Validate => content.push(validate_controls(app)),
        Workflow::Predict | Workflow::Live => content.push(predict_controls(app)),
        Workflow::Annotate => content.push(annotate_controls(app)),
        Workflow::Export => content.push(export_controls(app)),
    };

    content = content.push(primary_action_button(app));
    content.into()
}

pub(super) fn primary_action_button(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let standard_height = shell.primary_font_size + shell.control_padding_y.max(5.0) * 2.0;
    let label = text(app.primary_label())
        .font(Font::new("Bitstream Vera Sans").weight(iced::font::Weight::Bold))
        .size(16)
        .width(Fill)
        .height(Fill)
        .align_x(iced::Center)
        .align_y(iced::Center);
    button(label)
        .on_press(Message::Run(Action::Primary))
        .style(primary_button_style)
        .padding(0)
        .width(Fill)
        .height(Length::Fixed(standard_height * 2.0))
        .into()
}

fn primary_button_style(
    _theme: &iced::Theme,
    status: iced::widget::button::Status,
) -> iced::widget::button::Style {
    let green = iced::Color::from_rgb8(0x00, 0xc8, 0x53);
    let background = match status {
        iced::widget::button::Status::Hovered => green.mix(iced::Color::WHITE, 0.14),
        iced::widget::button::Status::Pressed => green.mix(iced::Color::BLACK, 0.16),
        iced::widget::button::Status::Active | iced::widget::button::Status::Disabled => green,
    };
    iced::widget::button::Style {
        background: Some(background.into()),
        text_color: iced::Color::WHITE,
        border: iced::Border {
            color: iced::Color::WHITE,
            width: 3.0,
            radius: 10.0.into(),
        },
        ..Default::default()
    }
}

fn model_card(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let model_text_size = shell.secondary_font_size;
    const MODEL_ROW_PADDING: f32 = 5.0;
    let model_row_height = model_text_size + MODEL_ROW_PADDING * 2.0;
    let selected_index = app
        .drafts
        .uses_canonical_weights(app.selected_workflow)
        .then(|| {
            app.compatible_presets.iter().position(|preset| {
                preset.preset_name == app.drafts.preset_name(app.selected_workflow)
            })
        })
        .flatten();
    let list_height =
        (app.compatible_presets.len() as f32 * model_row_height + 2.0).max(model_row_height + 2.0);
    let choices = selection_list_with(
        &app.compatible_presets,
        |_, preset| Message::SetPreset(preset.preset_name.to_owned()),
        model_text_size,
        MODEL_ROW_PADDING,
        iced_aw::style::selection_list::primary,
        selected_index,
        Font::default(),
    )
    .height(Length::Fixed(list_height));

    let selector = container(
        column![
            choices,
            row![
                space::horizontal().width(Length::FillPortion(1)),
                container(rule::horizontal(1)).width(Length::FillPortion(6)),
                space::horizontal().width(Length::FillPortion(1)),
            ]
            .width(Fill),
            button("Custom Weights")
                .on_press(Message::ChooseCustomWeights)
                .width(Fill),
        ]
        .spacing(shell.field_spacing),
    )
    .padding(2)
    .width(Fill)
    .style(iced::widget::container::bordered_box);
    let mut body = Column::new().spacing(shell.field_spacing).push(selector);
    let preflight_resolution = (app.snapshot.model_preflight.status != ModelPreflightStatus::Idle
        && app.snapshot.model_preflight.workflow
            == if app.selected_workflow == Workflow::Live {
                Workflow::Predict
            } else {
                app.selected_workflow
            })
    .then_some(app.snapshot.model_preflight.resolution)
    .filter(|resolution| *resolution > 0);
    let draft_resolution = app
        .drafts
        .active(app.selected_workflow)
        .entries()
        .iter()
        .find(|entry| entry.path.as_str().ends_with("model_artifacts.resolution"))
        .and_then(|entry| match &entry.value {
            DraftValue::Number(value) if value.is_finite() && *value >= 1.0 => {
                Some(value.round().clamp(1.0, u32::MAX as f64) as u32)
            }
            _ => None,
        });
    let effective_resolution = (app.selected_workflow == Workflow::Train)
        .then(|| app.compiled_resolution())
        .flatten()
        .or(preflight_resolution)
        .or(draft_resolution)
        .or_else(|| app.active_preset().map(|preset| preset.resolution));
    if let Some(resolution) = effective_resolution {
        body = body.push(
            text(format!("Input size: {} x {}", resolution, resolution))
                .size(shell.primary_font_size),
        );
    }

    let presentation = model_status(app);
    let status = text(presentation.label).style(match presentation.tone {
        ModelStatusTone::Neutral => iced::widget::text::secondary,
        ModelStatusTone::Ready => iced::widget::text::success,
        ModelStatusTone::Active => iced::widget::text::warning,
        ModelStatusTone::Error => iced::widget::text::danger,
    });
    body = body.push(row![text("Status:"), status].spacing(5));
    if presentation.show_progress {
        let weight = &app.snapshot.artifacts.weight;
        if weight.total_bytes > 0 {
            let value = (weight.downloaded_bytes.min(weight.total_bytes) as f64
                / weight.total_bytes as f64) as f32;
            body = body.push(
                progress_bar(0.0..=1.0, value)
                    .girth(Length::Fixed(7.0))
                    .style(iced::widget::progress_bar::warning),
            );
        }
        body = body.push(
            text(download_progress_label(
                weight.downloaded_bytes,
                weight.total_bytes,
                weight.resumable,
            ))
            .size(shell.secondary_font_size)
            .style(iced::widget::text::secondary),
        );
    }
    if let Some(detail) = presentation.detail {
        body = body.push(text(detail).size(shell.secondary_font_size));
    }
    card(text("RF-DETR Weights"), body).width(Fill).into()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ModelStatusTone {
    Neutral,
    Ready,
    Active,
    Error,
}

struct ModelStatus<'a> {
    label: String,
    tone: ModelStatusTone,
    detail: Option<&'a str>,
    show_progress: bool,
}

fn model_status(app: &App) -> ModelStatus<'_> {
    if !app.ready() {
        return ModelStatus {
            label: "No connection".into(),
            tone: ModelStatusTone::Error,
            detail: non_empty(&app.transport_error),
            show_progress: false,
        };
    }

    if app.weight_request_pending() {
        return ModelStatus {
            label: "Verifying".into(),
            tone: ModelStatusTone::Neutral,
            detail: None,
            show_progress: false,
        };
    }
    if !app.weight_request_error().is_empty() {
        return ModelStatus {
            label: "Cannot start download".into(),
            tone: ModelStatusTone::Error,
            detail: Some(app.weight_request_error()),
            show_progress: false,
        };
    }

    if !app.drafts.has_model_input(app.selected_workflow) {
        return ModelStatus {
            label: "No weights selected".into(),
            tone: ModelStatusTone::Neutral,
            detail: None,
            show_progress: false,
        };
    }

    let workflow = if app.selected_workflow == Workflow::Live {
        Workflow::Predict
    } else {
        app.selected_workflow
    };
    let preflight = &app.snapshot.model_preflight;
    if !app.drafts.uses_canonical_weights(app.selected_workflow)
        && preflight.workflow == workflow
        && preflight.status != ModelPreflightStatus::Idle
    {
        return match preflight.status {
            ModelPreflightStatus::Idle => unreachable!("idle preflight was excluded"),
            ModelPreflightStatus::Verifying => ModelStatus {
                label: "Verifying".into(),
                tone: ModelStatusTone::Neutral,
                detail: None,
                show_progress: false,
            },
            ModelPreflightStatus::Ready => ModelStatus {
                label: "Ready".into(),
                tone: ModelStatusTone::Ready,
                detail: None,
                show_progress: false,
            },
            ModelPreflightStatus::Incompatible => ModelStatus {
                label: "Incompatible weights".into(),
                tone: ModelStatusTone::Error,
                detail: non_empty(&preflight.error),
                show_progress: false,
            },
            ModelPreflightStatus::Failed => ModelStatus {
                label: "Cannot load weights".into(),
                tone: ModelStatusTone::Error,
                detail: non_empty(&preflight.error),
                show_progress: false,
            },
        };
    }

    let weight = &app.snapshot.artifacts.weight;
    let status = match weight.status {
        WeightArtifactStatus::Idle => ("Not started".into(), ModelStatusTone::Neutral, false),
        WeightArtifactStatus::Verifying => ("Verifying".into(), ModelStatusTone::Neutral, false),
        WeightArtifactStatus::Downloading => {
            let percent = (weight.total_bytes > 0).then(|| {
                ((u128::from(weight.downloaded_bytes.min(weight.total_bytes)) * 100)
                    / u128::from(weight.total_bytes)) as u64
            });
            (
                percent.map_or_else(
                    || "Downloading".into(),
                    |percent| format!("Downloading · {percent}%"),
                ),
                ModelStatusTone::Active,
                true,
            )
        }
        WeightArtifactStatus::RetryWaiting => (
            format!(
                "Retrying in {}s",
                weight.retry_after_ms.saturating_add(999) / 1_000
            ),
            ModelStatusTone::Active,
            weight.downloaded_bytes > 0,
        ),
        WeightArtifactStatus::Ready => ("Ready".into(), ModelStatusTone::Ready, false),
        WeightArtifactStatus::NoConnection => {
            ("No connection".into(), ModelStatusTone::Error, false)
        }
        WeightArtifactStatus::CannotDownload => {
            ("Cannot download".into(), ModelStatusTone::Error, false)
        }
        WeightArtifactStatus::ChecksumError => {
            ("Checksum failed".into(), ModelStatusTone::Error, false)
        }
        WeightArtifactStatus::FilesystemError => {
            ("Cannot save weights".into(), ModelStatusTone::Error, false)
        }
        WeightArtifactStatus::HttpError => (
            "Download server error".into(),
            ModelStatusTone::Error,
            false,
        ),
        WeightArtifactStatus::Incompatible => {
            ("Incompatible weights".into(), ModelStatusTone::Error, false)
        }
        WeightArtifactStatus::Cancelled => ("Cancelled".into(), ModelStatusTone::Neutral, false),
    };
    ModelStatus {
        label: status.0,
        tone: status.1,
        detail: non_empty(&weight.error),
        show_progress: status.2,
    }
}

fn non_empty(value: &str) -> Option<&str> {
    (!value.is_empty()).then_some(value)
}

fn download_progress_label(downloaded_bytes: u64, total_bytes: u64, resumable: bool) -> String {
    let resume_label = if resumable { " · resumable" } else { "" };
    if total_bytes == 0 {
        return format!(
            "{} downloaded{resume_label}",
            format_bytes(downloaded_bytes)
        );
    }
    format!(
        "{} / {}{resume_label}",
        format_bytes(downloaded_bytes),
        format_bytes(total_bytes)
    )
}

fn format_bytes(bytes: u64) -> String {
    const MEBIBYTE: f64 = 1024.0 * 1024.0;
    format!("{:.1} MiB", bytes as f64 / MEBIBYTE)
}

fn format_duration(milliseconds: u64) -> String {
    let total_seconds = (milliseconds + 999) / 1_000;
    let hours = total_seconds / 3_600;
    let minutes = (total_seconds % 3_600) / 60;
    let seconds = total_seconds % 60;
    if hours > 0 {
        format!("{hours}h {minutes:02}m")
    } else if minutes > 0 {
        format!("{minutes}m {seconds:02}s")
    } else {
        format!("{seconds}s")
    }
}

fn train_dataset_card(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let infer_splits = app
        .drafts
        .train
        .boolean(SettingsPath::WorkflowsTrainDatasetPathsUseCompiledDirectoryDefaults);
    let compile_dimensions = app
        .drafts
        .train
        .boolean(SettingsPath::WorkflowsTrainDatasetPathsCompileDimensions);
    let input_size = app
        .drafts
        .train
        .number(SettingsPath::WorkflowsTrainModelArtifactsResolution)
        .round()
        .max(1.0) as u32;
    let mut body = Column::new()
        .spacing(shell.field_spacing)
        .push(path_field(
            app,
            "Dataset",
            SettingsPath::WorkflowsTrainDatasetPathsSourceDir,
            Some(FileDialogId::TrainDatasetSourceDir),
        ))
        .push(path_field(
            app,
            "Compiled",
            SettingsPath::WorkflowsTrainDatasetPathsCompiledDirectory,
            Some(FileDialogId::TrainDatasetCompiledDirectory),
        ))
        .push(boolean_field(
            app,
            "Infer splits",
            SettingsPath::WorkflowsTrainDatasetPathsUseCompiledDirectoryDefaults,
        ));
    if !infer_splits {
        body = body
            .push(path_field(
                app,
                "Train split override",
                SettingsPath::WorkflowsTrainDatasetPathsTrainCompiledPath,
                Some(FileDialogId::TrainDatasetTrainCompiledPath),
            ))
            .push(path_field(
                app,
                "Validation split override",
                SettingsPath::WorkflowsTrainDatasetPathsValCompiledPath,
                Some(FileDialogId::TrainDatasetValCompiledPath),
            ))
            .push(path_field(
                app,
                "Test split override",
                SettingsPath::WorkflowsTrainDatasetPathsTestCompiledPath,
                Some(FileDialogId::TrainDatasetTestCompiledPath),
            ));
    }
    body = body
        .push(boolean_field(
            app,
            "Overwrite",
            SettingsPath::WorkflowsTrainDatasetPathsOverwrite,
        ))
        .push(boolean_field(
            app,
            "Compile Dimensions",
            SettingsPath::WorkflowsTrainDatasetPathsCompileDimensions,
        ));
    if compile_dimensions {
        body = body.push(number_field(
            app,
            "Compile size",
            SettingsPath::WorkflowsTrainModelArtifactsResolution,
        ));
    }
    body = body.push(
        text(format!("Compile Size: {input_size} x {input_size}")).size(shell.primary_font_size),
    );

    let dataset = &app.snapshot.artifacts.dataset;
    if dataset.compiling {
        let maximum = dataset.total.max(1) as f32;
        let eta = if dataset.eta_ready {
            format_duration(dataset.remaining_ms)
        } else {
            "—".to_owned()
        };
        body = body
            .push(text(format!("{} / {}", dataset.done, dataset.total)))
            .push(progress_bar(
                0.0..=maximum,
                dataset.done.min(dataset.total) as f32,
            ))
            .push(text(format!(
                "{} · {}",
                format_duration(dataset.elapsed_ms),
                eta
            )))
            .push(
                button("Cancel compilation")
                    .on_press(Message::Run(Action::CancelDatasetCompile))
                    .width(Fill),
            );
    } else if app.dataset_compile_pending() {
        body = body.push(button("Applying settings…").width(Fill));
    } else {
        body = body.push(
            button("Compile Dataset")
                .on_press(Message::Run(Action::CompileDataset))
                .width(Fill),
        );
    }
    card(text("Dataset"), body).width(Fill).into()
}

pub(super) fn dataset_split_summary(app: &App) -> Element<'_, Message> {
    let dataset = &app.snapshot.artifacts.dataset;
    let mut content = Column::new().spacing(4);
    for split in &dataset.splits {
        content = content.push(
            text(format!(
                "Compiled: {} · {} images · {}x{}x{} · {} classes",
                split.path,
                split.image_count,
                split.width,
                split.height,
                split.channels,
                split.class_count
            ))
            .size(12),
        );
    }
    content.into()
}

pub(super) fn train_controls(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let target_path = SettingsPath::WorkflowsTrainTrainingExecutionTarget;
    let target = app.drafts.train.integer(target_path);
    let mut body = Column::new()
        .spacing(shell.field_spacing)
        .push(choice_field(
            app,
            "Execution target",
            target_path,
            &EXECUTION_TARGETS,
        ))
        .push(choice_field(
            app,
            "Input mode",
            SettingsPath::WorkflowsTrainTrainingInputMode,
            &TRAIN_INPUTS,
        ));
    if target == 0 {
        body = body.push(local_gpu_controls(app));
    } else {
        body = body.push(remote_train_controls(app));
    }
    body = body
        .push(number_field(
            app,
            "Batch size",
            SettingsPath::WorkflowsTrainTrainingBatchSize,
        ))
        .push(number_field(
            app,
            "Validation batch size",
            SettingsPath::WorkflowsTrainTrainingValBatchSize,
        ))
        .push(number_field(
            app,
            "Epochs",
            SettingsPath::WorkflowsTrainTrainingEpochs,
        ))
        .push(choice_field(
            app,
            "Optimizer",
            SettingsPath::WorkflowsTrainTrainingOptimizer,
            &OPTIMIZERS,
        ))
        .push(text_field(
            app,
            "LR scheduler",
            SettingsPath::WorkflowsTrainTrainingLrScheduler,
        ))
        .push(boolean_field(
            app,
            "Automatic mixed precision",
            SettingsPath::WorkflowsTrainTrainingAmp,
        ))
        .push(boolean_field(
            app,
            "Exponential moving average",
            SettingsPath::WorkflowsTrainTrainingUseEma,
        ))
        .push(boolean_field(
            app,
            "Validation loss",
            SettingsPath::WorkflowsTrainTrainingValidationLoss,
        ))
        .push(boolean_field(
            app,
            "Enable Augmentation",
            SettingsPath::WorkflowsTrainAugmentationEnabled,
        ))
        .push(boolean_field(
            app,
            "Freeze encoder",
            SettingsPath::WorkflowsTrainTrainingFreezeEncoder,
        ))
        .push(path_field(
            app,
            "Resume checkpoint",
            SettingsPath::WorkflowsTrainTrainingResumePath,
            Some(FileDialogId::TrainTrainingResumePath),
        ))
        .push(path_field(
            app,
            "Output directory",
            SettingsPath::WorkflowsTrainTrainingOutputDir,
            Some(FileDialogId::TrainTrainingOutputDir),
        ));
    card(text("Training"), body).width(Fill).into()
}

fn local_gpu_controls(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let mut content = Column::new().spacing(shell.field_spacing).push(
        button("Refresh visible GPUs")
            .on_press(Message::Run(Action::RefreshLocalGpus))
            .width(Fill),
    );
    let devices = value_at(
        &app.snapshot.workflow_state,
        &["workflows", "train", "local_gpu", "devices"],
    )
    .and_then(Value::as_array);
    if let Some(devices) = devices {
        for device in devices.iter().filter_map(Value::as_object) {
            let id = device
                .get("device_id")
                .and_then(Value::as_i64)
                .unwrap_or(-1) as i32;
            let label = device
                .get("label")
                .and_then(Value::as_str)
                .unwrap_or("Unnamed GPU");
            let selected = app
                .drafts
                .train
                .number_array(SettingsPath::WorkflowsTrainTrainingLocalDeviceIds)
                .iter()
                .any(|value| value.round() as i32 == id);
            content = content.push(
                checkbox(selected)
                    .label(label)
                    .size(shell.checkbox_size)
                    .spacing(shell.field_spacing)
                    .text_size(shell.primary_font_size)
                    .on_toggle(move |enabled| Message::ToggleLocalGpu(id, enabled)),
            );
        }
    }
    if let Some(error) = string_at(
        &app.snapshot.workflow_state,
        &["workflows", "train", "local_gpu", "error"],
    ) && !error.is_empty()
    {
        content = content.push(text(error).size(12));
    }
    content.into()
}

fn remote_train_controls(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    const FAMILIES: [&str; 5] = ["RTX 4090", "A100", "H100", "L40S", "RTX 5090"];
    let values = app
        .drafts
        .train
        .boolean_array(SettingsPath::WorkflowsTrainTrainingRemoteFamilyEnabled);
    let mut content = Column::new()
        .spacing(shell.field_spacing)
        .push(text("Remote GPU families"));
    for (index, label) in FAMILIES.into_iter().enumerate() {
        content = content.push(
            checkbox(values.get(index).copied().unwrap_or(true))
                .label(label)
                .size(shell.checkbox_size)
                .spacing(shell.field_spacing)
                .text_size(shell.primary_font_size)
                .on_toggle(move |enabled| Message::ToggleRemoteFamily(index, enabled)),
        );
    }
    content = content
        .push(path_field(
            app,
            "Container image",
            SettingsPath::WorkflowsTrainTrainingRemoteContainerImage,
            None,
        ))
        .push(text_field(
            app,
            "Launch template JSON",
            SettingsPath::WorkflowsTrainTrainingRemoteLaunchTemplate,
        ))
        .push(
            button("Query remote offers")
                .on_press(Message::Run(Action::QueryRemoteOffers))
                .width(Fill),
        );
    let query = value_at(
        &app.snapshot.workflow_state,
        &["workflows", "train", "remote_query"],
    )
    .and_then(Value::as_object);
    if let Some(query) = query {
        if let Some(error) = query.get("last_error").and_then(Value::as_str)
            && !error.is_empty()
        {
            content = content.push(text(error).size(12));
        }
        let armed = query.get("armed_offer_id").and_then(Value::as_i64);
        if let Some(offers) = query.get("results").and_then(Value::as_array) {
            for offer in offers.iter().filter_map(Value::as_object) {
                let offer_id = offer
                    .get("offer_id")
                    .and_then(Value::as_i64)
                    .unwrap_or_default();
                let gpu = offer
                    .get("gpu_name")
                    .and_then(Value::as_str)
                    .unwrap_or("GPU");
                let family = offer
                    .get("family")
                    .and_then(Value::as_str)
                    .unwrap_or("unknown");
                let dph = offer.get("dph").and_then(Value::as_f64).unwrap_or_default();
                let label = format!("{family} · {gpu} · ${dph:.2}/hr");
                content = content.push(
                    button(text(if armed == Some(offer_id) {
                        format!("Armed · {label}")
                    } else {
                        label
                    }))
                    .on_press(Message::ArmRemoteOffer(offer_id as i32))
                    .width(Fill),
                );
            }
        }
        if armed.is_some() {
            content = content.push(
                button("Clear armed offer")
                    .on_press(Message::Run(Action::ClearRemoteOffer))
                    .width(Fill),
            );
        }
    }
    content.into()
}

fn validate_controls(app: &App) -> Element<'_, Message> {
    let body = column![
        path_field(
            app,
            "Compiled dataset",
            SettingsPath::WorkflowsValidateDatasetPathsCompiledPath,
            Some(FileDialogId::ValidateDatasetCompiledPath),
        ),
        path_field(
            app,
            "Source root",
            SettingsPath::WorkflowsValidateDatasetPathsSourceDir,
            Some(FileDialogId::ValidateDatasetSourceDir),
        ),
    ]
    .spacing(8);
    card(text("Validation"), body).width(Fill).into()
}

fn predict_controls(app: &App) -> Element<'_, Message> {
    let live = app.selected_workflow == Workflow::Live;
    let mut body = Column::new()
        .spacing(8)
        .push(source_kind_selector(app))
        .push(source_path_field(app))
        .push(number_field(
            app,
            "Threshold",
            SettingsPath::WorkflowsPredictPredictThreshold,
        ));
    if !live {
        body = body.push(path_field(
            app,
            "Output JSON",
            SettingsPath::WorkflowsPredictPredictOutputPath,
            Some(FileDialogId::PredictOutputPath),
        ));
    }
    card(
        text(if live {
            "Live Prediction"
        } else {
            "Prediction"
        }),
        body,
    )
    .width(Fill)
    .into()
}

fn annotate_controls(app: &App) -> Element<'_, Message> {
    let body = column![
        source_kind_selector(app),
        source_path_field(app),
        path_field(
            app,
            "Annotation output",
            SettingsPath::WorkflowsAnnotateAnnotateOutputDir,
            Some(FileDialogId::AnnotateOutputDir),
        ),
        number_field(
            app,
            "Assist threshold",
            SettingsPath::WorkflowsAnnotateAnnotateThreshold,
        ),
        button("Save now")
            .on_press(Message::Run(Action::SaveAnnotation))
            .width(Fill),
        button(if app.annotation_live {
            "Stop live annotation"
        } else {
            "Start live annotation"
        })
        .on_press(Message::Run(Action::ToggleLiveAnnotation))
        .width(Fill),
    ]
    .spacing(8);
    card(text("Annotation"), body).width(Fill).into()
}

fn export_controls(app: &App) -> Element<'_, Message> {
    let body = column![path_field(
        app,
        "Output engine",
        SettingsPath::WorkflowsExportExportOutputPath,
        Some(FileDialogId::ExportOutputPath),
    ),]
    .spacing(8);
    card(text("Export"), body).width(Fill).into()
}

pub fn advanced_card(app: &App) -> Element<'_, Message> {
    let draft = app.drafts.active(app.selected_workflow);
    let advanced_entries = || {
        draft
            .entries()
            .iter()
            .filter(|entry| !setup_owns(entry.path))
    };
    let mut body = Column::new().spacing(10);

    let mut non_numeric = Grid::new().columns(4).height(Length::Shrink).spacing(10);
    let mut has_non_numeric = false;
    for entry in advanced_entries().filter(|entry| !uses_number_input(entry)) {
        non_numeric = non_numeric.push(setting_control(app, entry));
        has_non_numeric = true;
    }
    if has_non_numeric {
        body = body.push(non_numeric);
    }

    let mut numeric = Grid::new().columns(4).height(Length::Shrink).spacing(10);
    let mut has_numeric = false;
    if app.selected_workflow == Workflow::Train {
        numeric = numeric.push(number_field(
            app,
            "Input size",
            SettingsPath::WorkflowsTrainModelArtifactsResolution,
        ));
        has_numeric = true;
    }
    for entry in advanced_entries().filter(|entry| uses_number_input(entry)) {
        numeric = numeric.push(setting_control(app, entry));
        has_numeric = true;
    }
    if has_numeric {
        body = body.push(numeric);
    }

    if app.selected_workflow == Workflow::Train
        && app
            .drafts
            .train
            .boolean(SettingsPath::WorkflowsTrainAugmentationEnabled)
    {
        let divider = rule::horizontal(1).style(|theme: &iced::Theme| {
            let mut style = rule::default(theme);
            style.color = Color::from_rgba8(0xd3, 0xd3, 0xd3, 0.50);
            style.fill_mode = rule::FillMode::Percent(75.0);
            style
        });
        body = body.push(container(divider).padding([15, 0]).width(Fill));

        let mut augmentation = Grid::new().columns(4).height(Length::Shrink).spacing(10);
        for &(label, path) in &GPU_AUGMENTATION_FIELDS {
            augmentation = augmentation.push(number_field(app, label, path));
        }
        body = body.push(augmentation);
    }
    card(text("Advanced"), container(body).padding([10, 0]))
        .width(Fill)
        .into()
}

fn uses_number_input(entry: &DraftEntry) -> bool {
    matches!(&entry.value, DraftValue::Number(_))
        && !matches!(
            entry.kind,
            SettingsValueKind::TrainInputMode | SettingsValueKind::CompileMode
        )
}

fn setup_owns(path: SettingsPath) -> bool {
    path.as_str().contains(".model_artifacts.")
        || path.as_str().contains(".augmentation.")
        || matches!(
            path,
            SettingsPath::WorkflowsTrainDatasetPathsTrainCompiledPath
                | SettingsPath::WorkflowsTrainDatasetPathsValCompiledPath
                | SettingsPath::WorkflowsTrainDatasetPathsTestCompiledPath
                | SettingsPath::WorkflowsTrainDatasetPathsSourceDir
                | SettingsPath::WorkflowsTrainDatasetPathsCompiledDirectory
                | SettingsPath::WorkflowsTrainDatasetPathsUseCompiledDirectoryDefaults
                | SettingsPath::WorkflowsTrainDatasetPathsOverwrite
                | SettingsPath::WorkflowsTrainDatasetPathsCompileDimensions
                | SettingsPath::WorkflowsTrainTrainingExecutionTarget
                | SettingsPath::WorkflowsTrainTrainingInputMode
                | SettingsPath::WorkflowsTrainTrainingLocalDeviceIds
                | SettingsPath::WorkflowsTrainTrainingRemoteFamilyEnabled
                | SettingsPath::WorkflowsTrainTrainingRemoteContainerImage
                | SettingsPath::WorkflowsTrainTrainingRemoteLaunchTemplate
                | SettingsPath::WorkflowsTrainTrainingBatchSize
                | SettingsPath::WorkflowsTrainTrainingValBatchSize
                | SettingsPath::WorkflowsTrainTrainingEpochs
                | SettingsPath::WorkflowsTrainTrainingOptimizer
                | SettingsPath::WorkflowsTrainTrainingLrScheduler
                | SettingsPath::WorkflowsTrainTrainingAmp
                | SettingsPath::WorkflowsTrainTrainingUseEma
                | SettingsPath::WorkflowsTrainTrainingFreezeEncoder
                | SettingsPath::WorkflowsTrainTrainingResumePath
                | SettingsPath::WorkflowsTrainTrainingOutputDir
                | SettingsPath::WorkflowsValidateDatasetPathsCompiledPath
                | SettingsPath::WorkflowsValidateDatasetPathsSourceDir
                | SettingsPath::WorkflowsPredictSourceKind
                | SettingsPath::WorkflowsPredictSourceCompiledPath
                | SettingsPath::WorkflowsPredictSourceSingleImagePath
                | SettingsPath::WorkflowsPredictSourceImageDirectory
                | SettingsPath::WorkflowsPredictSourceDeviceIndex
                | SettingsPath::WorkflowsPredictPredictThreshold
                | SettingsPath::WorkflowsPredictPredictOutputPath
                | SettingsPath::WorkflowsAnnotateSourceKind
                | SettingsPath::WorkflowsAnnotateSourceSingleImagePath
                | SettingsPath::WorkflowsAnnotateSourceImageDirectory
                | SettingsPath::WorkflowsAnnotateSourceDeviceIndex
                | SettingsPath::WorkflowsAnnotateAnnotateOutputDir
                | SettingsPath::WorkflowsAnnotateAnnotateThreshold
                | SettingsPath::WorkflowsExportExportOutputPath
        )
}

fn setting_control<'a>(app: &'a App, entry: &'a DraftEntry) -> Element<'a, Message> {
    match (&entry.value, entry.kind) {
        (DraftValue::Text(_), _) => text_field(app, &entry.label, entry.path),
        (DraftValue::Number(_), SettingsValueKind::TrainInputMode) => {
            choice_field(app, &entry.label, entry.path, &TRAIN_INPUTS)
        }
        (DraftValue::Number(_), SettingsValueKind::CompileMode) => {
            choice_field(app, &entry.label, entry.path, &COMPILE_MODES)
        }
        (DraftValue::Number(_), _) => number_field(app, &entry.label, entry.path),
        (DraftValue::Boolean(_), _) => boolean_field(app, &entry.label, entry.path),
        (DraftValue::NumberArray(_), _) | (DraftValue::BooleanArray(_), _) => column![
            text(&entry.label).size(13),
            text("Managed by the local GPU / remote family controls above.").size(12)
        ]
        .spacing(4)
        .into(),
    }
}

fn source_kind_selector(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let kinds: &[SourceKind] = match app.selected_workflow {
        Workflow::Predict => &[
            SourceKind::CompiledDataset,
            SourceKind::SingleImage,
            SourceKind::ImageFolder,
            SourceKind::VideoStream,
        ],
        Workflow::Annotate => &[
            SourceKind::SingleImage,
            SourceKind::ImageFolder,
            SourceKind::VideoStream,
        ],
        Workflow::Live => &[SourceKind::VideoStream],
        _ => &[],
    };
    let mut choices = Column::new().spacing(5);
    for &kind in kinds {
        choices = choices.push(
            button(text(source_kind_label(kind)).size(shell.primary_font_size))
                .on_press(Message::SetSourceKind(kind))
                .style(if kind == app.source_kind() {
                    iced::widget::button::primary
                } else {
                    iced::widget::button::secondary
                })
                .width(Fill),
        );
    }
    column![text("Source kind").size(13), choices]
        .spacing(5)
        .into()
}

fn source_path_field(app: &App) -> Element<'_, Message> {
    match (app.selected_workflow, app.source_kind()) {
        (Workflow::Predict | Workflow::Live, SourceKind::CompiledDataset) => path_field(
            app,
            "Compiled source",
            SettingsPath::WorkflowsPredictSourceCompiledPath,
            None,
        ),
        (Workflow::Predict | Workflow::Live, SourceKind::SingleImage) => path_field(
            app,
            "Source image",
            SettingsPath::WorkflowsPredictSourceSingleImagePath,
            Some(FileDialogId::PredictSourceSingleImage),
        ),
        (Workflow::Predict | Workflow::Live, SourceKind::ImageFolder) => path_field(
            app,
            "Source folder",
            SettingsPath::WorkflowsPredictSourceImageDirectory,
            Some(FileDialogId::PredictSourceImageFolder),
        ),
        (Workflow::Annotate, SourceKind::SingleImage) => path_field(
            app,
            "Source image",
            SettingsPath::WorkflowsAnnotateSourceSingleImagePath,
            Some(FileDialogId::AnnotateSourceSingleImage),
        ),
        (Workflow::Annotate, SourceKind::ImageFolder) => path_field(
            app,
            "Source folder",
            SettingsPath::WorkflowsAnnotateSourceImageDirectory,
            Some(FileDialogId::AnnotateSourceImageFolder),
        ),
        (Workflow::Predict | Workflow::Live, SourceKind::VideoStream) => number_field(
            app,
            "Capture device",
            SettingsPath::WorkflowsPredictSourceDeviceIndex,
        ),
        (Workflow::Annotate, SourceKind::VideoStream) => number_field(
            app,
            "Capture device",
            SettingsPath::WorkflowsAnnotateSourceDeviceIndex,
        ),
        _ => text("No source path is required.").into(),
    }
}

fn path_field<'a>(
    app: &'a App,
    label: &'a str,
    path: SettingsPath,
    dialog: Option<FileDialogId>,
) -> Element<'a, Message> {
    let shell = app.drafts.shell_style();
    let id = path.as_str();
    let value = app.drafts.active(app.selected_workflow).text(path);
    let input = text_input(label, value)
        .id(id)
        .on_input(move |value| Message::EditText(path, value))
        .on_focus(Message::FieldInteracted(path))
        .size(shell.text_input_font_size)
        .padding([shell.control_padding_y.max(6.0), shell.control_padding_x])
        .width(Fill);
    let input = if app.field_needs_attention(path) {
        input.style(attention_text_input_style)
    } else {
        input
    };
    let input = copyable_input(id, input);
    let row = if let Some(dialog) = dialog {
        row![input, button("Browse").on_press(Message::Browse(dialog))].spacing(5)
    } else {
        row![input]
    };
    column![text(label).size(shell.primary_font_size), row]
        .spacing(shell.field_spacing.max(4.0))
        .into()
}

fn text_field<'a>(app: &'a App, label: &'a str, path: SettingsPath) -> Element<'a, Message> {
    path_field(app, label, path, None)
}

fn number_field<'a>(app: &'a App, label: &'a str, path: SettingsPath) -> Element<'a, Message> {
    let shell = app.drafts.shell_style();
    let id = path.as_str();
    let value = app.drafts.active(app.selected_workflow).number(path);
    let (minimum, maximum, step) = number_bounds(path);
    let input = number_input(&value, minimum..=maximum, move |value| {
        Message::SetNumber(path, value)
    })
    .id(id)
    .step(step)
    .ignore_scroll(true)
    .ignore_buttons(true)
    .set_size(shell.text_input_font_size)
    .padding([shell.control_padding_y, shell.control_padding_x])
    .width(Length::FillPortion(super::PROPERTY_INPUT_PORTION));
    row![
        text(label)
            .size(shell.secondary_font_size)
            .width(Length::FillPortion(super::PROPERTY_LABEL_PORTION)),
        copyable_input(id, input)
    ]
    .spacing(shell.field_spacing)
    .align_y(iced::Center)
    .into()
}

fn boolean_field<'a>(app: &'a App, label: &'a str, path: SettingsPath) -> Element<'a, Message> {
    let shell = app.drafts.shell_style();
    checkbox(app.drafts.active(app.selected_workflow).boolean(path))
        .label(label)
        .size(shell.checkbox_size)
        .spacing(shell.field_spacing)
        .text_size(shell.primary_font_size)
        .on_toggle(move |value| Message::SetBoolean(path, value))
        .into()
}

fn choice_field<'a>(
    app: &'a App,
    label: &'a str,
    path: SettingsPath,
    choices: &'static [NumericChoice],
) -> Element<'a, Message> {
    let shell = app.drafts.shell_style();
    let value = app.drafts.active(app.selected_workflow).integer(path);
    let selected = choices.iter().find(|choice| choice.value == value).copied();
    row![
        text(label)
            .size(shell.secondary_font_size)
            .width(Length::FillPortion(super::PROPERTY_LABEL_PORTION)),
        pick_list(selected, choices, NumericChoice::to_string)
            .on_select(move |choice| Message::SetNumericChoice(path, choice.value))
            .text_size(shell.primary_font_size)
            .padding([shell.control_padding_y, shell.control_padding_x])
            .width(Length::FillPortion(super::PROPERTY_INPUT_PORTION))
    ]
    .spacing(shell.field_spacing)
    .align_y(iced::Center)
    .into()
}

pub(crate) fn copyable_input<'a>(
    id: &'static str,
    input: impl Into<Element<'a, Message>>,
) -> Element<'a, Message> {
    ContextMenu::new(input, move || {
        container(
            column![
                input_context_action("Cut", Message::CutTextSelection(id.to_owned())),
                input_context_action("Copy", Message::CopyTextSelection(id.to_owned())),
                input_context_action("Paste", Message::PasteTextSelection(id.to_owned())),
                input_context_action("Select All", Message::SelectAllText(id.to_owned())),
            ]
            .spacing(2),
        )
        .padding(4)
        .width(Length::Fixed(128.0))
        .style(input_context_menu_style)
        .into()
    })
    .into()
}

fn input_context_action(label: &'static str, message: Message) -> Element<'static, Message> {
    button(label)
        .on_press(message)
        .padding([5, 8])
        .width(Fill)
        .style(input_context_action_style)
        .into()
}

fn input_context_menu_style(_theme: &iced::Theme) -> iced::widget::container::Style {
    iced::widget::container::Style {
        text_color: Some(Color::from_rgb8(0x22, 0x22, 0x22)),
        background: Some(Color::from_rgb8(0xf7, 0xf7, 0xf7).into()),
        border: Border {
            color: Color::from_rgb8(0xc7, 0xc7, 0xc7),
            width: 1.0,
            radius: 8.0.into(),
        },
        shadow: Shadow {
            color: Color::from_rgba8(0, 0, 0, 0.22),
            offset: Vector::new(0.0, 2.0),
            blur_radius: 4.0,
        },
        ..Default::default()
    }
}

fn input_context_action_style(
    _theme: &iced::Theme,
    status: iced::widget::button::Status,
) -> iced::widget::button::Style {
    let background = match status {
        iced::widget::button::Status::Hovered => Color::from_rgb8(0xe9, 0xe9, 0xe9),
        iced::widget::button::Status::Pressed => Color::from_rgb8(0xdd, 0xdd, 0xdd),
        iced::widget::button::Status::Active | iced::widget::button::Status::Disabled => {
            Color::TRANSPARENT
        }
    };
    iced::widget::button::Style {
        background: Some(background.into()),
        text_color: Color::from_rgb8(0x22, 0x22, 0x22),
        border: Border {
            radius: 5.0.into(),
            ..Border::default()
        },
        ..Default::default()
    }
}

fn attention_text_input_style(
    theme: &iced::Theme,
    status: iced::widget::text_input::Status,
) -> iced::widget::text_input::Style {
    let mut style = iced::widget::text_input::default(theme, status);
    style.background = iced::Color::from_rgb8(0xff, 0xd9, 0xdf).into();
    style.border.color = iced::Color::from_rgb8(0xd7, 0x6b, 0x7b);
    style.value = iced::Color::from_rgb8(0x32, 0x12, 0x17);
    style
}

fn number_bounds(path: SettingsPath) -> (f64, f64, f64) {
    let name = path.as_str();
    if name.ends_with("resolution") {
        (1.0, i32::MAX as f64, 1.0)
    } else if name.ends_with("threshold")
        || name.ends_with("probability")
        || name.ends_with("min_strength")
        || name.ends_with("max_strength")
        || name.ends_with("momentum")
        || name.ends_with("ema_decay")
        || name.ends_with("lr_min_factor")
    {
        (0.0, 1.0, 0.01)
    } else if name.contains("lr") || name.contains("decay") || name.contains("norm") {
        (0.0, 1_000.0, 0.0001)
    } else {
        (0.0, 1_000_000.0, 1.0)
    }
}
