use crate::generated::{
    ArtifactOperationPhase, CustomModelArtifactKind, FileDialogId, FileDialogResultStatus,
    HOST_API_CONTRACT_HASH, HOST_API_PROTOCOL_VERSION, IntentId, ModelPreflightStatus,
    RfDetrPresetOption, SETTINGS_PATHS, SettingsPath, SettingsValueKind, SourceKind, StateSnapshot,
    WeightArtifactStatus, Workflow, WorkspacePresent,
};
use crate::message::{
    Action, HostIntent, LivePreviewControl, MaskColorField, MaskColorRange, Message, Tool,
    WorkspaceAspectRatio,
};
use crate::model::{
    BridgeState, CustomModelConfirmation, TransportStatus, WorkflowDrafts, compatible_presets,
    preset_by_name, settings_workflow, source_kind_value,
};
use crate::transport::{Connection, TransportConfig, TransportEvent};
use iced::{Color, Subscription, Task, Theme, theme::palette};
use serde_json::{Map, Value, json};

struct PendingWeightRequest {
    preset_name: String,
    after_generation: u64,
}

#[derive(Debug, Clone)]
pub struct UiError {
    pub title: String,
    pub context: String,
    pub message: String,
}

#[derive(Debug, Clone)]
struct CompileDatasetRequest {
    source_dir: String,
    output_dir: String,
    preset_name: String,
    resolution: u32,
    overwrite: bool,
}

#[derive(Debug, Clone)]
enum DeferredAction {
    CompileDataset(CompileDatasetRequest),
    Primary(Workflow),
    FileDialog {
        workflow: Workflow,
        token: u64,
        payload: Value,
    },
}

#[derive(Debug, Clone)]
struct PendingSettingsAction {
    patch: Value,
    action: DeferredAction,
}

struct PrimaryIssue {
    context: &'static str,
    message: String,
    field: Option<SettingsPath>,
}

const WORKFLOW_PERSIST_DEBOUNCE_MS: u32 = 450;
const WORKFLOW_COUNT: usize = 6;

pub struct App {
    pub config: TransportConfig,
    pub snapshot: StateSnapshot,
    pub workspace_present: Option<WorkspacePresent>,
    pub bridge: BridgeState,
    pub transport_status: TransportStatus,
    pub transport_error: String,
    pub selected_workflow: Workflow,
    pub selected_tool: Tool,
    pub workspace_aspect_ratio: WorkspaceAspectRatio,
    pub settings_open: bool,
    pub accent_picker_open: bool,
    pub drafts: WorkflowDrafts,
    pub compatible_presets: Vec<RfDetrPresetOption>,
    pub theme: Theme,
    pub annotation_live: bool,
    pub hold_save: bool,
    pub brush_radius: u16,
    pub mask_cleanup_radius: u16,
    pub new_annotation_category: String,
    pub mask_sup: Value,
    pub mask_nosup: Value,
    pub fit_to_capture: bool,
    pub full_frame_display: bool,
    pub custom_model_confirmation: Option<CustomModelConfirmation>,
    pub settings_reset_confirmation: bool,
    pub active_error: Option<UiError>,
    attention_field: Option<SettingsPath>,
    connection: Option<Connection>,
    request_id: u64,
    file_dialog_token: u64,
    pending_file_dialog_token: Option<u64>,
    pending_custom_dialog: Option<(u64, Workflow, CustomModelArtifactKind, String, u32)>,
    pending_weight_request: Option<PendingWeightRequest>,
    pending_settings_action: Option<PendingSettingsAction>,
    settings_reset_pending: bool,
    weight_request_error: String,
    last_error_key: String,
    dataset_error_modal_armed: bool,
    weight_error_modal_armed: bool,
    applied_font_size: f32,
    received_snapshot: bool,
    workflow_persist_generations: [u64; WORKFLOW_COUNT],
}

pub fn boot() -> (App, Task<Message>) {
    let config = TransportConfig::from_page();
    let mut presets = Vec::with_capacity(crate::generated::RF_DETR_PRESET_OPTIONS.len());
    compatible_presets(None, &mut presets);
    let accent = Color::from_rgb8(0x2a, 0xc3, 0xde);
    let transport_error = if config.websocket_url.is_empty() {
        "browser page is missing mmltk_ws_url".into()
    } else {
        String::new()
    };
    (
        App {
            config,
            snapshot: StateSnapshot::default(),
            workspace_present: None,
            bridge: BridgeState::default(),
            transport_status: TransportStatus::Connecting,
            transport_error,
            selected_workflow: Workflow::Train,
            selected_tool: Tool::Select,
            workspace_aspect_ratio: WorkspaceAspectRatio::Widescreen,
            settings_open: false,
            accent_picker_open: false,
            drafts: WorkflowDrafts::default(),
            compatible_presets: presets,
            theme: app_theme(true, accent),
            annotation_live: false,
            hold_save: false,
            brush_radius: 12,
            mask_cleanup_radius: 2,
            new_annotation_category: String::new(),
            mask_sup: default_mask_color_range(),
            mask_nosup: default_mask_color_range(),
            fit_to_capture: true,
            full_frame_display: false,
            custom_model_confirmation: None,
            settings_reset_confirmation: false,
            active_error: None,
            attention_field: None,
            connection: None,
            request_id: 0,
            file_dialog_token: 0,
            pending_file_dialog_token: None,
            pending_custom_dialog: None,
            pending_weight_request: None,
            pending_settings_action: None,
            settings_reset_pending: false,
            weight_request_error: String::new(),
            last_error_key: String::new(),
            dataset_error_modal_armed: false,
            weight_error_modal_armed: false,
            applied_font_size: 14.0,
            received_snapshot: false,
            workflow_persist_generations: [0; WORKFLOW_COUNT],
        },
        Task::none(),
    )
}

pub fn subscription(app: &App) -> Subscription<Message> {
    Subscription::batch([
        crate::transport::subscription(app.config.clone()).map(Message::Transport),
        iced::event::listen_with(clipboard_runtime_event),
    ])
}

