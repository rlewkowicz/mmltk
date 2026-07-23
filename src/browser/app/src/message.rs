use crate::generated::{
    FileDialogId, HOST_API_PROTOCOL_VERSION, IntentId, IntentMessage, SettingsPath, SourceKind,
    Workflow,
};
use crate::transport::TransportEvent;
use iced::Color;
use serde_json::{Value, json};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Tool {
    Select,
    Direct,
    Box,
    Spline,
    Paint,
    Erase,
    Fill,
    Point,
    Skeleton,
    ColorSample,
}

const TOOL_IDS: [&str; 10] = [
    "select",
    "direct",
    "box",
    "spline",
    "paint",
    "erase",
    "fill",
    "point",
    "skeleton",
    "mask.color_sample",
];

impl Tool {
    pub const fn id(self) -> &'static str {
        TOOL_IDS[self as usize]
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum WorkspaceAspectRatio {
    Widescreen,
    Portrait,
    Standard,
    Photo,
    Square,
}

const WORKSPACE_ASPECT_RATIO_LABELS: [&str; 5] = ["16:9", "9:16", "4:3", "3:2", "1:1"];

impl WorkspaceAspectRatio {
    pub const ALL: [Self; 5] = [
        Self::Widescreen,
        Self::Portrait,
        Self::Standard,
        Self::Photo,
        Self::Square,
    ];

    pub const fn label(self) -> &'static str {
        WORKSPACE_ASPECT_RATIO_LABELS[self as usize]
    }

    pub const fn height_factor(self) -> f32 {
        match self {
            Self::Widescreen => 9.0 / 16.0,
            Self::Portrait => 16.0 / 9.0,
            Self::Standard => 3.0 / 4.0,
            Self::Photo => 2.0 / 3.0,
            Self::Square => 1.0,
        }
    }

    pub const fn index(self) -> i32 {
        self as i32
    }

    pub const fn from_index(index: i32) -> Self {
        match index {
            1 => Self::Portrait,
            2 => Self::Standard,
            3 => Self::Photo,
            4 => Self::Square,
            _ => Self::Widescreen,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LivePreviewControl {
    FitToCapture,
    FullFrameDisplay,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MaskColorRange {
    Sup,
    Nosup,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MaskColorField {
    HueDegrees,
    Saturation,
    Value,
    HueMinus,
    HuePlus,
    SaturationMinus,
    SaturationPlus,
    ValueMinus,
    ValuePlus,
}

#[derive(Debug, Clone)]
pub struct HostIntent {
    pub id: IntentId,
    pub payload: Value,
}

impl HostIntent {
    pub fn empty(id: IntentId) -> Self {
        Self {
            id,
            payload: json!({}),
        }
    }

    pub fn with_payload(id: IntentId, payload: Value) -> Self {
        Self { id, payload }
    }

    pub fn into_wire(self, request_id: u64, workflow: Workflow) -> IntentMessage {
        IntentMessage {
            message_type: "intent".into(),
            protocol_version: HOST_API_PROTOCOL_VERSION,
            request_id,
            workflow,
            intent: self.id.as_str().into(),
            payload: self.payload,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Action {
    Primary,
    ApplyShellSettings,
    CompileDataset,
    CancelDatasetCompile,
    RefreshLocalGpus,
    QueryRemoteOffers,
    ClearRemoteOffer,
    SaveAnnotation,
    ToggleLiveAnnotation,
    SetupPrevious,
    SetupReload,
    SetupNext,
    ToggleHoldSave,
}

#[derive(Debug, Clone)]
pub enum Message {
    Transport(TransportEvent),
    SelectWorkflow(Workflow),
    SetSourceKind(SourceKind),
    EditText(SettingsPath, String),
    SetNumber(SettingsPath, f64),
    SetBoolean(SettingsPath, bool),
    SetNumericChoice(SettingsPath, i32),
    SetPreset(String),
    ToggleLocalGpu(i32, bool),
    ToggleRemoteFamily(usize, bool),
    ArmRemoteOffer(i32),
    EditAnnotationCategory(String),
    AddAnnotationCategory,
    SetMaskCleanupRadius(u16),
    SetMaskColorNumber(MaskColorRange, MaskColorField, f64),
    SetMaskColorSampling(MaskColorRange, bool),
    ResetMaskColorRange(MaskColorRange),
    AnnotationSidebar(&'static str, Value),
    SetBrushRadius(u16),
    OpenAccentColorPicker,
    CancelAccentColorPicker,
    SetAccentColor(Color),
    SetFitToCapture(bool),
    SetFullFrameDisplay(bool),
    SelectTool(Tool),
    SetWorkspaceAspectRatio(WorkspaceAspectRatio),
    WorkspacePointer {
        phase: &'static str,
        canvas_x: f64,
        canvas_y: f64,
        capture_x: i32,
        capture_y: i32,
        object_index: Option<u32>,
        drag_kind: Option<&'static str>,
        handle_element_index: Option<u32>,
        handle_role: Option<String>,
    },
    PersistUiSetting(SettingsPath),
    ToggleSettings,
    Browse(FileDialogId),
    ChooseCustomWeights,
    ConfirmCustomWeights,
    CancelCustomWeights,
    RequestSettingsReset,
    ConfirmSettingsReset,
    CancelSettingsReset,
    DismissError,
    CopyError,
    CopyTextSelection(String),
    CutTextSelection(String),
    PasteTextSelection(String),
    SelectAllText(String),
    TextSelectionResolved(String, Option<String>),
    TextCutResolved(
        String,
        Option<iced::advanced::widget::operation::text_input::TextCut>,
    ),
    ClipboardTextRead(String, Result<String, String>),
    TextPasteResolved(String, Option<String>),
    ClipboardWriteResolved(&'static str, String, usize, Option<String>),
    ClipboardRuntimeResolved(&'static str, String, usize, Option<String>),
    WorkflowPersistDebounced(Workflow, u64),
    FieldInteracted(SettingsPath),
    Run(Action),
}
