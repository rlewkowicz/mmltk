use crate::generated::{
    BrowserHostBackend, BrowserRuntimeCapabilityStatus, CustomModelArtifactKind,
    RF_DETR_PRESET_OPTIONS, RfDetrPresetOption, SETTINGS_PATHS, SettingsPath, SettingsValueKind,
    SourceKind, StateSnapshot, Workflow,
};
use iced::Color;
use serde::Deserialize;
use serde_json::{Map, Number, Value};
use std::fmt;

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BridgePhase {
    Idle,
    #[default]
    Polling,
    Dispatch,
}

#[derive(Debug, Clone, Default, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BridgeRuntimeCapabilities {
    pub host_backend: BrowserHostBackend,
    pub navigator_gpu: BrowserRuntimeCapabilityStatus,
    pub workspace_surface_bridge: BrowserRuntimeCapabilityStatus,
    pub workspace_surface_zero_copy: BrowserRuntimeCapabilityStatus,
}

#[derive(Debug, Clone, Default, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BridgeState {
    pub phase: BridgePhase,
    pub connected: bool,
    #[serde(default)]
    pub last_error: String,
    pub last_success_revision: Option<u64>,
    #[serde(default)]
    pub runtime_capabilities: BridgeRuntimeCapabilities,
    #[serde(default)]
    pub capabilities: Map<String, Value>,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum TransportStatus {
    #[default]
    Connecting,
    Connected,
    Reconnecting,
}

impl TransportStatus {
    pub const fn label(self) -> &'static str {
        const LABELS: [&str; 3] = ["connecting", "connected", "reconnecting"];
        LABELS[self as usize]
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CustomModelConfirmation {
    pub dialog_token: u64,
    pub workflow: Workflow,
    pub artifact_kind: CustomModelArtifactKind,
    pub preset_name: String,
    pub resolution: u32,
    pub path: String,
}

#[derive(Debug, Clone, PartialEq)]
pub enum DraftValue {
    Text(String),
    Number(f64),
    Boolean(bool),
    NumberArray(Vec<f64>),
    BooleanArray(Vec<bool>),
}

impl DraftValue {
    fn from_json(kind: SettingsValueKind, value: Option<&Value>) -> Self {
        match kind {
            SettingsValueKind::Workflow | SettingsValueKind::Preset | SettingsValueKind::String => {
                Self::Text(value.and_then(Value::as_str).unwrap_or_default().to_owned())
            }
            SettingsValueKind::Boolean => {
                Self::Boolean(value.and_then(Value::as_bool).unwrap_or_default())
            }
            SettingsValueKind::NumberArray => Self::NumberArray(
                value
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
                    .filter_map(Value::as_f64)
                    .collect(),
            ),
            SettingsValueKind::BooleanArray => Self::BooleanArray(
                value
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
                    .filter_map(Value::as_bool)
                    .collect(),
            ),
            SettingsValueKind::Number
            | SettingsValueKind::TrainInputMode
            | SettingsValueKind::ModelSource
            | SettingsValueKind::ModelInput
            | SettingsValueKind::CompileMode => {
                Self::Number(value.and_then(Value::as_f64).unwrap_or_default())
            }
        }
    }

    fn as_json(&self) -> Value {
        match self {
            Self::Text(value) => Value::String(value.clone()),
            Self::Number(value) => Number::from_f64(*value)
                .map(Value::Number)
                .unwrap_or(Value::Null),
            Self::Boolean(value) => Value::Bool(*value),
            Self::NumberArray(values) => Value::Array(
                values
                    .iter()
                    .filter_map(|value| Number::from_f64(*value).map(Value::Number))
                    .collect(),
            ),
            Self::BooleanArray(values) => {
                Value::Array(values.iter().copied().map(Value::Bool).collect())
            }
        }
    }

