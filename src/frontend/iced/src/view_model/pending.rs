use super::*;
use crate::generated::EncodedApplicationIntent;
use crate::protocol::client_records::Intent;

impl ApplicationModel {
    pub fn register_intent(
        &mut self,
        expected: ApplicationIntentEndpoint,
        encode: impl FnOnce(u64) -> EncodedApplicationIntent,
    ) -> Result<Intent, UiError> {
        self.register_intent_with_detail(expected, PendingDetail::None, encode)
    }

    pub fn register_model_select_intent(
        &mut self,
        receipt: ModelSelectionReceipt,
        encode: impl FnOnce(u64) -> EncodedApplicationIntent,
    ) -> Result<Intent, UiError> {
        self.register_intent_with_detail(
            ApplicationIntentEndpoint::ModelSelect,
            PendingDetail::ModelSelect(receipt),
            encode,
        )
    }

    pub fn register_model_stop_intent(
        &mut self,
        feature: FeatureId,
        encode: impl FnOnce(u64) -> EncodedApplicationIntent,
    ) -> Result<Intent, UiError> {
        self.register_intent_with_detail(
            ApplicationIntentEndpoint::ModelStop,
            PendingDetail::ModelStop(feature),
            encode,
        )
    }

    fn register_intent_with_detail(
        &mut self,
        expected: ApplicationIntentEndpoint,
        detail: PendingDetail,
        encode: impl FnOnce(u64) -> EncodedApplicationIntent,
    ) -> Result<Intent, UiError> {
        if self.connection != ConnectionState::Connected {
            return Err(UiError::transport(
                "The application is not ready to accept requests.",
            ));
        }
        let correlation = self.next_correlation;
        let next = correlation.checked_add(1).ok_or_else(|| UiError::protocol("Request correlation exhausted"))?;
        let encoded = encode(correlation);
        if encoded.record.correlation != correlation || encoded.record.endpoint_id == 0 {
            return Err(UiError::protocol(
                "generated intent encoder returned an invalid identity",
            ));
        }
        let Some(decoded) =
            crate::generated::decode_application_intent_endpoint(encoded.record.endpoint_id)
        else {
            return Err(UiError::protocol(
                "generated intent encoder returned an unknown or interaction identity",
            ));
        };
        if encoded.endpoint != expected || decoded != expected {
            return Err(UiError::protocol(
                "generated intent wrapper and record identities do not match",
            ));
        }
        let detail = match (expected, detail) {
            (
                ApplicationIntentEndpoint::FileDialogOpen
                | ApplicationIntentEndpoint::FileDialogStop,
                PendingDetail::None,
            ) => PendingDetail::FileDialog(
                self.dialog_context
                    .as_ref()
                    .map(|dialog| dialog.target.clone())
                    .ok_or_else(|| {
                        UiError::invalid("No typed file-dialog target is registered.")
                    })?,
            ),
            (
                ApplicationIntentEndpoint::FileDialogOpen
                | ApplicationIntentEndpoint::FileDialogStop,
                _,
            ) => {
                return Err(UiError::protocol("invalid file-dialog pending detail"));
            }
            (ApplicationIntentEndpoint::ModelSelect, PendingDetail::ModelSelect(receipt)) => {
                PendingDetail::ModelSelect(receipt)
            }
            (ApplicationIntentEndpoint::ModelStop, PendingDetail::ModelStop(feature)) => {
                PendingDetail::ModelStop(feature)
            }
            (ApplicationIntentEndpoint::ModelSelect | ApplicationIntentEndpoint::ModelStop, _) => {
                return Err(UiError::protocol("missing model pending detail"));
            }
            (_, PendingDetail::None) => PendingDetail::None,
            _ => {
                return Err(UiError::protocol(
                    "pending detail does not belong to endpoint",
                ));
            }
        };
        if self.pending.len() >= MAX_PENDING_INTENTS {
            return Err(UiError {
                kind: UiErrorKind::Busy,
                title: "Too many pending operations",
                detail: "Wait for an operation to finish before starting another.".into(),
            });
        }
        let owner = crate::generated::application_intent_system(expected);
        if owner != crate::generated::ApplicationSystem::Annotation
            && self.pending.values().any(|pending| {
                crate::generated::application_intent_system(pending.endpoint) == owner
            })
        {
            let detail = match owner {
                crate::generated::ApplicationSystem::Explore => {
                    "An Explore request is already pending."
                }
                crate::generated::ApplicationSystem::FileDialog => {
                    "A file selection request is already pending."
                }
                crate::generated::ApplicationSystem::Model => "A model request is already pending.",
                _ => "A start or stop request for this operation is already pending.",
            };
            return Err(UiError::busy(detail));
        }
        self.next_correlation = next;
        self.pending.insert(
            correlation,
            PendingRequest {
                endpoint: expected,
                detail,
            },
        );
        self.begin_admission(expected);
        Ok(encoded.record)
    }