pub fn update(app: &mut App, message: Message) -> Task<Message> {
    if app.config.trace_enabled
        && let Some((event, fields)) = trace_message(&message)
    {
        app.trace_ui(event, || fields);
    }
    match message {
        Message::Transport(event) => return app.on_transport(event),
        Message::SelectWorkflow(workflow) => {
            app.selected_workflow = workflow;
            app.dispatch(
                workflow,
                HostIntent::with_payload(
                    IntentId::SettingsUpdate,
                    json!({"patch": {"current_view": workflow}}),
                ),
            );
        }
        Message::SetSourceKind(kind) => {
            if let Some(path) = app.source_kind_path() {
                app.drafts
                    .active_mut(app.selected_workflow)
                    .set_number(path, source_kind_value(kind).into());
                return app.schedule_workflow_persist(app.selected_workflow);
            }
        }
        Message::EditText(path, value) => {
            app.edit_text(path, value);
            return app.schedule_path_persist(path);
        }
        Message::SetNumber(path, value) => {
            app.edit_number(path, value);
            return app.schedule_path_persist(path);
        }
        Message::SetBoolean(path, value) => {
            app.edit_boolean(path, value);
            return app.schedule_path_persist(path);
        }
        Message::SetNumericChoice(path, value) => {
            app.edit_number(path, value.into());
            return app.schedule_path_persist(path);
        }
        Message::SetPreset(preset) => {
            if let Some(option) = preset_by_name(&preset) {
                let workflow = app.selected_workflow;
                let patch = app
                    .drafts
                    .set_preset(workflow, preset.clone(), option.resolution);
                if app.try_dispatch(
                    workflow,
                    HostIntent::with_payload(IntentId::SettingsUpdate, json!({"patch": patch})),
                ) {
                    app.begin_canonical_weight_verification(preset);
                } else {
                    app.weight_request_error = app.transport_error.clone();
                }
            }
        }
        Message::ToggleLocalGpu(device_id, selected) => {
            app.toggle_local_gpu(device_id, selected);
            return app.schedule_workflow_persist(Workflow::Train);
        }
        Message::ToggleRemoteFamily(index, enabled) => {
            app.toggle_remote_family(index, enabled);
            return app.schedule_workflow_persist(Workflow::Train);
        }
        Message::ArmRemoteOffer(offer_id) => app.dispatch(
            Workflow::Train,
            HostIntent::with_payload(IntentId::TrainRemoteOfferArm, json!({"offer_id": offer_id})),
        ),
        Message::EditAnnotationCategory(value) => app.new_annotation_category = value,
        Message::AddAnnotationCategory => {
            let category = app.new_annotation_category.trim().to_owned();
            if !category.is_empty() {
                app.new_annotation_category.clear();
                app.dispatch(
                    Workflow::Annotate,
                    HostIntent::with_payload(
                        IntentId::AnnotateSidebar,
                        json!({"action": "add_category", "category_name": category}),
                    ),
                );
            }
        }
        Message::SetMaskCleanupRadius(radius) => {
            app.mask_cleanup_radius = radius.clamp(1, 32);
            app.drafts.ui.set_number(
                SettingsPath::UiMaskCleanupRadius,
                app.mask_cleanup_radius.into(),
            );
        }
        Message::SetMaskColorNumber(range, field, value) => {
            app.set_mask_color_number(range, field, value);
        }
        Message::SetMaskColorSampling(range, enabled) => {
            app.set_mask_color_sampling(range, enabled);
        }
        Message::ResetMaskColorRange(range) => {
            match range {
                MaskColorRange::Sup => app.mask_sup = default_mask_color_range(),
                MaskColorRange::Nosup => app.mask_nosup = default_mask_color_range(),
            }
            app.dispatch_mask_color_ranges();
        }
        Message::AnnotationSidebar(action, payload) => {
            let mut payload = payload.as_object().cloned().unwrap_or_default();
            payload.insert("action".into(), Value::String(action.into()));
            app.dispatch(
                Workflow::Annotate,
                HostIntent::with_payload(IntentId::AnnotateSidebar, Value::Object(payload)),
            );
        }
        Message::SetBrushRadius(radius) => {
            app.brush_radius = radius.clamp(1, 128);
            app.drafts.ui.set_number(
                SettingsPath::UiAnnotationBrushRadius,
                app.brush_radius.into(),
            );
            app.dispatch(
                Workflow::Annotate,
                HostIntent::with_payload(
                    IntentId::AnnotateBrushRadius,
                    json!({"radius": app.brush_radius}),
                ),
            );
        }
        Message::OpenAccentColorPicker => app.accent_picker_open = true,
        Message::CancelAccentColorPicker => app.accent_picker_open = false,
        Message::SetAccentColor(color) => {
            let color = Color { a: 1.0, ..color };
            app.drafts
                .ui
                .set_text(SettingsPath::UiAccentColor, color.to_string());
            app.accent_picker_open = false;
            app.refresh_theme();
        }
        Message::SetFitToCapture(enabled) => {
            app.fit_to_capture = enabled;
            app.dispatch_live_preview(LivePreviewControl::FitToCapture, enabled);
        }
        Message::SetFullFrameDisplay(enabled) => {
            app.full_frame_display = enabled;
            app.dispatch_live_preview(LivePreviewControl::FullFrameDisplay, enabled);
        }
        Message::SelectTool(tool) => {
            app.selected_tool = tool;
            if app.selected_workflow == Workflow::Annotate && tool != Tool::ColorSample {
                app.dispatch(
                    app.selected_workflow,
                    HostIntent::with_payload(IntentId::ToolSelect, json!({"tool": tool.id()})),
                );
            }
        }
        Message::SetWorkspaceAspectRatio(aspect_ratio) => {
            app.workspace_aspect_ratio = aspect_ratio;
            app.drafts.ui.set_number(
                SettingsPath::UiWorkspaceAspectRatio,
                aspect_ratio.index().into(),
            );
            app.persist_ui_setting(SettingsPath::UiWorkspaceAspectRatio);
        }
        Message::WorkspacePointer {
            phase,
            canvas_x,
            canvas_y,
            capture_x,
            capture_y,
            object_index,
            drag_kind,
            handle_element_index,
            handle_role,
        } => {
            if app.selected_workflow == Workflow::Annotate {
                let mut payload = json!({
                    "phase": phase,
                    "pointer_id": 0,
                    "button": 0,
                    "buttons": if matches!(phase, "end" | "cancel" | "hover") { 0 } else { 1 },
                    "canvas_x": canvas_x,
                    "canvas_y": canvas_y,
                    "capture_x": capture_x,
                    "capture_y": capture_y,
                    "tool": app.selected_tool.id(),
                    "brush_radius": app.brush_radius,
                    "erase": app.selected_tool == Tool::Erase,
                });
                if let Some(object_index) = object_index {
                    payload["object_index"] = json!(object_index);
                }
                if let Some(drag_kind) = drag_kind {
                    payload["drag_kind"] = json!(drag_kind);
                }
                if let (Some(element_index), Some(role), Some(object_index)) =
                    (handle_element_index, handle_role, object_index)
                {
                    payload["handle"] = json!({
                        "object_index": object_index,
                        "element_index": element_index,
                        "role": role,
                    });
                }
                app.dispatch(
                    Workflow::Annotate,
                    HostIntent::with_payload(IntentId::AnnotateWorkspacePointer, payload),
                );
            }
        }
        Message::PersistUiSetting(path) => app.persist_ui_setting(path),
        Message::ToggleSettings => {
            app.settings_open = !app.settings_open;
            if !app.settings_open {
                app.accent_picker_open = false;
            }
        }
        Message::Browse(dialog_id) => {
            if let Some(path) = attention_path_for_dialog(dialog_id) {
                app.clear_attention(path);
            }
            app.open_file_dialog(dialog_id);
        }
        Message::ChooseCustomWeights => app.open_custom_weights_dialog(),
        Message::ConfirmCustomWeights => app.confirm_custom_weights(),
        Message::CancelCustomWeights => app.cancel_custom_weights(),
        Message::RequestSettingsReset => app.settings_reset_confirmation = true,
        Message::ConfirmSettingsReset => {
            app.settings_reset_confirmation = false;
            app.settings_reset_pending = true;
            app.dispatch(
                app.selected_workflow,
                HostIntent::with_payload(IntentId::SettingsReset, json!({"confirmed": true})),
            );
        }
        Message::CancelSettingsReset => app.settings_reset_confirmation = false,
        Message::DismissError => {
            app.active_error = None;
            app.last_error_key.clear();
        }
        Message::CopyError => {
            if let Some(error) = &app.active_error {
                let text = format!("{}\n{}\n{}", error.title, error.context, error.message);
                return clipboard_write_task("copy", "error.modal".to_owned(), text);
            }
        }
        Message::CopyTextSelection(id) => {
            if let Some(path) = settings_path_for_input(&id) {
                app.clear_attention(path);
            }
            return text_input_operation_task(
                id,
                iced::advanced::widget::operation::text_input::selected_text,
                Message::TextSelectionResolved,
            );
        }
        Message::CutTextSelection(id) => {
            if let Some(path) = settings_path_for_input(&id) {
                app.clear_attention(path);
            }
            return text_input_operation_task(
                id,
                iced::advanced::widget::operation::text_input::cut_text,
                Message::TextCutResolved,
            );
        }
        Message::PasteTextSelection(id) => {
            if let Some(path) = settings_path_for_input(&id) {
                app.clear_attention(path);
            }
            return clipboard_read_task(id);
        }
        Message::SelectAllText(id) => {
            if let Some(path) = settings_path_for_input(&id) {
                app.clear_attention(path);
            }
            return iced::widget::operation::select_all(iced::advanced::widget::Id::from(id));
        }
        Message::TextSelectionResolved(id, selection) => {
            if let Some(selection) = selection.filter(|selection| !selection.is_empty()) {
                return clipboard_write_task("copy", id, selection);
            }
        }
        Message::TextCutResolved(id, Some(cut)) => {
            if cut.selection.is_empty() {
                return Task::none();
            }
            let write = clipboard_write_task("cut", id.clone(), cut.selection);
            if let Some(message) = pasted_value_message(&id, cut.value) {
                return Task::batch([write, Task::done(message)]);
            }
            app.present_error(
                "Cannot cut",
                id,
                "Removing the selection would not leave a valid value for this field.",
            );
            return write;
        }
        Message::TextCutResolved(_, None) => {}
        Message::ClipboardTextRead(id, Ok(text)) => {
            return paste_text_selection_task(id, text);
        }
        Message::ClipboardTextRead(id, Err(error)) => {
            app.present_error("Cannot paste", id, error);
        }
        Message::TextPasteResolved(id, Some(value)) => {
            if let Some(message) = pasted_value_message(&id, value) {
                return Task::done(message);
            }
            app.present_error(
                "Cannot paste",
                id,
                "The pasted value is not valid for this field.",
            );
        }
        Message::TextPasteResolved(id, None) => {
            app.present_error("Cannot paste", id, "The input is no longer available.");
        }
        Message::ClipboardWriteResolved(_, target, _, Some(error)) => {
            app.present_error("Cannot copy", target, error);
        }
        Message::ClipboardWriteResolved(_, _, _, None) => {}
        Message::ClipboardRuntimeResolved(operation, target, _, Some(error)) => {
            let title = if operation == "read" {
                "Cannot paste"
            } else {
                "Cannot copy"
            };
            app.present_error(title, target, error);
        }
        Message::ClipboardRuntimeResolved(_, _, _, None) => {}
        Message::WorkflowPersistDebounced(workflow, generation) => {
            return app.persist_workflow_after_debounce(workflow, generation);
        }
        Message::FieldInteracted(path) => app.clear_attention(path),
        Message::Run(action) => app.run_action(action),
    }
    Task::none()
}

impl App {
    pub fn ready(&self) -> bool {
        self.connection.is_some() && self.bridge.connected
    }

    pub fn ui_scale(&self) -> f32 {
        self.drafts.shell_style().ui_scale
    }

