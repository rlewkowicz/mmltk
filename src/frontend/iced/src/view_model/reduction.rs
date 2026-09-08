use super::*;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Observation {
    Installed,
    Current,
    Stale,
}

impl ApplicationModel {
    pub fn peer_connected(&mut self) {
        self.clear_peer_state();
        self.connection = ConnectionState::AwaitingBootstrap;
    }

    pub fn peer_disconnected(&mut self, error: UiError) {
        self.clear_peer_state();
        self.connection = ConnectionState::Reconnecting;
        self.error = Some(error);
    }

    pub fn clear_peer_state(&mut self) {
        self.pending.clear();
        self.settings_snapshot = None;
        self.file_dialog = None;
        self.presentation = None;
        self.model_snapshot = None;
        self.live_snapshot = None;
        self.predict_snapshot = None;
        self.upscale_snapshot = None;
        self.workflow = WorkflowModel::default();
        self.explore.reset_transport();
        self.annotation = AnnotationModel::default();
        self.dialog_context = None;
        self.presentation_model = presentation::PresentationModel::default();
    }

    pub fn install_bootstrap(
        &mut self,
        fingerprint: [u64; 2],
        snapshots: Vec<ApplicationSnapshot>,
    ) -> Result<(), UiError> {
        if fingerprint != crate::generated::SCHEMA_FINGERPRINT {
            return Err(UiError::protocol("application schema fingerprint mismatch"));
        }
        if !crate::generated::application_bootstrap_complete(&snapshots) {
            return Err(UiError::protocol(
                "bootstrap does not contain every generated application snapshot exactly once",
            ));
        }
        let mut replacement = Self {
            window_width: self.window_width,
            window_height: self.window_height,
            scale_factor: self.scale_factor,
            next_correlation: self.next_correlation,
            connection: ConnectionState::AwaitingBootstrap,
            ..Self::default()
        };
        for snapshot in snapshots {
            replacement.install_snapshot(snapshot)?;
        }
        replacement.connection = ConnectionState::Connected;
        *self = replacement;
        Ok(())
    }

    pub fn reduce_reply(
        &mut self,
        correlation: u64,
        result: Result<ApplicationReply, ApplicationError>,
    ) -> Option<VisualFrame> {
        let Some(context) = self
            .pending
            .get(&correlation)
            .map(|pending| pending.endpoint)
        else {
            self.error = Some(UiError::protocol("unknown or duplicate IntentReply"));
            return None;
        };
        let failed_presentation =
            context == ApplicationIntentEndpoint::PresentationSelect && result.is_err();
        match result {
            Err(error) => {
                let current_failure = context != ApplicationIntentEndpoint::UpscaleStart
                    || self.explore.requested_upscale == self.explore.sent_upscale;
                if context == ApplicationIntentEndpoint::UpscaleStart {
                    if self.explore.requested_upscale == self.explore.sent_upscale {
                        self.explore.requested_upscale = None;
                        if self.presentation_model.foreground() == Some(PresentationSourceKind::Upscale) {
                            self.set_foreground_visual(Some(PresentationSourceKind::Explore));
                        }
                    }
                    self.explore.sent_upscale = None;
                }
                if matches!(
                    context,
                    ApplicationIntentEndpoint::FileDialogOpen
                        | ApplicationIntentEndpoint::FileDialogStop
                ) {
                    let target = self
                        .pending_detail(correlation)
                        .and_then(|detail| match detail {
                            PendingDetail::FileDialog(target) => Some(target.clone()),
                            _ => None,
                        });
                    if let Some(target) = target.as_ref() {
                        self.clear_dialog_target_if_matches(target);
                    }
                }
                if current_failure {
                    self.error = Some(error.into());
                }
            }
            Ok(reply) => {
                if crate::generated::application_reply_endpoint(&reply) != context {
                    if matches!(
                        context,
                        ApplicationIntentEndpoint::FileDialogOpen
                            | ApplicationIntentEndpoint::FileDialogStop
                    ) {
                        self.dialog_context = None;
                    }
                    self.error = Some(UiError::protocol(
                        "IntentReply kind did not match its pending endpoint",
                    ));
                } else {
                    self.install_reply(correlation, reply);
                }
            }
        }
        self.pending.remove(&correlation);
        let refresh = self.presentation_refresh();
        if failed_presentation {
            self.presentation_model.clear_sent();
        }
        refresh
    }

    pub fn reduce_event(&mut self, event: ApplicationEvent) -> Option<VisualFrame> {
        crate::generated::dispatch_application_event::<_, UiError>(self, event);
        self.presentation_refresh()
    }

    pub(super) fn install_settings_snapshot(
        &mut self,
        snapshot: SettingsUiState,
    ) -> Result<Observation, UiError> {
        let observation = merge_observation(
            &mut self.settings_snapshot,
            snapshot,
            |value| value.revision,
            "Settings",
        )?;
        if observation == Observation::Installed {
            let installed = self
                .settings_snapshot
                .as_ref()
                .expect("installed Settings observation");
            self.workflow.install_settings(installed);
        }
        Ok(observation)
    }

    pub(super) fn install_snapshot(
        &mut self,
        snapshot: ApplicationSnapshot,
    ) -> Result<(), UiError> {
        crate::generated::dispatch_application_snapshot(self, snapshot)
    }

    pub(super) fn install_reply(&mut self, correlation: u64, reply: ApplicationReply) {
        crate::generated::dispatch_application_reply::<_, UiError>(self, correlation, reply);
    }

