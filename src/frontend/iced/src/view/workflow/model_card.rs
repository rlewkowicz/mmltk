use crate::fluent_theme::Element;
use crate::generated::{FeatureId, ModelArtifactInputKind, ModelSelectionSource, ModelUiState};
use crate::view::shared::status_text;
use iced::widget::{button, column, container, row, rule, space, text};
use iced::{Fill, Font, Length};
use std::hash::{Hash, Hasher};

impl Eq for crate::generated::PresetCatalogEntry {}

impl Hash for crate::generated::PresetCatalogEntry {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.presetname.hash(state);
    }
}

impl std::fmt::Display for crate::generated::PresetCatalogEntry {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(self.displayname.as_ref())
    }
}

#[derive(Debug, Clone)]
pub enum Message {
    PresetSelected(usize),
    BrowseRequested,
    ArtifactSelected { path: String, generation: u64 },
    PrepareRequested,
    StopRequested,
}

#[derive(Debug, Clone)]
pub enum Outcome {
    SettingsEdited(crate::view::settings::EditSchedule),
    ArtifactSelected(crate::view::settings::EditSchedule),
    BrowseRequested(crate::generated::FileDialogTarget),
    PrepareRequested,
    StopRequested,
}

pub struct Component {
    workflow: FeatureId,
    applied_dialog_generation: u64,
}

impl Default for Component {
    fn default() -> Self {
        Self::new(FeatureId::Train)
    }
}

impl Component {
    pub const fn new(workflow: FeatureId) -> Self {
        Self {
            workflow,
            applied_dialog_generation: 0,
        }
    }

    pub fn rebase(&mut self, _model: &crate::view_model::ApplicationModel, workflow: FeatureId) {
        self.workflow = workflow;
    }

    pub fn update(
        &mut self,
        message: Message,
        file_dialog: Option<&crate::generated::FileDialogSnapshot>,
        settings: &mut crate::view::settings::SettingsModel,
    ) -> Result<Option<Outcome>, String> {
        use crate::view::settings::EditCadence;
        let outcome = match message {
            Message::PresetSelected(index) => {
                let preset = crate::generated::RFDETR_PRESET_CATALOG
                    .get(index)
                    .ok_or_else(|| "Generated model preset is unavailable.".to_owned())?;
                let draft = settings
                    .draft
                    .as_ref()
                    .ok_or_else(|| "Model settings are unavailable.".to_owned())?;
                let projection = projection(draft, self.workflow)?;
                let input = compatible_input(
                    self.workflow,
                    ModelSelectionSource::Canonical,
                    projection.selection.key.input,
                )?;
                let resolution = preset.resolution as i32;
                let name = preset.presetname.to_string();
                let schedule = settings.edit_fields(
                    EditCadence::Debounced,
                    [
                        (
                            projection.fields.key_fields.source,
                            crate::generated::SettingsFieldValue::ModelSelectionSource(
                                ModelSelectionSource::Canonical,
                            ),
                        ),
                        (
                            projection.fields.key_fields.input,
                            crate::generated::SettingsFieldValue::ModelArtifactInputKind(input),
                        ),
                        (
                            projection.fields.key_fields.resolution,
                            crate::generated::SettingsFieldValue::I32(resolution),
                        ),
                        (
                            projection.fields.key_fields.preset,
                            crate::generated::SettingsFieldValue::String(name),
                        ),
                        (
                            projection.fields.key_fields.classlayoutpath,
                            crate::generated::SettingsFieldValue::String(String::new()),
                        ),
                    ],
                )?;
                Outcome::SettingsEdited(schedule)
            }
            Message::BrowseRequested => {
                let draft = settings
                    .draft
                    .as_ref()
                    .ok_or_else(|| "Model settings are unavailable.".to_owned())?;
                let projection = projection(draft, self.workflow)?;
                let row = custom_compatible_row(&projection)?;
                let target = dialog_for_row(row)?.target.clone();
                Outcome::BrowseRequested(crate::generated::FileDialogTarget::ModelArtifactTarget(
                    target,
                ))
            }
            Message::ArtifactSelected { path, generation } => {
                let workflow = self.workflow;
                let draft = settings
                    .draft
                    .as_ref()
                    .ok_or_else(|| "Model settings are unavailable.".to_owned())?;
                let projection = projection(draft, workflow)?;
                let opening = dialog_for_row(custom_compatible_row(&projection)?)?;
                if pending_artifact_selection(
                    file_dialog,
                    &opening.target,
                    self.applied_dialog_generation,
                ) != Some((path.as_str(), generation))
                {
                    return Err("The custom model selection is stale.".to_owned());
                }
                let row = artifact_row(workflow, &path)?;
                let dialog = dialog_for_row(row)?;
                let schedule = settings.edit_fields(
                    EditCadence::Immediate,
                    [
                        (
                            dialog.key_fields.source,
                            crate::generated::SettingsFieldValue::ModelSelectionSource(
                                ModelSelectionSource::Custom,
                            ),
                        ),
                        (
                            dialog.key_fields.input,
                            crate::generated::SettingsFieldValue::ModelArtifactInputKind(row.input),
                        ),
                        (
                            dialog.stable_field_id,
                            crate::generated::SettingsFieldValue::String(path),
                        ),
                        (
                            dialog.key_fields.classlayoutpath,
                            crate::generated::SettingsFieldValue::String(String::new()),
                        ),
                    ],
                )?;
                self.applied_dialog_generation = generation;
                Outcome::ArtifactSelected(schedule)
            }
            Message::PrepareRequested => Outcome::PrepareRequested,
            Message::StopRequested => Outcome::StopRequested,
        };
        Ok(Some(outcome))
    }