    pub fn active_preset(&self) -> Option<&'static RfDetrPresetOption> {
        self.preset_for(self.selected_workflow)
    }

    pub fn preset_for(&self, workflow: Workflow) -> Option<&'static RfDetrPresetOption> {
        preset_by_name(self.drafts.preset_name(workflow))
    }

    pub fn source_kind(&self) -> SourceKind {
        let Some(path) = self.source_kind_path() else {
            return SourceKind::CompiledDataset;
        };
        match self.drafts.active(self.selected_workflow).integer(path) {
            1 => SourceKind::SingleImage,
            2 => SourceKind::ImageFolder,
            3 => SourceKind::VideoStream,
            _ => SourceKind::CompiledDataset,
        }
    }

    pub fn primary_label(&self) -> &'static str {
        match self.selected_workflow {
            Workflow::Train if self.train_running() => "Stop Training",
            Workflow::Train if self.remote_train_running() => "Stop Remote Training",
            Workflow::Train if self.train_execution_target() == 1 => "Start Remote Training",
            Workflow::Train => "Start Training",
            Workflow::Validate => "Run Validation",
            Workflow::Predict if self.snapshot.job.running => "Stop Prediction",
            Workflow::Predict => "Run Prediction",
            Workflow::Live if self.live_predict_active() => "Stop Live Prediction",
            Workflow::Live => "Start Live Prediction",
            Workflow::Annotate => "Save Annotations",
            Workflow::Export => "Export Model",
        }
    }

    fn primary_issue(&self, workflow: Workflow) -> Option<PrimaryIssue> {
        if !self.ready() {
            return Some(primary_issue(
                "Native bridge",
                "Native bridge is not ready.",
                None,
            ));
        }
        if self.train_running()
            || self.remote_train_running()
            || (workflow == Workflow::Predict && self.snapshot.job.running)
            || (workflow == Workflow::Live && self.live_predict_active())
        {
            return None;
        }
        if self.snapshot.job.running {
            return Some(primary_issue(
                "Training runtime",
                "Another job is running.",
                None,
            ));
        }
        let model_workflow = if workflow == Workflow::Live {
            Workflow::Predict
        } else {
            workflow
        };
        let preflight = &self.snapshot.model_preflight;
        if preflight.workflow == model_workflow && preflight.status != ModelPreflightStatus::Idle {
            match preflight.status {
                ModelPreflightStatus::Ready => {}
                ModelPreflightStatus::Verifying => {
                    return Some(primary_issue(
                        "Model weights",
                        "Custom weights are being verified.",
                        None,
                    ));
                }
                ModelPreflightStatus::Incompatible | ModelPreflightStatus::Failed => {
                    let message = if preflight.error.is_empty() {
                        "Custom weights are incompatible with the selected RF-DETR architecture."
                            .into()
                    } else {
                        preflight.error.clone()
                    };
                    return Some(primary_issue("Model weights", message, None));
                }
                ModelPreflightStatus::Idle => {}
            }
        }
        let weights_required = match workflow {
            Workflow::Train => {
                self.drafts
                    .train
                    .integer(SettingsPath::WorkflowsTrainTrainingInputMode)
                    == 0
            }
            Workflow::Validate | Workflow::Predict | Workflow::Live | Workflow::Export => true,
            Workflow::Annotate => false,
        };
        if weights_required && !self.drafts.has_model_input(model_workflow) {
            return Some(primary_issue(
                "Model weights",
                "Select model weights before starting this workflow.",
                None,
            ));
        }
        if workflow != Workflow::Train {
            return None;
        }

        if let Some(issue) = self.primary_draft_issue(workflow) {
            return Some(issue);
        }

        let dataset = &self.snapshot.artifacts.dataset;
        if dataset.phase != ArtifactOperationPhase::Complete {
            let message = if dataset.error.is_empty() {
                "Select or compile a dataset and wait for inspection.".into()
            } else {
                dataset.error.clone()
            };
            return Some(primary_issue(
                "Compiled dataset",
                message,
                Some(SettingsPath::WorkflowsTrainDatasetPathsCompiledDirectory),
            ));
        }
        if !dataset.compatible {
            let message = if dataset.error.is_empty() {
                "The compiled dataset is not ready for training.".into()
            } else {
                dataset.error.clone()
            };
            return Some(primary_issue(
                "Compiled dataset",
                message,
                Some(SettingsPath::WorkflowsTrainDatasetPathsCompiledDirectory),
            ));
        }
        if self.train_execution_target() == 1 {
            if value_at(
                &self.snapshot.workflow_state,
                &["workflows", "train", "remote_query", "armed_offer_id"],
            )
            .is_none_or(Value::is_null)
            {
                return Some(primary_issue(
                    "Remote training",
                    "Arm a remote GPU offer before launching.",
                    None,
                ));
            }
        }

        if self
            .drafts
            .train
            .integer(SettingsPath::WorkflowsTrainTrainingInputMode)
            == 0
        {
            let custom_weight = self
                .drafts
                .train
                .text(SettingsPath::WorkflowsTrainModelArtifactsWeightsPath);
            let weight = &self.snapshot.artifacts.weight;
            if custom_weight.trim().is_empty()
                && (weight.phase != ArtifactOperationPhase::Complete
                    || weight.preset_name != self.drafts.preset_name(Workflow::Train))
            {
                let message = if weight.error.is_empty() {
                    "Canonical weights are still being verified.".into()
                } else {
                    weight.error.clone()
                };
                return Some(primary_issue("Model weights", message, None));
            }
        }
        None
    }

    fn primary_draft_issue(&self, workflow: Workflow) -> Option<PrimaryIssue> {
        if workflow != Workflow::Train {
            return None;
        }
        let compiled_path = SettingsPath::WorkflowsTrainDatasetPathsCompiledDirectory;
        let compiled = self.drafts.train.text(compiled_path).trim();
        if compiled.is_empty() {
            return Some(primary_issue(
                "Compiled dataset",
                "Choose the compiled dataset directory before starting training.",
                Some(compiled_path),
            ));
        }
        let source = self
            .drafts
            .train
            .text(SettingsPath::WorkflowsTrainDatasetPathsSourceDir)
            .trim();
        if !source.is_empty() && paths_overlap(source, compiled) {
            return Some(primary_issue(
                "Dataset paths",
                "Dataset and Compiled directories must not overlap.",
                Some(compiled_path),
            ));
        }
        if self.train_execution_target() == 0
            && self
                .drafts
                .train
                .number_array(SettingsPath::WorkflowsTrainTrainingLocalDeviceIds)
                .is_empty()
        {
            return Some(primary_issue(
                "Local training",
                "Select at least one local GPU.",
                None,
            ));
        }
        let template_path = SettingsPath::WorkflowsTrainTrainingRemoteLaunchTemplate;
        if self.train_execution_target() == 1
            && self.drafts.train.text(template_path).trim().is_empty()
        {
            return Some(primary_issue(
                "Remote training",
                "A remote launch template is required.",
                Some(template_path),
            ));
        }
        None
    }

    pub fn field_needs_attention(&self, path: SettingsPath) -> bool {
        self.attention_field == Some(path)
    }

    pub fn compiled_resolution(&self) -> Option<u32> {
        let splits = &self.snapshot.artifacts.dataset.splits;
        let first = splits.first()?;
        if first.width == 0 || first.width != first.height {
            return None;
        }
        splits
            .iter()
            .all(|split| split.width == first.width && split.height == first.height)
            .then_some(first.width)
    }

    fn on_transport(&mut self, event: TransportEvent) -> Task<Message> {
        match event {
            TransportEvent::Connected(connection) => {
                crate::transport::set_workspace_release_connection(Some(connection.clone()));
                self.connection = Some(connection);
                self.transport_status = TransportStatus::Connected;
                self.transport_error.clear();
            }
            TransportEvent::Disconnected(reason) => {
                crate::transport::set_workspace_release_connection(None);
                self.connection = None;
                self.bridge.connected = false;
                self.transport_status = TransportStatus::Reconnecting;
                self.transport_error = reason;
                self.pending_weight_request = None;
                self.pending_settings_action = None;
                self.dataset_error_modal_armed = false;
                self.weight_error_modal_armed = false;
            }
            TransportEvent::Snapshot(snapshot) => {
                if snapshot.protocol_version != HOST_API_PROTOCOL_VERSION {
                    let error = format!(
                        "host protocol {} does not match UI protocol {}",
                        snapshot.protocol_version, HOST_API_PROTOCOL_VERSION
                    );
                    self.transport_error.clone_from(&error);
                    self.present_error("Protocol error", "State synchronization", error);
                    return Task::none();
                }
                if snapshot.contract_hash != HOST_API_CONTRACT_HASH {
                    self.transport_error = "host/UI contract hash mismatch".into();
                    self.present_error(
                        "Contract error",
                        "State synchronization",
                        "host/UI contract hash mismatch",
                    );
                    return Task::none();
                }
                if !self.received_snapshot {
                    self.selected_workflow = snapshot.active_workflow;
                    self.received_snapshot = true;
                }
                if self.pending_weight_request.as_ref().is_some_and(|request| {
                    snapshot.artifacts.weight.preset_name == request.preset_name
                        && snapshot.artifacts.weight.generation > request.after_generation
                        && snapshot.artifacts.weight.phase != ArtifactOperationPhase::Idle
                        && snapshot.artifacts.weight.status != WeightArtifactStatus::Idle
                }) {
                    self.pending_weight_request = None;
                    self.weight_request_error.clear();
                }
                let reset_succeeded =
                    self.settings_reset_pending && snapshot_has_reset_defaults(&snapshot);
                if reset_succeeded {
                    self.drafts = WorkflowDrafts::default();
                    self.settings_reset_pending = false;
                }
                if self.workspace_present.as_ref().is_some_and(|present| {
                    !snapshot.workspace_surface.as_ref().is_some_and(|surface| {
                        surface.ready && surface.generation == present.generation
                    })
                }) && let Some(previous) = self.workspace_present.take()
                {
                    crate::transport::release_workspace_surface_if_unsubmitted(&previous);
                }
                self.snapshot = snapshot;
                self.drafts.sync(&self.snapshot);
                self.consume_file_dialog_result();
                self.sync_runtime_controls();
                self.refresh_compatible_presets();
                self.refresh_theme();
                self.present_armed_artifact_error();
                if self
                    .pending_settings_action
                    .as_ref()
                    .is_some_and(|pending| {
                        json_patch_matches(&self.snapshot.workflow_state, &pending.patch)
                    })
                {
                    let pending = self
                        .pending_settings_action
                        .take()
                        .expect("pending action exists");
                    let _ = self.execute_deferred_action(pending.action);
                }
                return self.refresh_renderer_defaults();
            }
            TransportEvent::Bridge(bridge) => {
                if !bridge.last_error.is_empty() {
                    if self.pending_weight_request.take().is_some() {
                        self.weight_request_error.clone_from(&bridge.last_error);
                    }
                    self.transport_error.clone_from(&bridge.last_error);
                    self.pending_settings_action = None;
                    self.dataset_error_modal_armed = false;
                    self.weight_error_modal_armed = false;
                    if self.pending_file_dialog_token.is_none() {
                        self.pending_custom_dialog = None;
                    }
                    self.settings_reset_pending = false;
                    self.present_error(
                        "Action failed",
                        "Native bridge rejected the request",
                        bridge.last_error.clone(),
                    );
                } else if bridge.connected {
                    self.transport_error.clear();
                    self.last_error_key.clear();
                }
                self.bridge = bridge;
            }
            TransportEvent::WorkspacePresent(present) => {
                let valid = self
                    .snapshot
                    .workspace_surface
                    .as_ref()
                    .is_some_and(|surface| {
                        surface.ready
                            && surface.generation == present.generation
                            && present.slot < surface.slot_count
                            && present.width > 0
                            && present.height > 0
                            && present.width <= surface.capacity_width
                            && present.height <= surface.capacity_height
                            && present.source_region.width > 0
                            && present.source_region.height > 0
                    });
                if !valid {
                    crate::transport::release_workspace_surface_if_unsubmitted(&present);
                } else {
                    if let Some(previous) = self.workspace_present.as_ref()
                        && (previous.generation != present.generation
                            || previous.slot != present.slot
                            || previous.revision != present.revision)
                    {
                        crate::transport::release_workspace_surface_if_unsubmitted(previous);
                    }
                    self.workspace_present = Some(present);
                }
            }
            TransportEvent::Error(error) => {
                if self.pending_weight_request.take().is_some() {
                    self.weight_request_error = error.clone();
                }
                self.pending_settings_action = None;
                self.settings_reset_pending = false;
                self.dataset_error_modal_armed = false;
                self.weight_error_modal_armed = false;
                self.transport_error.clone_from(&error);
                self.present_error("Action failed", "Browser transport", error);
            }
        }
        Task::none()
    }

    fn refresh_renderer_defaults(&mut self) -> Task<Message> {
        let font_size = self.drafts.shell_style().primary_font_size;
        if (font_size - self.applied_font_size).abs() <= f32::EPSILON {
            return Task::none();
        }
        self.applied_font_size = font_size;
        iced::font::set_defaults(iced::Font::default(), font_size)
    }

    fn present_error(
        &mut self,
        title: impl Into<String>,
        context: impl Into<String>,
        message: impl Into<String>,
    ) {
        let title = title.into();
        let context = context.into();
        let message = message.into();
        let key = format!("{title}\u{1f}{context}\u{1f}{message}");
        if key == self.last_error_key {
            return;
        }
        self.last_error_key = key;
        self.active_error = Some(UiError {
            title,
            context,
            message,
        });
    }

    fn present_armed_artifact_error(&mut self) {
        let dataset = &self.snapshot.artifacts.dataset;
        let weight = &self.snapshot.artifacts.weight;
        if self.dataset_error_modal_armed
            && dataset.phase == ArtifactOperationPhase::Failed
            && !dataset.error.is_empty()
        {
            let context = format!("{} → {}", dataset.source_dir, dataset.output_dir);
            let message = dataset.error.clone();
            self.dataset_error_modal_armed = false;
            self.present_error("Dataset compilation failed", context, message);
            return;
        }
        if self.dataset_error_modal_armed
            && matches!(
                dataset.phase,
                ArtifactOperationPhase::Complete | ArtifactOperationPhase::Cancelled
            )
        {
            self.dataset_error_modal_armed = false;
        }
        if self.weight_error_modal_armed
            && matches!(
                weight.status,
                WeightArtifactStatus::CannotDownload
                    | WeightArtifactStatus::ChecksumError
                    | WeightArtifactStatus::FilesystemError
                    | WeightArtifactStatus::HttpError
                    | WeightArtifactStatus::Incompatible
            )
            && !weight.error.is_empty()
        {
            let context = weight.preset_name.clone();
            let message = weight.error.clone();
            self.weight_error_modal_armed = false;
            self.present_error("Weights operation failed", context, message);
            return;
        }
        if self.weight_error_modal_armed
            && (weight.phase == ArtifactOperationPhase::Cancelled
                || (weight.phase == ArtifactOperationPhase::Complete
                    && weight.status == WeightArtifactStatus::Ready))
        {
            self.weight_error_modal_armed = false;
        }
    }

    fn edit_text(&mut self, path: SettingsPath, value: String) {
        self.draft_for_path_mut(path).set_text(path, value);
        self.clear_attention(path);
    }

    fn edit_number(&mut self, path: SettingsPath, value: f64) {
        let value =
            if value.is_finite() && path.as_str().starts_with("workflows.train.augmentation.") {
                (value * 100.0).round() / 100.0
            } else {
                value
            };
        self.edit_draft(path, |draft| draft.set_number(path, value));
        self.clear_attention(path);
    }

    fn edit_boolean(&mut self, path: SettingsPath, value: bool) {
        self.edit_draft(path, |draft| draft.set_boolean(path, value));
        self.clear_attention(path);
    }

    fn edit_draft(&mut self, path: SettingsPath, edit: impl FnOnce(&mut crate::model::TypedDraft)) {
        edit(self.draft_for_path_mut(path));
        if matches!(path, SettingsPath::UiUiScale | SettingsPath::UiDarkMode) {
            self.refresh_theme();
        }
    }

    fn draft_for_path_mut(&mut self, path: SettingsPath) -> &mut crate::model::TypedDraft {
        if path.as_str().starts_with("ui.") {
            &mut self.drafts.ui
        } else {
            self.drafts.active_mut(self.selected_workflow)
        }
    }

    fn clear_attention(&mut self, path: SettingsPath) {
        if self.attention_field == Some(path) {
            self.attention_field = None;
        }
    }

    fn schedule_path_persist(&mut self, path: SettingsPath) -> Task<Message> {
        if path.as_str().starts_with("ui.") {
            Task::none()
        } else {
            self.schedule_workflow_persist(self.selected_workflow)
        }
    }

    fn schedule_workflow_persist(&mut self, workflow: Workflow) -> Task<Message> {
        let generation = &mut self.workflow_persist_generations[workflow_index(workflow)];
        *generation = generation.wrapping_add(1).max(1);
        workflow_persist_task(workflow, *generation)
    }

    fn persist_workflow_after_debounce(
        &mut self,
        workflow: Workflow,
        generation: u64,
    ) -> Task<Message> {
        if self.workflow_persist_generations[workflow_index(workflow)] != generation {
            return Task::none();
        }
        if !self.ready() || self.pending_settings_action.is_some() {
            return workflow_persist_task(workflow, generation);
        }
        let _ = self.try_apply_workflow_settings(workflow);
        Task::none()
    }

    fn present_primary_issue(&mut self, issue: PrimaryIssue) {
        self.attention_field = issue.field;
        self.present_error("Training needs attention", issue.context, issue.message);
    }

    fn run_action(&mut self, action: Action) {
        match action {
            Action::Primary => self.run_primary(),
            Action::ApplyShellSettings => self.apply_shell_settings(),
            Action::CompileDataset => self.compile_dataset(),
            Action::CancelDatasetCompile => {
                self.dataset_error_modal_armed = false;
                self.dispatch(
                    Workflow::Train,
                    HostIntent::empty(IntentId::DatasetCompileCancel),
                );
            }
            Action::RefreshLocalGpus => self.dispatch(
                Workflow::Train,
                HostIntent::empty(IntentId::TrainLocalGpuRefresh),
            ),
            Action::QueryRemoteOffers => self.dispatch(
                Workflow::Train,
                HostIntent::empty(IntentId::TrainRemoteQuery),
            ),
            Action::ClearRemoteOffer => self.dispatch(
                Workflow::Train,
                HostIntent::empty(IntentId::TrainRemoteOfferClear),
            ),
            Action::SaveAnnotation => self.dispatch(
                Workflow::Annotate,
                HostIntent::empty(IntentId::AnnotateSaveNow),
            ),
            Action::ToggleLiveAnnotation => {
                self.annotation_live = !self.annotation_live;
                self.dispatch(
                    Workflow::Annotate,
                    HostIntent::empty(if self.annotation_live {
                        IntentId::AnnotateLiveStart
                    } else {
                        IntentId::AnnotateLiveStop
                    }),
                );
            }
            Action::SetupPrevious => self.dispatch(
                Workflow::Annotate,
                HostIntent::empty(IntentId::AnnotateSetupFramePrev),
            ),
            Action::SetupReload => self.dispatch(
                Workflow::Annotate,
                HostIntent::empty(IntentId::AnnotateSetupFrameReload),
            ),
            Action::SetupNext => self.dispatch(
                Workflow::Annotate,
                HostIntent::empty(IntentId::AnnotateSetupFrameNext),
            ),
            Action::ToggleHoldSave => {
                self.hold_save = !self.hold_save;
                self.dispatch(
                    Workflow::Annotate,
                    HostIntent::with_payload(
                        IntentId::AnnotateHoldSave,
                        json!({"enabled": self.hold_save}),
                    ),
                );
            }
        }
    }

    fn run_primary(&mut self) {
        let workflow = self.selected_workflow;
        if self.train_running()
            || self.remote_train_running()
            || self.snapshot.job.running
            || self.live_predict_active()
        {
            self.run_primary_now(workflow);
            return;
        }
        let settings_dirty = self.drafts.active(workflow).is_dirty();
        let issue = if settings_dirty {
            self.primary_draft_issue(workflow)
        } else {
            self.primary_issue(workflow)
        };
        if let Some(issue) = issue {
            self.present_primary_issue(issue);
            return;
        }
        self.defer_after_workflow_settings(workflow, DeferredAction::Primary(workflow));
    }

    fn run_primary_now(&mut self, workflow: Workflow) {
        let intent = match workflow {
            Workflow::Train if self.train_running() => {
                HostIntent::with_payload(IntentId::TrainStop, json!({"force": false}))
            }
            Workflow::Train if self.remote_train_running() => {
                HostIntent::empty(IntentId::TrainRemoteStop)
            }
            Workflow::Train if self.train_execution_target() == 1 => {
                HostIntent::empty(IntentId::TrainRemoteStart)
            }
            Workflow::Train => HostIntent::empty(IntentId::TrainStart),
            Workflow::Validate => HostIntent::empty(IntentId::ValidateStart),
            Workflow::Predict if self.snapshot.job.running => {
                HostIntent::empty(IntentId::PredictStop)
            }
            Workflow::Predict | Workflow::Live if self.live_predict_active() => {
                HostIntent::empty(IntentId::PredictStop)
            }
            Workflow::Predict | Workflow::Live => HostIntent::empty(IntentId::PredictStart),
            Workflow::Annotate => HostIntent::empty(IntentId::AnnotateSave),
            Workflow::Export => HostIntent::empty(IntentId::ExportStart),
        };
        self.dispatch(workflow, intent);
    }

    fn try_apply_workflow_settings(&mut self, workflow: Workflow) -> bool {
        let patch = self.drafts.active_patch(workflow);
        if patch.as_object().is_some_and(Map::is_empty) {
            return false;
        }
        self.try_dispatch(
            workflow,
            HostIntent::with_payload(IntentId::SettingsUpdate, json!({"patch": patch})),
        )
    }

    fn begin_canonical_weight_verification(&mut self, preset_name: String) {
        let observed_generation = self.snapshot.artifacts.weight.generation;
        let after_generation = self
            .pending_weight_request
            .as_ref()
            .map_or(observed_generation, |request| {
                observed_generation.max(request.after_generation.saturating_add(1))
            });
        self.pending_weight_request = Some(PendingWeightRequest {
            preset_name,
            after_generation,
        });
        self.weight_error_modal_armed = true;
        self.weight_request_error.clear();
    }

    pub fn weight_request_pending(&self) -> bool {
        self.pending_weight_request.is_some()
    }

    pub fn weight_request_error(&self) -> &str {
        &self.weight_request_error
    }

    pub fn dataset_compile_pending(&self) -> bool {
        self.pending_settings_action
            .as_ref()
            .is_some_and(|pending| matches!(pending.action, DeferredAction::CompileDataset(_)))
    }

    fn apply_shell_settings(&mut self) {
        let patch = self.drafts.ui.patch();
        if patch.as_object().is_some_and(Map::is_empty) {
            return;
        }
        self.dispatch(
            self.selected_workflow,
            HostIntent::with_payload(IntentId::SettingsUpdate, json!({"patch": patch})),
        );
    }

    fn persist_ui_setting(&mut self, path: SettingsPath) {
        let patch = self.drafts.ui.patch_for(&[path]);
        if patch.as_object().is_some_and(Map::is_empty) {
            return;
        }
        self.dispatch(
            self.selected_workflow,
            HostIntent::with_payload(IntentId::SettingsUpdate, json!({"patch": patch})),
        );
    }

    fn compile_dataset(&mut self) {
        let source_dir = self
            .drafts
            .train
            .text(SettingsPath::WorkflowsTrainDatasetPathsSourceDir)
            .trim()
            .to_owned();
        let output_dir = self
            .drafts
            .train
            .text(SettingsPath::WorkflowsTrainDatasetPathsCompiledDirectory)
            .trim()
            .to_owned();
        if source_dir.is_empty() || output_dir.is_empty() {
            self.attention_field = Some(if source_dir.is_empty() {
                SettingsPath::WorkflowsTrainDatasetPathsSourceDir
            } else {
                SettingsPath::WorkflowsTrainDatasetPathsCompiledDirectory
            });
            self.transport_error = "Dataset and Compiled directories are required".into();
            self.present_error(
                "Cannot compile dataset",
                "Dataset paths",
                self.transport_error.clone(),
            );
            return;
        }
        if paths_overlap(&source_dir, &output_dir) {
            self.attention_field = Some(SettingsPath::WorkflowsTrainDatasetPathsCompiledDirectory);
            self.transport_error = "Dataset and Compiled directories must not overlap".into();
            self.present_error(
                "Cannot compile dataset",
                format!("{source_dir} → {output_dir}"),
                self.transport_error.clone(),
            );
            return;
        }
        let resolution = self
            .drafts
            .train
            .number(SettingsPath::WorkflowsTrainModelArtifactsResolution);
        if !resolution.is_finite() || resolution < 1.0 || resolution > u32::MAX as f64 {
            self.transport_error = "Input size must be a positive integer".into();
            self.present_error(
                "Cannot compile dataset",
                "Compile dimensions",
                self.transport_error.clone(),
            );
            return;
        }
        let request = CompileDatasetRequest {
            source_dir,
            output_dir,
            preset_name: self.drafts.preset_name(Workflow::Train).to_owned(),
            resolution: resolution.round() as u32,
            overwrite: self
                .drafts
                .train
                .boolean(SettingsPath::WorkflowsTrainDatasetPathsOverwrite),
        };
        self.defer_after_workflow_settings(
            Workflow::Train,
            DeferredAction::CompileDataset(request),
        );
    }

    fn defer_after_workflow_settings(
        &mut self,
        workflow: Workflow,
        action: DeferredAction,
    ) -> bool {
        if self.pending_settings_action.is_some() {
            self.present_error(
                "Action already pending",
                "Settings synchronization",
                "Wait for the current settings update to finish before starting another action.",
            );
            return false;
        }
        let patch = self.drafts.active_patch(workflow);
        if patch.as_object().is_some_and(Map::is_empty) {
            return self.execute_deferred_action(action);
        }
        let confirmation_patch = match &action {
            DeferredAction::CompileDataset(request) => json!({
                "workflows": {
                    "train": {
                        "dataset_paths": {
                            "source_dir": request.source_dir,
                            "compiled_directory": request.output_dir,
                            "overwrite": request.overwrite,
                        },
                        "model_artifacts": {
                            "preset_name": request.preset_name,
                            "resolution": request.resolution,
                        },
                    },
                },
            }),
            DeferredAction::Primary(_) | DeferredAction::FileDialog { .. } => patch.clone(),
        };
        if self.try_dispatch(
            workflow,
            HostIntent::with_payload(IntentId::SettingsUpdate, json!({"patch": patch})),
        ) {
            self.pending_settings_action = Some(PendingSettingsAction {
                patch: confirmation_patch,
                action,
            });
            true
        } else {
            false
        }
    }

    fn execute_deferred_action(&mut self, action: DeferredAction) -> bool {
        match action {
            DeferredAction::CompileDataset(request) => self.start_dataset_compile(request),
            DeferredAction::Primary(workflow) => {
                if let Some(issue) = self.primary_issue(workflow) {
                    self.present_primary_issue(issue);
                    return false;
                }
                self.run_primary_now(workflow);
                true
            }
            DeferredAction::FileDialog {
                workflow,
                token,
                payload,
            } => {
                let dispatched = self.try_dispatch(
                    workflow,
                    HostIntent::with_payload(IntentId::FileDialogRequest, payload),
                );
                if dispatched {
                    self.pending_file_dialog_token = Some(token);
                } else {
                    self.pending_custom_dialog = None;
                }
                dispatched
            }
        }
    }

    fn start_dataset_compile(&mut self, request: CompileDatasetRequest) -> bool {
        let dispatched = self.try_dispatch(
            Workflow::Train,
            HostIntent::with_payload(
                IntentId::DatasetCompileStart,
                json!({
                    "source_dir": request.source_dir,
                    "output_dir": request.output_dir,
                    "preset_name": request.preset_name,
                    "resolution": request.resolution,
                    "overwrite": request.overwrite,
                }),
            ),
        );
        self.dataset_error_modal_armed = dispatched;
        dispatched
    }

    fn open_file_dialog(&mut self, dialog_id: FileDialogId) {
        if self.pending_file_dialog_token.is_some() || self.custom_model_confirmation.is_some() {
            self.transport_error =
                "finish the current file selection before opening another".into();
            self.present_error(
                "Cannot open file picker",
                dialog_id.spec().title,
                self.transport_error.clone(),
            );
            return;
        }
        if let Some(artifact_kind) = model_artifact_kind(dialog_id) {
            let workflow = dialog_workflow(dialog_id, self.selected_workflow);
            let (preset_name, catalog_resolution) = self
                .preset_for(workflow)
                .map(|preset| (preset.preset_name.to_owned(), preset.resolution))
                .unwrap_or_else(|| ("rf-detr-nano".into(), 384));
            let resolution = self
                .drafts
                .active(workflow)
                .entries()
                .iter()
                .find(|entry| entry.path.as_str().ends_with("model_artifacts.resolution"))
                .and_then(|entry| match &entry.value {
                    crate::model::DraftValue::Number(value)
                        if value.is_finite() && *value >= 1.0 =>
                    {
                        Some(value.round().clamp(1.0, u32::MAX as f64) as u32)
                    }
                    _ => None,
                })
                .unwrap_or(catalog_resolution);
            if let Some(token) = self.dispatch_file_dialog(dialog_id, true) {
                self.pending_custom_dialog =
                    Some((token, workflow, artifact_kind, preset_name, resolution));
            }
        } else {
            self.dispatch_file_dialog(dialog_id, false);
        }
    }

    fn open_custom_weights_dialog(&mut self) {
        self.pending_weight_request = None;
        self.weight_error_modal_armed = false;
        self.weight_request_error.clear();
        let dialog_id = match self.selected_workflow {
            Workflow::Train => FileDialogId::TrainModelWeights,
            Workflow::Validate => FileDialogId::ValidateModelWeights,
            Workflow::Predict | Workflow::Live => FileDialogId::PredictModelWeights,
            Workflow::Annotate => FileDialogId::AnnotateModelWeights,
            Workflow::Export => FileDialogId::ExportModelWeights,
        };
        self.open_file_dialog(dialog_id);
    }

    fn dispatch_file_dialog(&mut self, dialog_id: FileDialogId, defer_apply: bool) -> Option<u64> {
        self.file_dialog_token = self.file_dialog_token.wrapping_add(1).max(1);
        let token = self.file_dialog_token;
        let dialog = dialog_id.spec();
        let draft_workflow =
            if dialog.workflow == Workflow::Predict && self.selected_workflow == Workflow::Live {
                Workflow::Live
            } else {
                dialog.workflow
            };
        let filters = if dialog.filter_patterns.is_empty() {
            json!([])
        } else {
            json!([{"name": dialog.filter_name, "patterns": dialog.filter_patterns}])
        };
        let payload = json!({
            "token": token,
            "dialog_id": dialog.id,
            "target_field": dialog.field,
            "mode": dialog.mode.as_str(),
            "title": dialog.title,
            "filters": filters,
            "defer_apply": defer_apply
        });
        if !self.defer_after_workflow_settings(
            draft_workflow,
            DeferredAction::FileDialog {
                workflow: dialog.workflow,
                token,
                payload,
            },
        ) {
            return None;
        }
        Some(token)
    }

    fn confirm_custom_weights(&mut self) {
        let Some(confirmation) = self.custom_model_confirmation.take() else {
            return;
        };
        self.pending_weight_request = None;
        self.weight_request_error.clear();
        self.dispatch(
            confirmation.workflow,
            HostIntent::with_payload(
                IntentId::ModelCustomSelect,
                json!({
                    "dialog_token": confirmation.dialog_token,
                    "artifact_kind": custom_model_kind_name(confirmation.artifact_kind),
                    "preset_name": confirmation.preset_name,
                    "resolution": confirmation.resolution,
                    "path": confirmation.path,
                }),
            ),
        );
    }

    fn cancel_custom_weights(&mut self) {
        self.custom_model_confirmation = None;
        self.pending_custom_dialog = None;
    }

    fn consume_file_dialog_result(&mut self) {
        let Some(token) = self.pending_file_dialog_token else {
            return;
        };
        if self.snapshot.file_dialog.token != token
            || matches!(
                self.snapshot.file_dialog.status,
                FileDialogResultStatus::Idle | FileDialogResultStatus::Pending
            )
        {
            return;
        }
        self.pending_file_dialog_token = None;
        let result = std::mem::take(&mut self.snapshot.file_dialog);
        let Some((custom_token, workflow, artifact_kind, preset_name, resolution)) =
            self.pending_custom_dialog.take()
        else {
            if result.status == FileDialogResultStatus::Failed {
                self.transport_error = if result.error.is_empty() {
                    "native file dialog failed".into()
                } else {
                    result.error
                };
                self.present_error(
                    "File picker failed",
                    result.dialog_id,
                    self.transport_error.clone(),
                );
            }
            return;
        };
        if custom_token != token {
            self.transport_error = "native custom-model dialog token mismatch".into();
            self.present_error(
                "File picker failed",
                "Custom model selection",
                self.transport_error.clone(),
            );
            return;
        }
        match result.status {
            FileDialogResultStatus::Selected => {
                self.custom_model_confirmation = Some(CustomModelConfirmation {
                    dialog_token: token,
                    workflow,
                    artifact_kind,
                    preset_name,
                    resolution,
                    path: result.path,
                });
            }
            FileDialogResultStatus::Cancelled => {}
            FileDialogResultStatus::Failed => {
                self.transport_error = if result.error.is_empty() {
                    "native custom-weight dialog failed".into()
                } else {
                    result.error
                };
                self.present_error(
                    "File picker failed",
                    result.dialog_id,
                    self.transport_error.clone(),
                );
            }
            FileDialogResultStatus::Idle | FileDialogResultStatus::Pending => unreachable!(),
        }
    }

    fn toggle_local_gpu(&mut self, device_id: i32, selected: bool) {
        let path = SettingsPath::WorkflowsTrainTrainingLocalDeviceIds;
        let mut values = self.drafts.train.number_array(path).to_vec();
        values.retain(|value| value.round() as i32 != device_id);
        if selected {
            values.push(device_id.into());
            values.sort_by(f64::total_cmp);
            values.dedup_by(|left, right| left.round() == right.round());
        }
        self.drafts.train.set_number_array(path, values);
    }

    fn toggle_remote_family(&mut self, index: usize, enabled: bool) {
        let path = SettingsPath::WorkflowsTrainTrainingRemoteFamilyEnabled;
        let mut values = self.drafts.train.boolean_array(path).to_vec();
        values.resize(5, true);
        if let Some(value) = values.get_mut(index) {
            *value = enabled;
            self.drafts.train.set_boolean_array(path, values);
        }
    }

    fn source_kind_path(&self) -> Option<SettingsPath> {
        match self.selected_workflow {
            Workflow::Predict | Workflow::Live => Some(SettingsPath::WorkflowsPredictSourceKind),
            Workflow::Annotate => Some(SettingsPath::WorkflowsAnnotateSourceKind),
            _ => None,
        }
    }

    fn dispatch_live_preview(&mut self, control: LivePreviewControl, enabled: bool) {
        let id = match control {
            LivePreviewControl::FitToCapture => IntentId::LivePreviewFitToCapture,
            LivePreviewControl::FullFrameDisplay => IntentId::LivePreviewFullFrameDisplay,
        };
        self.dispatch(
            Workflow::Live,
            HostIntent::with_payload(id, json!({"enabled": enabled})),
        );
    }

    fn dispatch(&mut self, workflow: Workflow, intent: HostIntent) {
        let _ = self.try_dispatch(workflow, intent);
    }

    fn try_dispatch(&mut self, workflow: Workflow, intent: HostIntent) -> bool {
        if !self.ready() {
            self.transport_error = "native bridge is not ready".into();
            self.present_error(
                "Action unavailable",
                "Native bridge",
                self.transport_error.clone(),
            );
            return false;
        }
        self.request_id = self.request_id.wrapping_add(1).max(1);
        let message = intent.into_wire(self.request_id, workflow);
        let encoded = match serde_json::to_string(&message) {
            Ok(encoded) => encoded,
            Err(error) => {
                self.transport_error = format!("failed to encode browser intent: {error}");
                self.present_error(
                    "Action failed",
                    "Intent encoding",
                    self.transport_error.clone(),
                );
                return false;
            }
        };
        let Some(connection) = self.connection.as_mut() else {
            self.transport_error = "native bridge connection is absent".into();
            self.present_error(
                "Action unavailable",
                "Native bridge",
                self.transport_error.clone(),
            );
            return false;
        };
        if let Err(error) = connection.send(encoded) {
            self.transport_error = error.into();
            self.present_error(
                "Action failed",
                "Browser outbound queue",
                self.transport_error.clone(),
            );
            return false;
        }
        true
    }

    fn trace_ui(&mut self, event: &'static str, fields: impl FnOnce() -> Value) {
        if !self.config.trace_enabled || !self.ready() {
            return;
        }
        self.dispatch(
            self.selected_workflow,
            HostIntent::with_payload(
                IntentId::UiLog,
                json!({"level": "trace", "event": event, "fields": fields()}),
            ),
        );
    }

    fn sync_runtime_controls(&mut self) {
        self.workspace_aspect_ratio = WorkspaceAspectRatio::from_index(
            self.drafts.ui.integer(SettingsPath::UiWorkspaceAspectRatio),
        );
        self.mask_cleanup_radius = self
            .drafts
            .ui
            .integer(SettingsPath::UiMaskCleanupRadius)
            .clamp(1, 32) as u16;
        self.brush_radius = self
            .drafts
            .ui
            .integer(SettingsPath::UiAnnotationBrushRadius)
            .clamp(1, 128) as u16;
        self.annotation_live = bool_at(
            &self.snapshot.workflow_state,
            &["annotate_runtime_controls", "live_annotate", "running"],
        );
        self.hold_save = bool_at(
            &self.snapshot.workflow_state,
            &["annotate_runtime_controls", "save", "hold_save", "value"],
        );
        self.fit_to_capture = bool_at(
            &self.snapshot.workflow_state,
            &["live_preview_controls", "fit_to_capture", "value"],
        );
        self.full_frame_display = bool_at(
            &self.snapshot.workflow_state,
            &["live_preview_controls", "full_frame_display", "value"],
        );
        if let Some(radius) = value_at(
            &self.snapshot.workflow_state,
            &["annotate_runtime_controls", "brush", "radius", "value"],
        )
        .and_then(Value::as_u64)
        {
            self.brush_radius = radius.clamp(1, 128) as u16;
        }
        if let Some(selected) = value_at(
            &self.snapshot.workflow_state,
            &["annotate_sidebar", "selected_object"],
        )
        .and_then(Value::as_object)
        {
            if let Some(sup) = selected.get("sup") {
                self.mask_sup = sup.clone();
            }
            if let Some(nosup) = selected.get("nosup") {
                self.mask_nosup = nosup.clone();
            }
        }
    }

    fn set_mask_color_number(&mut self, range: MaskColorRange, field: MaskColorField, value: f64) {
        if !value.is_finite() {
            return;
        }
        let (section, key, minimum, maximum) = match field {
            MaskColorField::HueDegrees => ("center", "hue_degrees", 0.0, 360.0),
            MaskColorField::Saturation => ("center", "saturation", 0.0, 1.0),
            MaskColorField::Value => ("center", "value", 0.0, 1.0),
            MaskColorField::HueMinus => ("tolerance", "hue_minus_pct", 0.0, 100.0),
            MaskColorField::HuePlus => ("tolerance", "hue_plus_pct", 0.0, 100.0),
            MaskColorField::SaturationMinus => ("tolerance", "saturation_minus_pct", 0.0, 100.0),
            MaskColorField::SaturationPlus => ("tolerance", "saturation_plus_pct", 0.0, 100.0),
            MaskColorField::ValueMinus => ("tolerance", "value_minus_pct", 0.0, 100.0),
            MaskColorField::ValuePlus => ("tolerance", "value_plus_pct", 0.0, 100.0),
        };
        let target = match range {
            MaskColorRange::Sup => &mut self.mask_sup,
            MaskColorRange::Nosup => &mut self.mask_nosup,
        };
        if let Some(section) = target
            .as_object_mut()
            .and_then(|target| target.get_mut(section))
            .and_then(Value::as_object_mut)
        {
            section.insert(key.into(), json!(value.clamp(minimum, maximum)));
            self.dispatch_mask_color_ranges();
        }
    }

    fn set_mask_color_sampling(&mut self, range: MaskColorRange, enabled: bool) {
        if enabled {
            let other = match range {
                MaskColorRange::Sup => &mut self.mask_nosup,
                MaskColorRange::Nosup => &mut self.mask_sup,
            };
            if let Some(other) = other.as_object_mut() {
                other.insert("sampling".into(), Value::Bool(false));
            }
        }
        let target = match range {
            MaskColorRange::Sup => &mut self.mask_sup,
            MaskColorRange::Nosup => &mut self.mask_nosup,
        };
        if let Some(target) = target.as_object_mut() {
            target.insert("sampling".into(), Value::Bool(enabled));
            self.dispatch_mask_color_ranges();
        }
    }

    fn dispatch_mask_color_ranges(&mut self) {
        self.dispatch(
            Workflow::Annotate,
            HostIntent::with_payload(
                IntentId::AnnotateSidebar,
                json!({
                    "action": "mask.update_color_ranges",
                    "sup": self.mask_sup.clone(),
                    "nosup": self.mask_nosup.clone()
                }),
            ),
        );
    }

    fn refresh_compatible_presets(&mut self) {
        compatible_presets(None, &mut self.compatible_presets);
    }

    fn refresh_theme(&mut self) {
        let style = self.drafts.shell_style();
        self.theme = app_theme(style.dark_mode, style.accent_color);
    }

    fn train_running(&self) -> bool {
        bool_at(&self.snapshot.workflow_state, &["train_runtime", "running"])
    }

    fn remote_train_running(&self) -> bool {
        bool_at(
            &self.snapshot.workflow_state,
            &["workflows", "train", "remote_session", "instance_running"],
        )
    }

    fn train_execution_target(&self) -> i32 {
        self.drafts
            .train
            .integer(SettingsPath::WorkflowsTrainTrainingExecutionTarget)
    }

    fn live_predict_active(&self) -> bool {
        string_at(
            &self.snapshot.workflow_state,
            &["live_runtime", "active_mode"],
        ) == Some("predict")
            && (bool_at(&self.snapshot.workflow_state, &["live_runtime", "starting"])
                || bool_at(&self.snapshot.workflow_state, &["live_runtime", "stopping"])
                || bool_at(
                    &self.snapshot.workflow_state,
                    &["live_runtime", "controller", "running"],
                )
                || bool_at(
                    &self.snapshot.workflow_state,
                    &["live_runtime", "show_running_section"],
                ))
    }
}

