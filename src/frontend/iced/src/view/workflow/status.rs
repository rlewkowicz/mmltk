pub fn compute_progress_label(progress: Option<&crate::generated::ComputeProgress>) -> String {
    progress.map_or_else(
        || "Idle".to_owned(),
        |progress| {
            if progress.total == 0 {
                progress.status.clone()
            } else {
                format!(
                    "{} — {} / {}",
                    progress.status, progress.completed, progress.total
                )
            }
        },
    )
}

pub fn compute_status(state: Option<&crate::generated::ComputeUiState>) -> String {
    let Some(state) = state else {
        return "Waiting for native compute state".to_owned();
    };
    let terminal = &state.terminal;
    let detail = if !terminal.detail.is_empty() {
        terminal.detail.as_str()
    } else {
        terminal.output.as_str()
    };
    format!(
        "{} · {:?}{}{}",
        compute_progress_label(Some(&state.progress)),
        terminal.outcome,
        (!detail.is_empty()).then_some(" · ").unwrap_or_default(),
        detail
    )
}

pub fn artifact_status(state: Option<&crate::generated::ArtifactUiState>) -> String {
    let Some(state) = state else {
        return "Waiting for native Dataset state".to_owned();
    };
    let terminal = &state.terminal;
    let detail = if !terminal.detail.is_empty() {
        terminal.detail.as_str()
    } else {
        terminal.artifact.as_str()
    };
    format!(
        "{} · {:?}{}{}",
        state.progress.activity,
        terminal.outcome,
        (!detail.is_empty()).then_some(" · ").unwrap_or_default(),
        detail
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn generated_terminal_facts_drive_status_copy() {
        let snapshots = crate::generated::application_snapshot_defaults().unwrap();
        let mut compute = snapshots
            .iter()
            .find_map(|fact| match &fact.value {
                crate::generated::ApplicationSnapshot::Validation(value) => Some(value.clone()),
                _ => None,
            })
            .unwrap();
        compute.operation.terminal.outcome = crate::generated::ComputeOperationOutcome::Cancelled;
        compute.operation.terminal.detail = "cancelled by native owner".into();
        let compute_label = compute_status(Some(&compute.operation));
        assert!(compute_label.contains("Cancelled"));
        assert!(compute_label.contains("cancelled by native owner"));

        let mut dataset = snapshots
            .into_iter()
            .find_map(|fact| match fact.value {
                crate::generated::ApplicationSnapshot::Dataset(value) => Some(value),
                _ => None,
            })
            .unwrap();
        dataset.terminal.outcome = crate::generated::ArtifactTerminalOutcome::Succeeded;
        dataset.terminal.artifact = "generated artifact".into();
        let dataset_label = artifact_status(Some(&dataset));
        assert!(dataset_label.contains("Succeeded"));
        assert!(dataset_label.contains("generated artifact"));
    }
}