    pub fn view<'a>(&'a self, state: State<'a>) -> Element<'a, Message> {
        debug_assert_eq!(self.workflow, state.workflow);
        view(state)
    }
    pub fn view_with<'a, M: Clone + 'a>(
        &'a self,
        state: State<'a>,
        content: Element<'a, M>,
        map: fn(Message) -> M,
    ) -> Element<'a, M> {
        view_with(state, Some(content), map)
    }
}

fn projection(
    draft: &crate::generated::GuiSettingsState,
    workflow: FeatureId,
) -> Result<crate::view_model::ModelSettingsProjection, String> {
    crate::view_model::model_settings_projection(draft, workflow)
        .ok_or_else(|| "Generated model settings projection is unavailable.".to_owned())
}

pub struct State<'a> {
    pub workflow: FeatureId,
    pub preset: Option<String>,
    pub source: ModelSelectionSource,
    pub from_scratch: bool,
    pub artifact: String,
    pub model: Option<&'a ModelUiState>,
    pub settings_enabled: bool,
    pub prepare_enabled: bool,
    pub stop_enabled: bool,
}

impl<'a> State<'a> {
    pub fn from_settings(
        workflow: FeatureId,
        settings: Option<&'a crate::generated::GuiSettingsState>,
        model: Option<&'a ModelUiState>,
        settings_enabled: bool,
        prepare_enabled: bool,
        stop_enabled: bool,
    ) -> Self {
        let projection = settings
            .and_then(|settings| crate::view_model::model_settings_projection(settings, workflow));
        let source = projection
            .as_ref()
            .map_or(ModelSelectionSource::Canonical, |value| {
                value.selection.key.source
            });
        let preset = projection
            .as_ref()
            .map(|value| value.selection.key.preset.clone());
        let model = model.filter(|model| {
            model.active
                || model.terminal.outcome != crate::generated::ModelSelectionOutcome::Accepted
                || projection
                    .as_ref()
                    .is_some_and(|projection| projection.matches(&model.selection))
        });
        let from_scratch = projection
            .as_ref()
            .is_some_and(|value| value.selection.key.fromscratch);
        let artifact = projection.map_or_else(String::new, |value| value.selection.artifact);
        Self {
            workflow,
            preset,
            source,
            from_scratch,
            artifact,
            model,
            settings_enabled,
            prepare_enabled,
            stop_enabled,
        }
    }
}

fn compatibility(
    workflow: FeatureId,
) -> impl Iterator<Item = &'static crate::generated::ModelSelectionCompatibility> {
    crate::generated::MODEL_SELECTION_COMPATIBILITY_CATALOG
        .iter()
        .filter(move |row| row.workflow == workflow)
}

fn source_allowed(
    row: &crate::generated::ModelSelectionCompatibility,
    source: ModelSelectionSource,
) -> bool {
    match source {
        ModelSelectionSource::Canonical => row.canonicalallowed,
        ModelSelectionSource::Custom => row.customallowed,
    }
}

fn compatible_input(
    workflow: FeatureId,
    source: ModelSelectionSource,
    current: ModelArtifactInputKind,
) -> Result<ModelArtifactInputKind, String> {
    compatibility(workflow)
        .find(|row| row.input == current && source_allowed(row, source))
        .or_else(|| compatibility(workflow).find(|row| source_allowed(row, source)))
        .map(|row| row.input)
        .ok_or_else(|| "Generated catalog has no compatible input for this source.".to_owned())
}

fn custom_compatible_row(
    projection: &crate::view_model::ModelSettingsProjection,
) -> Result<&'static crate::generated::ModelSelectionCompatibility, String> {
    if projection.selection.key.fromscratch {
        return Err("Custom detector weights are unavailable in From scratch mode.".to_owned());
    }
    let workflow = projection.fields.target.workflow;
    compatibility(workflow)
        .find(|row| row.input == projection.selection.key.input && row.customallowed)
        .or_else(|| compatibility(workflow).find(|row| row.customallowed))
        .ok_or_else(|| {
            "Generated catalog has no custom row for the current model input.".to_owned()
        })
}

const fn supported_workflow(workflow: FeatureId) -> bool {
    matches!(
        workflow,
        FeatureId::Train | FeatureId::Validate | FeatureId::Predict | FeatureId::Export
    )
}

fn artifact_row(
    workflow: FeatureId,
    path: &str,
) -> Result<&'static crate::generated::ModelSelectionCompatibility, String> {
    compatibility(workflow)
        .find(|row| {
            row.customallowed
                && row.dialogpattern.split_whitespace().any(|pattern| {
                    pattern.strip_prefix('*').is_some_and(|suffix| {
                        path.get(path.len().saturating_sub(suffix.len())..)
                            .is_some_and(|ending| ending.eq_ignore_ascii_case(suffix))
                    })
                })
        })
        .ok_or_else(|| "The selected file extension is not supported by this workflow.".to_owned())
}

fn dialog_for_row(
    row: &crate::generated::ModelSelectionCompatibility,
) -> Result<&'static crate::generated::ModelArtifactDialogFact, String> {
    crate::generated::MODEL_ARTIFACT_DIALOGS
        .iter()
        .find(|dialog| {
            dialog.target.workflow == row.workflow
                && dialog.target.input == row.input
                && dialog.field_path == row.artifactfieldpath
        })
        .ok_or_else(|| "Generated model artifact dialog is unavailable.".to_owned())
}