fn app_theme(dark_mode: bool, accent_color: Color) -> Theme {
    let mut seed = if dark_mode {
        palette::Seed::TOKYO_NIGHT
    } else {
        palette::Seed::LIGHT
    };
    seed.primary = accent_color;
    Theme::custom(
        if dark_mode {
            "mmltk dark"
        } else {
            "mmltk light"
        },
        seed,
    )
}

pub(crate) fn bool_at(root: &Map<String, Value>, path: &[&str]) -> bool {
    value_at(root, path)
        .and_then(Value::as_bool)
        .unwrap_or(false)
}

pub(crate) fn string_at<'a>(root: &'a Map<String, Value>, path: &[&str]) -> Option<&'a str> {
    value_at(root, path).and_then(Value::as_str)
}

fn primary_issue(
    context: &'static str,
    message: impl Into<String>,
    field: Option<SettingsPath>,
) -> PrimaryIssue {
    PrimaryIssue {
        context,
        message: message.into(),
        field,
    }
}

const fn workflow_index(workflow: Workflow) -> usize {
    match workflow {
        Workflow::Train => 0,
        Workflow::Validate => 1,
        Workflow::Predict => 2,
        Workflow::Annotate => 3,
        Workflow::Export => 4,
        Workflow::Live => 5,
    }
}