    pub(super) fn install_dialog_reply(
        &mut self,
        correlation: u64,
        reply_endpoint: ApplicationIntentEndpoint,
        snapshot: FileDialogSnapshot,
    ) {
        let expected_endpoint = self.pending_intent(correlation);
        let expected_target = self
            .pending_detail(correlation)
            .and_then(|detail| match detail {
                PendingDetail::FileDialog(target) => Some(target.clone()),
                _ => None,
            });
        if expected_endpoint != Some(reply_endpoint)
            || expected_target.as_ref() != Some(&snapshot.target)
        {
            if let Some(target) = expected_target.as_ref() {
                self.clear_dialog_target_if_matches(target);
            }
            self.error = Some(UiError::protocol(
                "FileDialog reply kind or target did not match its pending context",
            ));
            return;
        }
        let terminal = !snapshot.active;
        if terminal {
            if let Err(error) = self.validate_dialog_terminal(&snapshot) {
                self.clear_dialog_target_if_matches(&snapshot.target);
                self.error = Some(error);
                return;
            }
        }
        match merge_dialog_snapshot(&mut self.file_dialog, snapshot) {
            Err(error) => {
                if let Some(target) = expected_target.as_ref() {
                    self.clear_dialog_target_if_matches(target);
                }
                self.error = Some(error);
            }
            Ok(Observation::Installed | Observation::Current) if terminal => {
                if let Some(target) = expected_target.as_ref() {
                    self.clear_dialog_target_if_matches(target);
                }
            }
            Ok(Observation::Installed | Observation::Current | Observation::Stale) => {}
        }
    }

    pub(super) fn install_compute_snapshot(&mut self, page: FeatureId, snapshot: ComputeUiState) {
        let failed = snapshot.terminal.outcome == ComputeOperationOutcome::Failed;
        let detail = snapshot.terminal.detail.clone();
        let result = self
            .compute_target(page)
            .map_or(Ok(Observation::Stale), |target| {
                merge_compute_state(target, snapshot)
            });
        match result {
            Err(error) => self.error = Some(error),
            Ok(Observation::Installed) => {
                if failed {
                    self.failed(detail);
                }
            }
            Ok(Observation::Current | Observation::Stale) => {}
        }
    }

    pub(super) fn install_predict_snapshot(
        &mut self,
        snapshot: PredictSnapshot,
    ) -> Result<Observation, UiError> {
        merge_predict_snapshot(&mut self.predict_snapshot, snapshot)
    }

    pub(super) fn install_training_snapshot(
        &mut self,
        snapshot: crate::generated::TrainingSnapshot,
    ) -> Result<(Observation, Option<String>), UiError> {
        let prior = self.workflow.training.as_ref().map(|value| {
            (
                value.local.terminal.outcome,
                value.local.terminal.generation,
                value.offers.outcome,
                value.offers.revision,
                value.remote.outcome,
                value.remote.revision,
            )
        });
        let observation = merge_observation(
            &mut self.workflow.training,
            snapshot,
            |value| value.revision,
            "Training",
        )?;
        let failure = if observation == Observation::Installed {
            prior.and_then(|prior| {
                let installed = self.workflow.training.as_ref()?;
                if installed.local.terminal.outcome == ComputeOperationOutcome::Failed
                    && (prior.0 != ComputeOperationOutcome::Failed
                        || installed.local.terminal.generation > prior.1)
                {
                    Some(installed.local.terminal.detail.clone())
                } else if installed.offers.outcome == crate::generated::ProviderQueryOutcome::Failed
                    && (prior.2 != crate::generated::ProviderQueryOutcome::Failed
                        || installed.offers.revision > prior.3)
                {
                    Some(installed.offers.detail.clone())
                } else if matches!(
                    installed.remote.outcome,
                    crate::generated::RemoteOperationOutcome::Failed
                        | crate::generated::RemoteOperationOutcome::Inconclusive
                ) && (!matches!(
                    prior.4,
                    crate::generated::RemoteOperationOutcome::Failed
                        | crate::generated::RemoteOperationOutcome::Inconclusive
                ) || installed.remote.revision > prior.5)
                {
                    Some(installed.remote.detail.clone())
                } else {
                    None
                }
            })
        } else {
            None
        };
        Ok((observation, failure))
    }

    pub(super) fn install_training_progress(
        &mut self,
        progress: crate::generated::TrainingProgress,
    ) -> Result<Observation, UiError> {
        let Some(installed) = self.workflow.training.as_mut() else {
            return Ok(Observation::Stale);
        };
        if progress.revision < installed.revision {
            return Ok(Observation::Stale);
        }
        if progress.revision == installed.revision {
            if progress.activity != installed.activity || progress.local != installed.local {
                return Err(UiError::protocol(
                    "inconsistent Training progress observation revision",
                ));
            }
            return Ok(Observation::Current);
        }
        installed.revision = progress.revision;
        installed.activity = progress.activity;
        installed.local = progress.local;
        Ok(Observation::Installed)
    }

    pub(super) fn compute_target(&mut self, page: FeatureId) -> Option<&mut ComputeUiState> {
        match page {
            FeatureId::Train => self
                .workflow
                .training
                .as_mut()
                .map(|snapshot| &mut snapshot.local),
            FeatureId::Validate => self.workflow.validation.as_mut(),
            FeatureId::Export => self.workflow.export.as_mut(),
            FeatureId::Predict | FeatureId::Live | FeatureId::Annotate | FeatureId::Explore => None,
        }
    }