    #[cfg(test)]
    pub fn begin_intent(&mut self, context: ApplicationIntentEndpoint) -> Result<u64, UiError> {
        self.register_intent(context, |correlation| EncodedApplicationIntent {
            endpoint: context,
            record: Intent {
                correlation,
                endpoint_id: crate::generated::application_intent_endpoint_stable_id(context),
                fields: Vec::new(),
            },
        })
        .map(|intent| intent.correlation)
    }

    #[cfg(test)]
    pub fn begin_model_select_intent(
        &mut self,
        receipt: ModelSelectionReceipt,
    ) -> Result<u64, UiError> {
        self.register_model_select_intent(receipt, |correlation| EncodedApplicationIntent {
            endpoint: ApplicationIntentEndpoint::ModelSelect,
            record: Intent {
                correlation,
                endpoint_id: crate::generated::application_intent_endpoint_stable_id(
                    ApplicationIntentEndpoint::ModelSelect,
                ),
                fields: Vec::new(),
            },
        })
        .map(|intent| intent.correlation)
    }

    pub fn abandon_intent(&mut self, correlation: u64) {
        let Some(pending) = self.pending.remove(&correlation) else {
            return;
        };
        match pending.endpoint {
            ApplicationIntentEndpoint::FileDialogOpen
            | ApplicationIntentEndpoint::FileDialogStop => {
                if let PendingDetail::FileDialog(target) = pending.detail {
                    self.clear_dialog_target_if_matches(&target);
                }
            }
            ApplicationIntentEndpoint::PresentationSelect => {
                self.presentation_model.clear_sent();
            }
            _ => {}
        }
    }

    pub fn pending_count(&self) -> usize {
        self.pending.len()
    }

    pub fn has_pending(&self, context: ApplicationIntentEndpoint) -> bool {
        self.pending
            .values()
            .any(|pending| pending.endpoint == context)
    }

    pub(super) fn has_system_pending(&self, system: crate::generated::ApplicationSystem) -> bool {
        self.pending
            .values()
            .any(|pending| crate::generated::application_intent_system(pending.endpoint) == system)
    }

    pub(crate) fn has_explore_pending(&self) -> bool {
        self.has_system_pending(crate::generated::ApplicationSystem::Explore)
    }

    pub(crate) fn has_upscale_pending(&self) -> bool {
        self.has_system_pending(crate::generated::ApplicationSystem::Upscale)
    }

    pub fn register_dialog(
        &mut self,
        fact: &'static FileDialogFact,
        feature: FeatureId,
    ) -> Result<(), UiError> {
        if fact.stable_field_id == 0 || !fact.workflows.contains(&feature) {
            return Err(UiError::invalid(
                "The selected file dialog is unavailable for this workflow.",
            ));
        }
        if self.dialog_context.is_some()
            || self.has_system_pending(crate::generated::ApplicationSystem::FileDialog)
        {
            return Err(UiError::busy(
                "Finish the current file selection before opening another.",
            ));
        }
        self.dialog_context = Some(DialogContext {
            target: crate::generated::FileDialogTarget::SettingsFieldTarget(
                crate::generated::SettingsFieldTarget {
                    stableid: fact.stable_field_id,
                },
            ),
            title: fact.title,
        });
        Ok(())
    }