#[cfg(target_arch = "wasm32")]
fn workflow_persist_task(workflow: Workflow, generation: u64) -> Task<Message> {
    Task::perform(
        async move {
            gloo_timers::future::TimeoutFuture::new(WORKFLOW_PERSIST_DEBOUNCE_MS).await;
            (workflow, generation)
        },
        |(workflow, generation)| Message::WorkflowPersistDebounced(workflow, generation),
    )
}

#[cfg(not(target_arch = "wasm32"))]
fn workflow_persist_task(_workflow: Workflow, _generation: u64) -> Task<Message> {
    Task::none()
}

fn text_input_operation_task<Output, Operation>(
    id: String,
    operation: impl FnOnce(iced::advanced::widget::Id) -> Operation,
    message: fn(String, Option<Output>) -> Message,
) -> Task<Message>
where
    Output: Send + 'static,
    Operation: iced::advanced::widget::Operation<Option<Output>> + 'static,
{
    let target = id.clone();
    iced::advanced::widget::operate(operation(iced::advanced::widget::Id::from(target)))
        .map(move |result| message(id.clone(), result))
}

fn paste_text_selection_task(id: String, text: String) -> Task<Message> {
    let target = id.clone();
    iced::advanced::widget::operate(iced::advanced::widget::operation::text_input::paste_text(
        iced::advanced::widget::Id::from(target),
        text,
    ))
    .map(move |value| Message::TextPasteResolved(id.clone(), value))
}