    pub(super) fn install_dataset_progress(
        &mut self,
        generation: u64,
        active: bool,
        terminal: crate::generated::ArtifactTerminal,
        progress: crate::generated::ArtifactProgress,
    ) -> Result<Observation, UiError> {
        let Some(dataset) = self.workflow.dataset.as_mut() else {
            return Ok(Observation::Stale);
        };
        if generation < dataset.generation {
            return Ok(Observation::Stale);
        }
        if generation > dataset.generation {
            dataset.generation = generation;
            dataset.active = active;
            dataset.terminal = terminal;
            dataset.progress = progress;
            return Ok(Observation::Installed);
        }
        if artifact_outcome_is_final(dataset.terminal.outcome) {
            return Ok(Observation::Stale);
        }
        if dataset.active == active && dataset.terminal == terminal && dataset.progress == progress
        {
            return Ok(Observation::Current);
        }
        if artifact_outcome_rank(terminal.outcome) < artifact_outcome_rank(dataset.terminal.outcome)
        {
            return Err(UiError::protocol(
                "Dataset progress regressed its operation terminal",
            ));
        }
        dataset.active = active;
        dataset.terminal = terminal;
        dataset.progress = progress;
        Ok(Observation::Installed)
    }

    pub(super) fn install_dataset_snapshot(
        &mut self,
        snapshot: crate::generated::ArtifactUiState,
    ) -> Result<Observation, UiError> {
        merge_dataset_state(&mut self.workflow.dataset, snapshot)
    }

    pub(super) fn install_explore_snapshot(
        &mut self,
        snapshot: crate::generated::ExploreSnapshot,
        _allow_clean_hydration: bool,
    ) -> Result<Observation, UiError> {
        let dataset_changed = self
            .explore
            .snapshot
            .as_ref()
            .is_some_and(|previous| previous.dataset.identity != snapshot.dataset.identity);
        let same_image = self.explore.snapshot.as_ref().is_some_and(|previous|
            previous.selectedimage == snapshot.selectedimage);
        let observation = merge_explore_snapshot(&mut self.explore.snapshot, snapshot)?;
        if observation == Observation::Installed {
            let snapshot = self.explore.snapshot.as_ref().expect("installed snapshot");
            if let Some(request) = self.explore.requested_upscale.as_mut() {
                if !dataset_changed
                    && same_image
                    && snapshot.mode == crate::generated::ExploreMode::Detail
                {
                    request.source = snapshot.frame.clone();
                    request.document = snapshot.document.clone();
                } else {
                    self.explore.requested_upscale = None;
                    self.explore.sent_upscale = None;
                    if self.presentation_model.foreground() == Some(PresentationSourceKind::Upscale) {
                        self.set_foreground_visual(Some(PresentationSourceKind::Explore));
                    }
                }
            }
        }
        if observation == Observation::Installed && self.explore.requested_upscale.is_some()
            && self.current_upscale().is_none()
        {
            self.set_foreground_visual(Some(PresentationSourceKind::Explore));
        }
        Ok(observation)
    }

    pub(super) fn install_dialog_terminal(
        &mut self,
        snapshot: FileDialogSnapshot,
    ) -> Result<Observation, UiError> {
        self.validate_dialog_terminal(&snapshot)?;
        let observation = merge_dialog_snapshot(&mut self.file_dialog, snapshot)?;
        if observation == Observation::Installed {
            self.dialog_context = None;
        }
        Ok(observation)
    }

    pub(super) fn validate_dialog_terminal(
        &self,
        snapshot: &FileDialogSnapshot,
    ) -> Result<(), UiError> {
        let expected_target = self
            .dialog_context
            .as_ref()
            .map(|context| &context.target)
            .or_else(|| {
                self.pending
                    .values()
                    .find_map(|pending| match &pending.detail {
                        PendingDetail::FileDialog(target) => Some(target),
                        _ => None,
                    })
            });
        let expected = expected_target.map(dialog_target_stable_id);
        if snapshot.active {
            return Err(UiError::protocol(
                "FileDialog terminal did not match its initiating field",
            ));
        }
        let Some(expected) = expected else {
            if self.file_dialog.as_ref() == Some(&snapshot) {
                return Ok(());
            }
            return Err(UiError::protocol(
                "unsolicited FileDialog terminal without an initiating context",
            ));
        };
        if dialog_target_stable_id(&snapshot.target) != expected
            || expected_target.is_some_and(|target| target != &snapshot.target)
        {
            return Err(UiError::protocol(
                "FileDialog terminal did not match its initiating field",
            ));
        }
        if let Some(selection) = snapshot.selection.as_ref() {
            if selection.target != snapshot.target {
                return Err(UiError::protocol(
                    "FileDialog terminal model target did not match its initiating context",
                ));
            }
        }
        Ok(())
    }

    pub(super) fn clear_dialog_target_if_matches(&mut self, target: &FileDialogTarget) {
        if self
            .dialog_context
            .as_ref()
            .is_some_and(|context| &context.target == target)
        {
            self.dialog_context = None;
        }
    }

    pub(super) fn failed(&mut self, detail: String) {
        self.error = Some(UiError {
            kind: UiErrorKind::Failed,
            title: "Operation failed",
            detail,
        });
    }
}

pub(super) fn merge_observation<T: PartialEq>(
    target: &mut Option<T>,
    incoming: T,
    revision: impl Fn(&T) -> u64,
    family: &'static str,
) -> Result<Observation, UiError> {
    let Some(installed) = target.as_mut() else {
        *target = Some(incoming);
        return Ok(Observation::Installed);
    };
    let installed_revision = revision(installed);
    let incoming_revision = revision(&incoming);
    if incoming_revision < installed_revision {
        return Ok(Observation::Stale);
    }
    if incoming_revision == installed_revision {
        if incoming != *installed {
            return Err(UiError::protocol(format!(
                "inconsistent {family} snapshot observation revision"
            )));
        }
        return Ok(Observation::Current);
    }
    *installed = incoming;
    Ok(Observation::Installed)
}

