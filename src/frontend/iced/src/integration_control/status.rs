//! Status evidence belongs to the existing accepted browser session.
use super::{Driver, Phase, Message};
use crate::view_model::{ApplicationModel, notices::{CopyToken, NoticeId, Notice, Origin, Severity}, UiErrorKind};
use iced::Task;
const STOP_DETAIL: &str = "The operation is inactive or already changing state.";
const SECOND_DETAIL: &str = "Status acceptance provider failure";
#[derive(Clone, Copy)]
enum Expected { Stop, Oom, Provider, ShortTraining }
impl Expected {
    fn matches(self, notice: &Notice) -> bool {
        let (origin, severity, kind, title, detail) = match self {
            Self::Stop => (Origin::Local(UiErrorKind::Busy), Severity::Warning, UiErrorKind::Busy, "Operation already running", STOP_DETAIL),
            Self::Oom => (Origin::Compute(crate::generated::FeatureId::Train), Severity::Error, UiErrorKind::Failed, "Operation failed", oom_detail()),
            Self::ShortTraining => (Origin::Compute(crate::generated::FeatureId::Train), Severity::Error, UiErrorKind::Failed, "Operation failed", "Status acceptance training failure"),
            Self::Provider => (Origin::Provider, Severity::Error, UiErrorKind::Failed, "Operation failed", SECOND_DETAIL),
        };
        notice.origin == origin && notice.severity == severity && notice.kind == kind && notice.title == title && notice.detail == detail
    }
}
pub(super) fn expected_initial(model: &ApplicationModel, fixture: bool) -> Option<NoticeId> {
    let notice = model.notices.latest()?;
    (model.notices.len() == 1 && (if fixture { Expected::Oom } else { Expected::Stop }).matches(notice)).then_some(notice.id)
}
#[derive(Default)]
pub(super) struct Evidence { allowed: Option<NoticeId>, outage: Option<NoticeId>, outage_removed: bool, payload: Option<String>, pub(super) fixture: Option<ApplicationModel>, stage: u32, waiting: bool, healthy_completed: bool, selection_pending: bool, pair: Vec<(NoticeId, Expected)>, removing: Option<NoticeId>, cleanup_started: bool, dialog_started: bool, fixture_mode: bool, original_dark: Option<bool> }
impl Evidence {
    pub(super) fn begin_healthy(&mut self) -> bool { if self.healthy_completed { false } else { self.stage = 32; self.waiting = false; true } }
    pub(super) fn permits(&mut self, phase: &Phase, model: &ApplicationModel) -> bool {
        if matches!(phase, Phase::Disabled | Phase::Complete | Phase::Failed) { return true; }
        if matches!(phase, Phase::ViewerAwaitDisconnect | Phase::ViewerReconnect) {
            if model.notices.is_empty() {
                if self.outage.is_none() { return matches!(phase, Phase::ViewerAwaitDisconnect) && model.connection == crate::view_model::ConnectionState::Connected; }
                if matches!(phase, Phase::ViewerReconnect) && model.connection == crate::view_model::ConnectionState::Connected { self.outage_removed = true; return true; }
                return false;
            }
            if self.outage_removed { return false; }
            if model.notices.len() != 1 { return false; }
            let notice = model.notices.latest().unwrap();
            if notice.origin != Origin::Transport || notice.severity != Severity::Error || notice.kind != UiErrorKind::Transport || notice.title != "Connection interrupted" || notice.detail != "rendered continuity acceptance" { return false; }
            if self.outage.is_none() { self.outage = Some(notice.id); }
            return Some(notice.id) == self.outage;
        }
        self.outage = None; self.outage_removed = false;
        let status_step = matches!(phase, Phase::AwaitStatusNotice | Phase::StatusTrigger | Phase::StatusPanel | Phase::StatusCopy | Phase::AwaitStatusCopy | Phase::AwaitStatusClipboard | Phase::StatusExercise | Phase::StatusDismiss | Phase::AwaitStatusDismissed);
        if !status_step { self.allowed = None; self.payload = None; return model.notices.is_empty(); }
        if matches!(phase, Phase::AwaitStatusNotice) && self.allowed.is_none() {
            if model.notices.is_empty() { return true; }
            let Some(id) = expected_initial(model, self.fixture_mode) else { return false; };
            self.allowed = Some(id);
            let notice = model.notices.get(id).unwrap();
            self.payload = Some(format!("{}\n\n{}", notice.title, notice.detail));
            super::reporting::emit(|sink| sink.record("integration.status.notice", "status.panel", if self.fixture_mode { "oom-added" } else { "stop-added" }, [id.0 as f64, 1.0, 0.0, 0.0]));
        }
        if matches!(phase, Phase::AwaitStatusDismissed) && model.notices.is_empty() { return true; }
        model.notices.len() == 1 && model.notices.rows().all(|notice| Some(notice.id) == self.allowed && (if self.fixture_mode { Expected::Oom } else { Expected::Stop }).matches(notice))
    }
    pub(super) fn fixture_valid(&self) -> bool {
        let Some(model) = &self.fixture else { return false; };
        if self.stage >= 50 {
            return self.pair.iter().all(|(id, expected)| model.notices.get(*id).is_some_and(|notice| expected.matches(notice)) || Some(*id) == self.removing)
                && model.notices.rows().all(|notice| self.pair.iter().any(|(id, expected)| notice.id == *id && expected.matches(notice)));
        }
        if (32..=36).contains(&self.stage) { return model.notices.is_empty(); }
        model.notices.len() == 1 && model.notices.rows().all(|notice| Some(notice.id) == self.allowed && Expected::Oom.matches(notice))
    }
    pub(super) fn clipboard_read(&mut self, driver: &mut Driver, result: Result<std::sync::Arc<iced::clipboard::Content>, iced::clipboard::Error>) {
        if !matches!(driver.phase, Phase::AwaitStatusClipboard) { return; }
        let exact = result.as_ref().is_ok_and(|content| matches!(content.as_ref(), iced::clipboard::Content::Text(text) if Some(text.as_str()) == self.payload.as_deref()));
        if exact {
            super::reporting::emit(|sink| sink.record("integration.status.clipboard", crate::view::status::PANEL_ID, "exact-title-blank-line-detail", [1.0, 0.0, 0.0, 0.0]));
            driver.phase = Phase::StatusDismiss;
        } else { driver.fail("Status clipboard readback differed from its complete title and detail"); }
    }
}
impl super::Controller {
    pub(crate) fn status_copied(&mut self, token: CopyToken, success: bool) -> Task<crate::message::Message> {
        if !matches!(self.driver.phase, Phase::AwaitStatusCopy) || self.status.allowed != Some(token.id) { return Task::none(); }
        if !success { self.driver.fail("Status Copy failed"); return Task::none(); }
        self.driver.phase = Phase::AwaitStatusClipboard;
        let generation = self.driver.generation;
        iced::clipboard::read(iced::clipboard::Kind::Text).map(move |result| crate::message::Message::Integration(Message::Scoped { generation, receipt: None, message: Box::new(Message::StatusClipboardRead(result)) }))
    }
}

