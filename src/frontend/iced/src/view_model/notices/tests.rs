use super::*;
use crate::view_model::test_support::bootstrapped;
use crate::generated::*;

#[test]
fn retention_acknowledges_eviction_and_overflow_without_replaying_conditions() {
    let mut store = NoticeStore::default();
    store.condition(Origin::Transport, true, || failure("offline"));
    let removed = store.latest().unwrap().id;
    for generation in 1..=CAPACITY as u64 { store.terminal(Origin::Provider, 0, generation, || Some(failure("same cause, another query"))); }
    assert_eq!(store.rows.len(), CAPACITY);
    assert!(store.get(removed).is_none());
    assert_eq!(store.overflow.as_ref().unwrap().occurrences, 1);
    store.condition(Origin::Transport, true, || failure("offline, revised detail"));
    assert_eq!(store.rows.len(), CAPACITY);
    let summary = store.overflow.as_ref().unwrap().id;
    store.dismiss(summary);
    assert!(store.overflow.is_none());
    store.terminal(Origin::Provider, 0, CAPACITY as u64, || Some(failure("updated cause")));
    assert!(store.overflow.is_none());
    store.terminal(Origin::Provider, 0, CAPACITY as u64 + 1, || Some(failure("next query")));
    assert_eq!(store.overflow.as_ref().unwrap().occurrences, 1);
}
#[test]
fn conditions_retain_order_and_acknowledgement_until_cleared() {
    let mut store = NoticeStore::default();
    for origin in [Origin::History, Origin::Chart, Origin::Gpu(FeatureId::Train), Origin::Settings, Origin::Clipboard] {
        store.condition(origin, true, || warning("Warning", "first\nsecond\nlast"));
        let original = store.latest().unwrap().clone();
        store.condition(origin, true, || warning("Warning", "changed"));
        assert_eq!(store.latest().unwrap().id, original.id);
        assert_eq!(store.latest().unwrap().content_version, original.content_version + 1);
        store.dismiss(original.id);
        store.condition(origin, true, || warning("Warning", "changed again"));
        assert!(store.is_empty());
        store.clear_condition(origin);
        store.condition(origin, true, || warning("Warning", "recurred"));
        assert_ne!(store.latest().unwrap().id, original.id);
        store.dismiss_all();
    }
}

#[test]
fn retained_conditions_rebase_only_the_replaced_dataset_or_run() {
    let mut store = NoticeStore::default();
    store.owned_condition(Origin::Explore, 4, true, || failure("retained preview"));
    store.run_condition(Origin::History, "run-a", true, || warning("History", "dropped"));
    store.dismiss_all();
    store.owned_condition(Origin::Explore, 4, true, || failure("retained preview"));
    store.run_condition(Origin::History, "run-a", true, || warning("History", "dropped"));
    assert!(store.is_empty());
    store.owned_condition(Origin::Explore, 5, true, || failure("retained preview"));
    assert_eq!(store.len(), 1);
    store.run_condition(Origin::History, "run-b", true, || warning("History", "dropped"));
    assert_eq!(store.len(), 2);
    store.dismiss_all();
    store.begin_bootstrap();
    store.run_condition(Origin::History, "run-c", true, || warning("History", "older terminal"));
    store.end_bootstrap();
    assert!(store.is_empty());
}
#[test]
fn local_payload_bound_and_identity_exhaustion_are_explicit() {
    let mut store = NoticeStore::default();
    store.local(Origin::Clipboard, failure("x".repeat(LOCAL_TEXT_LIMIT + 1)));
    assert_eq!(store.latest().unwrap().origin, Origin::Protocol);
    assert!(store.latest().unwrap().detail.contains("64 KiB"));
    store.next_id = u64::MAX;
    let count = store.len();
    store.terminal(Origin::Provider, 0, 1, || Some(failure("last")));
    assert_eq!(store.len(), count);
    assert_eq!(store.next_id, u64::MAX);
}
#[test]
fn clipboard_copies_exact_text_and_ignores_every_retired_completion() {
    let mut store = NoticeStore::default();
    store.condition(Origin::History, true, || warning("Long error", "first\n\nlast"));
    let id = store.latest().unwrap().id;
    let (old, payload) = store.begin_copy(id).unwrap();
    assert_eq!(payload, "Long error\n\nfirst\n\nlast");
    let (new, _) = store.begin_copy(id).unwrap();
    store.finish_copy(old, false);
    assert_eq!(store.len(), 1);
    store.condition(Origin::History, true, || warning("Long error", "updated"));
    store.finish_copy(new, false);
    assert_eq!(store.len(), 1);
    let (current, _) = store.begin_copy(id).unwrap();
    store.finish_copy(current, false);
    assert_eq!(store.len(), 2);
    let failure_id = store.latest().unwrap().id;
    let (recursive, _) = store.begin_copy(failure_id).unwrap();
    store.finish_copy(recursive, false);
    assert_eq!(store.len(), 2);
    store.dismiss(id);
    store.finish_copy(current, false);
    assert_eq!(store.len(), 1);
}