    fn matches_json(&self, kind: SettingsValueKind, value: Option<&Value>) -> bool {
        let remote = Self::from_json(kind, value);
        match (self, remote) {
            (Self::Number(left), Self::Number(right)) => (left - right).abs() <= f64::EPSILON,
            (left, right) => *left == right,
        }
    }
}

#[derive(Debug, Clone)]
pub struct DraftEntry {
    pub path: SettingsPath,
    pub kind: SettingsValueKind,
    pub label: String,
    pub value: DraftValue,
    pub dirty: bool,
}

#[derive(Debug, Clone)]
pub struct TypedDraft {
    entries: Vec<DraftEntry>,
    initialized: bool,
}

impl TypedDraft {
    fn new(prefix: &'static str) -> Self {
        let entries = SETTINGS_PATHS
            .iter()
            .copied()
            .filter(|spec| spec.path.as_str().starts_with(prefix))
            .map(|spec| DraftEntry {
                path: spec.path,
                kind: spec.value_kind,
                label: setting_label(spec.path.as_str()),
                value: DraftValue::from_json(spec.value_kind, None),
                dirty: false,
            })
            .collect();
        Self {
            entries,
            initialized: false,
        }
    }

    pub fn entries(&self) -> &[DraftEntry] {
        &self.entries
    }

    pub fn sync(&mut self, snapshot_root: &Map<String, Value>, strip_prefix: Option<&str>) {
        for entry in &mut self.entries {
            let path = strip_prefix
                .and_then(|prefix| entry.path.as_str().strip_prefix(prefix))
                .unwrap_or_else(|| entry.path.as_str());
            let remote = value_at_path(snapshot_root, path);
            if entry.dirty && entry.value.matches_json(entry.kind, remote) {
                entry.dirty = false;
            }
            if !self.initialized || !entry.dirty {
                entry.value = DraftValue::from_json(entry.kind, remote);
            }
        }
        self.initialized = true;
    }

    pub fn is_dirty(&self) -> bool {
        self.entries.iter().any(|entry| entry.dirty)
    }

    pub fn text(&self, path: SettingsPath) -> &str {
        match self.value(path) {
            Some(DraftValue::Text(value)) => value,
            _ => "",
        }
    }

    pub fn number(&self, path: SettingsPath) -> f64 {
        match self.value(path) {
            Some(DraftValue::Number(value)) => *value,
            _ => 0.0,
        }
    }

    pub fn integer(&self, path: SettingsPath) -> i32 {
        self.number(path)
            .round()
            .clamp(i32::MIN as f64, i32::MAX as f64) as i32
    }

    pub fn boolean(&self, path: SettingsPath) -> bool {
        matches!(self.value(path), Some(DraftValue::Boolean(true)))
    }

    pub fn number_array(&self, path: SettingsPath) -> &[f64] {
        match self.value(path) {
            Some(DraftValue::NumberArray(values)) => values,
            _ => &[],
        }
    }

    pub fn boolean_array(&self, path: SettingsPath) -> &[bool] {
        let Some(DraftValue::BooleanArray(values)) = self.value(path) else {
            return &[];
        };
        values
    }

    pub fn set_text(&mut self, path: SettingsPath, value: String) {
        self.set(path, DraftValue::Text(value));
    }

    pub fn set_number(&mut self, path: SettingsPath, value: f64) {
        if value.is_finite() {
            self.set(path, DraftValue::Number(value));
        }
    }

    pub fn set_boolean(&mut self, path: SettingsPath, value: bool) {
        self.set(path, DraftValue::Boolean(value));
    }

    pub fn set_number_array(&mut self, path: SettingsPath, values: Vec<f64>) {
        self.set(path, DraftValue::NumberArray(values));
    }

    pub fn set_boolean_array(&mut self, path: SettingsPath, values: Vec<bool>) {
        self.set(path, DraftValue::BooleanArray(values));
    }

    pub fn patch(&self) -> Value {
        let mut root = Map::new();
        for entry in self.entries.iter().filter(|entry| entry.dirty) {
            insert_json_path(&mut root, entry.path.as_str(), entry.value.as_json());
        }
        Value::Object(root)
    }

    pub fn patch_for(&self, paths: &[SettingsPath]) -> Value {
        let mut root = Map::new();
        for entry in self
            .entries
            .iter()
            .filter(|entry| entry.dirty && paths.contains(&entry.path))
        {
            insert_json_path(&mut root, entry.path.as_str(), entry.value.as_json());
        }
        Value::Object(root)
    }