pub(super) fn merge_model_snapshot(
    target: &mut Option<ModelUiState>,
    incoming: ModelUiState,
) -> Result<Observation, UiError> {
    let Some(installed) = target.as_mut() else {
        *target = Some(incoming);
        return Ok(Observation::Installed);
    };
    if incoming.generation < installed.generation {
        return Ok(Observation::Stale);
    }
    if incoming.generation > installed.generation {
        *installed = incoming;
        return Ok(Observation::Installed);
    }
    if *installed == incoming {
        return Ok(Observation::Current);
    }
    if installed.active && !incoming.active {
        *installed = incoming;
        return Ok(Observation::Installed);
    }
    if installed.active && incoming.active {
        if incoming.terminal.outcome == ModelSelectionOutcome::CancellationRequested {
            *installed = incoming;
            return Ok(Observation::Installed);
        }
        if incoming.progress.stage == crate::generated::ModelProgressStage::Idle
            && installed.progress.stage != crate::generated::ModelProgressStage::Idle
        {
            return Ok(Observation::Stale);
        }
    }
    if !installed.active && incoming.active {
        return Ok(Observation::Stale);
    }
    Err(UiError::protocol(
        "inconsistent Model observation generation",
    ))
}

pub(super) fn merge_presentation_snapshot(
    target: &mut Option<PresentationSnapshot>,
    incoming: PresentationSnapshot,
) -> Result<Observation, UiError> {
    merge_observation(target, incoming, |value| value.revision, "Presentation")
}

pub(super) fn merge_explore_snapshot(
    target: &mut Option<crate::generated::ExploreSnapshot>,
    incoming: crate::generated::ExploreSnapshot,
) -> Result<Observation, UiError> {
    merge_observation(target, incoming, |value| value.revision, "Explore")
}

pub(super) fn merge_compute_state(
    target: &mut ComputeUiState,
    mut incoming: ComputeUiState,
) -> Result<Observation, UiError> {
    if incoming.terminal.generation > incoming.generationfrontier {
        return Err(UiError::protocol(
            "Compute snapshot terminal exceeds its generation frontier",
        ));
    }
    if incoming.generationfrontier < target.generationfrontier {
        return Ok(Observation::Stale);
    }
    if incoming.generationfrontier > target.generationfrontier {
        *target = incoming;
        return Ok(Observation::Installed);
    }
    if compute_outcome_is_final(target.terminal.outcome) {
        if !compute_outcome_is_final(incoming.terminal.outcome) {
            return Ok(Observation::Stale);
        }
        if incoming.terminal != target.terminal {
            return Err(UiError::protocol(
                "inconsistent final Compute snapshot for one generation",
            ));
        }
        return Ok(Observation::Current);
    }
    if incoming.progress.sequence < target.progress.sequence {
        incoming.progress = target.progress.clone();
    }
    if compute_outcome_rank(incoming.terminal.outcome)
        < compute_outcome_rank(target.terminal.outcome)
    {
        incoming.terminal = target.terminal.clone();
        incoming.active = target.active;
    }
    if incoming == *target {
        return Ok(Observation::Current);
    }
    *target = incoming;
    Ok(Observation::Installed)
}

pub(super) fn merge_compute_snapshot(
    target: &mut Option<ComputeUiState>,
    incoming: ComputeUiState,
) -> Result<Observation, UiError> {
    match target {
        Some(target) => merge_compute_state(target, incoming),
        None => {
            *target = Some(incoming);
            Ok(Observation::Installed)
        }
    }
}

pub(super) fn merge_dataset_state(
    target: &mut Option<crate::generated::ArtifactUiState>,
    // CLEANUP-IGNORE: Dataset's incoming state enters artifact-specific reconciliation.
    mut incoming: crate::generated::ArtifactUiState,
    // CLEANUP-IGNORE: Dataset installation begins artifact-specific reconciliation.
) -> Result<Observation, UiError> {
    let Some(installed) = target.as_mut() else {
        *target = Some(incoming);
        return Ok(Observation::Installed);
    };
    if incoming.generation < installed.generation {
        return Ok(Observation::Stale);
    }
    if incoming.generation > installed.generation {
        *installed = incoming;
        return Ok(Observation::Installed);
    }
    if artifact_outcome_is_final(installed.terminal.outcome) {
        if !artifact_outcome_is_final(incoming.terminal.outcome) {
            return Ok(Observation::Stale);
        }
        if incoming.terminal != installed.terminal {
            return Err(UiError::protocol(
                "inconsistent final Dataset snapshot for one generation",
            ));
        }
        return Ok(Observation::Current);
    }
    if installed.progress.phase != crate::generated::DatasetCompilePhase::Idle
        && incoming.progress.phase == crate::generated::DatasetCompilePhase::Idle
    {
        incoming.progress = installed.progress.clone();
    }
    if artifact_outcome_rank(incoming.terminal.outcome)
        < artifact_outcome_rank(installed.terminal.outcome)
    {
        incoming.terminal = installed.terminal.clone();
        incoming.active = installed.active;
    }
    if incoming == *installed {
        return Ok(Observation::Current);
    }
    *installed = incoming;
    Ok(Observation::Installed)
}

pub(super) fn merge_dialog_snapshot(
    target: &mut Option<FileDialogSnapshot>,
    // CLEANUP-IGNORE: FileDialog's incoming state enters lifecycle-specific reconciliation.
    incoming: FileDialogSnapshot,
    // CLEANUP-IGNORE: FileDialog installation begins lifecycle-specific reconciliation.
) -> Result<Observation, UiError> {
    let Some(installed) = target.as_mut() else {
        *target = Some(incoming);
        return Ok(Observation::Installed);
    };
    if incoming.generation < installed.generation {
        return Ok(Observation::Stale);
    }
    if incoming.generation > installed.generation {
        *installed = incoming;
        return Ok(Observation::Installed);
    }
    if !installed.active {
        if incoming.active {
            return Ok(Observation::Stale);
        }
        if incoming != *installed {
            return Err(UiError::protocol(
                "inconsistent final FileDialog snapshot revision",
            ));
        }
        return Ok(Observation::Current);
    }
    if incoming == *installed {
        return Ok(Observation::Current);
    }
    *installed = incoming;
    Ok(Observation::Installed)
}