#[test]
fn eviction_retires_an_outstanding_clipboard_attempt() {
    let mut store = NoticeStore::default();
    store.condition(Origin::Clipboard, true, || failure("initial clipboard failure"));
    let id = store.latest().unwrap().id;
    let (copy, _) = store.begin_copy(id).unwrap();
    for generation in 1..=CAPACITY as u64 {
        store.terminal(Origin::Provider, 0, generation, || Some(failure("provider failure")));
    }
    assert!(store.get(id).is_none());
    assert!(!store.finish_copy(copy, false));
    assert!(!store.rows().any(|notice| notice.origin == Origin::Clipboard));
    assert_eq!(store.len(), CAPACITY + 1);
}
#[test]
fn every_fixed_source_keeps_terminal_identity_independent_of_text_and_route() {
    let mut store = NoticeStore::default();
    let sources = [Origin::Compute(FeatureId::Train), Origin::Compute(FeatureId::Validate), Origin::Compute(FeatureId::Predict), Origin::Compute(FeatureId::Export), Origin::Compute(FeatureId::Live), Origin::Dataset, Origin::Model(FeatureId::Train), Origin::Provider, Origin::Remote, Origin::Checkpoint, Origin::Dialog, Origin::AnnotationSave, Origin::Annotation, Origin::Explore, Origin::Presentation, Origin::Upscale, Origin::PredictionInspection, Origin::PredictionPreview];
    for origin in sources {
        store.terminal(origin, 0, 1, || Some(failure("same text")));
        let id = store.latest().unwrap().id;
        store.dismiss(id);
        store.terminal(origin, 0, 1, || Some(failure("new detail after dismissal")));
        assert!(store.is_empty());
        store.terminal(origin, 0, 2, || Some(failure("same text")));
        assert_ne!(store.latest().unwrap().id, id);
        store.dismiss_all();
    }
    assert_eq!(store.frontiers.len(), sources.len() + 1);
}
#[test]
fn simultaneous_training_causes_and_nested_checkpoint_are_observed_independently() {
    let mut model = bootstrapped();
    let mut snapshot = model.workflow.training.clone().unwrap();
    snapshot.revision += 1;
    snapshot.local.generationfrontier += 1;
    snapshot.local.terminal.generation = snapshot.local.generationfrontier;
    snapshot.local.terminal.outcome = ComputeOperationOutcome::Failed;
    snapshot.local.terminal.detail = "local".into();
    snapshot.offers.revision += 1;
    snapshot.offers.outcome = ProviderQueryOutcome::Failed;
    snapshot.offers.detail = "provider".into();
    snapshot.remote.revision += 1;
    snapshot.remote.outcome = RemoteOperationOutcome::Inconclusive;
    snapshot.remote.detail = "remote".into();
    model.reduce_event(ApplicationEvent::TrainingTrainingChanged(TrainingChanged { snapshot: snapshot.clone() }));
    assert_eq!(model.notices.len(), 3);
    snapshot.inspection.generation += 1;
    snapshot.inspection.status = TrainingInspectionStatus::Failed;
    snapshot.inspection.error = "checkpoint".into();
    model.reduce_event(ApplicationEvent::TrainingTrainingChanged(TrainingChanged { snapshot: snapshot.clone() }));
    assert_eq!(model.notices.len(), 4);
    model.notices.dismiss_all();
    model.reduce_event(ApplicationEvent::TrainingTrainingChanged(TrainingChanged { snapshot }));
    assert!(model.notices.is_empty());
}
#[test]
fn equal_dialog_snapshot_can_carry_its_first_event_only_failure() {
    let mut model = bootstrapped();
    let mut snapshot = model.file_dialog.clone().unwrap();
    snapshot.generation += 1;
    snapshot.active = false;
    model.file_dialog = Some(snapshot.clone());
    model.reduce_event(ApplicationEvent::FileDialogFileDialogFailed(FileDialogFailed { snapshot: snapshot.clone(), detail: "dialog failed\ncomplete detail".into() }));
    assert_eq!(model.notices.len(), 1);
    model.notices.dismiss_all();
    model.reduce_event(ApplicationEvent::FileDialogFileDialogFailed(FileDialogFailed { snapshot, detail: "dialog failed\ncomplete detail".into() }));
    assert!(model.notices.is_empty());
}
#[test]
fn bootstrap_is_atomic_seeds_history_and_preserves_reconnect_frontiers() {
    let mut model = bootstrapped();
    model.report_error(crate::view_model::notices::Origin::Transport, UiError::transport("offline"));
    let before = model.settings_snapshot.clone();
    assert!(model.install_bootstrap([0, 0], Vec::new()).is_err());
    assert_eq!(model.settings_snapshot, before);
    let mut snapshots: Vec<_> = application_snapshot_defaults().unwrap().into_iter().map(|fact| fact.value).collect();
    for snapshot in &mut snapshots { if let ApplicationSnapshot::Training(state) = snapshot {
        state.revision += 1; state.local.generationfrontier = 3; state.local.terminal.generation = 3;
        state.local.terminal.outcome = ComputeOperationOutcome::Failed; state.local.terminal.detail = "completed offline".into();
    } }
    model.install_bootstrap(SCHEMA_FINGERPRINT, snapshots.clone()).unwrap();
    assert!(model.notices.rows().any(|row| row.detail == "completed offline"));
    model.notices.dismiss_all();
    model.install_bootstrap(SCHEMA_FINGERPRINT, snapshots.clone()).unwrap();
    assert!(model.notices.is_empty());
    let mut first = super::super::ApplicationModel::default();
    first.install_bootstrap(SCHEMA_FINGERPRINT, snapshots).unwrap();
    assert!(first.notices.is_empty());
}
#[test]
fn bootstrap_lower_source_generation_rebases_without_restoring_retained_terminals() {
    let mut store = NoticeStore::default();
    store.terminal(Origin::Provider, 0, 8, || Some(failure("old")));
    store.dismiss_all(); store.initialized = true;
    store.begin_bootstrap(); store.terminal(Origin::Provider, 0, 2, || Some(failure("old process history"))); store.end_bootstrap();
    assert!(store.is_empty());
    store.terminal(Origin::Provider, 0, 3, || Some(failure("new operation")));
    assert_eq!(store.len(), 1);
}
#[test]
fn reply_ring_detects_changed_recent_results_and_ignores_retired_ids() {
    let mut model = bootstrapped();
    let correlation = model.begin_intent(ApplicationIntentEndpoint::TrainingStop).unwrap();
    let reply = crate::protocol::IntentReply { correlation, result: Err(ApplicationErrorRecord { category: ApplicationErrorCategory::Failed, detail: "stopped".into() }) };
    assert!(model.accept_reply(&reply));
    model.abandon_intent(correlation);
    assert!(!model.accept_reply(&reply));
    let mut changed = reply.clone(); changed.result = Ok(crate::application_codec::Value::Null);
    assert!(!model.accept_reply(&changed));
    assert_eq!(model.notices.latest().unwrap().origin, Origin::Protocol);
    for _ in 0..65 { let correlation = model.begin_intent(ApplicationIntentEndpoint::TrainingStop).unwrap(); let reply = crate::protocol::IntentReply { correlation, ..reply.clone() }; assert!(model.accept_reply(&reply)); model.abandon_intent(correlation); }
    assert_eq!(model.settled_replies.len(), 64);
    assert!(!model.accept_reply(&reply));
    assert!(!model.accept_reply(&crate::protocol::IntentReply { correlation: 0, ..reply.clone() }));
    assert!(!model.accept_reply(&crate::protocol::IntentReply { correlation: model.next_correlation, ..reply }));
}