    fn value(&self, path: SettingsPath) -> Option<&DraftValue> {
        self.entries
            .iter()
            .find(|entry| entry.path == path)
            .map(|entry| &entry.value)
    }

    fn set(&mut self, path: SettingsPath, value: DraftValue) {
        if let Some(entry) = self.entries.iter_mut().find(|entry| entry.path == path)
            && entry.value != value
        {
            entry.value = value;
            entry.dirty = true;
        }
    }

    fn set_forced(&mut self, path: SettingsPath, value: DraftValue) {
        if let Some(entry) = self.entries.iter_mut().find(|entry| entry.path == path) {
            entry.value = value;
            entry.dirty = true;
        }
    }
}

#[derive(Debug, Clone)]
pub struct WorkflowDrafts {
    pub train: TypedDraft,
    pub validate: TypedDraft,
    pub predict: TypedDraft,
    pub live: TypedDraft,
    pub annotate: TypedDraft,
    pub export: TypedDraft,
    pub ui: TypedDraft,
}

impl Default for WorkflowDrafts {
    fn default() -> Self {
        Self {
            train: TypedDraft::new("workflows.train."),
            validate: TypedDraft::new("workflows.validate."),
            predict: TypedDraft::new("workflows.predict."),
            live: TypedDraft::new("workflows.predict."),
            annotate: TypedDraft::new("workflows.annotate."),
            export: TypedDraft::new("workflows.export."),
            ui: TypedDraft::new("ui."),
        }
    }
}

impl WorkflowDrafts {
    pub fn active(&self, workflow: Workflow) -> &TypedDraft {
        match workflow {
            Workflow::Train => &self.train,
            Workflow::Validate => &self.validate,
            Workflow::Predict => &self.predict,
            Workflow::Live => &self.live,
            Workflow::Annotate => &self.annotate,
            Workflow::Export => &self.export,
        }
    }

    pub fn active_mut(&mut self, workflow: Workflow) -> &mut TypedDraft {
        match workflow {
            Workflow::Train => &mut self.train,
            Workflow::Validate => &mut self.validate,
            Workflow::Predict => &mut self.predict,
            Workflow::Live => &mut self.live,
            Workflow::Annotate => &mut self.annotate,
            Workflow::Export => &mut self.export,
        }
    }

    pub fn sync(&mut self, snapshot: &StateSnapshot) {
        for draft in [
            &mut self.train,
            &mut self.validate,
            &mut self.predict,
            &mut self.live,
            &mut self.annotate,
            &mut self.export,
        ] {
            draft.sync(&snapshot.workflow_state, None);
        }
        self.ui.sync(&snapshot.settings_state, Some("ui."));
    }

    pub fn preset_name(&self, workflow: Workflow) -> &str {
        let (preset_path, _, _, _) = model_selection_paths(workflow);
        self.active(workflow).text(preset_path)
    }

    pub fn has_model_input(&self, workflow: Workflow) -> bool {
        let (_, _, _, input_path) = model_selection_paths(workflow);
        self.active(workflow).integer(input_path) != 3
    }

    pub fn set_preset(&mut self, workflow: Workflow, preset: String, resolution: u32) -> Value {
        let (preset_path, resolution_path, source_path, input_path) =
            model_selection_paths(workflow);
        let draft = self.active_mut(workflow);
        draft.set_forced(preset_path, DraftValue::Text(preset.clone()));
        draft.set_forced(resolution_path, DraftValue::Number(resolution.into()));
        draft.set_forced(source_path, DraftValue::Number(0.0));
        draft.set_forced(input_path, DraftValue::Number(0.0));

        let mut root = Map::new();
        insert_json_path(&mut root, preset_path.as_str(), Value::String(preset));
        insert_json_path(
            &mut root,
            resolution_path.as_str(),
            Value::Number(resolution.into()),
        );
        insert_json_path(&mut root, source_path.as_str(), Value::Number(0.into()));
        insert_json_path(&mut root, input_path.as_str(), Value::Number(0.into()));
        Value::Object(root)
    }