pub(super) fn merge_live_snapshot(
    target: &mut Option<LiveSnapshot>,
    incoming: LiveSnapshot,
) -> Result<Observation, UiError> {
    merge_observation(target, incoming, |value| value.revision, "Live")
}

pub(super) fn merge_predict_snapshot(
    target: &mut Option<PredictSnapshot>,
    mut incoming: PredictSnapshot,
) -> Result<Observation, UiError> {
    if incoming.operation.terminal.outcome != ComputeOperationOutcome::Succeeded {
        incoming.frame = invalid_visual_frame();
    }
    let Some(installed) = target.as_mut() else {
        *target = Some(incoming);
        return Ok(Observation::Installed);
    };
    if incoming.revision < installed.revision {
        return Ok(Observation::Stale);
    }
    if incoming.revision == installed.revision {
        return if incoming == *installed {
            Ok(Observation::Current)
        } else {
            Err(UiError::protocol("inconsistent Predict snapshot revision"))
        };
    }
    let prior_generation = installed.operation.generationfrontier;
    let incoming_generation = incoming.operation.generationfrontier;
    let mut operation = installed.operation.clone();
    let operation_observation = merge_compute_state(&mut operation, incoming.operation.clone())?;
    if operation_observation == Observation::Stale {
        return Ok(Observation::Stale);
    }
    if incoming_generation > prior_generation {
        *installed = incoming;
        return Ok(Observation::Installed);
    }
    if operation_observation == Observation::Installed {
        installed.revision = incoming.revision;
        installed.operation = operation;
        installed.frame = incoming.frame;
        return Ok(Observation::Installed);
    }
    if operation_observation == Observation::Current && incoming.frame == installed.frame {
        installed.revision = incoming.revision;
        return Ok(Observation::Installed);
    }
    if incoming.frame.revision < installed.frame.revision {
        return Ok(Observation::Stale);
    }
    if incoming.frame.revision == installed.frame.revision {
        return Err(UiError::protocol(
            "inconsistent Predict frame for one operation observation",
        ));
    }
    installed.revision = incoming.revision;
    installed.frame = incoming.frame;
    Ok(Observation::Installed)
}

pub(crate) fn invalid_visual_frame() -> VisualFrame {
    VisualFrame {
        source: PresentationSourceIdentity {
            kind: PresentationSourceKind::None,
            instance: 0,
        },
        extent: VisualExtent {
            width: 0,
            height: 0,
        },
        revision: 0,
        cleanrevision: 0,
        content: crate::generated::VisualRegion {
            x: 0,
            y: 0,
            width: 0,
            height: 0,
        },
    }
}

const fn compute_outcome_is_final(outcome: ComputeOperationOutcome) -> bool {
    matches!(
        outcome,
        ComputeOperationOutcome::Succeeded
            | ComputeOperationOutcome::Failed
            | ComputeOperationOutcome::Cancelled
            | ComputeOperationOutcome::Refused
    )
}

const fn compute_outcome_rank(outcome: ComputeOperationOutcome) -> u8 {
    match outcome {
        ComputeOperationOutcome::Idle => 0,
        ComputeOperationOutcome::Running => 1,
        ComputeOperationOutcome::CancellationRequested => 2,
        ComputeOperationOutcome::Succeeded
        | ComputeOperationOutcome::Failed
        | ComputeOperationOutcome::Cancelled
        | ComputeOperationOutcome::Refused => 3,
    }
}

const fn artifact_outcome_is_final(outcome: crate::generated::ArtifactTerminalOutcome) -> bool {
    matches!(
        outcome,
        crate::generated::ArtifactTerminalOutcome::Succeeded
            | crate::generated::ArtifactTerminalOutcome::Failed
            | crate::generated::ArtifactTerminalOutcome::Cancelled
            | crate::generated::ArtifactTerminalOutcome::Refused
    )
}