#[test]
fn unchanged_uncertain_annotation_saves_are_distinct_actual_attempts() {
    let mut model = bootstrapped();
    let mut snapshot = model.annotation.snapshot.clone().unwrap();
    for generation in 1..=2 {
        snapshot.revision += 1;
        snapshot.uirevision = snapshot.revision;
        snapshot.ui.savegeneration = generation;
        snapshot.ui.savestatus = AnnotationSaveStatus::Uncertain;
        model.reduce_event(ApplicationEvent::AnnotationAnnotationChanged(AnnotationChanged { snapshot: snapshot.clone() }));
        assert_eq!(model.notices.len(), generation as usize);
    }
    model.notices.dismiss_all();
    // An admitted, canceled or preempted command retains the actual Save result.
    for busy in [true, false] {
        snapshot.revision += 1;
        snapshot.uirevision = snapshot.revision;
        snapshot.busy = busy;
        model.reduce_event(ApplicationEvent::AnnotationAnnotationChanged(AnnotationChanged { snapshot: snapshot.clone() }));
        assert!(model.notices.is_empty());
    }
    snapshot.revision += 1;
    snapshot.uirevision = snapshot.revision;
    snapshot.ui.savegeneration = 3;
    snapshot.ui.savestatus = AnnotationSaveStatus::Failed;
    model.reduce_event(ApplicationEvent::AnnotationAnnotationChanged(AnnotationChanged { snapshot: snapshot.clone() }));
    let id = model.notices.latest().unwrap().id;
    model.reduce_event(ApplicationEvent::AnnotationAnnotationFailed(AnnotationFailed { snapshot: snapshot.clone(), detail: "filesystem error\nfull detail".into() }));
    assert_eq!(model.notices.latest().unwrap().id, id);
    assert_eq!(model.notices.latest().unwrap().detail, "filesystem error\nfull detail");
    model.reduce_event(ApplicationEvent::AnnotationAnnotationChanged(AnnotationChanged { snapshot }));
    assert_eq!(model.notices.latest().unwrap().detail, "filesystem error\nfull detail");
}

