//! Session-only observations. Rows and source acknowledgements have separate lifetimes.
use super::{ApplicationIntentEndpoint, UiError, UiErrorKind};
use crate::generated::FeatureId;
use std::collections::VecDeque;
use std::sync::Arc;

pub const CAPACITY: usize = 128;
const LOCAL_TEXT_LIMIT: usize = 64 * 1024;
const SOURCE_CAPACITY: usize = 192;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct NoticeId(pub u64);
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity { Warning, Error }
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Origin {
    Compute(FeatureId), Dataset, Model(FeatureId), Provider, Remote, Checkpoint,
    Dialog, AnnotationSave, Annotation, Explore, Presentation, Upscale, PredictionInspection, PredictionPreview,
    Request(ApplicationIntentEndpoint, u64), Interaction(u64), History, HistoryDropped, HistoryDroppedSaved, Chart, ChartSaved, Gpu(FeatureId), Settings,
    Transport, Protocol, Clipboard, Admission(ApplicationIntentEndpoint), Editor(FeatureId), PredictionTotal, Overflow,
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
    presentation: Presentation,
}
impl Notice {
    pub fn presentation(&self) -> &Presentation { &self.presentation }
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
    run: Option<String>,
}
/// Identity of retained row content, independent of source frontiers and copy attempts.
/// Clones share it until either store changes, so branching fixture/bootstrap models
/// cannot alias different content at the same local revision. Retained readers keep
/// the identity alive; it cannot wrap or be reused while a cache still holds it.
#[derive(Debug, Clone)]
pub struct Presentation(Arc<()>);
impl Default for Presentation {
    fn default() -> Self { Self(Arc::new(())) }
}
impl PartialEq for Presentation {
    fn eq(&self, other: &Self) -> bool { Arc::ptr_eq(&self.0, &other.0) }
}
impl Eq for Presentation {}
#[derive(Debug, Clone)]
pub struct NoticeStore {
    presentation: Presentation,
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
        Self { presentation: Presentation::default(), rows: VecDeque::with_capacity(CAPACITY), overflow: None, evicted: 0,
            next_id: 1, frontiers: { let mut slots = Vec::with_capacity(SOURCE_CAPACITY); slots.push(Frontier { origin: Origin::Protocol, generation: 0, observed: false, row: None, owner: 0, run: None }); slots }, bootstrapping: false, initialized: false }
    }
}
impl NoticeStore {
    pub fn presentation(&self) -> &Presentation { &self.presentation }
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
    /// Accepted request occurrences are already deduplicated by the pending reply owner.
    /// Their delivery order is independent of their issued correlation order.
    pub(super) fn occurrence(&mut self, endpoint: ApplicationIntentEndpoint, correlation: u64, error: UiError) {
        self.push(Origin::Request(endpoint, correlation), error);
    }
    pub(super) fn bootstrapping(&self) -> bool { self.bootstrapping }
    /// A source has a fixed slot; operation identities never accumulate in an acknowledgement set.
    pub fn terminal(&mut self, origin: Origin, owner: u64, generation: u64, error: impl FnOnce() -> Option<UiError>) {
        if self.frontiers.len() == SOURCE_CAPACITY && !self.frontiers.iter().any(|slot| slot.origin == origin) {
            self.local(Origin::Protocol, UiError::protocol("Notification source capacity exhausted")); return;
        }
        let index = self.frontiers.iter().position(|slot| slot.origin == origin).unwrap_or_else(|| {
            self.frontiers.push(Frontier { origin, generation: 0, observed: false, row: None, owner, run: None });
            self.frontiers.len() - 1
        });
        let slot = &mut self.frontiers[index];
        let rebase = slot.owner != owner || (self.bootstrapping && generation < slot.generation);
        if rebase { slot.owner = owner; slot.generation = generation; slot.observed = false; slot.row = None; }
        if generation < slot.generation { return; }
        if generation > slot.generation { slot.generation = generation; slot.observed = false; slot.row = None; }
        if self.bootstrapping && (!self.initialized || rebase) { slot.observed = true; return; }
        if slot.observed && slot.row.is_none() { return; }
        let Some(error) = error() else { return; };
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
        self.terminal(origin, owner, frame, || error);
        if arm && let Some(slot) = self.frontiers.iter_mut().find(|slot| slot.origin == origin) { slot.observed = false; }
    }
    /// Clearing is evidence from the owning action/component, never unrelated input.
    pub fn clear_condition(&mut self, origin: Origin) {
        if let Some(slot) = self.frontiers.iter_mut().find(|slot| slot.origin == origin) {
            if slot.observed {
                if let Some(next) = slot.generation.checked_add(1) {
                    slot.generation = next;
                    slot.observed = false;
                    slot.row = None;
                }
            }
        }
    }
    /// Prepare payloads only for an active condition with an eligible retained row.
    pub fn condition(&mut self, origin: Origin, active: bool, error: impl FnOnce() -> UiError) {
        self.owned_condition(origin, 0, active, error);
    }
    pub fn owned_condition(&mut self, origin: Origin, owner: u64, active: bool, error: impl FnOnce() -> UiError) {
        let generation = self.frontiers.iter().find(|slot| slot.origin == origin).map_or(1, |slot| slot.generation);
        self.terminal(origin, owner, generation, || None);
        if active { self.terminal(origin, owner, generation, || Some(error())); }
        else { self.clear_condition(origin); }
    }
    /// Native run names are authoritative ownership, retained once per bounded source.
    pub fn run_condition(&mut self, origin: Origin, run: &str, active: bool, error: impl FnOnce() -> UiError) {
        if !self.frontiers.iter().any(|slot| slot.origin == origin) { self.terminal(origin, 0, 1, || None); }
        let Some(slot) = self.frontiers.iter_mut().find(|slot| slot.origin == origin) else { return; };
        if slot.run.as_deref() != Some(run) {
            let Some(generation) = slot.generation.checked_add(1) else { return; };
            slot.generation = generation;
            slot.run = Some(run.to_owned());
            slot.observed = self.bootstrapping;
            slot.row = None;
        }
        self.condition(origin, active, error);
    }
    pub fn local(&mut self, origin: Origin, error: UiError) {
        if error.title.len().checked_add(error.detail.len()).is_none_or(|size| size > LOCAL_TEXT_LIMIT) {
            self.condition(Origin::Protocol, true, || UiError::protocol("Local notification exceeds the 64 KiB payload limit."));
        } else { self.condition(origin, true, || error); }
    }
    fn push(&mut self, origin: Origin, error: UiError) -> Option<NoticeId> {
        let id = self.allocate()?;
        if self.rows.len() == CAPACITY {
            if let Some(row) = self.rows.pop_front() { self.acknowledge(row.id); }
            self.evicted = self.evicted.saturating_add(1);
            self.update_overflow();
        }
        self.presentation = Presentation::default();
        self.rows.push_back(Notice { id, origin, severity: severity(&error), error,
            content_version: 1, occurrences: 1, copy_attempt: 0, presentation: self.presentation.clone() });
        Some(id)
    }
    fn update(&mut self, id: NoticeId, error: UiError) {
        if let Some(row) = self.get_mut(id) {
            if row.error == error { return; }
            let Some(version) = row.content_version.checked_add(1) else { return; };
            row.content_version = version; row.severity = severity(&error); row.error = error;
            row.presentation = Presentation::default();
            self.presentation = row.presentation.clone();
        }
    }
    fn update_overflow(&mut self) {
        let error = UiError { kind: UiErrorKind::Busy, title: "Older notifications removed",
            detail: format!("{} older notifications were removed from this session's bounded history.", self.evicted) };
        if let Some(row) = &self.overflow { self.update(row.id, error); }
        else if let Some(id) = self.allocate() {
            self.presentation = Presentation::default();
            self.overflow = Some(Notice { id, origin: Origin::Overflow, severity: Severity::Warning,
                error, content_version: 1, occurrences: self.evicted, copy_attempt: 0, presentation: self.presentation.clone() });
        }
        if let Some(row) = &mut self.overflow && row.occurrences != self.evicted {
            row.occurrences = self.evicted;
            self.presentation = Presentation::default();
        }
    }
    fn acknowledge(&mut self, id: NoticeId) {
        for slot in &mut self.frontiers { if slot.row == Some(id) { slot.row = None; } }
    }
    pub fn dismiss(&mut self, id: NoticeId) {
        let before = self.len();
        if self.overflow.as_ref().is_some_and(|row| row.id == id) { self.overflow = None; self.evicted = 0; }
        else { self.rows.retain(|row| row.id != id); }
        self.acknowledge(id);
        if self.len() != before { self.presentation = Presentation::default(); }
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
        if success { self.clear_condition(Origin::Clipboard); }
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