fn oom_detail() -> &'static str {
    static DETAIL: std::sync::OnceLock<String> = std::sync::OnceLock::new();
    DETAIL.get_or_init(|| format!("CUDA out of memory: first line\n{}\nCUDA allocation failed: last line", (0..80).map(|line| if line == 79 { "Allocation 79: /data/training/model/checkpoint/device/allocator/state".into() } else { format!("Allocation {line:02}: CUDA memory") }).collect::<Vec<_>>().join("\n")))
}
pub(super) fn oom_event(model: &ApplicationModel) -> crate::generated::ApplicationEvent {
    let mut snapshot = model.workflow.training.clone().unwrap();
    snapshot.revision += 1; snapshot.local.generationfrontier += 1;
    snapshot.local.terminal.generation = snapshot.local.generationfrontier;
    snapshot.local.terminal.outcome = crate::generated::ComputeOperationOutcome::Failed;
    snapshot.local.active = false;
    snapshot.local.terminal.detail = oom_detail().into();
    crate::generated::ApplicationEvent::TrainingTrainingChanged(crate::generated::TrainingChanged { snapshot })
}
impl Evidence {
    fn add_pair_notice(&mut self, expected: Expected) {
        let model = self.fixture.as_mut().unwrap();
        let mut snapshot = model.workflow.training.clone().unwrap();
        snapshot.revision += 1;
        match expected {
            Expected::Provider => {
                snapshot.offers.revision += 1;
                snapshot.offers.outcome = crate::generated::ProviderQueryOutcome::Failed;
                snapshot.offers.detail = SECOND_DETAIL.into();
            }
            Expected::ShortTraining => {
                snapshot.local.generationfrontier += 1;
                snapshot.local.terminal.generation = snapshot.local.generationfrontier;
                snapshot.local.terminal.outcome = crate::generated::ComputeOperationOutcome::Failed;
                snapshot.local.terminal.detail = "Status acceptance training failure".into();
            }
            _ => unreachable!(),
        }
        model.reduce_event(crate::generated::ApplicationEvent::TrainingTrainingChanged(crate::generated::TrainingChanged { snapshot }));
        if let Some(notice) = model.notices.latest().filter(|notice| expected.matches(notice)) {
            self.pair.push((notice.id, expected));
            super::reporting::emit(|sink| sink.record("integration.status.notice", "status.panel", "pair-added", [notice.id.0 as f64, self.pair.len() as f64, 0.0, 0.0]));
        }
    }
    pub(super) fn prepare(&mut self, driver: &mut Driver, model: &ApplicationModel, settings: &crate::view::settings::SettingsModel) -> bool {
        if self.waiting {
            if !self.fixture_valid() { driver.fail("Status callback changed undeclared notice identities or content"); return false; }
            return true;
        }
        if matches!(self.stage, 38 | 44) && (settings.has_local_edits() || model.native_settings_unsettled()) { return false; }
        if self.stage == 44 { self.dialog_started = true; }
        if self.dialog_started {
            let fixture = self.fixture.as_mut().unwrap();
            fixture.clear_dialog_context();
            if let Some(context) = model.dialog_context() {
                if let crate::generated::FileDialogTarget::SettingsFieldTarget(target) = context.target {
                    if let Some(fact) = crate::generated::FILE_DIALOGS.iter().find(|fact| fact.stable_field_id == target.stableid) {
                        if fixture.register_dialog(fact, crate::generated::FeatureId::Train).is_err() { driver.fail("Status dialog context projection failed"); return false; }
                    }
                }
            }
            fixture.file_dialog = model.file_dialog.clone();
        }
        if self.stage == 45 && !model.file_dialog.as_ref().is_some_and(|dialog| dialog.active) { return false; }
        if self.stage == 49 && model.dialog_context().is_some() { return false; }
        if self.stage == 50 && self.pair.is_empty() { self.add_pair_notice(Expected::ShortTraining); self.add_pair_notice(Expected::Provider); }
        if self.stage == 53 && self.pair.len() == 1 { self.add_pair_notice(Expected::ShortTraining); }
        if matches!(self.stage, 52 | 55 | 57) {
            if let Some(id) = self.removing.take() {
                if self.fixture.as_ref().unwrap().notices.get(id).is_some() { driver.fail("Status row dismissal did not remove its exact identity"); return false; }
                self.pair.retain(|(expected, _)| *expected != id);
                super::reporting::emit(|sink| sink.record("integration.status.notice", "status.panel", "pair-removed", [id.0 as f64, self.pair.len() as f64, 0.0, 0.0]));
            }
        }
        if (matches!(self.stage, 50 | 53) && self.pair.len() != 2) || !self.fixture_valid() { driver.fail("Status fixture notice identity/content/delta mismatch"); return false; }
        true
    }
    pub(super) fn cleanup(&mut self, model: &ApplicationModel) -> Task<crate::message::Message> {
        if !self.fixture_mode { return Task::none(); }
        self.fixture = None;
        let mut tasks = Vec::new();
        if !self.cleanup_started {
            self.cleanup_started = true;
            #[cfg(target_arch = "wasm32")]
            status_restore_js();
            tasks.push(Task::done(crate::message::Message::Settings(crate::view::settings::Message::ResetCancelled)));
            tasks.push(Task::done(crate::message::Message::Settings(crate::view::settings::Message::Close)));
            if let Some(dark) = self.original_dark { tasks.push(Task::done(crate::message::Message::Settings(crate::view::settings::Message::DarkModeChanged(dark)))); }
        }
        // An Open reply may arrive after cancellation. Keep the cleanup owner
        // until the real dialog becomes stoppable or its context is retired.
        if self.dialog_started && model.dialog_stop_available() { tasks.push(Task::done(crate::message::Message::FileDialog(crate::view::file_dialog::Message::StopRequested))); }
        if model.dialog_context().is_none() { self.dialog_started = false; }
        Task::batch(tasks)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn generated_oom_fixture_uses_the_live_reducer_and_exact_notice_allowlist() {
        let mut model = crate::view_model::test_support::bootstrapped();
        model.reduce_event(oom_event(&model));
        let mut evidence = Evidence::default();
        evidence.fixture = Some(model.clone());
        evidence.fixture_mode = true;
        assert!(evidence.permits(&Phase::AwaitStatusNotice, &model));
        let notice = model.notices.latest().unwrap();
        assert!(notice.detail.starts_with("CUDA out of memory: first line\n"));
        assert!(notice.detail.ends_with("CUDA allocation failed: last line"));
        assert!(notice.detail.contains("Allocation 40:"));
        model.report_error(crate::view_model::UiError::protocol("unexpected"));
        assert!(!evidence.permits(&Phase::StatusCopy, &model));
    }

    #[test]
    fn inactive_stop_requires_exact_source_severity_detail_and_identity() {
        let mut model = crate::view_model::test_support::bootstrapped();
        model.report_error(crate::view_model::UiError::busy("unrelated warning"));
        let mut evidence = Evidence::default();
        assert!(!evidence.permits(&Phase::AwaitStatusNotice, &model));
        model.notices.dismiss_all();
        model.notices.condition(Origin::Local(UiErrorKind::Busy), None);
        model.report_error(crate::view_model::UiError::busy(STOP_DETAIL));
        assert!(evidence.permits(&Phase::AwaitStatusNotice, &model));
        let id = evidence.allowed.unwrap();
        model.notices.dismiss(id);
        assert!(!evidence.permits(&Phase::StatusCopy, &model));
        assert!(evidence.permits(&Phase::AwaitStatusDismissed, &model));
        model.notices.condition(Origin::Local(UiErrorKind::Busy), None);
        model.report_error(crate::view_model::UiError::busy(STOP_DETAIL));
        assert!(!evidence.permits(&Phase::AwaitStatusDismissed, &model));
    }

    #[test]
    fn generated_pair_allows_only_declared_row_removal() {
        let mut evidence = Evidence { fixture: Some(crate::view_model::test_support::bootstrapped()), stage: 50, ..Evidence::default() };
        evidence.add_pair_notice(Expected::ShortTraining);
        evidence.add_pair_notice(Expected::Provider);
        assert_eq!(evidence.pair.len(), 2);
        assert!(evidence.fixture_valid());
        let first = evidence.pair[0].0;
        evidence.fixture.as_mut().unwrap().notices.dismiss(first);
        assert!(!evidence.fixture_valid());
        evidence.removing = Some(first);
        assert!(evidence.fixture_valid());
        evidence.fixture.as_mut().unwrap().report_error(crate::view_model::UiError::protocol("unexpected"));
        assert!(!evidence.fixture_valid());
    }

    #[test]
    fn reconnect_permits_only_the_exact_expected_transport_episode() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut evidence = Evidence::default();
        model.peer_disconnected(crate::view_model::UiError::transport("rendered continuity acceptance"));
        assert!(evidence.permits(&Phase::ViewerAwaitDisconnect, &model));
        assert!(evidence.permits(&Phase::ViewerReconnect, &model));
        model.report_error(crate::view_model::UiError::protocol("unexpected during reconnect"));
        assert!(!evidence.permits(&Phase::ViewerReconnect, &model));
    }
}