#[test]
fn interaction_endpoint_episode_rearms_without_an_invented_native_identity() {
    let mut notices = NoticeStore::default();
    let origin = Origin::Interaction(interaction_endpoint(0).unwrap());
    notices.condition(origin, true, || failure("rejected"));
    let id = notices.latest().unwrap().id; notices.dismiss(id);
    notices.condition(origin, true, || failure("another delivery")); assert!(notices.is_empty());
    notices.clear_condition(origin);
    notices.condition(origin, true, || failure("next submission rejected"));
    assert_ne!(notices.latest().unwrap().id, id);
}

#[test]
fn predict_preview_failure_preserves_compute_and_acknowledgement_until_a_new_frame() {
    let mut model = bootstrapped();
    let mut snapshot = model.predict_snapshot.clone().unwrap();
    snapshot.revision += 1;
    snapshot.operation.active = true;
    snapshot.operation.generationfrontier += 1;
    snapshot.operation.terminal.outcome = ComputeOperationOutcome::Idle;
    model.reduce_event(ApplicationEvent::PredictPredictFailed(PredictFailed { snapshot: snapshot.clone(), detail: "retained preview failed".into() }));
    let notice = model.notices.latest().unwrap();
    assert_eq!(notice.origin, Origin::PredictionPreview);
    assert_eq!(notice.severity, Severity::Warning);
    model.notices.dismiss_all();
    snapshot.revision += 1;
    model.reduce_event(ApplicationEvent::PredictPredictFailed(PredictFailed { snapshot: snapshot.clone(), detail: "updated preview detail".into() }));
    assert!(model.notices.is_empty());
    snapshot.revision += 1; snapshot.frame.revision += 1;
    model.reduce_event(ApplicationEvent::PredictPredictChanged(PredictChanged { snapshot: snapshot.clone() }));
    snapshot.revision += 1;
    model.reduce_event(ApplicationEvent::PredictPredictFailed(PredictFailed { snapshot, detail: "another preview episode".into() }));
    assert_eq!(model.notices.len(), 1);
}