pub const fn stable_id(workflow: FeatureId) -> &'static str {
    match workflow {
        FeatureId::Train => "train.card.model",
        FeatureId::Validate => "validate.card.model",
        FeatureId::Predict => "predict.card.model",
        FeatureId::Export => "export.card.model",
        FeatureId::Live | FeatureId::Annotate | FeatureId::Explore => "workflow.card.model",
    }
}

pub const fn progress_id(workflow: FeatureId) -> &'static str {
    match workflow {
        FeatureId::Train => "train.card.model.progress",
        FeatureId::Validate => "validate.card.model.progress",
        FeatureId::Predict => "predict.card.model.progress",
        FeatureId::Export => "export.card.model.progress",
        FeatureId::Live | FeatureId::Annotate | FeatureId::Explore => {
            "workflow.card.model.progress"
        }
    }
}

pub const fn artifact_id(workflow: FeatureId) -> &'static str {
    match workflow {
        FeatureId::Train => "train.card.model.artifact",
        FeatureId::Validate => "validate.card.model.artifact",
        FeatureId::Predict => "predict.card.model.artifact",
        FeatureId::Export => "export.card.model.artifact",
        FeatureId::Live | FeatureId::Annotate | FeatureId::Explore => {
            "workflow.card.model.artifact"
        }
    }
}

fn selector_id(
    workflow: FeatureId,
    train: &'static str,
    validate: &'static str,
    predict: &'static str,
    export: &'static str,
) -> &'static str {
    match workflow {
        FeatureId::Train => train,
        FeatureId::Validate => validate,
        FeatureId::Predict => predict,
        FeatureId::Export => export,
        _ => unreachable!("model selector requires a model workflow"),
    }
}

pub const TRAIN_SELECTOR_ID: &str = "train.model.selector";
pub const TRAIN_PRESETS_ID: &str = "train.model.presets";
pub const TRAIN_DIVIDER_ID: &str = "train.model.divider";
pub const TRAIN_CUSTOM_ID: &str = "train.model.custom_weights";
pub const TRAIN_STATUS_ID: &str = "train.model.status";
pub const TRAIN_ACTION_ID: &str = "train.model.action";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum StatusTone {
    Neutral,
    Ready,
    Active,
}

pub(crate) struct StatusPresentation {
    pub label: String,
    pub detail: String,
    pub tone: StatusTone,
}

pub(crate) fn status_presentation(state: Option<&ModelUiState>) -> StatusPresentation {
    let Some(state) = state else {
        return StatusPresentation {
            label: "Model state unavailable".to_owned(),
            detail: String::new(),
            tone: StatusTone::Neutral,
        };
    };
    if state.active {
        return StatusPresentation {
            label: "Preparing".to_owned(),
            detail: state.progress.activity.clone(),
            tone: StatusTone::Active,
        };
    }
    let (label, tone) = match state.terminal.outcome {
        crate::generated::ModelSelectionOutcome::Idle => ("Not prepared", StatusTone::Neutral),
        crate::generated::ModelSelectionOutcome::Accepted => ("Ready", StatusTone::Ready),
        crate::generated::ModelSelectionOutcome::Rejected => ("Not prepared", StatusTone::Neutral),
        crate::generated::ModelSelectionOutcome::CancellationRequested => {
            ("Stopping", StatusTone::Active)
        }
        crate::generated::ModelSelectionOutcome::Cancelled => ("Cancelled", StatusTone::Neutral),
    };
    StatusPresentation {
        label: label.to_owned(),
        detail: if state.terminal.outcome == crate::generated::ModelSelectionOutcome::Accepted
            && !state.selection.key.fromscratch
        {
            let layout = &state.selection.classlayout;
            let meaning = match layout.domain {
                crate::generated::ClassReferenceDomain::Foreground => {
                    format!("{} foreground classes", layout.foregroundcount)
                }
                crate::generated::ClassReferenceDomain::RawOutputSlot => {
                    "Raw output slots; class identity unresolved".to_owned()
                }
            };
            format!(
                "{meaning} · {} outputs · {} background · {} unused · {:?}",
                layout.outputcount,
                layout.backgroundcount,
                layout.unusedcount,
                layout.provenance.origin
            )
        } else if state.terminal.outcome == crate::generated::ModelSelectionOutcome::Rejected {
            String::new()
        } else {
            state.terminal.detail.clone()
        },
        tone,
    }
}

pub(crate) const fn card_title(workflow: FeatureId) -> &'static str {
    if supported_workflow(workflow) {
        "RF-DETR Weights"
    } else {
        "RF-DETR Model"
    }
}

pub(crate) fn encoder_lineage<'a, M: Clone + 'a>(
    provenance: &crate::generated::EncoderPretrainingProvenance,
) -> Element<'a, M> {
    column![
        text(format!("Base encoder: {}", provenance.assetid)).size(12),
        status_text(format!("SHA-256 {}", provenance.sha256)).size(12),
        status_text(format!(
            "Revision {} · adaptation {}",
            provenance.sourcerevision, provenance.adaptationrevision
        ))
        .size(12)
    ]
    .spacing(4)
    .into()
}

