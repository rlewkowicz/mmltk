use super::*;
use crate::view_model::test_support::bootstrapped;
use crate::generated::*;

#[test]
fn retention_acknowledges_eviction_and_overflow_without_replaying_conditions() {
    let mut store = NoticeStore::default();
    store.condition(Origin::Transport, Some(failure("offline")));
    let removed = store.latest().unwrap().id;
    for generation in 1..=CAPACITY as u64 { store.observe(Origin::Provider, 0, generation, Some(failure("same cause, another query"))); }
    assert_eq!(store.rows.len(), CAPACITY);
    assert!(store.get(removed).is_none());
    assert_eq!(store.overflow.as_ref().unwrap().occurrences, 1);
    store.condition(Origin::Transport, Some(failure("offline, revised detail")));
    assert_eq!(store.rows.len(), CAPACITY);
    let summary = store.overflow.as_ref().unwrap().id;
    store.dismiss(summary);
    assert!(store.overflow.is_none());
    store.observe(Origin::Provider, 0, CAPACITY as u64, Some(failure("updated cause")));
    assert!(store.overflow.is_none());
    store.observe(Origin::Provider, 0, CAPACITY as u64 + 1, Some(failure("next query")));
    assert_eq!(store.overflow.as_ref().unwrap().occurrences, 1);
}
#[test]
fn conditions_retain_order_and_acknowledgement_until_cleared() {
    let mut store = NoticeStore::default();
    for origin in [Origin::History, Origin::Chart, Origin::Gpu(FeatureId::Train), Origin::Settings, Origin::Clipboard] {
        store.condition(origin, Some(warning("Warning", "first\nsecond\nlast")));
        let original = store.latest().unwrap().clone();
        store.condition(origin, Some(warning("Warning", "changed")));
        assert_eq!(store.latest().unwrap().id, original.id);
        assert_eq!(store.latest().unwrap().content_version, original.content_version + 1);
        store.dismiss(original.id);
        store.condition(origin, Some(warning("Warning", "changed again")));
        assert!(store.is_empty());
        store.condition(origin, None);
        store.condition(origin, Some(warning("Warning", "recurred")));
        assert_ne!(store.latest().unwrap().id, original.id);
        store.dismiss_all();
    }
}

#[test]
fn retained_conditions_rebase_only_the_replaced_dataset_or_run() {
    let mut store = NoticeStore::default();
    store.owned_condition(Origin::Explore, 4, Some(failure("retained preview")));
    store.run_condition(Origin::History, "run-a", Some(warning("History", "dropped")));
    store.dismiss_all();
    store.owned_condition(Origin::Explore, 4, Some(failure("retained preview")));
    store.run_condition(Origin::History, "run-a", Some(warning("History", "dropped")));
    assert!(store.is_empty());
    store.owned_condition(Origin::Explore, 5, Some(failure("retained preview")));
    assert_eq!(store.len(), 1);
    store.run_condition(Origin::History, "run-b", Some(warning("History", "dropped")));
    assert_eq!(store.len(), 2);
    store.dismiss_all();
    store.begin_bootstrap();
    store.run_condition(Origin::History, "run-c", Some(warning("History", "older terminal")));
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
    store.observe(Origin::Provider, 0, 1, Some(failure("last")));
    assert_eq!(store.len(), count);
    assert_eq!(store.next_id, u64::MAX);
}
#[test]
fn clipboard_copies_exact_text_and_ignores_every_retired_completion() {
    let mut store = NoticeStore::default();
    store.condition(Origin::History, Some(warning("Long error", "first\n\nlast")));
    let id = store.latest().unwrap().id;
    let (old, payload) = store.begin_copy(id).unwrap();
    assert_eq!(payload, "Long error\n\nfirst\n\nlast");
    let (new, _) = store.begin_copy(id).unwrap();
    store.finish_copy(old, false);
    assert_eq!(store.len(), 1);
    store.condition(Origin::History, Some(warning("Long error", "updated")));
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
    store.condition(Origin::Clipboard, Some(failure("initial clipboard failure")));
    let id = store.latest().unwrap().id;
    let (copy, _) = store.begin_copy(id).unwrap();
    for generation in 1..=CAPACITY as u64 {
        store.observe(Origin::Provider, 0, generation, Some(failure("provider failure")));
    }
    assert!(store.get(id).is_none());
    assert!(!store.finish_copy(copy, false));
    assert!(!store.rows().any(|notice| notice.origin == Origin::Clipboard));
    assert_eq!(store.len(), CAPACITY + 1);
}
#[test]
fn every_fixed_source_keeps_terminal_identity_independent_of_text_and_route() {
    let mut store = NoticeStore::default();
    let sources = [Origin::Compute(FeatureId::Train), Origin::Compute(FeatureId::Validate), Origin::Compute(FeatureId::Predict), Origin::Compute(FeatureId::Export), Origin::Compute(FeatureId::Live), Origin::Dataset, Origin::Model(FeatureId::Train), Origin::Provider, Origin::Remote, Origin::Checkpoint, Origin::Dialog, Origin::AnnotationSave, Origin::Annotation, Origin::Explore, Origin::Presentation, Origin::Upscale, Origin::PredictionInspection, Origin::Request(ApplicationIntentEndpoint::TrainingStop)];
    for origin in sources {
        store.observe(origin, 0, 1, Some(failure("same text")));
        let id = store.latest().unwrap().id;
        store.dismiss(id);
        store.observe(origin, 0, 1, Some(failure("new detail after dismissal")));
        assert!(store.is_empty());
        store.observe(origin, 0, 2, Some(failure("same text")));
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
    model.report_error(UiError::transport("offline"));
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
    store.observe(Origin::Provider, 0, 8, Some(failure("old")));
    store.dismiss_all(); store.initialized = true;
    store.begin_bootstrap(); store.observe(Origin::Provider, 0, 2, Some(failure("old process history"))); store.end_bootstrap();
    assert!(store.is_empty());
    store.observe(Origin::Provider, 0, 3, Some(failure("new operation")));
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
    for generation in 1..=2 {
        let snapshot = model.annotation.snapshot.as_mut().unwrap();
        snapshot.ui.savegeneration = generation;
        snapshot.ui.savestatus = AnnotationSaveStatus::Uncertain;
        model.observe_annotation(None);
        assert_eq!(model.notices.len(), generation as usize);
    }
    model.notices.dismiss_all(); model.observe_annotation(None); assert!(model.notices.is_empty());
    let snapshot = model.annotation.snapshot.as_mut().unwrap();
    snapshot.ui.savegeneration = 3; snapshot.ui.savestatus = AnnotationSaveStatus::Failed;
    model.observe_annotation(None);
    let id = model.notices.latest().unwrap().id;
    model.observe_annotation(Some("filesystem error\nfull detail".into()));
    assert_eq!(model.notices.latest().unwrap().id, id);
    assert_eq!(model.notices.latest().unwrap().detail, "filesystem error\nfull detail");
    model.observe_annotation(None);
    assert_eq!(model.notices.latest().unwrap().detail, "filesystem error\nfull detail");
}

#[test]
fn interaction_endpoint_episode_rearms_without_an_invented_native_identity() {
    let mut notices = NoticeStore::default();
    let origin = Origin::Interaction(interaction_endpoint(0).unwrap());
    notices.condition(origin, Some(failure("rejected")));
    let id = notices.latest().unwrap().id; notices.dismiss(id);
    notices.condition(origin, Some(failure("another delivery"))); assert!(notices.is_empty());
    notices.condition(origin, None);
    notices.condition(origin, Some(failure("next submission rejected")));
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