#[test]
fn a_later_annotation_failure_is_not_suppressed_by_a_retained_save_failure() {
    let mut model = bootstrapped();
    let mut snapshot = model.annotation.snapshot.clone().unwrap();
    snapshot.revision += 1; snapshot.uirevision = snapshot.revision;
    snapshot.ui.savegeneration = 1; snapshot.ui.savestatus = AnnotationSaveStatus::Failed;
    model.reduce_event(ApplicationEvent::AnnotationAnnotationFailed(AnnotationFailed { snapshot: snapshot.clone(), detail: "save failed".into() }));
    assert_eq!(model.notices.len(), 1);
    assert_eq!(model.notices.latest().unwrap().origin, Origin::AnnotationSave);
    model.reduce_event(ApplicationEvent::AnnotationAnnotationFailed(AnnotationFailed { snapshot: snapshot.clone(), detail: "save failed".into() }));
    assert_eq!(model.notices.len(), 1);
    model.notices.dismiss_all();
    snapshot.revision += 1; snapshot.uirevision = snapshot.revision;
    model.reduce_event(ApplicationEvent::AnnotationAnnotationFailed(AnnotationFailed { snapshot, detail: "another command failed".into() }));
    assert_eq!(model.notices.len(), 1);
    assert_eq!(model.notices.latest().unwrap().origin, Origin::Annotation);
}

#[test]
fn accepted_request_occurrences_preserve_both_annotation_completion_orders() {
    for reverse in [false, true] {
        let mut model = bootstrapped();
        let endpoint = ApplicationIntentEndpoint::AnnotationEdit;
        let first = model.begin_intent(endpoint).unwrap();
        let second = model.begin_intent(endpoint).unwrap();
        let order = if reverse { [second, first] } else { [first, second] };
        for correlation in order {
            let result = ApplicationErrorRecord { category: ApplicationErrorCategory::Failed, detail: "same rejected edit".into() };
            let reply = crate::protocol::IntentReply { correlation, result: Err(result.clone()) };
            assert!(model.accept_reply(&reply));
            model.reduce_reply(correlation, Err(crate::protocol::ApplicationError { category: result.category, detail: result.detail }));
            assert_eq!(model.notices.latest().unwrap().origin, Origin::Request(endpoint, correlation));
        }
        assert_eq!(model.notices.len(), 2);
        model.notices.dismiss_all();
        for correlation in order {
            let reply = crate::protocol::IntentReply { correlation, result: Err(ApplicationErrorRecord { category: ApplicationErrorCategory::Failed, detail: "same rejected edit".into() }) };
            assert!(!model.accept_reply(&reply));
        }
        assert!(model.notices.is_empty());
        assert!(!model.notices.frontiers.iter().any(|slot| matches!(slot.origin, Origin::Request(..))));
    }
}

#[test]
fn disconnect_retires_pending_requests_and_keeps_recent_fingerprints() {
    let mut model = bootstrapped();
    let retired = model.begin_intent(ApplicationIntentEndpoint::AnnotationEdit).unwrap();
    let settled = model.begin_intent(ApplicationIntentEndpoint::AnnotationSave).unwrap();
    let reply = crate::protocol::IntentReply { correlation: settled, result: Err(ApplicationErrorRecord { category: ApplicationErrorCategory::Failed, detail: "save refused".into() }) };
    assert!(model.accept_reply(&reply));
    model.reduce_reply(settled, Err(crate::protocol::ApplicationError { category: ApplicationErrorCategory::Failed, detail: "save refused".into() }));
    model.peer_disconnected(UiError::transport("offline"));
    model.notices.dismiss_all();
    assert!(!model.accept_reply(&reply));
    assert!(!model.accept_reply(&crate::protocol::IntentReply { correlation: retired, ..reply.clone() }));
    assert!(model.notices.is_empty());
    let defaults = application_snapshot_defaults().unwrap().into_iter().map(|fact| fact.value).collect();
    model.install_bootstrap(SCHEMA_FINGERPRINT, defaults).unwrap();
    assert!(!model.accept_reply(&reply));
    assert!(!model.accept_reply(&crate::protocol::IntentReply { correlation: retired, ..reply }));
    assert!(model.notices.is_empty());
}