    pub fn uses_canonical_weights(&self, workflow: Workflow) -> bool {
        let (_, _, source_path, input_path) = model_selection_paths(workflow);
        let draft = self.active(workflow);
        draft.integer(source_path) == 0 && draft.integer(input_path) == 0
    }

    pub fn active_patch(&self, workflow: Workflow) -> Value {
        self.active(workflow).patch()
    }
}

fn model_selection_paths(
    workflow: Workflow,
) -> (SettingsPath, SettingsPath, SettingsPath, SettingsPath) {
    match workflow {
        Workflow::Train => (
            SettingsPath::WorkflowsTrainModelArtifactsPresetName,
            SettingsPath::WorkflowsTrainModelArtifactsResolution,
            SettingsPath::WorkflowsTrainModelArtifactsSource,
            SettingsPath::WorkflowsTrainModelArtifactsInput,
        ),
        Workflow::Validate => (
            SettingsPath::WorkflowsValidateModelArtifactsPresetName,
            SettingsPath::WorkflowsValidateModelArtifactsResolution,
            SettingsPath::WorkflowsValidateModelArtifactsSource,
            SettingsPath::WorkflowsValidateModelArtifactsInput,
        ),
        Workflow::Predict | Workflow::Live => (
            SettingsPath::WorkflowsPredictModelArtifactsPresetName,
            SettingsPath::WorkflowsPredictModelArtifactsResolution,
            SettingsPath::WorkflowsPredictModelArtifactsSource,
            SettingsPath::WorkflowsPredictModelArtifactsInput,
        ),
        Workflow::Annotate => (
            SettingsPath::WorkflowsAnnotateModelArtifactsPresetName,
            SettingsPath::WorkflowsAnnotateModelArtifactsResolution,
            SettingsPath::WorkflowsAnnotateModelArtifactsSource,
            SettingsPath::WorkflowsAnnotateModelArtifactsInput,
        ),
        Workflow::Export => (
            SettingsPath::WorkflowsExportModelArtifactsPresetName,
            SettingsPath::WorkflowsExportModelArtifactsResolution,
            SettingsPath::WorkflowsExportModelArtifactsSource,
            SettingsPath::WorkflowsExportModelArtifactsInput,
        ),
    }
}

#[derive(Debug, Clone)]
pub struct ShellStyle {
    pub dark_mode: bool,
    pub ui_scale: f32,
    pub accent_color: Color,
    pub primary_font_size: f32,
    pub secondary_font_size: f32,
    pub mono_font_size: f32,
    pub text_input_font_size: f32,
    pub section_spacing: f32,
    pub field_spacing: f32,
    pub card_padding: f32,
    pub control_padding_x: f32,
    pub control_padding_y: f32,
    pub checkbox_size: f32,
}

impl WorkflowDrafts {
    pub fn shell_style(&self) -> ShellStyle {
        let dark_mode = self.ui.boolean(SettingsPath::UiDarkMode);
        let ui_scale = self.ui.number(SettingsPath::UiUiScale).clamp(0.85, 1.75) as f32;
        let accent_color = self
            .ui
            .text(SettingsPath::UiAccentColor)
            .parse::<Color>()
            .map(opaque)
            .unwrap_or_else(|_| Color::from_rgb8(0x2a, 0xc3, 0xde));
        let primary_font_size = self.ui.number(SettingsPath::UiFontSize);
        let secondary_font_size = self.ui.number(SettingsPath::UiSecondaryFontSize);
        let mono_font_size = self.ui.number(SettingsPath::UiMonoFontSize);
        let text_input_font_size = self.ui.number(SettingsPath::UiTextInputFontSize);
        let density = self.ui.integer(SettingsPath::UiDensity).clamp(0, 2);
        let (
            section_spacing,
            field_spacing,
            card_padding,
            control_padding_x,
            control_padding_y,
            checkbox_size,
        ) = match density {
            0 => (8.0, 3.0, 6.0, 6.0, 3.0, 14.0),
            2 => (14.0, 6.0, 14.0, 10.0, 7.0, 18.0),
            _ => (10.0, 4.0, 10.0, 8.0, 5.0, 16.0),
        };
        ShellStyle {
            dark_mode,
            ui_scale: if ui_scale == 0.0 { 1.0 } else { ui_scale },
            accent_color,
            primary_font_size: if primary_font_size == 0.0 {
                14.0
            } else {
                primary_font_size.clamp(10.0, 32.0) as f32
            },
            secondary_font_size: if secondary_font_size == 0.0 {
                12.0
            } else {
                secondary_font_size.clamp(9.0, 28.0) as f32
            },
            mono_font_size: if mono_font_size == 0.0 {
                12.0
            } else {
                mono_font_size.clamp(9.0, 28.0) as f32
            },
            text_input_font_size: if text_input_font_size == 0.0 {
                13.0
            } else {
                text_input_font_size.clamp(9.0, 31.0) as f32
            },
            section_spacing,
            field_spacing,
            card_padding,
            control_padding_x,
            control_padding_y,
            checkbox_size,
        }
    }
}

impl fmt::Display for RfDetrPresetOption {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{} [{}]", self.display_name, self.size_label)
    }
}

