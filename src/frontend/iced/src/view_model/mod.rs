mod annotation;
mod availability;
mod explore;
mod model_selection;
mod pending;
mod presentation;
mod reduction;
mod settings;
#[cfg(test)]
pub(crate) mod test_support;
mod workflow;

#[cfg(test)]
pub(crate) use reduction::invalid_visual_frame;

pub use crate::generated::ApplicationIntentEndpoint;
pub use annotation::AnnotationModel;
pub use explore::{ExploreModel, ExplorePresentationState};
pub(crate) use presentation::Reconciliation as PresentationReconciliation;
pub(crate) use model_selection::{ModelSettingsProjection, model_settings_projection};
use reduction::{
    Observation, merge_compute_snapshot, merge_dialog_snapshot, merge_live_snapshot,
    merge_model_snapshot, merge_observation, merge_presentation_snapshot,
};
pub use workflow::{ModelSelectionReceipt, WorkflowModel};

use std::collections::BTreeMap;

use crate::generated::{
    ApplicationErrorCategory, ApplicationEvent, ApplicationReply, ApplicationSnapshot,
    ComputeOperationOutcome, ComputeUiState, FeatureId, FileDialogFact, FileDialogSnapshot,
    FileDialogTarget, GuiSettingsState, LiveSnapshot, ModelSelection, ModelSelectionOutcome,
    ModelSelectionSource, ModelUiState, PredictSnapshot, PresentationSnapshot,
    PresentationSourceIdentity, PresentationSourceKind, SettingsUiState, UpscaleSnapshot,
    VisualExtent, VisualFrame,
};
use crate::protocol::ApplicationError;

const MAX_PENDING_INTENTS: usize = 64;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnectionState {
    Connecting,
    AwaitingBootstrap,
    Connected,
    Reconnecting,
}

impl ConnectionState {
    pub const fn label(self) -> &'static str {
        match self {
            Self::Connecting => "Connecting",
            Self::AwaitingBootstrap => "Awaiting bootstrap",
            Self::Connected => "Connected",
            Self::Reconnecting => "Reconnecting",
        }
    }
}

#[derive(Debug, Clone)]
struct PendingRequest {
    endpoint: ApplicationIntentEndpoint,
    detail: PendingDetail,
}

#[derive(Debug, Clone)]
enum PendingDetail {
    None,
    FileDialog(FileDialogTarget),
    ModelSelect(ModelSelectionReceipt),
    ModelStop(FeatureId),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UiErrorKind {
    InvalidIntent,
    Busy,
    Unavailable,
    Failed,
    Transport,
    Protocol,
    Presentation,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UiError {
    pub kind: UiErrorKind,
    pub title: &'static str,
    pub detail: String,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Typography {
    pub primary: f32,
    pub secondary: f32,
    pub monospace: f32,
    pub text_input: f32,
}

impl UiError {
    pub fn invalid(detail: impl Into<String>) -> Self {
        Self {
            kind: UiErrorKind::InvalidIntent,
            title: "Request unavailable",
            detail: detail.into(),
        }
    }

    pub fn busy(detail: impl Into<String>) -> Self {
        Self {
            kind: UiErrorKind::Busy,
            title: "Operation already running",
            detail: detail.into(),
        }
    }

    pub fn transport(detail: impl Into<String>) -> Self {
        Self {
            kind: UiErrorKind::Transport,
            title: "Connection interrupted",
            detail: detail.into(),
        }
    }

    pub fn protocol(detail: impl Into<String>) -> Self {
        Self {
            kind: UiErrorKind::Protocol,
            title: "Protocol error",
            detail: detail.into(),
        }
    }

    pub fn presentation(detail: impl Into<String>) -> Self {
        Self {
            kind: UiErrorKind::Presentation,
            title: "Presentation unavailable",
            detail: detail.into(),
        }
    }
}

impl From<ApplicationError> for UiError {
    fn from(error: ApplicationError) -> Self {
        let (kind, title) = match error.category {
            ApplicationErrorCategory::InvalidIntent => {
                (UiErrorKind::InvalidIntent, "Request rejected")
            }
            ApplicationErrorCategory::Busy => (UiErrorKind::Busy, "Operation already running"),
            ApplicationErrorCategory::Unavailable => {
                (UiErrorKind::Unavailable, "Service unavailable")
            }
            ApplicationErrorCategory::Failed => (UiErrorKind::Failed, "Operation failed"),
        };
        Self {
            kind,
            title,
            detail: error.detail,
        }
    }
}

#[derive(Debug, Clone)]
pub struct ApplicationModel {
    pub connection: ConnectionState,
    pub settings_snapshot: Option<SettingsUiState>,
    pub file_dialog: Option<FileDialogSnapshot>,
    pub presentation: Option<PresentationSnapshot>,
    pub model_snapshot: Option<ModelUiState>,
    pub live_snapshot: Option<LiveSnapshot>,
    pub predict_snapshot: Option<PredictSnapshot>,
    pub upscale_snapshot: Option<UpscaleSnapshot>,
    pub workflow: WorkflowModel,
    pub explore: ExploreModel,
    pub annotation: AnnotationModel,
    pub error: Option<UiError>,
    pub window_width: u32,
    pub window_height: u32,
    pub scale_factor: f64,
    dialog_context: Option<DialogContext>,
    pending: BTreeMap<u64, PendingRequest>,
    next_correlation: u64,
    presentation_model: presentation::PresentationModel,
}

#[derive(Debug, Clone)]
pub struct DialogContext {
    pub target: FileDialogTarget,
    pub title: &'static str,
}

fn dialog_target_stable_id(target: &FileDialogTarget) -> u64 {
    match target {
        FileDialogTarget::SettingsFieldTarget(value) => value.stableid,
        FileDialogTarget::ModelArtifactTarget(value) => value.stableid,
    }
}

impl Default for ApplicationModel {
    fn default() -> Self {
        Self {
            connection: ConnectionState::Connecting,
            settings_snapshot: None,
            file_dialog: None,
            presentation: None,
            model_snapshot: None,
            live_snapshot: None,
            predict_snapshot: None,
            upscale_snapshot: None,
            workflow: WorkflowModel::default(),
            explore: ExploreModel::default(),
            annotation: AnnotationModel::default(),
            error: None,
            window_width: 0,
            window_height: 0,
            scale_factor: 1.0,
            dialog_context: None,
            pending: BTreeMap::new(),
            next_correlation: 1,
            presentation_model: presentation::PresentationModel::default(),
        }
    }
}

impl ApplicationModel {
    pub fn typography(&self) -> Typography {
        let ui = self
            .settings_snapshot
            .as_ref()
            .map(|snapshot| &snapshot.settingsstate.ui);
        Typography {
            primary: ui.map_or(14.0, |value| value.fontsize),
            secondary: ui.map_or(12.0, |value| value.secondaryfontsize),
            monospace: ui.map_or(12.0, |value| value.monofontsize),
            text_input: ui.map_or(13.0, |value| value.textinputfontsize),
        }
    }
}