#[test]
fn bootstrap_failed_save_seeds_its_terminal_ui_revision() {
    let mut model = super::super::ApplicationModel::default();
    let mut snapshots: Vec<_> = application_snapshot_defaults().unwrap().into_iter().map(|fact| fact.value).collect();
    let failed = snapshots.iter_mut().find_map(|snapshot| match snapshot {
        ApplicationSnapshot::Annotation(state) => Some(state), _ => None,
    }).unwrap();
    failed.revision = 20;
    failed.uirevision = 18;
    failed.inputdocumentepoch = 4;
    failed.ui.savegeneration = 7;
    failed.ui.savestatus = AnnotationSaveStatus::Failed;
    let failed = failed.clone();
    model.install_bootstrap(SCHEMA_FINGERPRINT, snapshots).unwrap();
    assert!(model.notices.is_empty());
    model.reduce_event(ApplicationEvent::AnnotationAnnotationFailed(AnnotationFailed { snapshot: failed.clone(), detail: "acknowledged save detail".into() }));
    assert!(model.notices.is_empty());
    let mut unrelated = failed;
    unrelated.revision += 1;
    unrelated.uirevision = unrelated.revision;
    model.reduce_event(ApplicationEvent::AnnotationAnnotationFailed(AnnotationFailed { snapshot: unrelated, detail: "unrelated command failed".into() }));
    assert_eq!(model.notices.len(), 1);
    assert_eq!(model.notices.latest().unwrap().origin, Origin::Annotation);
    assert_eq!(model.notices.latest().unwrap().detail, "unrelated command failed");
}

#[test]
fn save_reply_and_event_share_detail_independently_of_newer_display_frames() {
    for event_first in [false, true] {
        let mut model = bootstrapped();
        let mut failed = model.annotation.snapshot.clone().unwrap();
        failed.revision += 1;
        failed.uirevision = failed.revision;
        failed.ui.savegeneration = 1;
        failed.ui.savestatus = AnnotationSaveStatus::Failed;
        let correlation = model.begin_intent(ApplicationIntentEndpoint::AnnotationSave).unwrap();
        let event = ApplicationEvent::AnnotationAnnotationFailed(AnnotationFailed { snapshot: failed.clone(), detail: "complete save detail\nlast line".into() });
        if event_first { model.reduce_event(event.clone()); }
        model.reduce_reply(correlation, Ok(ApplicationReply::AnnotationSave(failed.clone())));
        let id = model.notices.latest().unwrap().id;
        model.reduce_event(ApplicationEvent::AnnotationAnnotationFrameChanged(AnnotationFrameChanged {
            snapshot: AnnotationFrameState { revision: failed.revision + 1, uirevision: failed.uirevision, frame: failed.frame.clone() },
        }));
        model.reduce_event(event.clone());
        let notice = model.notices.latest().unwrap().clone();
        assert_eq!(notice.id, id);
        assert_eq!(notice.origin, Origin::AnnotationSave);
        assert_eq!(notice.detail, "complete save detail\nlast line");
        model.reduce_event(event.clone());
        assert_eq!(model.notices.latest().unwrap(), &notice);
        assert_eq!(model.annotation.snapshot.as_ref().unwrap().revision, failed.revision + 1);
        model.notices.dismiss_all();
        model.reduce_event(event);
        assert!(model.notices.is_empty());
        // An actual later Save and document replacement each own a new terminal.
        for replacement in [false, true] {
            failed.revision += 2;
            failed.uirevision = failed.revision;
            if replacement { failed.inputdocumentepoch += 1; } else { failed.ui.savegeneration += 1; }
            model.reduce_event(ApplicationEvent::AnnotationAnnotationFailed(AnnotationFailed { snapshot: failed.clone(), detail: "complete save detail\nlast line".into() }));
            assert_eq!(model.notices.latest().unwrap().origin, Origin::AnnotationSave);
            assert_ne!(model.notices.latest().unwrap().id, id);
            model.notices.dismiss_all();
        }
    }
}