pub fn preset_by_name(name: &str) -> Option<&'static RfDetrPresetOption> {
    RF_DETR_PRESET_OPTIONS
        .iter()
        .find(|preset| preset.preset_name == name)
}

pub fn compatible_presets(resolution: Option<u32>, output: &mut Vec<RfDetrPresetOption>) {
    output.clear();
    output.extend(
        RF_DETR_PRESET_OPTIONS
            .iter()
            .filter(|preset| resolution.is_none_or(|value| preset.resolution == value))
            .copied(),
    );
}

pub const fn settings_workflow(workflow: Workflow) -> &'static str {
    match workflow {
        Workflow::Train => "train",
        Workflow::Validate => "validate",
        Workflow::Predict | Workflow::Live => "predict",
        Workflow::Annotate => "annotate",
        Workflow::Export => "export",
    }
}

pub const fn source_kind_value(kind: SourceKind) -> u8 {
    match kind {
        SourceKind::CompiledDataset => 0,
        SourceKind::SingleImage => 1,
        SourceKind::ImageFolder => 2,
        SourceKind::VideoStream => 3,
    }
}

pub const fn source_kind_label(kind: SourceKind) -> &'static str {
    match kind {
        SourceKind::CompiledDataset => "Compiled dataset",
        SourceKind::SingleImage => "Single image",
        SourceKind::ImageFolder => "Image folder",
        SourceKind::VideoStream => "Video stream",
    }
}

const fn opaque(color: Color) -> Color {
    Color { a: 1.0, ..color }
}

fn value_at_path<'a>(root: &'a Map<String, Value>, path: &str) -> Option<&'a Value> {
    let mut parts = path.split('.');
    let mut value = root.get(parts.next()?);
    for part in parts {
        value = value
            .and_then(Value::as_object)
            .and_then(|object| object.get(part));
    }
    value
}

fn insert_json_path(root: &mut Map<String, Value>, path: &str, value: Value) {
    let mut parts = path.split('.').peekable();
    let mut current = root;
    while let Some(part) = parts.next() {
        if parts.peek().is_none() {
            current.insert(part.to_owned(), value);
            return;
        }
        let child = current
            .entry(part.to_owned())
            .or_insert_with(|| Value::Object(Map::new()));
        if !child.is_object() {
            *child = Value::Object(Map::new());
        }
        current = child
            .as_object_mut()
            .expect("typed settings patch branch is an object");
    }
}

fn setting_label(path: &str) -> String {
    let leaf = path.rsplit('.').next().unwrap_or(path);
    let mut label = String::with_capacity(leaf.len());
    let mut capitalize = true;
    for character in leaf.chars() {
        if character == '_' {
            label.push(' ');
            capitalize = true;
        } else if capitalize {
            label.extend(character.to_uppercase());
            capitalize = false;
        } else {
            label.push(character);
        }
    }
    label
}