fn status<'a>(state: Option<&ModelUiState>, workflow: FeatureId) -> Element<'a, Message> {
    let mut status = status_presentation(state);
    if supported_workflow(workflow) && status.tone == StatusTone::Ready {
        status.detail.clear();
    }
    let label = match status.tone {
        StatusTone::Neutral => status_text(status.label).style(crate::fluent_theme::text_secondary),
        StatusTone::Ready => status_text(status.label).style(crate::fluent_theme::text_success),
        StatusTone::Active => status_text(status.label).style(crate::fluent_theme::text_warning),
    };
    let mut content = if status.detail.is_empty() {
        column![row![text("Status:"), label].spacing(5)]
    } else {
        column![
            row![text("Status:"), label].spacing(5),
            status_text(status.detail).size(12)
        ]
        .spacing(4)
    };
    if let Some(provenance) = state
        .filter(|value| {
            !value.active
                && value.terminal.outcome == crate::generated::ModelSelectionOutcome::Accepted
        })
        .and_then(|value| value.selection.encoderpretraining.as_ref())
    {
        content = content.push(encoder_lineage(provenance));
    }
    let content = container(content)
        .id(selector_id(
            workflow,
            TRAIN_STATUS_ID,
            "validate.model.status",
            "predict.model.status",
            "export.model.status",
        ))
        .width(Fill);
    content.into()
}

fn pending_artifact_selection<'a>(
    file_dialog: Option<&'a crate::generated::FileDialogSnapshot>,
    target: &crate::generated::ModelArtifactTarget,
    applied_dialog_generation: u64,
) -> Option<(&'a str, u64)> {
    let dialog = file_dialog.filter(|dialog| {
        !dialog.active
            && !dialog.cancellationrequested
            && dialog.generation > applied_dialog_generation
            && matches!(
                &dialog.target,
                crate::generated::FileDialogTarget::ModelArtifactTarget(current)
                    if current == target
            )
    })?;
    let selection = dialog.selection.as_ref()?;
    if selection.target != dialog.target {
        return None;
    }
    let crate::generated::FileDialogCancelledOrFileDialogSelectedVariant::FileDialogSelected(
        selected,
    ) = &selection.result
    else {
        return None;
    };
    Some((selected.path.as_str(), dialog.generation))
}

fn shared_selector<'a>(
    workflow: FeatureId,
    presets: Element<'a, Message>,
    enabled: bool,
) -> Element<'a, Message> {
    container(
        column![
            container(presets)
                .id(selector_id(
                    workflow,
                    TRAIN_PRESETS_ID,
                    "validate.model.presets",
                    "predict.model.presets",
                    "export.model.presets"
                ))
                .width(Fill),
            container(
                row![
                    space::horizontal().width(Length::FillPortion(1)),
                    container(rule::horizontal(1)).width(Length::FillPortion(6)),
                    space::horizontal().width(Length::FillPortion(1)),
                ]
                .width(Fill)
            )
            .id(selector_id(
                workflow,
                TRAIN_DIVIDER_ID,
                "validate.model.divider",
                "predict.model.divider",
                "export.model.divider"
            ))
            .width(Fill),
            container(
                button("Custom Weights")
                    .on_press_maybe(enabled.then_some(Message::BrowseRequested))
                    .style(crate::fluent_theme::button_primary)
                    .width(Fill)
            )
            .id(selector_id(
                workflow,
                TRAIN_CUSTOM_ID,
                "validate.model.custom_weights",
                "predict.model.custom_weights",
                "export.model.custom_weights"
            ))
            .width(Fill),
        ]
        .spacing(super::FIELD_SPACING),
    )
    .id(selector_id(
        workflow,
        TRAIN_SELECTOR_ID,
        "validate.model.selector",
        "predict.model.selector",
        "export.model.selector",
    ))
    .padding(2)
    .width(Fill)
    .style(crate::fluent_theme::container_bordered_box)
    .into()
}