#[test]
fn reconnect_and_eviction_preserve_save_detail_and_acknowledgement() {
    let mut model = bootstrapped();
    let mut failed = model.annotation.snapshot.clone().unwrap();
    failed.revision += 1;
    failed.uirevision = failed.revision;
    failed.ui.savegeneration = 1;
    failed.ui.savestatus = AnnotationSaveStatus::Failed;
    let event = ApplicationEvent::AnnotationAnnotationFailed(AnnotationFailed { snapshot: failed.clone(), detail: "full native save failure".into() });
    model.reduce_event(event.clone());
    let id = model.notices.latest().unwrap().id;
    let snapshots = application_snapshot_defaults().unwrap().into_iter().map(|fact| match fact.value {
        ApplicationSnapshot::Annotation(_) => ApplicationSnapshot::Annotation(failed.clone()), other => other,
    }).collect();
    model.install_bootstrap(SCHEMA_FINGERPRINT, snapshots).unwrap();
    assert_eq!(model.notices.get(id).unwrap().detail, "full native save failure");
    for generation in 1..=CAPACITY as u64 { model.notices.terminal(Origin::Provider, 0, generation, || Some(failure("another query"))); }
    assert!(model.notices.get(id).is_none());
    let count = model.notices.len();
    model.reduce_event(event);
    assert_eq!(model.notices.len(), count);
    assert!(!model.notices.rows().any(|row| row.origin == Origin::AnnotationSave));
}

#[test]
fn local_sources_share_classification_without_sharing_recovery() {
    let mut model = bootstrapped();
    let save = ApplicationIntentEndpoint::AnnotationSave;
    let stop = ApplicationIntentEndpoint::TrainingStop;
    model.report_admission_error(save, UiError::busy("save unavailable"));
    model.report_admission_error(stop, UiError::busy("stop unavailable"));
    assert_eq!(model.notices.len(), 2);
    model.notices.dismiss_all();
    model.report_admission_error(save, UiError::busy("same save episode"));
    model.report_admission_error(stop, UiError::busy("same stop episode"));
    assert!(model.notices.is_empty());
    // Successful registration proves recovery for Save, leaving Stop acknowledged.
    let admitted = model.begin_intent(save).unwrap();
    model.abandon_intent(admitted);
    model.report_admission_error(save, UiError::busy("save unavailable again"));
    model.report_admission_error(stop, UiError::busy("stop still unavailable"));
    assert_eq!(model.notices.len(), 1);
    assert_eq!(model.notices.latest().unwrap().origin, Origin::Admission(save));
    model.begin_admission(stop);
    model.report_admission_error(stop, UiError::busy("new stop attempt"));
    assert_eq!(model.notices.len(), 2);
}

#[test]
fn acknowledged_and_bootstrap_sources_do_not_prepare_payloads() {
    let mut store = NoticeStore::default();
    store.begin_bootstrap();
    store.terminal(Origin::Provider, 0, 7, || panic!("historical terminal text"));
    store.condition(Origin::Settings, true, || panic!("historical condition text"));
    store.end_bootstrap();
    store.terminal(Origin::Provider, 0, 7, || panic!("acknowledged terminal text"));
    store.condition(Origin::Settings, true, || panic!("acknowledged condition text"));
    store.clear_condition(Origin::Settings);
    store.condition(Origin::Settings, true, || warning("New episode", "new text"));
    store.dismiss_all();
    store.condition(Origin::Settings, true, || panic!("dismissed condition text"));
}