const fn artifact_outcome_rank(outcome: crate::generated::ArtifactTerminalOutcome) -> u8 {
    match outcome {
        crate::generated::ArtifactTerminalOutcome::Idle => 0,
        crate::generated::ArtifactTerminalOutcome::CancellationRequested => 1,
        crate::generated::ArtifactTerminalOutcome::Succeeded
        | crate::generated::ArtifactTerminalOutcome::Failed
        | crate::generated::ArtifactTerminalOutcome::Cancelled
        | crate::generated::ArtifactTerminalOutcome::Refused => 2,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view_model::test_support::*;

    #[test]
    fn application_model_retains_authoritative_component_snapshots() {
        let model = bootstrapped();
        assert!(model.settings_snapshot.is_some());
        assert!(model.explore.snapshot.is_some());
        assert!(model.annotation.snapshot.is_some());
    }

    #[test]
    fn bootstrap_is_atomic_and_fingerprint_checked() {
        let snapshots = crate::generated::application_snapshot_defaults().unwrap();
        let complete: Vec<_> = snapshots.iter().map(|fact| fact.value.clone()).collect();
        let mut model = ApplicationModel::default();
        assert!(model.install_bootstrap([0, 0], complete.clone()).is_err());
        let mut missing = complete.clone();
        missing.pop();
        assert!(
            model
                .install_bootstrap(crate::generated::SCHEMA_FINGERPRINT, missing)
                .is_err()
        );
        let mut duplicate = complete.clone();
        duplicate[0] = duplicate[1].clone();
        assert!(
            model
                .install_bootstrap(crate::generated::SCHEMA_FINGERPRINT, duplicate)
                .is_err()
        );
        assert_eq!(model.connection, ConnectionState::Connecting);
        model
            .install_bootstrap(crate::generated::SCHEMA_FINGERPRINT, complete)
            .unwrap();
        assert_eq!(model.connection, ConnectionState::Connected);
    }

    #[test]
    fn compute_merge_ignores_stale_activity_after_terminal() {
        let crate::generated::ApplicationSnapshot::Validation(mut installed) =
            crate::generated::application_snapshot_defaults()
                .unwrap()
                .into_iter()
                .find_map(|fact| match fact.value {
                    crate::generated::ApplicationSnapshot::Validation(value) => {
                        Some(crate::generated::ApplicationSnapshot::Validation(value))
                    }
                    _ => None,
                })
                .unwrap()
        else {
            unreachable!()
        };
        installed.generationfrontier = 4;
        installed.active = true;
        installed.terminal.generation = 4;
        installed.terminal.outcome = ComputeOperationOutcome::Running;
        let stale = installed.clone();
        let mut terminal = installed.clone();
        terminal.active = false;
        terminal.terminal.outcome = ComputeOperationOutcome::Succeeded;
        assert_eq!(
            merge_compute_state(&mut installed, terminal.clone()).unwrap(),
            Observation::Installed
        );
        assert_eq!(
            merge_compute_state(&mut installed, stale).unwrap(),
            Observation::Stale
        );
        assert_eq!(installed, terminal);
    }

    #[test]
    fn dialog_terminal_wins_over_late_active_observation() {
        let fact = crate::generated::FILE_DIALOGS
            .iter()
            .find(|fact| fact.workflows.contains(&FeatureId::Train))
            .unwrap();
        let mut model = bootstrapped();
        model.register_dialog(fact, FeatureId::Train).unwrap();
        let active = FileDialogSnapshot {
            generation: 7,
            active: true,
            cancellationrequested: false,
            target: crate::generated::FileDialogTarget::SettingsFieldTarget(
                crate::generated::SettingsFieldTarget {
                    stableid: fact.stable_field_id,
                },
            ),
            selection: None,
        };
        model.file_dialog = Some(active.clone());
        let terminal = FileDialogSnapshot {
            active: false,
            ..active.clone()
        };
        model.install_dialog_terminal(terminal.clone()).unwrap();
        assert_eq!(
            merge_dialog_snapshot(&mut model.file_dialog, active).unwrap(),
            Observation::Stale
        );
        assert_eq!(model.file_dialog, Some(terminal));
        assert!(model.dialog_context().is_none());
    }

    #[test]
    fn dialog_reply_kind_identity_and_current_terminal_share_one_installation_path() {
        let fact = crate::generated::FILE_DIALOGS
            .iter()
            .find(|fact| fact.workflows.contains(&FeatureId::Train))
            .unwrap();
        let snapshot = FileDialogSnapshot {
            generation: 11,
            active: true,
            cancellationrequested: false,
            target: crate::generated::FileDialogTarget::SettingsFieldTarget(
                crate::generated::SettingsFieldTarget {
                    stableid: fact.stable_field_id,
                },
            ),
            selection: None,
        };
        for (context, reply) in [
            (
                ApplicationIntentEndpoint::FileDialogOpen,
                ApplicationReply::FileDialogStop(snapshot.clone()),
            ),
            (
                ApplicationIntentEndpoint::FileDialogStop,
                ApplicationReply::FileDialogOpen(snapshot.clone()),
            ),
            (
                ApplicationIntentEndpoint::FileDialogOpen,
                ApplicationReply::FileDialogOpen(FileDialogSnapshot {
                    target: crate::generated::FileDialogTarget::SettingsFieldTarget(
                        crate::generated::SettingsFieldTarget {
                            stableid: fact.stable_field_id + 1,
                        },
                    ),
                    ..snapshot.clone()
                }),
            ),
        ] {
            let mut model = bootstrapped();
            model.register_dialog(fact, FeatureId::Train).unwrap();
            let correlation = model.begin_intent(context).unwrap();
            let installed = model.file_dialog.clone();
            model.reduce_reply(correlation, Ok(reply));
            assert_eq!(model.file_dialog, installed);
            assert!(model.dialog_context().is_none());
            assert_eq!(model.error.as_ref().unwrap().kind, UiErrorKind::Protocol);
        }

        let mut model = bootstrapped();
        model.register_dialog(fact, FeatureId::Train).unwrap();
        let correlation = model
            .begin_intent(ApplicationIntentEndpoint::FileDialogOpen)
            .unwrap();
        let terminal = FileDialogSnapshot {
            active: false,
            ..snapshot
        };
        model.file_dialog = Some(terminal.clone());
        model.reduce_reply(
            correlation,
            Ok(ApplicationReply::FileDialogOpen(terminal.clone())),
        );
        assert_eq!(model.file_dialog, Some(terminal));
        assert!(model.dialog_context().is_none());
        assert!(model.error.is_none());
    }

    #[test]
    fn model_dialog_terminals_correlate_the_complete_registered_target() {
        use crate::generated::{
            FileDialogCancelled, FileDialogCancelledOrFileDialogSelectedVariant,
            FileDialogSelected, FileDialogSelection, FileDialogTarget, ModelArtifactTarget,
        };

        let fact = crate::generated::MODEL_ARTIFACT_DIALOGS.first().unwrap();
        let terminal = |generation, target: ModelArtifactTarget, selected| FileDialogSnapshot {
            generation,
            active: false,
            cancellationrequested: false,
            target: FileDialogTarget::ModelArtifactTarget(target.clone()),
            selection: Some(FileDialogSelection {
                target: FileDialogTarget::ModelArtifactTarget(target),
                result: if selected {
                    FileDialogCancelledOrFileDialogSelectedVariant::FileDialogSelected(
                        FileDialogSelected {
                            path: "/tmp/model.pth".into(),
                        },
                    )
                } else {
                    FileDialogCancelledOrFileDialogSelectedVariant::FileDialogCancelled(
                        FileDialogCancelled {},
                    )
                },
            }),
        };

        for selected in [true, false] {
            let mut model = bootstrapped();
            model
                .register_model_dialog(&FileDialogTarget::ModelArtifactTarget(fact.target.clone()))
                .unwrap();
            assert_eq!(
                model
                    .install_dialog_terminal(terminal(7, fact.target.clone(), selected))
                    .unwrap(),
                Observation::Installed
            );
            assert!(model.dialog_context().is_none());
        }

        let mut failed = bootstrapped();
        failed
            .register_model_dialog(&FileDialogTarget::ModelArtifactTarget(fact.target.clone()))
            .unwrap();
        assert_eq!(
            failed
                .install_dialog_terminal(FileDialogSnapshot {
                    generation: 8,
                    active: false,
                    cancellationrequested: false,
                    target: FileDialogTarget::ModelArtifactTarget(fact.target.clone()),
                    selection: None,
                })
                .unwrap(),
            Observation::Installed
        );

        for wrong_target in [
            ModelArtifactTarget {
                stableid: fact.stable_field_id.wrapping_add(1),
                ..fact.target.clone()
            },
            ModelArtifactTarget {
                workflow: FeatureId::Validate,
                ..fact.target.clone()
            },
            ModelArtifactTarget {
                input: crate::generated::ModelArtifactInputKind::Onnx,
                ..fact.target.clone()
            },
        ] {
            let mut mismatched = bootstrapped();
            mismatched
                .register_model_dialog(&FileDialogTarget::ModelArtifactTarget(fact.target.clone()))
                .unwrap();
            assert!(
                mismatched
                    .install_dialog_terminal(terminal(9, wrong_target, true))
                    .is_err()
            );
        }

        let mut stale = bootstrapped();
        stale.file_dialog = Some(terminal(11, fact.target.clone(), true));
        stale
            .register_model_dialog(&FileDialogTarget::ModelArtifactTarget(fact.target.clone()))
            .unwrap();
        assert_eq!(
            stale
                .install_dialog_terminal(terminal(10, fact.target.clone(), true))
                .unwrap(),
            Observation::Stale
        );
        assert!(stale.dialog_context().is_some());
    }

    #[test]
    fn native_training_failure_wins_over_late_admission_reply() {
        let mut model = bootstrapped();
        let correlation = model
            .begin_intent(ApplicationIntentEndpoint::TrainingQuery)
            .unwrap();
        let mut admitted = model.workflow.training.clone().unwrap();
        admitted.revision += 1;
        admitted.activity = crate::generated::TrainingActivity::ProviderQuery;
        admitted.offers.revision += 1;
        let mut failed = admitted.clone();
        failed.revision += 1;
        failed.activity = crate::generated::TrainingActivity::Idle;
        failed.offers.revision += 1;
        failed.offers.outcome = crate::generated::ProviderQueryOutcome::Failed;
        failed.offers.detail = "provider unavailable".into();
        model.reduce_event(ApplicationEvent::TrainingTrainingChanged(
            crate::generated::TrainingChanged {
                snapshot: failed.clone(),
            },
        ));
        assert_eq!(model.error.as_ref().unwrap().detail, "provider unavailable");
        model.error = None;
        model.reduce_reply(correlation, Ok(ApplicationReply::TrainingQuery(admitted)));
        assert_eq!(model.workflow.training, Some(failed));
        assert!(model.error.is_none());
    }

    #[test]
    fn dataset_terminal_and_progress_ordering_preserve_native_truth() {
        let mut model = bootstrapped();
        let compile = model
            .begin_intent(ApplicationIntentEndpoint::DatasetCompile)
            .unwrap();
        let mut active = model.workflow.dataset.clone().unwrap();
        active.generation += 1;
        active.active = true;
        active.progress.phase = crate::generated::DatasetCompilePhase::Pixels;
        active.progress.activity = "Encoding images".into();
        active.progress.completed = 12;
        active.progress.total = 40;
        model.reduce_event(ApplicationEvent::DatasetDatasetChanged(
            crate::generated::DatasetChanged {
                snapshot: active.clone(),
            },
        ));
        assert_eq!(
            model.workflow.dataset.as_ref().unwrap().progress.completed,
            12
        );
        let mut settled = active.clone();
        settled.active = false;
        settled.terminal.outcome = crate::generated::ArtifactTerminalOutcome::Cancelled;
        model.reduce_event(ApplicationEvent::DatasetDatasetChanged(
            crate::generated::DatasetChanged {
                snapshot: settled.clone(),
            },
        ));
        model.reduce_reply(compile, Ok(ApplicationReply::DatasetCompile(active)));
        assert_eq!(model.workflow.dataset, Some(settled));
    }

    #[test]
    fn visual_reducers_ignore_late_admission_and_equal_conflicts() {
        let mut model = bootstrapped();
        let mut admitted = model.explore.snapshot.clone().unwrap();
        admitted.revision += 1;
        admitted.busy = true;
        let mut settled = admitted.clone();
        settled.revision += 1;
        settled.busy = false;
        assert_eq!(
            merge_explore_snapshot(&mut model.explore.snapshot, settled.clone()).unwrap(),
            Observation::Installed
        );
        assert_eq!(
            merge_explore_snapshot(&mut model.explore.snapshot, admitted).unwrap(),
            Observation::Stale
        );
        let mut conflicting = settled;
        conflicting.ready = !conflicting.ready;
        assert!(merge_explore_snapshot(&mut model.explore.snapshot, conflicting).is_err());
    }

    #[test]
    fn live_failure_wins_over_late_admission_and_stale_failure_is_quiet() {
        let mut model = bootstrapped();
        let start = model
            .begin_intent(ApplicationIntentEndpoint::LiveStart)
            .unwrap();
        let mut admitted = model.live_snapshot.clone().unwrap();
        admitted.revision += 1;
        admitted.running = true;
        model.reduce_reply(start, Ok(ApplicationReply::LiveStart(admitted.clone())));
        let mut failed = admitted.clone();
        failed.revision += 1;
        failed.running = false;
        model.reduce_event(ApplicationEvent::LiveLiveFailed(
            crate::generated::LiveFailed {
                snapshot: failed.clone(),
                detail: "capture failed".into(),
            },
        ));
        assert_eq!(model.live_snapshot, Some(failed.clone()));
        model.error = None;
        model.reduce_event(ApplicationEvent::LiveLiveFailed(
            crate::generated::LiveFailed {
                snapshot: admitted,
                detail: "stale failure".into(),
            },
        ));
        assert_eq!(model.live_snapshot, Some(failed));
        assert!(model.error.is_none());
    }

    #[test]
    fn generated_settings_constraints_and_typography_remain_authoritative() {
        let model = bootstrapped();
        assert_eq!(
            model.typography().primary,
            model
                .settings_snapshot
                .as_ref()
                .unwrap()
                .settingsstate
                .ui
                .fontsize
        );
        let items = crate::generated::constraint_workflowstrainrequestdeviceids();
        let leaf = crate::generated::SETTINGS_LEAVES
            .iter()
            .find(|leaf| leaf.stable_field_id == items.stable_field_id)
            .unwrap();
        assert_eq!(items.maximum_items, leaf.maximum_items);
        assert_ne!(items.maximum_items, 0);
    }

    #[test]
    fn generated_annotation_inventories_are_directly_usable() {
        assert!(
            crate::generated::ANNOTATION_TOOL_VALUES
                .contains(&crate::generated::AnnotationTool::Box)
        );
        assert!(
            crate::generated::ANNOTATION_MASK_CLEANUP_VALUES
                .contains(&crate::generated::AnnotationMaskCleanup::LargestComponent)
        );
        let request = crate::generated::AnnotationEditRequest {
            edit: crate::generated::AnnotationEdit::AnnotationUndoEdit(
                crate::generated::AnnotationUndoEdit {},
            ),
        };
        assert!(matches!(
            request.edit,
            crate::generated::AnnotationEdit::AnnotationUndoEdit(_)
        ));
    }

    #[test]
    fn local_provider_and_remote_failures_follow_their_admissions() {
        let mut local = bootstrapped();
        let start = local
            .begin_intent(ApplicationIntentEndpoint::TrainingStart)
            .unwrap();
        let mut training = local.workflow.training.clone().unwrap();
        training.revision += 1;
        training.activity = crate::generated::TrainingActivity::Local;
        training.local.generationfrontier += 1;
        training.local.active = true;
        training.local.terminal.generation = training.local.generationfrontier;
        training.local.terminal.outcome = ComputeOperationOutcome::Running;
        local.reduce_reply(start, Ok(ApplicationReply::TrainingStart(training.clone())));
        training.revision += 1;
        training.activity = crate::generated::TrainingActivity::Idle;
        training.local.active = false;
        training.local.terminal.outcome = ComputeOperationOutcome::Failed;
        training.local.terminal.detail = "local failed".into();
        local.reduce_event(ApplicationEvent::TrainingTrainingChanged(
            crate::generated::TrainingChanged { snapshot: training },
        ));
        assert_eq!(local.error.as_ref().unwrap().detail, "local failed");

        let mut provider = bootstrapped();
        let query = provider
            .begin_intent(ApplicationIntentEndpoint::TrainingQuery)
            .unwrap();
        let mut training = provider.workflow.training.clone().unwrap();
        training.revision += 1;
        training.activity = crate::generated::TrainingActivity::ProviderQuery;
        training.offers.revision += 1;
        provider.reduce_reply(query, Ok(ApplicationReply::TrainingQuery(training.clone())));
        training.revision += 1;
        training.activity = crate::generated::TrainingActivity::Idle;
        training.offers.revision += 1;
        training.offers.outcome = crate::generated::ProviderQueryOutcome::Failed;
        training.offers.detail = "provider failed".into();
        provider.reduce_event(ApplicationEvent::TrainingTrainingChanged(
            crate::generated::TrainingChanged { snapshot: training },
        ));
        assert_eq!(provider.error.as_ref().unwrap().detail, "provider failed");

        let mut remote = bootstrapped();
        let mut training = remote.workflow.training.clone().unwrap();
        training.activity = crate::generated::TrainingActivity::Idle;
        training.offers.selected = Some(crate::generated::ProviderOfferIdentity { offerid: 17 });
        let start = remote
            .begin_intent(ApplicationIntentEndpoint::TrainingStartRemote)
            .unwrap();
        training.revision += 1;
        training.activity = crate::generated::TrainingActivity::Remote;
        training.remote.revision += 1;
        training.remote.outcome = crate::generated::RemoteOperationOutcome::Idle;
        remote.reduce_reply(
            start,
            Ok(ApplicationReply::TrainingStartRemote(training.clone())),
        );
        training.revision += 1;
        training.activity = crate::generated::TrainingActivity::Idle;
        training.remote.revision += 1;
        training.remote.outcome = crate::generated::RemoteOperationOutcome::Inconclusive;
        training.remote.detail = "remote failed".into();
        remote.reduce_event(ApplicationEvent::TrainingTrainingChanged(
            crate::generated::TrainingChanged { snapshot: training },
        ));
        assert_eq!(remote.error.as_ref().unwrap().detail, "remote failed");
    }
}