pub fn view(state: State<'_>) -> Element<'_, Message> {
    view_with(state, None, |message| message)
}
fn view_with<'a, M: Clone + 'a>(
    state: State<'a>,
    content: Option<Element<'a, M>>,
    map: fn(Message) -> M,
) -> Element<'a, M> {
    const ROW_PADDING: f32 = 5.0;
    const TEXT_SIZE: f32 = 12.0;
    let card_id = stable_id(state.workflow);
    let selected_index = crate::generated::RFDETR_PRESET_CATALOG
        .iter()
        .position(|preset| state.preset.as_deref() == Some(preset.presetname.as_ref()));
    let preset_label = state
        .preset
        .clone()
        .unwrap_or_else(|| "Model settings unavailable".to_owned());
    let list_height = crate::generated::RFDETR_PRESET_CATALOG.len() as f32
        * (TEXT_SIZE + ROW_PADDING * 2.0)
        + 2.0;
    let presets: Element<'_, Message> = if state.settings_enabled {
        iced_aw::helpers::selection_list_with(
            crate::generated::RFDETR_PRESET_CATALOG,
            |index, _| Message::PresetSelected(index),
            TEXT_SIZE,
            ROW_PADDING,
            iced_fluent_theme::selection_list_style,
            selected_index,
            Font::default(),
        )
        .width(Fill)
        .height(Length::Fixed(list_height))
        .into()
    } else {
        text(preset_label).into()
    };
    let artifact: Element<'_, Message> = if state.source == ModelSelectionSource::Custom {
        status_text(if state.artifact.is_empty() {
            "No custom model selected".to_owned()
        } else {
            state.artifact.clone()
        })
        .size(12)
        .into()
    } else {
        space::vertical().height(0).into()
    };
    let artifact = crate::view::shared::disclosure(
        artifact_id(state.workflow),
        state.source == ModelSelectionSource::Custom,
        artifact,
    )
    .animate_resize();
    let progress: Element<'_, Message> = state.model.map_or_else(
        || status_text("Model state unavailable").size(12).into(),
        |model| {
            let progress = if !model.active
                && model.terminal.outcome == crate::generated::ModelSelectionOutcome::Accepted
            {
                crate::view::workflow::progress::Presentation::Hidden
            } else {
                crate::view::workflow::progress::model_presentation(Some(model))
            };
            crate::view::workflow::progress::presentation(progress)
        },
    );
    let action = if state.model.is_some_and(|model| model.active) {
        button("Stop").on_press_maybe(state.stop_enabled.then_some(Message::StopRequested))
    } else {
        button("Prepare model")
            .on_press_maybe(state.prepare_enabled.then_some(Message::PrepareRequested))
            .style(crate::fluent_theme::button_primary)
    };
    let title = card_title(state.workflow);
    let body = if supported_workflow(state.workflow) {
        let weights_selector = shared_selector(
            state.workflow,
            presets,
            state.settings_enabled && !state.from_scratch,
        );
        column![
            weights_selector,
            status_text(
                selected_index
                    .map(|index| {
                        let preset = &crate::generated::RFDETR_PRESET_CATALOG[index];
                        let task_kind = state
                            .model
                            .filter(|model| {
                                !model.active
                                    && model.terminal.outcome
                                        == crate::generated::ModelSelectionOutcome::Accepted
                                    && model.selection.artifact == state.artifact
                                    && model.selection.key.source == state.source
                            })
                            .map_or(preset.task, |model| model.selection.task);
                        let task = match task_kind {
                            crate::generated::ModelTask::Detection => "Detection",
                            crate::generated::ModelTask::Segmentation => "Segmentation",
                            crate::generated::ModelTask::Keypoints => "Keypoints",
                        };
                        let license = match preset.license {
                            crate::generated::ModelLicense::Apache20 => "Apache 2.0",
                            crate::generated::ModelLicense::PlatformModel10 => {
                                "Roboflow Platform Model License 1.0"
                            }
                        };
                        if state.source == ModelSelectionSource::Canonical {
                            format!("{task} · {license}")
                        } else {
                            format!("{task} · Custom artifact")
                        }
                    })
                    .unwrap_or_default()
            ),
            status(state.model, state.workflow),
            artifact,
            iced::widget::container(progress)
                .id(progress_id(state.workflow))
                .padding(iced::Padding {
                    bottom: 1.0,
                    ..iced::Padding::ZERO
                })
                .width(Fill),
            container(action).id(selector_id(
                state.workflow,
                TRAIN_ACTION_ID,
                "validate.model.action",
                "predict.model.action",
                "export.model.action"
            ))
        ]
        .spacing(super::FIELD_SPACING)
    } else {
        column![status_text("Model workflow unavailable")]
    };
    let body: Element<'a, Message> = body.into();
    let mut body = column![body.map(map)].spacing(super::FIELD_SPACING);
    if let Some(content) = content {
        body = body.push(content);
    }
    crate::view::shared::identified(
        card_id,
        crate::view::shared::card(
            title,
            "Select and prepare the exact artifact consumed by this workflow.",
            body,
        ),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view::settings::installed_settings_model;
    use crate::view_model::test_support::selected_model_dialog;

    #[test]
    fn unsupported_workflows_construct_the_unavailable_card() {
        let settings = installed_settings_model();
        for workflow in [FeatureId::Live, FeatureId::Annotate, FeatureId::Explore] {
            let mut card = view(State::from_settings(
                workflow,
                settings.draft.as_ref(),
                None,
                true,
                false,
                false,
            ));
            let mut tree = iced::advanced::widget::Tree::new(&card);
            tree.diff(&mut card);
            assert!(!tree.children.is_empty());
        }
    }

    #[test]
    fn train_and_validate_have_the_same_common_selector_widget_shape() {
        fn shape(tree: &iced::advanced::widget::Tree, result: &mut Vec<usize>) {
            result.push(tree.children.len());
            for child in &tree.children {
                shape(child, result);
            }
        }
        let widget_shape = |workflow| {
            let mut card = shared_selector(workflow, text("preset fixture").into(), true);
            let mut tree = iced::advanced::widget::Tree::new(&card);
            tree.diff(&mut card);
            assert!(!tree.children.is_empty());
            let mut result = Vec::new();
            shape(&tree, &mut result);
            result
        };
        assert_eq!(
            widget_shape(FeatureId::Train),
            widget_shape(FeatureId::Validate)
        );
        assert_eq!(
            widget_shape(FeatureId::Train),
            widget_shape(FeatureId::Predict)
        );
        assert_eq!(
            widget_shape(FeatureId::Train),
            widget_shape(FeatureId::Export)
        );
        let workflows = [
            FeatureId::Train,
            FeatureId::Validate,
            FeatureId::Predict,
            FeatureId::Export,
        ];
        for (index, workflow) in workflows.iter().enumerate() {
            for other in &workflows[index + 1..] {
                assert_ne!(stable_id(*workflow), stable_id(*other));
                assert_ne!(artifact_id(*workflow), artifact_id(*other));
                assert_ne!(progress_id(*workflow), progress_id(*other));
                assert_ne!(
                    selector_id(
                        *workflow,
                        TRAIN_SELECTOR_ID,
                        "validate.model.selector",
                        "predict.model.selector",
                        "export.model.selector"
                    ),
                    selector_id(
                        *other,
                        TRAIN_SELECTOR_ID,
                        "validate.model.selector",
                        "predict.model.selector",
                        "export.model.selector"
                    )
                );
            }
        }
    }

    #[test]
    fn component_admits_only_its_current_settled_selection() {
        for workflow in [
            FeatureId::Train,
            FeatureId::Validate,
            FeatureId::Predict,
            FeatureId::Export,
        ] {
            let mut settings = installed_settings_model();
            let mut component = Component::new(workflow);
            let path = "/tmp/model.pt";
            let dialog = selected_model_dialog(settings.draft.as_ref().unwrap(), workflow, path, 7);
            let select = || Message::ArtifactSelected {
                path: path.into(),
                generation: 7,
            };
            let before = settings.draft.clone();
            let queued = settings.clone().take_request([true; 3]);
            for (case, invalid) in
                crate::view_model::test_support::inadmissible_model_dialogs(&dialog)
            {
                assert!(
                    component
                        .update(select(), invalid.as_ref(), &mut settings)
                        .is_err(),
                    "{case}"
                );
                assert_eq!(settings.draft, before, "{case}");
                assert_eq!(settings.clone().take_request([true; 3]), queued, "{case}");
                assert_eq!(component.applied_dialog_generation, 0, "{case}");
            }
            // Unavailable settings must leave the selection available for retry.
            settings.draft = None;
            assert!(
                component
                    .update(select(), Some(&dialog), &mut settings)
                    .is_err()
            );
            assert_eq!(component.applied_dialog_generation, 0);
            assert_eq!(settings.clone().take_request([true; 3]), queued);
            settings.draft = before;
            assert!(matches!(
                component
                    .update(select(), Some(&dialog), &mut settings)
                    .unwrap(),
                Some(Outcome::ArtifactSelected(_))
            ));
            let selected = settings.draft.clone();
            let edits = settings.clone().take_request([true; 3]);
            component.rebase(&crate::view_model::ApplicationModel::default(), workflow);
            assert!(
                component
                    .update(select(), Some(&dialog), &mut settings)
                    .is_err()
            );
            assert_eq!(component.applied_dialog_generation, 7);
            assert_eq!(settings.draft, selected);
            assert_eq!(settings.clone().take_request([true; 3]), edits);
        }
    }

    #[test]
    fn only_successful_selection_emits_the_distinct_selection_outcome() {
        for workflow in [
            FeatureId::Train,
            FeatureId::Validate,
            FeatureId::Predict,
            FeatureId::Export,
        ] {
            let mut component = Component::new(workflow);
            let mut settings = installed_settings_model();
            for generation in [1, 2] {
                assert!(matches!(
                    component
                        .update(
                            Message::ArtifactSelected {
                                path: "/tmp/same.pt".into(),
                                generation,
                            },
                            Some(&selected_model_dialog(
                                settings.draft.as_ref().unwrap(),
                                component.workflow,
                                "/tmp/same.pt",
                                generation
                            )),
                            &mut settings
                        )
                        .unwrap(),
                    Some(Outcome::ArtifactSelected(_))
                ));
            }
            let before = settings.draft.clone();
            let queued = settings.queued_len();
            assert!(
                component
                    .update(
                        Message::ArtifactSelected {
                            path: "/tmp/invalid.txt".into(),
                            generation: 4,
                        },
                        Some(&selected_model_dialog(
                            settings.draft.as_ref().unwrap(),
                            component.workflow,
                            "/tmp/invalid.txt",
                            4
                        )),
                        &mut settings
                    )
                    .is_err()
            );
            assert_eq!(settings.draft, before);
            assert_eq!(settings.queued_len(), queued);
            assert!(
                component
                    .update(
                        Message::ArtifactSelected {
                            path: "/tmp/stale.pt".into(),
                            generation: 2
                        },
                        Some(&selected_model_dialog(
                            settings.draft.as_ref().unwrap(),
                            component.workflow,
                            "/tmp/stale.pt",
                            2
                        )),
                        &mut settings
                    )
                    .is_err()
            );
            assert_eq!(settings.draft, before);
            assert_eq!(settings.queued_len(), queued);
        }
    }

    #[test]
    fn common_selector_infers_each_native_extension_without_companion_leakage() {
        for workflow in [
            FeatureId::Train,
            FeatureId::Validate,
            FeatureId::Predict,
            FeatureId::Export,
        ] {
            assert!(supported_workflow(workflow));
            assert_eq!(card_title(workflow), "RF-DETR Weights");
            let mut component = Component::new(workflow);
            let mut settings = installed_settings_model();
            let mut generation = 0;
            for row in compatibility(workflow).filter(|row| row.customallowed) {
                for pattern in row.dialogpattern.split_whitespace() {
                    generation += 1;
                    let path =
                        format!("/tmp/model{}", pattern.trim_start_matches('*')).to_uppercase();
                    let fields = projection(settings.draft.as_ref().unwrap(), workflow)
                        .unwrap()
                        .fields;
                    crate::generated::apply_settings_field(
                        settings.draft.as_mut().unwrap(),
                        fields.key_fields.classlayoutpath,
                        crate::generated::SettingsFieldValue::String("/old/classes.json".into()),
                    )
                    .unwrap();
                    component
                        .update(
                            Message::ArtifactSelected {
                                path: path.clone(),
                                generation,
                            },
                            Some(&selected_model_dialog(
                                settings.draft.as_ref().unwrap(),
                                component.workflow,
                                &path,
                                generation,
                            )),
                            &mut settings,
                        )
                        .unwrap();
                    let selected = projection(settings.draft.as_ref().unwrap(), workflow).unwrap();
                    assert_eq!(selected.selection.key.input, row.input);
                    assert_eq!(selected.selection.artifact, path);
                    assert!(selected.selection.key.classlayoutpath.is_empty());
                }
            }
            let fields = projection(settings.draft.as_ref().unwrap(), workflow)
                .unwrap()
                .fields;
            crate::generated::apply_settings_field(
                settings.draft.as_mut().unwrap(),
                fields.key_fields.classlayoutpath,
                crate::generated::SettingsFieldValue::String("/old/classes.json".into()),
            )
            .unwrap();
            assert!(matches!(
                component
                    .update(Message::PresetSelected(0), None, &mut settings)
                    .unwrap(),
                Some(Outcome::SettingsEdited(_))
            ));
            let preset = projection(settings.draft.as_ref().unwrap(), workflow).unwrap();
            assert_eq!(preset.selection.key.source, ModelSelectionSource::Canonical);
            assert!(preset.selection.key.classlayoutpath.is_empty());
            let before = settings.draft.clone();
            assert!(
                component
                    .update(
                        Message::ArtifactSelected {
                            path: "/tmp/model.txt".into(),
                            generation: 2
                        },
                        Some(&selected_model_dialog(
                            settings.draft.as_ref().unwrap(),
                            component.workflow,
                            "/tmp/model.txt",
                            2
                        )),
                        &mut settings
                    )
                    .is_err()
            );
            assert_eq!(settings.draft, before);
        }
        assert!(artifact_row(FeatureId::Train, "/model.onnx").is_err());
        for workflow in [FeatureId::Validate, FeatureId::Predict, FeatureId::Export] {
            assert!(artifact_row(workflow, "/run/session.json").is_err());
        }
        assert_eq!(
            artifact_row(FeatureId::Validate, "/MODEL.ONNX")
                .unwrap()
                .input,
            ModelArtifactInputKind::Onnx
        );
        assert!(supported_workflow(FeatureId::Predict));
        assert!(supported_workflow(FeatureId::Export));
    }

    #[test]
    fn workflow_choices_and_open_ended_progress_are_truthful() {
        assert_eq!(compatibility(FeatureId::Train).count(), 1);
        assert_eq!(compatibility(FeatureId::Export).count(), 1);
        let mut settings = crate::view::settings::SettingsModel::default();
        assert!(matches!(
            Component::new(FeatureId::Train).update(Message::PrepareRequested, None, &mut settings),
            Ok(Some(Outcome::PrepareRequested))
        ));
        assert_eq!(stable_id(FeatureId::Train), "train.card.model");
        assert_eq!(card_title(FeatureId::Train), "RF-DETR Weights");
    }

    #[test]
    fn train_custom_weights_uses_the_existing_typed_dialog_for_atomic_selection() {
        for path in ["/tmp/custom.pth", "/tmp/run/session.json"] {
            let mut settings = installed_settings_model();
            let mut component = Component::new(FeatureId::Train);
            let browse = component
                .update(Message::BrowseRequested, None, &mut settings)
                .unwrap();
            assert!(matches!(
                browse,
                Some(Outcome::BrowseRequested(
                    crate::generated::FileDialogTarget::ModelArtifactTarget(_)
                ))
            ));
            component
                .update(
                    Message::ArtifactSelected {
                        path: path.into(),
                        generation: 1,
                    },
                    Some(&selected_model_dialog(
                        settings.draft.as_ref().unwrap(),
                        component.workflow,
                        path,
                        1,
                    )),
                    &mut settings,
                )
                .unwrap();
            let selected = projection(settings.draft.as_ref().unwrap(), FeatureId::Train).unwrap();
            assert_eq!(selected.selection.key.source, ModelSelectionSource::Custom);
            assert_eq!(
                selected.selection.key.input,
                ModelArtifactInputKind::Weights
            );
            assert_eq!(selected.selection.artifact, path);
        }
    }

    #[test]
    fn typed_model_outcomes_select_semantic_status_tones() {
        let mut model = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .find_map(|fact| match fact.value {
                crate::generated::ApplicationSnapshot::Model(value) => Some(value),
                _ => None,
            })
            .unwrap();
        for (outcome, tone) in [
            (
                crate::generated::ModelSelectionOutcome::Idle,
                StatusTone::Neutral,
            ),
            (
                crate::generated::ModelSelectionOutcome::Accepted,
                StatusTone::Ready,
            ),
            (
                crate::generated::ModelSelectionOutcome::Rejected,
                StatusTone::Neutral,
            ),
            (
                crate::generated::ModelSelectionOutcome::CancellationRequested,
                StatusTone::Active,
            ),
            (
                crate::generated::ModelSelectionOutcome::Cancelled,
                StatusTone::Neutral,
            ),
        ] {
            model.active = false;
            model.terminal.outcome = outcome;
            assert_eq!(status_presentation(Some(&model)).tone, tone);
        }
        model.active = true;
        assert_eq!(status_presentation(Some(&model)).tone, StatusTone::Active);
    }
    #[test]
    fn invalid_artifact_messages_preserve_draft_and_queue_exactly() {
        let configure = |settings: &mut crate::view::settings::SettingsModel,
                         row: &crate::generated::ModelSelectionCompatibility,
                         source| {
            let draft = settings.draft.as_mut().unwrap();
            let fields = projection(draft, row.workflow).unwrap().fields;

            crate::generated::apply_settings_field(
                draft,
                fields.key_fields.input,
                crate::generated::SettingsFieldValue::ModelArtifactInputKind(row.input),
            )
            .unwrap();
            crate::generated::apply_settings_field(
                draft,
                fields.key_fields.source,
                crate::generated::SettingsFieldValue::ModelSelectionSource(source),
            )
            .unwrap();
        };

        let canonical = crate::generated::MODEL_SELECTION_COMPATIBILITY_CATALOG
            .iter()
            .find(|row| row.canonicalallowed)
            .unwrap();
        let mut settings = installed_settings_model();
        configure(&mut settings, canonical, ModelSelectionSource::Canonical);
        if canonical.customallowed {
            Component::new(canonical.workflow)
                .update(
                    Message::ArtifactSelected {
                        path: "/tmp/model.pt".into(),
                        generation: 1,
                    },
                    Some(&selected_model_dialog(
                        settings.draft.as_ref().unwrap(),
                        canonical.workflow,
                        "/tmp/model.pt",
                        1,
                    )),
                    &mut settings,
                )
                .unwrap();
            let selected =
                projection(settings.draft.as_ref().unwrap(), canonical.workflow).unwrap();
            assert_eq!(selected.selection.key.source, ModelSelectionSource::Custom);
            assert_eq!(selected.selection.key.input, canonical.input);
            assert_eq!(selected.selection.artifact, "/tmp/model.pt");
        } else {
            let before = settings.draft.clone();
            let queued = settings.queued_len();
            assert!(
                Component::new(canonical.workflow)
                    .update(
                        Message::ArtifactSelected {
                            path: "/tmp/model".into(),
                            generation: 1,
                        },
                        Some(&selected_model_dialog(
                            settings.draft.as_ref().unwrap(),
                            canonical.workflow,
                            "/tmp/model",
                            1
                        )),
                        &mut settings,
                    )
                    .is_err()
            );
            assert_eq!(settings.draft, before);
            assert_eq!(settings.queued_len(), queued);
        }

        let custom = crate::generated::MODEL_SELECTION_COMPATIBILITY_CATALOG
            .iter()
            .find(|row| row.customallowed && row.workflow != FeatureId::Train)
            .unwrap();
        let mut settings = installed_settings_model();
        configure(&mut settings, custom, ModelSelectionSource::Custom);
        let input_field_id = projection(settings.draft.as_ref().unwrap(), custom.workflow)
            .unwrap()
            .fields
            .key_fields
            .input;
        crate::generated::apply_settings_field(
            settings.draft.as_mut().unwrap(),
            input_field_id,
            crate::generated::SettingsFieldValue::ModelArtifactInputKind(
                ModelArtifactInputKind::None,
            ),
        )
        .unwrap();
        let before = settings.draft.clone();
        let queued = settings.queued_len();
        assert!(
            Component::new(custom.workflow)
                .update(
                    Message::ArtifactSelected {
                        path: "/tmp/model".into(),
                        generation: 1,
                    },
                    Some(&selected_model_dialog(
                        settings.draft.as_ref().unwrap(),
                        custom.workflow,
                        "/tmp/model",
                        1
                    )),
                    &mut settings,
                )
                .is_err()
        );
        assert_eq!(settings.draft, before);
        assert_eq!(settings.queued_len(), queued);

        for unsupported in crate::generated::MODEL_SELECTION_COMPATIBILITY_CATALOG
            .iter()
            .filter(|row| !row.customallowed)
        {
            let mut settings = installed_settings_model();
            configure(&mut settings, unsupported, ModelSelectionSource::Custom);
            let before = settings.draft.clone();
            let queued = settings.queued_len();
            assert!(
                Component::new(unsupported.workflow)
                    .update(
                        Message::ArtifactSelected {
                            path: "/tmp/model".into(),
                            generation: 1,
                        },
                        Some(&selected_model_dialog(
                            settings.draft.as_ref().unwrap(),
                            unsupported.workflow,
                            "/tmp/model",
                            1
                        )),
                        &mut settings,
                    )
                    .is_err()
            );
            assert_eq!(settings.draft, before);
            assert_eq!(settings.queued_len(), queued);
        }
    }

    #[test]
    fn applied_dialog_generation_survives_settings_rebase_and_admits_newer_selections() {
        let row = crate::generated::MODEL_SELECTION_COMPATIBILITY_CATALOG
            .iter()
            .find(|row| row.workflow == FeatureId::Train && row.customallowed)
            .unwrap();
        let mut settings = installed_settings_model();
        let mut component = Component::new(FeatureId::Train);
        component
            .update(
                Message::ArtifactSelected {
                    path: "/tmp/model.pt".into(),
                    generation: 7,
                },
                Some(&selected_model_dialog(
                    settings.draft.as_ref().unwrap(),
                    component.workflow,
                    "/tmp/model.pt",
                    7,
                )),
                &mut settings,
            )
            .unwrap();
        component.rebase(
            &crate::view_model::ApplicationModel::default(),
            FeatureId::Train,
        );
        assert_eq!(component.applied_dialog_generation, 7);
        let snapshot = |generation, path: &str| {
            selected_model_dialog(
                settings.draft.as_ref().unwrap(),
                FeatureId::Train,
                path,
                generation,
            )
        };
        let settled = snapshot(7, "/tmp/model.pt");
        let target = &dialog_for_row(row).unwrap().target;
        assert!(
            pending_artifact_selection(Some(&settled), target, component.applied_dialog_generation)
                .is_none()
        );
        let newer = snapshot(8, "/tmp/newer-model.pth");
        assert_eq!(
            pending_artifact_selection(Some(&newer), target, component.applied_dialog_generation),
            Some(("/tmp/newer-model.pth", 8))
        );
        assert_eq!(row.input, ModelArtifactInputKind::Weights);
    }
}