#[test]
fn failed_bootstrap_keeps_every_installed_source_cursor_atomic() {
    let mut model = bootstrapped();
    let frontiers = |store: &NoticeStore| store.frontiers.iter().map(|slot|
        (slot.origin, slot.owner, slot.generation, slot.observed, slot.row, slot.run.clone())
    ).collect::<Vec<_>>();
    let before = frontiers(&model.notices);
    let settings = model.settings_snapshot.clone();
    let mut snapshots: Vec<_> = application_snapshot_defaults().unwrap().into_iter().map(|fact| fact.value).collect();
    let annotation_index = snapshots.iter().position(|snapshot| matches!(snapshot, ApplicationSnapshot::Annotation(_))).unwrap();
    let mut invalid = snapshots.remove(annotation_index);
    if let ApplicationSnapshot::Annotation(state) = &mut invalid { state.uirevision = state.revision + 1; }
    for snapshot in &mut snapshots {
        if let ApplicationSnapshot::Training(state) = snapshot {
            state.offers.revision += 10;
            state.offers.outcome = ProviderQueryOutcome::Failed;
            state.offers.detail = "uncommitted bootstrap failure".into();
        }
    }
    snapshots.push(invalid);
    assert!(model.install_bootstrap(SCHEMA_FINGERPRINT, snapshots).is_err());
    assert_eq!(frontiers(&model.notices), before);
    assert_eq!(model.settings_snapshot, settings);
    assert!(model.notices.is_empty());
}

#[test]
fn event_only_sources_seed_and_rebase_through_their_snapshot_owners() {
    let mut model = bootstrapped();
    let mut live = model.live_snapshot.clone().unwrap();
    let mut upscale = model.upscale_snapshot.clone().unwrap();
    let mut presentation = model.presentation.clone().unwrap();
    // The bootstrap seeded each source even though it carried no error detail.
    let fail = |model: &mut super::super::ApplicationModel, live: &LiveSnapshot, upscale: &UpscaleSnapshot, presentation: &PresentationState| {
        model.reduce_event(ApplicationEvent::LiveLiveFailed(LiveFailed { snapshot: live.clone(), detail: "live detail".into() }));
        model.reduce_event(ApplicationEvent::UpscaleUpscaleFailed(UpscaleFailed { snapshot: upscale.clone(), detail: "upscale detail".into(), request: None, kind: UpscaleFailureKind::Failed }));
        model.reduce_event(ApplicationEvent::PresentationPresentationFailed(PresentationFailed { snapshot: presentation.clone(), detail: "presentation detail".into() }));
    };
    fail(&mut model, &live, &upscale, &presentation);
    assert!(model.notices.is_empty());
    live.revision += 1;
    upscale.revision += 1;
    presentation.revision += 1;
    // A reply/state installation alone must not acknowledge new event-only detail.
    model.install_snapshot(ApplicationSnapshot::Live(live.clone())).unwrap();
    model.install_snapshot(ApplicationSnapshot::Upscale(upscale.clone())).unwrap();
    model.install_snapshot(ApplicationSnapshot::Presentation(presentation.clone())).unwrap();
    fail(&mut model, &live, &upscale, &presentation);
    assert_eq!(model.notices.len(), 3);
    model.notices.dismiss_all();
    fail(&mut model, &live, &upscale, &presentation);
    assert!(model.notices.is_empty());
    let baseline: Vec<_> = application_snapshot_defaults().unwrap().into_iter().map(|fact| fact.value).collect();
    model.install_bootstrap(SCHEMA_FINGERPRINT, baseline).unwrap();
    let lower_live = model.live_snapshot.clone().unwrap();
    let lower_upscale = model.upscale_snapshot.clone().unwrap();
    let lower_presentation = model.presentation.clone().unwrap();
    fail(&mut model, &lower_live, &lower_upscale, &lower_presentation);
    assert!(model.notices.is_empty());
    fail(&mut model, &live, &upscale, &presentation);
    assert_eq!(model.notices.len(), 3);
}

#[test]
fn source_capacity_rejects_new_slots_without_preparing_their_payloads() {
    let mut store = NoticeStore::default();
    for endpoint in 1..SOURCE_CAPACITY as u64 {
        store.terminal(Origin::Interaction(endpoint), 0, 1, || None);
    }
    assert_eq!(store.frontiers.len(), SOURCE_CAPACITY);
    store.condition(Origin::Editor(FeatureId::Train), true, || panic!("unadmitted source payload"));
    assert_eq!(store.frontiers.len(), SOURCE_CAPACITY);
    assert_eq!(store.len(), 1);
    assert_eq!(store.latest().unwrap().origin, Origin::Protocol);
    store.dismiss_all();
    store.condition(Origin::Editor(FeatureId::Train), true, || panic!("unadmitted source payload"));
    assert!(store.is_empty());
}