fn clipboard_read_task(id: String) -> Task<Message> {
    iced::clipboard::read_text().map(move |result| {
        Message::ClipboardTextRead(
            id.clone(),
            result
                .map(|text| (*text).clone())
                .map_err(clipboard_error_message),
        )
    })
}

fn clipboard_write_task(operation: &'static str, target: String, text: String) -> Task<Message> {
    let byte_count = text.len();
    iced::clipboard::write(text).map(move |result| {
        Message::ClipboardWriteResolved(
            operation,
            target.clone(),
            byte_count,
            result.err().map(clipboard_error_message),
        )
    })
}

fn clipboard_error_message(error: iced::advanced::clipboard::Error) -> String {
    use iced::advanced::clipboard::Error;

    match error {
        Error::ClipboardUnavailable => {
            "Clipboard access is unavailable in this browser context.".to_owned()
        }
        Error::ClipboardOccupied => "The clipboard is currently busy.".to_owned(),
        Error::ContentNotAvailable => "The clipboard does not currently contain text.".to_owned(),
        Error::ConversionFailure => "The clipboard text could not be decoded.".to_owned(),
        Error::Unknown { description } => (*description).clone(),
    }
}

fn clipboard_runtime_event(
    event: iced::Event,
    _status: iced::event::Status,
    _window: iced::window::Id,
) -> Option<Message> {
    let (operation, target, byte_count, error) = match event {
        iced::Event::Clipboard(iced::advanced::clipboard::Event::Read { target, result }) => {
            let byte_count = result.as_ref().map_or(0, |content| match content.as_ref() {
                iced::advanced::clipboard::Content::Text(text)
                | iced::advanced::clipboard::Content::Html(text) => text.len(),
                _ => 0,
            });
            (
                "read",
                target.unwrap_or_else(|| "clipboard.read".to_owned()),
                byte_count,
                result.err().map(clipboard_error_message),
            )
        }
        iced::Event::Clipboard(iced::advanced::clipboard::Event::Written {
            target,
            byte_count,
            result,
        }) => (
            "write",
            target.unwrap_or_else(|| "clipboard.write".to_owned()),
            byte_count,
            result.err().map(clipboard_error_message),
        ),
        _ => return None,
    };
    Some(Message::ClipboardRuntimeResolved(
        operation, target, byte_count, error,
    ))
}

