//! Session-only observations. Rows and source acknowledgements have separate lifetimes.
use super::{ApplicationIntentEndpoint, UiError, UiErrorKind};
use crate::generated::FeatureId;
use std::collections::VecDeque;

pub const CAPACITY: usize = 128;
const LOCAL_TEXT_LIMIT: usize = 64 * 1024;
const SOURCE_CAPACITY: usize = 192;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NoticeId(pub u64);
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity { Warning, Error }
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Origin {
    Compute(FeatureId), Dataset, Model(FeatureId), Provider, Remote, Checkpoint,
    Dialog, AnnotationSave, Annotation, Explore, Presentation, Upscale, PredictionInspection, PredictionPreview,
    Request(ApplicationIntentEndpoint), Interaction(u64), History, HistoryDropped, HistoryDroppedSaved, Chart, ChartSaved, Gpu(FeatureId), Settings,
    Transport, Protocol, Clipboard, Local(UiErrorKind), Overflow,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Notice {
    pub id: NoticeId,
    pub origin: Origin,
    pub severity: Severity,
    pub error: UiError,
    pub content_version: u64,
    pub occurrences: u64,
    copy_attempt: u64,
}
impl std::ops::Deref for Notice {
    type Target = UiError;
    fn deref(&self) -> &Self::Target { &self.error }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CopyToken { pub id: NoticeId, pub content_version: u64, pub attempt: u64 }
#[derive(Debug, Clone)]
struct Frontier {
    origin: Origin,
    generation: u64,
    observed: bool,
    row: Option<NoticeId>,
    owner: u64,
    detail_event: Option<u64>,
    run: Option<String>,
}
#[derive(Debug, Clone)]
pub struct NoticeStore {
    rows: VecDeque<Notice>,
    overflow: Option<Notice>,
    evicted: u64,
    next_id: u64,
    frontiers: Vec<Frontier>,
    bootstrapping: bool,
    initialized: bool,
}
impl Default for NoticeStore {
    fn default() -> Self {
        Self { rows: VecDeque::with_capacity(CAPACITY), overflow: None, evicted: 0,
            next_id: 1, frontiers: { let mut slots = Vec::with_capacity(SOURCE_CAPACITY); slots.push(Frontier { origin: Origin::Protocol, generation: 0, observed: false, row: None, owner: 0, detail_event: None, run: None }); slots }, bootstrapping: false, initialized: false }
    }
}
impl NoticeStore {
    pub(super) fn observe_compute(&mut self, feature: FeatureId, state: &crate::generated::ComputeTerminal) {
        use crate::generated::ComputeOperationOutcome as Outcome;
        let error = match state.outcome {
            Outcome::Failed => Some(failure(state.detail.clone())),
            Outcome::Refused => Some(warning("Operation refused", state.detail.clone())),
            _ => None,
        };
        self.observe(Origin::Compute(feature), 0, state.generation, error);
    }
    pub fn rows(&self) -> impl DoubleEndedIterator<Item=&Notice> { self.rows.iter().chain(self.overflow.iter()) }
    pub fn len(&self) -> usize { self.rows.len() + usize::from(self.overflow.is_some()) }
    pub fn is_empty(&self) -> bool { self.rows.is_empty() && self.overflow.is_none() }
    pub fn latest(&self) -> Option<&Notice> { self.rows.back().or(self.overflow.as_ref()) }
    pub fn get(&self, id: NoticeId) -> Option<&Notice> { self.rows().find(|row| row.id == id) }
    fn get_mut(&mut self, id: NoticeId) -> Option<&mut Notice> {
        self.rows.iter_mut().chain(self.overflow.iter_mut()).find(|row| row.id == id)
    }
    fn allocate(&mut self) -> Option<NoticeId> {
        let next = self.next_id.checked_add(1)?;
        let id = NoticeId(self.next_id); self.next_id = next; Some(id)
    }
    pub(super) fn begin_bootstrap(&mut self) { self.bootstrapping = true; }
    pub(super) fn end_bootstrap(&mut self) { self.bootstrapping = false; self.initialized = true; }
    /// A source has a fixed slot; operation identities never accumulate in an acknowledgement set.
    pub fn observe(&mut self, origin: Origin, owner: u64, generation: u64, error: Option<UiError>) {
        if self.frontiers.len() == SOURCE_CAPACITY && !self.frontiers.iter().any(|slot| slot.origin == origin) {
            self.local(Origin::Protocol, UiError::protocol("Notification source capacity exhausted")); return;
        }
        let index = self.frontiers.iter().position(|slot| slot.origin == origin).unwrap_or_else(|| {
            self.frontiers.push(Frontier { origin, generation: 0, observed: false, row: None, owner, detail_event: None, run: None });
            self.frontiers.len() - 1
        });
        let slot = &mut self.frontiers[index];
        let rebase = slot.owner != owner || (self.bootstrapping && generation < slot.generation);
        if rebase { slot.owner = owner; slot.generation = generation; slot.observed = false; slot.row = None; slot.detail_event = None; }
        if generation < slot.generation { return; }
        if generation > slot.generation { slot.generation = generation; slot.observed = false; slot.row = None; slot.detail_event = None; }
        if self.bootstrapping && (!self.initialized || rebase) { slot.observed = true; return; }
        let Some(error) = error else { return; };
        if slot.observed {
            if let Some(id) = slot.row { self.update(id, error); }
            return;
        }
        slot.observed = true;
        let row = self.push(origin, error);
        self.frontiers[index].row = row;
    }
    /// A visual condition clears when its owner successfully publishes another frame.
    /// A first snapshot cannot acknowledge event-only detail that it does not carry.
    pub fn visual_condition(&mut self, origin: Origin, owner: u64, frame: u64, error: Option<UiError>) {
        let arm = self.bootstrapping && error.is_none() && (!self.initialized || self.frontiers.iter().find(|slot| slot.origin == origin).is_none_or(|slot| slot.owner != owner || frame < slot.generation));
        self.observe(origin, owner, frame, error);
        if arm && let Some(slot) = self.frontiers.iter_mut().find(|slot| slot.origin == origin) { slot.observed = false; }
    }
    pub(super) fn annotation_save_event(&mut self, revision: u64) -> bool {
        let Some(slot) = self.frontiers.iter_mut().find(|slot| slot.origin == Origin::AnnotationSave) else { return false; };
        if let Some(event) = slot.detail_event { return event == revision; }
        slot.detail_event = Some(revision);
        true
    }
    /// Local/retained conditions are rearmed only by their owning component clearing the episode.
    pub fn condition(&mut self, origin: Origin, error: Option<UiError>) {
        self.owned_condition(origin, 0, error);
    }
    pub fn owned_condition(&mut self, origin: Origin, owner: u64, error: Option<UiError>) {
        let generation = self.frontiers.iter().find(|slot| slot.origin == origin).map_or(1, |slot| slot.generation);
        self.observe(origin, owner, generation, None);
        if error.is_none() {
            if let Some(slot) = self.frontiers.iter_mut().find(|slot| slot.origin == origin) {
                if slot.observed { if let Some(next) = slot.generation.checked_add(1) { slot.generation = next; slot.observed = false; slot.row = None; slot.detail_event = None; } }
            }
        } else { self.observe(origin, owner, generation, error); }
    }
    /// Native run names are authoritative ownership, retained once per bounded source.
    pub fn run_condition(&mut self, origin: Origin, run: &str, error: Option<UiError>) {
        if !self.frontiers.iter().any(|slot| slot.origin == origin) { self.observe(origin, 0, 1, None); }
        let Some(slot) = self.frontiers.iter_mut().find(|slot| slot.origin == origin) else { return; };
        if slot.run.as_deref() != Some(run) {
            let Some(generation) = slot.generation.checked_add(1) else { return; };
            slot.generation = generation;
            slot.run = Some(run.to_owned());
            slot.observed = self.bootstrapping;
            slot.row = None;
        }
        self.condition(origin, error);
    }
    pub fn local(&mut self, origin: Origin, error: UiError) {
        if error.title.len().checked_add(error.detail.len()).is_none_or(|size| size > LOCAL_TEXT_LIMIT) {
            self.condition(Origin::Protocol, Some(UiError::protocol("Local notification exceeds the 64 KiB payload limit.")));
        } else { self.condition(origin, Some(error)); }
    }
    fn push(&mut self, origin: Origin, error: UiError) -> Option<NoticeId> {
        let id = self.allocate()?;
        if self.rows.len() == CAPACITY {
            if let Some(row) = self.rows.pop_front() { self.acknowledge(row.id); }
            self.evicted = self.evicted.saturating_add(1);
            self.update_overflow();
        }
        self.rows.push_back(Notice { id, origin, severity: severity(&error), error,
            content_version: 1, occurrences: 1, copy_attempt: 0 });
        Some(id)
    }
    fn update(&mut self, id: NoticeId, error: UiError) {
        if let Some(row) = self.get_mut(id) {
            if row.error == error { return; }
            let Some(version) = row.content_version.checked_add(1) else { return; };
            row.content_version = version; row.severity = severity(&error); row.error = error;
        }
    }
    fn update_overflow(&mut self) {
        let error = UiError { kind: UiErrorKind::Busy, title: "Older notifications removed",
            detail: format!("{} older notifications were removed from this session's bounded history.", self.evicted) };
        if let Some(row) = &self.overflow { self.update(row.id, error); }
        else if let Some(id) = self.allocate() {
            self.overflow = Some(Notice { id, origin: Origin::Overflow, severity: Severity::Warning,
                error, content_version: 1, occurrences: self.evicted, copy_attempt: 0 });
        }
        if let Some(row) = &mut self.overflow { row.occurrences = self.evicted; }
    }
    fn acknowledge(&mut self, id: NoticeId) {
        for slot in &mut self.frontiers { if slot.row == Some(id) { slot.row = None; } }
    }
    pub fn dismiss(&mut self, id: NoticeId) {
        if self.overflow.as_ref().is_some_and(|row| row.id == id) { self.overflow = None; self.evicted = 0; }
        else { self.rows.retain(|row| row.id != id); }
        self.acknowledge(id);
    }
    #[cfg(test)]
    pub fn dismiss_all(&mut self) { loop { let id = self.rows().next().map(|row| row.id); let Some(id) = id else { break; }; self.dismiss(id); } }
    pub fn begin_copy(&mut self, id: NoticeId) -> Option<(CopyToken, String)> {
        let row = self.get_mut(id)?;
        row.copy_attempt = row.copy_attempt.checked_add(1)?;
        Some((CopyToken { id, content_version: row.content_version, attempt: row.copy_attempt }, format!("{}\n\n{}", row.title, row.detail)))
    }
    pub fn finish_copy(&mut self, token: CopyToken, success: bool) -> bool {
        let Some(row) = self.get(token.id) else { return false; };
        if row.content_version != token.content_version || row.copy_attempt != token.attempt { return false; }
        if success { self.condition(Origin::Clipboard, None); }
        else if row.origin != Origin::Clipboard {
            self.local(Origin::Clipboard, UiError { kind: UiErrorKind::Failed, title: "Cannot copy",
                detail: "The notification could not be written to the clipboard.".into() });
        }
        true
    }
}
fn severity(error: &UiError) -> Severity {
    if error.kind == UiErrorKind::Busy { Severity::Warning } else { Severity::Error }
}
#[cfg(test)] mod tests;

pub fn failure(detail: impl Into<String>) -> UiError {
    UiError { kind: UiErrorKind::Failed, title: "Operation failed", detail: detail.into() }
}
pub fn warning(title: &'static str, detail: impl Into<String>) -> UiError {
    UiError { kind: UiErrorKind::Busy, title, detail: detail.into() }
}
impl super::ApplicationModel {
    pub(super) fn observe_settings_notices(&mut self) {
        let Some(state) = &self.settings_snapshot else { return; };
        for feature in [FeatureId::Train, FeatureId::Validate, FeatureId::Predict, FeatureId::Export] {
            let missing: Vec<_> = super::selected_gpu_ordinals(feature, &state.settingsstate).iter().copied().filter(|ordinal| state.cudadevices.iter().all(|device| device.ordinal != *ordinal)).collect();
            self.notices.condition(Origin::Gpu(feature), (!missing.is_empty()).then(|| warning("Selected GPU unavailable", format!("{feature:?}: selected CUDA device ordinals {missing:?} are unavailable."))));
        }
        let missing = [crate::generated::constraint_uiuiscale(), crate::generated::constraint_uifontsize(), crate::generated::constraint_uisecondaryfontsize(), crate::generated::constraint_uimonofontsize(), crate::generated::constraint_uitextinputfontsize()].into_iter().any(|constraint| super::settings_constraint_bounds(constraint).is_none());
        self.notices.condition(Origin::Settings, missing.then(|| warning("Settings constraints unavailable", "Native range constraints are missing; affected controls are unavailable.")));
    }
    pub(super) fn observe_dataset(&mut self) {
        if let Some(state) = &self.workflow.dataset {
            use crate::generated::ArtifactTerminalOutcome as Outcome;
            let error = match state.terminal.outcome {
                Outcome::Failed => Some(failure(state.terminal.detail.clone())),
                Outcome::Refused => Some(warning("Compilation refused", state.terminal.detail.clone())),
                _ => None,
            };
            self.notices.observe(Origin::Dataset, 0, state.generation, error);
        }
    }
    pub(super) fn observe_training(&mut self) {
        let Some(state) = &self.workflow.training else { return; };
        let offers = (state.offers.outcome == crate::generated::ProviderQueryOutcome::Failed)
            .then(|| failure(state.offers.detail.clone()));
        self.notices.observe(Origin::Provider, 0, state.offers.revision, offers);
        let remote = matches!(state.remote.outcome, crate::generated::RemoteOperationOutcome::Failed | crate::generated::RemoteOperationOutcome::Inconclusive)
            .then(|| failure(state.remote.detail.clone()));
        self.notices.observe(Origin::Remote, 0, state.remote.revision, remote);
        let history = state.persistence.degraded.then(|| warning("History incomplete", format!("{} ({} records dropped)", state.persistence.error, state.persistence.droppedrecords)));
        let run = state.metrics.as_ref().map_or("", |record| record.runid.as_str());
        self.notices.run_condition(Origin::History, run, history);
        // After bootstrap, the retained history owns accumulated drop conditions.
        if self.notices.bootstrapping && !self.notices.initialized {
            let dropped = state.metrics.as_ref().map_or(0, |record| record.droppedbefore);
            self.notices.run_condition(Origin::HistoryDropped, run, (dropped > 0).then(|| warning("History incomplete", format!("{dropped} training records were dropped."))));
        }
        self.notices.observe_compute(FeatureId::Train, &state.local.terminal);
    }
    pub(super) fn observe_checkpoint(&mut self, state: &crate::generated::TrainingCheckpointInspection) {
        use crate::generated::TrainingInspectionStatus;
        let error = match state.status {
            TrainingInspectionStatus::Failed => Some(failure(state.error.clone())),
            TrainingInspectionStatus::Ready if state.checkpoint.is_none() => Some(UiError::protocol("Checkpoint inspection returned no capability.")),
            _ => None,
        };
        self.notices.observe(Origin::Checkpoint, 0, state.generation, error);
    }
    pub(super) fn observe_model(&mut self) {
        if let Some(state) = &self.model_snapshot {
            self.notices.observe(Origin::Model(state.selection.key.workflow), 0, state.generation,
                (state.terminal.outcome == crate::generated::ModelSelectionOutcome::Rejected).then(|| failure(state.terminal.detail.clone())));
        }
    }
    pub(super) fn observe_annotation(&mut self, detail: Option<String>) {
        if let Some(state) = &self.annotation.snapshot {
            use crate::generated::AnnotationSaveStatus;
            let retained_failure = detail.is_none() && self.notices.frontiers.iter().any(|slot| slot.origin == Origin::AnnotationSave && slot.owner == state.inputdocumentepoch && slot.generation == state.ui.savegeneration && slot.observed);
            let error = match state.ui.savestatus {
                AnnotationSaveStatus::Failed if retained_failure => None,
                AnnotationSaveStatus::Failed => Some(failure(detail.unwrap_or_else(|| "Annotation document save failed".into()))),
                AnnotationSaveStatus::Uncertain => Some(warning("Annotation save uncertain", "The annotation save could not be confirmed. Check the destination before trying again.")),
                _ => None,
            };
            self.notices.observe(Origin::AnnotationSave, state.inputdocumentepoch, state.ui.savegeneration, error);
        }
    }
    pub(super) fn observe_predict(&mut self) {
        if let Some(state) = &self.predict_snapshot {
            self.notices.visual_condition(Origin::PredictionPreview, state.operation.generationfrontier, state.frame.revision, None);
            self.notices.condition(Origin::PredictionInspection,
                (!state.inspection.error.is_empty()).then(|| failure(state.inspection.error.clone())));
            self.notices.observe_compute(FeatureId::Predict, &state.operation.terminal);
        }
    }
    pub(super) fn observe_explore(&mut self) {
        if let Some(state) = &self.explore.snapshot {
            let error = (!state.failure.is_empty()).then(|| {
                if state.ready { warning("Explore preview unavailable", state.failure.clone()) }
                else { failure(state.failure.clone()) }
            });
            self.notices.owned_condition(Origin::Explore, state.dataset.identity, error);
        }
    }
    pub(super) fn seed_event_frontiers(&mut self) {
        if let Some(state) = &self.file_dialog { self.notices.observe(Origin::Dialog, 0, state.generation, None); }
        if let Some(state) = &self.live_snapshot { self.notices.observe(Origin::Compute(FeatureId::Live), 0, state.revision, None); }
        if let Some(state) = &self.presentation { self.notices.observe(Origin::Presentation, 0, state.revision, None); }
        if let Some(state) = &self.upscale_snapshot { self.notices.observe(Origin::Upscale, 0, state.revision, None); }
        if let Some(state) = &self.annotation.snapshot { self.notices.observe(Origin::Annotation, state.inputdocumentepoch, state.uirevision, None); }
        self.observe_training(); self.observe_model(); self.observe_dataset(); self.observe_predict(); self.observe_annotation(None); self.observe_explore();
        if let Some(state) = &self.workflow.validation { self.notices.observe_compute(FeatureId::Validate, &state.operation.terminal); }
        if let Some(state) = &self.workflow.export { self.notices.observe_compute(FeatureId::Export, &state.terminal); }
    }
}