    pub fn register_model_dialog(
        &mut self,
        target: &crate::generated::FileDialogTarget,
    ) -> Result<(), UiError> {
        let crate::generated::FileDialogTarget::ModelArtifactTarget(model_target) = target else {
            return Err(UiError::invalid(
                "The selected file target is not a model artifact.",
            ));
        };
        let compatible = crate::generated::MODEL_ARTIFACT_DIALOGS
            .iter()
            .find(|dialog| dialog.target == *model_target);
        let Some(dialog) = compatible else {
            return Err(UiError::invalid(
                "The selected model artifact is unavailable for this workflow.",
            ));
        };
        if self.dialog_context.is_some()
            || self.has_system_pending(crate::generated::ApplicationSystem::FileDialog)
        {
            return Err(UiError::busy(
                "Finish the current file selection before opening another.",
            ));
        }
        self.dialog_context = Some(DialogContext {
            target: target.clone(),
            title: dialog.dialog.title.as_ref(),
        });
        Ok(())
    }

    pub fn prepare_dialog_stop(&mut self) -> Result<u64, UiError> {
        let Some(context) = self.dialog_context.as_ref() else {
            return Err(UiError::invalid("No file selection is active."));
        };
        if !self.dialog_stop_available() {
            return Err(UiError::busy(
                "The file selection is already stopping or has completed.",
            ));
        }
        Ok(match &context.target {
            crate::generated::FileDialogTarget::SettingsFieldTarget(target) => target.stableid,
            crate::generated::FileDialogTarget::ModelArtifactTarget(target) => target.stableid,
        })
    }

    pub fn dialog_context(&self) -> Option<DialogContext> {
        self.dialog_context.clone()
    }

    pub fn clear_dialog_context(&mut self) {
        self.dialog_context = None;
    }

    pub fn pending_endpoint(&self, correlation: u64) -> Option<u64> {
        self.pending.get(&correlation).map(|pending| {
            crate::generated::application_intent_endpoint_stable_id(pending.endpoint)
        })
    }

    pub fn pending_intent(&self, correlation: u64) -> Option<ApplicationIntentEndpoint> {
        self.pending
            .get(&correlation)
            .map(|pending| pending.endpoint)
    }