fn pasted_value_message(id: &str, value: String) -> Option<Message> {
    if id == "annotate.new_category" {
        return Some(Message::EditAnnotationCategory(value));
    }
    if let Some((range, field)) = mask_color_field_for_input(id) {
        return value
            .trim()
            .parse()
            .ok()
            .map(|value| Message::SetMaskColorNumber(range, field, value));
    }
    let spec = SETTINGS_PATHS
        .iter()
        .find(|spec| spec.path.as_str() == id)?;
    match spec.value_kind {
        SettingsValueKind::String | SettingsValueKind::Preset => {
            Some(Message::EditText(spec.path, value))
        }
        SettingsValueKind::Number
        | SettingsValueKind::ModelSource
        | SettingsValueKind::ModelInput
        | SettingsValueKind::CompileMode
        | SettingsValueKind::TrainInputMode => value
            .trim()
            .parse()
            .ok()
            .map(|value| Message::SetNumber(spec.path, value)),
        SettingsValueKind::Workflow
        | SettingsValueKind::Boolean
        | SettingsValueKind::NumberArray
        | SettingsValueKind::BooleanArray => None,
    }
}

fn settings_path_for_input(id: &str) -> Option<SettingsPath> {
    SETTINGS_PATHS
        .iter()
        .find(|spec| spec.path.as_str() == id)
        .map(|spec| spec.path)
}

fn mask_color_field_for_input(id: &str) -> Option<(MaskColorRange, MaskColorField)> {
    let range = if id.starts_with("annotate.mask.sup.") {
        MaskColorRange::Sup
    } else if id.starts_with("annotate.mask.nosup.") {
        MaskColorRange::Nosup
    } else {
        return None;
    };
    let field = match id.rsplit('.').next()? {
        "hue_degrees" => MaskColorField::HueDegrees,
        "saturation" => MaskColorField::Saturation,
        "value" => MaskColorField::Value,
        "hue_minus" => MaskColorField::HueMinus,
        "hue_plus" => MaskColorField::HuePlus,
        "saturation_minus" => MaskColorField::SaturationMinus,
        "saturation_plus" => MaskColorField::SaturationPlus,
        "value_minus" => MaskColorField::ValueMinus,
        "value_plus" => MaskColorField::ValuePlus,
        _ => return None,
    };
    Some((range, field))
}

const fn attention_path_for_dialog(dialog: FileDialogId) -> Option<SettingsPath> {
    match dialog {
        FileDialogId::TrainDatasetSourceDir => {
            Some(SettingsPath::WorkflowsTrainDatasetPathsSourceDir)
        }
        FileDialogId::TrainDatasetCompiledDirectory => {
            Some(SettingsPath::WorkflowsTrainDatasetPathsCompiledDirectory)
        }
        FileDialogId::TrainDatasetTrainCompiledPath => {
            Some(SettingsPath::WorkflowsTrainDatasetPathsTrainCompiledPath)
        }
        FileDialogId::TrainDatasetValCompiledPath => {
            Some(SettingsPath::WorkflowsTrainDatasetPathsValCompiledPath)
        }
        FileDialogId::TrainDatasetTestCompiledPath => {
            Some(SettingsPath::WorkflowsTrainDatasetPathsTestCompiledPath)
        }
        _ => None,
    }
}

fn snapshot_has_reset_defaults(snapshot: &StateSnapshot) -> bool {
    string_at(
        &snapshot.workflow_state,
        &["workflows", "train", "dataset_paths", "source_dir"],
    ) == Some("./dataset")
        && string_at(
            &snapshot.workflow_state,
            &["workflows", "train", "dataset_paths", "compiled_directory"],
        ) == Some("./compiled")
        && value_at(&snapshot.settings_state, &["font_size"]).and_then(Value::as_f64) == Some(14.0)
}

fn json_patch_matches(current: &Map<String, Value>, patch: &Value) -> bool {
    let Some(patch) = patch.as_object() else {
        return false;
    };
    patch.iter().all(|(key, expected)| {
        current
            .get(key)
            .is_some_and(|actual| json_value_contains(actual, expected))
    })
}

fn json_value_contains(actual: &Value, expected: &Value) -> bool {
    match (actual.as_object(), expected.as_object()) {
        (Some(actual), Some(expected)) => expected.iter().all(|(key, expected)| {
            actual
                .get(key)
                .is_some_and(|actual| json_value_contains(actual, expected))
        }),
        _ => actual == expected,
    }
}