impl super::Controller {
    pub(crate) fn status_model<'a>(&'a self, model: &'a ApplicationModel) -> &'a ApplicationModel { self.status.fixture.as_ref().unwrap_or(model) }
    pub(crate) fn status_notices<'a>(&'a mut self, notices: &'a mut crate::view_model::notices::NoticeStore) -> &'a mut crate::view_model::notices::NoticeStore { self.status.fixture.as_mut().map_or(notices, |model| &mut model.notices) }
    pub(super) fn begin_status_fixture(&mut self, model: &ApplicationModel) {
        let mut fixture = model.clone(); fixture.reduce_event(oom_event(&fixture));
        self.status.original_dark = model.settings_snapshot.as_ref().map(|state| state.settingsstate.ui.darkmode);
        self.status.fixture = Some(fixture); self.status.fixture_mode = true; self.status.allowed = None; self.status.payload = None;
        self.driver.phase = Phase::AwaitStatusNotice;
    }
}

#[cfg(target_arch = "wasm32")]
#[wasm_bindgen::prelude::wasm_bindgen(module = "/src/integration_control/browser.mjs")]
extern "C" {
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationStatusRestore)]
    fn status_restore_js();
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationStatusDraw)]
    fn status_draw_js(control: &str, facts: &[f64]);
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationStatusExercise)]
    fn status_exercise_js(stage: u32, bounds: &[f64], completed: &wasm_bindgen::JsValue);
}
pub(crate) fn control_draw(control: crate::view::status::Control, bounds: iced::Rectangle, dark: bool, alert: bool, environment: crate::view::status::environment::State, style: iced::widget::button::Style) {
    if !super::reporting_enabled() { return; }
    #[cfg(target_arch = "wasm32")]
    if matches!(control, crate::view::status::Control::Trigger | crate::view::status::Control::Settings | crate::view::status::Control::Close) {
        let color = match style.background { Some(iced::Background::Color(color)) => color, _ => iced::Color::TRANSPARENT };
        status_draw_js(&control.id(), &[bounds.x as f64, bounds.y as f64, bounds.width as f64, bounds.height as f64, dark as u8 as f64, alert as u8 as f64, environment.visible as u8 as f64, environment.reduced_motion as u8 as f64, color.r as f64, color.g as f64, color.b as f64, style.border.width as f64]);
    }
    #[cfg(not(target_arch = "wasm32"))]
    let _ = (control, bounds, dark, alert, environment, style);
}
impl Evidence {
    pub(super) fn exercise(&mut self, driver: &Driver) -> Task<crate::message::Message> {
        if self.waiting { return Task::none(); }
        self.waiting = true;
        let stage = self.stage;
        if self.selection_pending {
            let generation = driver.generation;
            return iced::clipboard::read(iced::clipboard::Kind::Text).map(move |result| crate::message::Message::Integration(Message::Scoped { generation, receipt: None, message: Box::new(Message::StatusSelectionRead(result)) }));
        }
        let id = self.allowed.expect("Status fixture observation").0;
        let first = self.pair.first().map_or(id, |(id, _)| id.0);
        let last = self.pair.last().map_or(id, |(id, _)| id.0);
        let controls = [crate::view::status::TRIGGER_ID.to_owned(), "navigation.settings".into(), crate::view::status::PANEL_ID.into(), format!("status.{id}.detail"), crate::view::status::CLOSE_ID.into(), "train.primary".into(), "navigation.train".into(), "settings.modal".into(), "settings.close".into(), "settings.dark_mode".into(), "navigation.live".into(), "train.offers.find".into(), "train.offers.clear".into(), "train.remote.start".into(), "train.remote.stop".into(), "train.remote.retry_reconciliation".into(), "settings.reset".into(), "settings.reset.confirm".into(), "dialog.file.active".into(), "dialog.file.stop".into(), "train.dataset.browse".into(), format!("status.{first}.copy"), format!("status.{first}.dismiss"), format!("status.{last}.copy"), format!("status.{last}.dismiss")];
        let mut task = Task::done(Vec::new());
        for control in controls { task = task.then(move |mut bounds| super::widget_ops::measure_control(control.clone()).map(move |measured| { bounds.push(measured.target); bounds.clone() })); }
        let generation = driver.generation;
        let measurement = task.map(move |bounds| crate::message::Message::Integration(Message::Scoped { generation, receipt: None, message: Box::new(Message::StatusMeasured { stage, bounds }) }));
        let focus = match stage {
            11 | 50 => Some(crate::view::status::TRIGGER_ID.to_owned()),
            51 | 56 => { self.removing = Some(NoticeId(first)); Some(format!("status.{first}.dismiss")) },
            54 => { self.removing = Some(NoticeId(last)); Some(format!("status.{last}.dismiss")) },
            _ => None,
        };
        if let Some(control) = focus { iced::widget::operation::focus(control).chain(measurement) } else { measurement }
    }
    pub(super) fn measured(&mut self, driver: &mut Driver, stage: u32, bounds: Vec<iced::Rectangle>) {
        if stage != self.stage || !matches!(driver.phase, Phase::StatusExercise) { return; }
        #[cfg(target_arch = "wasm32")]
        if let Some(mut output) = super::probe::scenario_output() {
            output.receipt = None;
            let callback = wasm_bindgen::closure::Closure::once_into_js(move |success: bool| output.send(Message::StatusExercised { stage, success }));
            let bounds: Vec<_> = bounds.into_iter().flat_map(|rect| [rect.x as f64, rect.y as f64, rect.width as f64, rect.height as f64]).collect();
            status_exercise_js(stage, &bounds, &callback);
        }
        #[cfg(not(target_arch = "wasm32"))]
        let _ = bounds;
    }
    pub(super) fn selection_read(&mut self, driver: &mut Driver, result: Result<std::sync::Arc<iced::clipboard::Content>, iced::clipboard::Error>) {
        if !self.selection_pending || !matches!(driver.phase, Phase::StatusExercise) { return; }
        self.selection_pending = false; self.waiting = false;
        let expected = self.payload.as_deref().and_then(|payload| payload.split_once("\n\n")).map(|(_, detail)| detail);
        if !result.as_ref().is_ok_and(|content| matches!(content.as_ref(), iced::clipboard::Content::Text(text) if Some(text.as_str()) == expected)) { driver.fail("Status text selection did not copy every original detail line"); return; }
        super::reporting::emit(|sink| sink.record("integration.status.selection", crate::view::status::PANEL_ID, "exact-selected-detail", [1.0, 0.0, 0.0, 0.0]));
        self.stage += 1;
    }
    pub(super) fn exercised(&mut self, driver: &mut Driver, stage: u32, success: bool) {
        if stage != self.stage || !matches!(driver.phase, Phase::StatusExercise) { return; }
        self.waiting = false;
        if !success { driver.fail("Status rendered interaction evidence failed"); return; }
        super::reporting::emit(|sink| sink.record("integration.status.stage", "status.panel", "completed", [stage as f64, 1.0, 0.0, 0.0]));
        if stage == 3 { self.selection_pending = true; return; }
        self.stage += 1;
        if stage == 31 { driver.phase = Phase::StatusCopy; }
        if stage == 26 { self.stage = 37; }
        if stage == 49 { self.stage = 27; }
        if stage == 36 { self.stage = 50; }
        if stage == 58 { self.healthy_completed = true; driver.phase = Phase::StatusFixtureEnd; }
    }
}