    pub(super) fn pending_detail(&self, correlation: u64) -> Option<&PendingDetail> {
        self.pending
            .get(&correlation)
            .map(|pending| &pending.detail)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view_model::test_support::*;
    #[test]
    fn annotation_commands_and_stop_retain_distinct_pending_replies() {
        let mut model = bootstrapped();
        let edit = model
            .begin_intent(ApplicationIntentEndpoint::AnnotationEdit)
            .unwrap();
        let stop = model
            .begin_intent(ApplicationIntentEndpoint::AnnotationStop)
            .unwrap();
        let next = model
            .begin_intent(ApplicationIntentEndpoint::AnnotationEdit)
            .unwrap();
        assert_ne!(edit, stop);
        assert_ne!(stop, next);
        model.abandon_intent(stop);
        assert_eq!(
            model.pending_intent(edit),
            Some(ApplicationIntentEndpoint::AnnotationEdit)
        );
        assert_eq!(
            model.pending_intent(next),
            Some(ApplicationIntentEndpoint::AnnotationEdit)
        );
        model.peer_disconnected(UiError::transport("closed"));
        assert!(model.pending.is_empty());
    }

    #[test]
    fn pending_admission_is_bounded_exclusive_and_cleared_on_disconnect() {
        let mut model = bootstrapped();
        let start = model
            .begin_intent(ApplicationIntentEndpoint::ValidationStart)
            .unwrap();
        assert!(
            model
                .begin_intent(ApplicationIntentEndpoint::ValidationStart)
                .is_err()
        );
        assert!(
            model
                .begin_intent(ApplicationIntentEndpoint::ValidationStop)
                .is_err()
        );
        model.abandon_intent(start);
        let stop = model
            .begin_intent(ApplicationIntentEndpoint::ValidationStop)
            .unwrap();
        assert!(model.pending_endpoint(stop).is_some());
        model.peer_disconnected(UiError::transport("gone"));
        assert_eq!(model.pending_count(), 0);
        assert_eq!(model.pending_endpoint(stop), None);
    }

    #[test]
    fn encoded_identity_is_stored_once_and_invalid_identity_consumes_no_capacity() {
        let mut model = bootstrapped();
        let intent = model
            .register_intent(ApplicationIntentEndpoint::DatasetStop, |correlation| {
                crate::generated::encode_dataset_Stop(correlation)
            })
            .unwrap();
        assert_eq!(
            model.pending_endpoint(intent.correlation),
            Some(intent.endpoint_id)
        );
        model.abandon_intent(intent.correlation);
        let invalid =
            model.register_intent(ApplicationIntentEndpoint::DatasetStop, |correlation| {
                crate::generated::EncodedApplicationIntent {
                    endpoint: ApplicationIntentEndpoint::DatasetStop,
                    record: crate::protocol::client_records::Intent {
                        correlation: correlation + 1,
                        endpoint_id: 0,
                        fields: Vec::new(),
                    },
                }
            });
        assert!(invalid.is_err());
        assert_eq!(model.pending_count(), 0);
    }

    #[test]
    fn interaction_unknown_and_wrapper_mismatch_never_enter_pending_admission() {
        let mut model = bootstrapped();
        for endpoint_id in [
            crate::generated::ENDPOINT_Explore_UpdateViewport,
            crate::generated::ENDPOINT_Annotation_Input,
            u64::MAX,
        ] {
            let result =
                model.register_intent(ApplicationIntentEndpoint::DatasetStop, |correlation| {
                    EncodedApplicationIntent {
                        endpoint: ApplicationIntentEndpoint::DatasetStop,
                        record: Intent {
                            correlation,
                            endpoint_id,
                            fields: Vec::new(),
                        },
                    }
                });
            assert!(result.is_err());
        }
        let mismatch = model
            .register_intent(ApplicationIntentEndpoint::DatasetStop, |correlation| {
                crate::generated::encode_validation_Stop(correlation)
            });
        assert!(mismatch.is_err());
        assert_eq!(model.pending_count(), 0);
    }

    #[test]
    fn every_training_endpoint_uses_one_generated_owner_admission_family() {
        let mut model = bootstrapped();
        for endpoint in [
            ApplicationIntentEndpoint::TrainingStart,
            ApplicationIntentEndpoint::TrainingStop,
            ApplicationIntentEndpoint::TrainingQuery,
            ApplicationIntentEndpoint::TrainingSelect,
            ApplicationIntentEndpoint::TrainingClear,
            ApplicationIntentEndpoint::TrainingStartRemote,
            ApplicationIntentEndpoint::TrainingStopRemote,
            ApplicationIntentEndpoint::TrainingRetryReconciliation,
        ] {
            let correlation = model.begin_intent(endpoint).unwrap();
            assert!(
                model
                    .begin_intent(ApplicationIntentEndpoint::TrainingStart)
                    .is_err()
            );
            model.abandon_intent(correlation);
        }
    }

    #[test]
    fn pending_capacity_and_correlation_exhaustion_remain_bounded() {
        let mut model = bootstrapped();
        for correlation in 1..=MAX_PENDING_INTENTS as u64 {
            model.pending.insert(
                correlation,
                PendingRequest {
                    endpoint: ApplicationIntentEndpoint::DatasetStop,
                    detail: PendingDetail::None,
                },
            );
        }
        assert!(
            model
                .begin_intent(ApplicationIntentEndpoint::ValidationStart)
                .is_err()
        );
        model.pending.clear();
        model.next_correlation = u64::MAX;
        assert!(model.begin_intent(ApplicationIntentEndpoint::ValidationStart).is_err());
        assert_eq!(model.next_correlation, u64::MAX);
        assert!(model.pending.is_empty());
    }

    #[test]
    fn settings_and_training_pending_families_are_exclusive() {
        let mut model = bootstrapped();
        let save = model
            .begin_intent(ApplicationIntentEndpoint::SettingsUpdate)
            .unwrap();
        assert!(
            model
                .begin_intent(ApplicationIntentEndpoint::SettingsReset)
                .is_err()
        );
        assert!(model.native_settings_unsettled());
        model.abandon_intent(save);
        let query = model
            .begin_intent(ApplicationIntentEndpoint::TrainingQuery)
            .unwrap();
        for conflict in [
            ApplicationIntentEndpoint::TrainingStart,
            ApplicationIntentEndpoint::TrainingStartRemote,
            ApplicationIntentEndpoint::TrainingClear,
        ] {
            assert!(model.begin_intent(conflict).is_err());
        }
        model.abandon_intent(query);
        assert!(
            model
                .begin_intent(ApplicationIntentEndpoint::TrainingStart)
                .is_ok()
        );
    }

    #[test]
    fn explore_preview_detail_and_rerolls_share_one_pending_admission() {
        let mut model = bootstrapped();
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.ready = true;
        snapshot.busy = false;
        assert!(model.explore_mutation_available());
        let endpoints = [
            ApplicationIntentEndpoint::ExploreUpdateAugmentation,
            ApplicationIntentEndpoint::ExploreRerollAugmentation,
            ApplicationIntentEndpoint::ExploreUpdateDetail,
            ApplicationIntentEndpoint::ExploreReroll,
        ];
        for endpoint in endpoints {
            let correlation = model.begin_intent(endpoint).unwrap();
            assert!(model.explore_mutation_available());
            assert!(model.has_explore_pending());
            for conflict in endpoints {
                assert!(model.begin_intent(conflict).is_err());
            }
            model.abandon_intent(correlation);
        }
    }

    #[test]
    fn dialog_completion_before_open_reply_and_stale_terminal_are_ordered() {
        let fact = crate::generated::FILE_DIALOGS
            .iter()
            .find(|fact| fact.workflows.contains(&FeatureId::Train))
            .unwrap();
        let mut model = bootstrapped();
        model.register_dialog(fact, FeatureId::Train).unwrap();
        let correlation = model
            .begin_intent(ApplicationIntentEndpoint::FileDialogOpen)
            .unwrap();
        let terminal = FileDialogSnapshot {
            generation: 3,
            active: false,
            cancellationrequested: false,
            target: crate::generated::FileDialogTarget::SettingsFieldTarget(
                crate::generated::SettingsFieldTarget {
                    stableid: fact.stable_field_id,
                },
            ),
            selection: None,
        };
        model.reduce_event(ApplicationEvent::FileDialogFileDialogCompleted(
            crate::generated::FileDialogCompleted {
                snapshot: terminal.clone(),
            },
        ));
        model.reduce_reply(
            correlation,
            Ok(ApplicationReply::FileDialogOpen(FileDialogSnapshot {
                active: true,
                ..terminal.clone()
            })),
        );
        assert_eq!(model.file_dialog, Some(terminal));
        assert!(model.dialog_context().is_none());

        model.register_dialog(fact, FeatureId::Train).unwrap();
        model.file_dialog = Some(FileDialogSnapshot {
            generation: 8,
            active: true,
            cancellationrequested: false,
            target: crate::generated::FileDialogTarget::SettingsFieldTarget(
                crate::generated::SettingsFieldTarget {
                    stableid: fact.stable_field_id,
                },
            ),
            selection: None,
        });
        model.reduce_event(ApplicationEvent::FileDialogFileDialogCompleted(
            crate::generated::FileDialogCompleted {
                snapshot: FileDialogSnapshot {
                    generation: 7,
                    active: false,
                    cancellationrequested: false,
                    target: crate::generated::FileDialogTarget::SettingsFieldTarget(
                        crate::generated::SettingsFieldTarget {
                            stableid: fact.stable_field_id,
                        },
                    ),
                    selection: None,
                },
            },
        ));
        assert!(model.dialog_context().is_some());
    }

    #[test]
    fn settings_dialog_predicates_and_reconnect_context_are_native_owned() {
        let mut model = bootstrapped();
        assert!(model.settings_edit_available());
        let save = model
            .begin_intent(ApplicationIntentEndpoint::SettingsUpdate)
            .unwrap();
        assert!(model.native_settings_unsettled());
        model.abandon_intent(save);
        let fact = crate::generated::FILE_DIALOGS
            .iter()
            .find(|fact| fact.workflows.contains(&FeatureId::Train))
            .unwrap();
        assert!(model.file_dialog_open_available(fact, FeatureId::Train));
        model.register_dialog(fact, FeatureId::Train).unwrap();
        let open = model
            .begin_intent(ApplicationIntentEndpoint::FileDialogOpen)
            .unwrap();
        assert!(!model.file_dialog_open_available(fact, FeatureId::Train));
        assert!(model.dialog_context().is_some());
        model.peer_disconnected(UiError::transport("closed"));
        assert_eq!(model.pending_endpoint(open), None);
        assert!(model.dialog_context().is_none());
        assert!(model.file_dialog.is_some());
    }
}

impl ApplicationModel {
    /// Settle wire fingerprints before looking up the pending endpoint or decoding a reply.
    pub(crate) fn accept_reply(&mut self, reply: &crate::protocol::IntentReply) -> bool {
        use std::hash::{Hash, Hasher};
        fn value_hash(value: &crate::application_codec::Value, hash: &mut impl Hasher) {
            use crate::application_codec::Value;
            std::mem::discriminant(value).hash(hash);
            match value {
                Value::Null => {}, Value::Bool(value) => value.hash(hash),
                Value::Signed(value) => value.hash(hash), Value::Unsigned(value) => value.hash(hash),
                Value::Float(value) => value.to_bits().hash(hash), Value::Text(value) => value.hash(hash),
                Value::Bytes(value) => value.hash(hash),
                Value::Array(values) => { values.len().hash(hash); for value in values { value_hash(value, hash); } },
                Value::Object(values) => { values.len().hash(hash); for (key, value) in values { key.hash(hash); value_hash(value, hash); } },
            }
        }
        // A retired or impossible correlation needs no payload traversal. Recent
        // settled fingerprints still take precedence over the pending ledger.
        let previous = self.settled_replies.iter().find(|(id, _)| *id == reply.correlation).map(|(_, fingerprint)| *fingerprint);
        if previous.is_none() {
            if reply.correlation == 0 || reply.correlation >= self.next_correlation {
                self.report_error(crate::view_model::notices::Origin::Protocol, UiError::protocol("IntentReply correlation was never issued"));
                return false;
            }
            if !self.pending.contains_key(&reply.correlation) { return false; }
        }
        let mut hash = std::collections::hash_map::DefaultHasher::new();
        match &reply.result {
            Ok(value) => { true.hash(&mut hash); value_hash(value, &mut hash); },
            Err(error) => { false.hash(&mut hash); std::mem::discriminant(&error.category).hash(&mut hash); error.detail.hash(&mut hash); },
        }
        let fingerprint = hash.finish();
        if let Some(previous) = previous {
            if previous != fingerprint { self.report_error(crate::view_model::notices::Origin::Protocol, UiError::protocol("IntentReply replay changed its result")); }
            return false;
        }
        if self.settled_replies.len() == MAX_PENDING_INTENTS { self.settled_replies.pop_front(); }
        self.settled_replies.push_back((reply.correlation, fingerprint));
        true
    }
}