fn paths_overlap(left: &str, right: &str) -> bool {
    let left = lexical_path_components(left);
    let right = lexical_path_components(right);
    !left.is_empty() && !right.is_empty() && (left.starts_with(&right) || right.starts_with(&left))
}

fn lexical_path_components(path: &str) -> Vec<&str> {
    let mut components = Vec::with_capacity(path.matches('/').count().saturating_add(1));
    for component in path.split('/') {
        match component {
            "" | "." => {}
            ".." => {
                let _ = components.pop();
            }
            component => components.push(component),
        }
    }
    components
}

pub(crate) fn value_at<'a>(root: &'a Map<String, Value>, path: &[&str]) -> Option<&'a Value> {
    let mut value = path.first().and_then(|key| root.get(*key));
    for key in path.iter().skip(1) {
        value = value
            .and_then(Value::as_object)
            .and_then(|object| object.get(*key));
    }
    value
}

const fn model_artifact_kind(dialog_id: FileDialogId) -> Option<CustomModelArtifactKind> {
    match dialog_id {
        FileDialogId::TrainModelWeights
        | FileDialogId::ValidateModelWeights
        | FileDialogId::PredictModelWeights
        | FileDialogId::AnnotateModelWeights
        | FileDialogId::ExportModelWeights => Some(CustomModelArtifactKind::Weights),
        _ => None,
    }
}

fn dialog_workflow(dialog_id: FileDialogId, selected_workflow: Workflow) -> Workflow {
    if dialog_id.spec().workflow == Workflow::Predict && selected_workflow == Workflow::Live {
        Workflow::Live
    } else {
        dialog_id.spec().workflow
    }
}

const fn custom_model_kind_name(kind: CustomModelArtifactKind) -> &'static str {
    match kind {
        CustomModelArtifactKind::Weights => "weights",
        CustomModelArtifactKind::Onnx => "onnx",
        CustomModelArtifactKind::TensorRt => "tensorrt",
    }
}

fn default_mask_color_range() -> Value {
    json!({
        "center": {"hue_degrees": 180.0, "saturation": 0.5, "value": 0.5},
        "tolerance": {
            "hue_minus_pct": 0.0,
            "hue_plus_pct": 0.0,
            "saturation_minus_pct": 0.0,
            "saturation_plus_pct": 0.0,
            "value_minus_pct": 0.0,
            "value_plus_pct": 0.0
        },
        "sampling": false
    })
}

fn trace_message(message: &Message) -> Option<(&'static str, Value)> {
    let traced = match message {
        Message::Transport(_) => return None,
        Message::SelectWorkflow(workflow) => (
            "workflow.select",
            json!({"workflow": settings_workflow(*workflow)}),
        ),
        Message::SetSourceKind(kind) => (
            "source.kind",
            json!({"source_kind": source_kind_value(*kind)}),
        ),
        Message::EditText(path, value) => (
            "setting.text",
            json!({"path": path.as_str(), "value_length": value.len()}),
        ),
        Message::SetNumber(path, value) => (
            "setting.number",
            json!({"path": path.as_str(), "value": value}),
        ),
        Message::SetBoolean(path, value) => (
            "setting.boolean",
            json!({"path": path.as_str(), "value": value}),
        ),
        Message::SetNumericChoice(path, value) => (
            "setting.numeric_choice",
            json!({"path": path.as_str(), "value": value}),
        ),
        Message::SetPreset(preset) => ("model.preset", json!({"preset": preset})),
        Message::ToggleLocalGpu(device_id, selected) => (
            "train.local_gpu.select",
            json!({"device_id": device_id, "selected": selected}),
        ),
        Message::ToggleRemoteFamily(index, enabled) => (
            "train.remote.family",
            json!({"index": index, "enabled": enabled}),
        ),
        Message::ArmRemoteOffer(offer_id) => {
            ("train.remote.offer.arm", json!({"offer_id": offer_id}))
        }
        Message::EditAnnotationCategory(value) => (
            "annotate.category.edit",
            json!({"value_length": value.len()}),
        ),
        Message::AddAnnotationCategory => ("annotate.category.add", json!({})),
        Message::SetMaskCleanupRadius(radius) => {
            ("annotate.mask.cleanup_radius", json!({"radius": radius}))
        }
        Message::SetMaskColorNumber(range, field, value) => (
            "annotate.mask.color_range",
            json!({"range": format!("{range:?}"), "field": format!("{field:?}"), "value": value}),
        ),
        Message::SetMaskColorSampling(range, enabled) => (
            "annotate.mask.color_sampling",
            json!({"range": format!("{range:?}"), "enabled": enabled}),
        ),
        Message::ResetMaskColorRange(range) => (
            "annotate.mask.color_reset",
            json!({"range": format!("{range:?}")}),
        ),
        Message::AnnotationSidebar(action, payload) => (
            "annotate.sidebar",
            json!({
                "action": action,
                "payload_keys": payload
                    .as_object()
                    .map(|object| object.keys().cloned().collect::<Vec<_>>())
            }),
        ),
        Message::SetBrushRadius(radius) => ("annotate.brush_radius", json!({"radius": radius})),
        Message::OpenAccentColorPicker => ("settings.accent.open", json!({})),
        Message::CancelAccentColorPicker => ("settings.accent.cancel", json!({})),
        Message::SetAccentColor(color) => ("settings.accent", json!({"color": color.to_string()})),
        Message::SetFitToCapture(enabled) => ("live.fit_to_capture", json!({"enabled": enabled})),
        Message::SetFullFrameDisplay(enabled) => {
            ("live.full_frame_display", json!({"enabled": enabled}))
        }
        Message::SelectTool(tool) => ("annotate.tool", json!({"tool": tool.id()})),
        Message::SetWorkspaceAspectRatio(ratio) => {
            ("workspace.aspect_ratio", json!({"ratio": ratio.label()}))
        }
        Message::WorkspacePointer {
            phase,
            capture_x,
            capture_y,
            ..
        } => (
            "workspace.pointer",
            json!({"phase": phase, "capture_x": capture_x, "capture_y": capture_y}),
        ),
        Message::PersistUiSetting(path) => ("setting.persist", json!({"path": path.as_str()})),
        Message::ToggleSettings => ("settings.toggle", json!({})),
        Message::Browse(dialog) => ("file_dialog.open", json!({"dialog_id": dialog.spec().id})),
        Message::ChooseCustomWeights => ("model.custom.open", json!({})),
        Message::ConfirmCustomWeights => ("model.custom.confirm", json!({})),
        Message::CancelCustomWeights => ("model.custom.cancel", json!({})),
        Message::RequestSettingsReset => ("settings.reset.request", json!({})),
        Message::ConfirmSettingsReset => ("settings.reset.confirm", json!({})),
        Message::CancelSettingsReset => ("settings.reset.cancel", json!({})),
        Message::DismissError => ("error.dismiss", json!({})),
        Message::CopyError => ("error.copy", json!({})),
        Message::CopyTextSelection(id) => ("text.copy", json!({"input_id": id})),
        Message::CutTextSelection(id) => ("text.cut", json!({"input_id": id})),
        Message::PasteTextSelection(id) => ("text.paste", json!({"input_id": id})),
        Message::SelectAllText(id) => ("text.select_all", json!({"input_id": id})),
        Message::TextSelectionResolved(id, selection) => (
            "text.copy.resolve",
            json!({
                "input_id": id,
                "success": selection.is_some(),
                "byte_count": selection.as_ref().map_or(0, String::len)
            }),
        ),
        Message::TextCutResolved(id, cut) => (
            "text.cut.resolve",
            json!({
                "input_id": id,
                "success": cut.is_some(),
                "byte_count": cut.as_ref().map_or(0, |cut| cut.selection.len()),
                "result_byte_count": cut.as_ref().map_or(0, |cut| cut.value.len())
            }),
        ),
        Message::ClipboardTextRead(id, result) => match result {
            Ok(value) => (
                "text.paste.clipboard",
                json!({"input_id": id, "success": true, "byte_count": value.len()}),
            ),
            Err(error) => (
                "text.paste.clipboard",
                json!({"input_id": id, "success": false, "error": error}),
            ),
        },
        Message::TextPasteResolved(id, value) => (
            "text.paste.resolve",
            json!({
                "input_id": id,
                "success": value.is_some(),
                "byte_count": value.as_ref().map_or(0, String::len)
            }),
        ),
        Message::ClipboardWriteResolved(operation, target, byte_count, error) => (
            "clipboard.write.resolve",
            json!({
                "operation": operation,
                "target": target,
                "success": error.is_none(),
                "byte_count": byte_count,
                "error": error
            }),
        ),
        Message::ClipboardRuntimeResolved(operation, target, byte_count, error) => (
            "clipboard.runtime.resolve",
            json!({
                "operation": operation,
                "target": target,
                "success": error.is_none(),
                "byte_count": byte_count,
                "error": error
            }),
        ),
        Message::WorkflowPersistDebounced(workflow, generation) => (
            "workflow_settings.persist",
            json!({"workflow": workflow, "generation": generation}),
        ),
        Message::FieldInteracted(path) => ("field.interacted", json!({"path": path.as_str()})),
        Message::Run(action) => ("action.activate", json!({"action": action_label(*action)})),
    };
    Some(traced)
}

const fn action_label(action: Action) -> &'static str {
    match action {
        Action::Primary => "primary",
        Action::ApplyShellSettings => "shell_settings.apply",
        Action::CompileDataset => "dataset.compile",
        Action::CancelDatasetCompile => "dataset.compile.cancel",
        Action::RefreshLocalGpus => "train.local_gpu.refresh",
        Action::QueryRemoteOffers => "train.remote.query",
        Action::ClearRemoteOffer => "train.remote.offer.clear",
        Action::SaveAnnotation => "annotate.save_now",
        Action::ToggleLiveAnnotation => "annotate.live.toggle",
        Action::SetupPrevious => "annotate.setup.previous",
        Action::SetupReload => "annotate.setup.reload",
        Action::SetupNext => "annotate.setup.next",
        Action::ToggleHoldSave => "annotate.hold_save",
    }
}
