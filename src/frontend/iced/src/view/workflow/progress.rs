use crate::fluent_theme::Element;
use iced::Fill;
use iced::widget::{column, progress_bar, text};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Presentation {
    Hidden,
    Active,
    Determinate {
        stage: String,
        activity: String,
        completed: u64,
        total: u64,
    },
    OpenEnded {
        stage: String,
        activity: String,
        completed: u64,
    },
    Terminal {
        outcome: String,
        detail: String,
    },
}

pub fn compute_presentation(state: Option<&crate::generated::ComputeUiState>) -> Presentation {
    let Some(state) = state else {
        return Presentation::Hidden;
    };
    if !state.active {
        return match state.terminal.outcome {
            crate::generated::ComputeOperationOutcome::Idle => Presentation::Hidden,
            outcome => Presentation::Terminal {
                outcome: format!("{outcome:?}"),
                detail: if state.terminal.detail.is_empty() {
                    state.terminal.output.clone()
                } else {
                    state.terminal.detail.clone()
                },
            },
        };
    }
    if state.progress.sequence == 0 || state.progress.status.is_empty() {
        return if state.terminal.detail.is_empty() {
            Presentation::Active
        } else {
            Presentation::OpenEnded {
                stage: state.terminal.detail.clone(),
                activity: String::new(),
                completed: 0,
            }
        };
    }
    if state.progress.total == 0 {
        Presentation::OpenEnded {
            stage: state.progress.status.clone(),
            activity: String::new(),
            completed: state.progress.completed,
        }
    } else {
        Presentation::Determinate {
            stage: state.progress.status.clone(),
            activity: String::new(),
            completed: state.progress.completed,
            total: state.progress.total,
        }
    }
}

pub fn artifact_presentation(state: Option<&crate::generated::ArtifactUiState>) -> Presentation {
    let Some(state) = state else {
        return Presentation::Hidden;
    };
    if !state.active {
        return match state.terminal.outcome {
            crate::generated::ArtifactTerminalOutcome::Idle => Presentation::Hidden,
            outcome => Presentation::Terminal {
                outcome: format!("{outcome:?}"),
                detail: if state.terminal.detail.is_empty() {
                    state.terminal.artifact.clone()
                } else {
                    state.terminal.detail.clone()
                },
            },
        };
    }
    if state.progress.phase == crate::generated::DatasetCompilePhase::Idle {
        return Presentation::Active;
    }
    if state.progress.total == 0 {
        Presentation::OpenEnded {
            stage: format!("{:?}", state.progress.phase),
            activity: state.progress.activity.clone(),
            completed: state.progress.completed,
        }
    } else {
        Presentation::Determinate {
            stage: format!("{:?}", state.progress.phase),
            activity: state.progress.activity.clone(),
            completed: state.progress.completed,
            total: state.progress.total,
        }
    }
}

pub fn model_presentation(state: Option<&crate::generated::ModelUiState>) -> Presentation {
    let Some(state) = state else {
        return Presentation::Hidden;
    };
    if state.terminal.outcome == crate::generated::ModelSelectionOutcome::CancellationRequested {
        return Presentation::Terminal {
            outcome: format!("{:?}", state.terminal.outcome),
            detail: state.terminal.detail.clone(),
        };
    }
    if state.active {
        if state.progress.stage == crate::generated::ModelProgressStage::Idle
            && state.progress.activity.is_empty()
            && state.progress.completed == 0
        {
            return Presentation::Active;
        }
        let stage = format!("{:?}", state.progress.stage);
        if state.progress.totalknown && state.progress.total > 0 {
            return Presentation::Determinate {
                stage,
                activity: state.progress.activity.clone(),
                completed: state.progress.completed.min(state.progress.total),
                total: state.progress.total,
            };
        }
        return Presentation::OpenEnded {
            stage,
            activity: state.progress.activity.clone(),
            completed: state.progress.completed,
        };
    }
    match state.terminal.outcome {
        crate::generated::ModelSelectionOutcome::Idle => Presentation::Hidden,
        outcome => Presentation::Terminal {
            outcome: format!("{outcome:?}"),
            detail: state.terminal.detail.clone(),
        },
    }
}

fn facts<Message: 'static>(
    stage: String,
    activity: String,
    completed: u64,
    total: u64,
    metrics: impl IntoIterator<Item = String>,
) -> Element<'static, Message> {
    let mut content = column![text(stage)].spacing(4).width(Fill);
    if !activity.is_empty() {
        content = content.push(text(activity).size(12));
    }
    content = if total == 0 {
        content.push(text(format!("{completed} completed")).size(12))
    } else {
        content
            .push(text(format!("{completed} / {total}")).size(12))
            .push(progress_bar(
                0.0..=total as f32,
                completed.min(total) as f32,
            ))
    };
    metrics
        .into_iter()
        .fold(content, |content, metric| {
            content.push(text(metric).size(12))
        })
        .into()
}

fn render<Message: 'static>(
    presentation: Presentation,
    metrics: impl IntoIterator<Item = String>,
) -> Element<'static, Message> {
    match presentation {
        Presentation::Hidden => return column![].into(),
        Presentation::Active => return text("Active").size(12).into(),
        Presentation::Terminal { outcome, detail } => {
            return text(if detail.is_empty() {
                outcome
            } else {
                format!("{outcome} · {detail}")
            })
            .size(12)
            .into();
        }
        Presentation::Determinate {
            stage,
            activity,
            completed,
            total,
        } => facts(stage, activity, completed, total, metrics),
        Presentation::OpenEnded {
            stage,
            activity,
            completed,
        } => facts(stage, activity, completed, 0, metrics),
    }
}

pub fn presentation<Message: 'static>(presentation: Presentation) -> Element<'static, Message> {
    render(presentation, std::iter::empty())
}

pub fn compute<Message: 'static>(
    state: Option<&crate::generated::ComputeUiState>,
) -> Element<'static, Message> {
    presentation(compute_presentation(state))
}

fn artifact_metrics(progress: &crate::generated::ArtifactProgress) -> [Option<String>; 6] {
    [
        (progress.elapsedseconds != 0).then(|| format!("{}s elapsed", progress.elapsedseconds)),
        (progress.remainingseconds != 0)
            .then(|| format!("{}s remaining", progress.remainingseconds)),
        (progress.throughputpersecond != 0).then(|| format!("{}/s", progress.throughputpersecond)),
        (progress.projectedoutputbytes != 0)
            .then(|| format!("{} bytes projected", progress.projectedoutputbytes)),
        (progress.droppedinstances != 0)
            .then(|| format!("{} instances dropped", progress.droppedinstances)),
        (progress.quarantinedimages != 0)
            .then(|| format!("{} images quarantined", progress.quarantinedimages)),
    ]
}

pub fn artifact<Message: 'static>(
    state: Option<&crate::generated::ArtifactUiState>,
) -> Element<'static, Message> {
    let Some(progress) = state.map(|state| &state.progress) else {
        return presentation(Presentation::Hidden);
    };
    let metrics = artifact_metrics(progress).into_iter().flatten();
    render(artifact_presentation(state), metrics)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn snapshots() -> Vec<crate::generated::ApplicationSnapshot> {
        crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .map(|fact| fact.value)
            .collect()
    }

    #[test]
    fn compile_progress_is_determinate_without_inventing_open_ended_totals() {
        let mut dataset = snapshots()
            .into_iter()
            .find_map(|snapshot| match snapshot {
                crate::generated::ApplicationSnapshot::Dataset(value) => Some(value),
                _ => None,
            })
            .unwrap();
        dataset.active = true;
        dataset.progress.phase = crate::generated::DatasetCompilePhase::Pixels;
        dataset.progress.activity = "Encoding images".into();
        dataset.progress.completed = 12;
        dataset.progress.total = 40;
        assert!(matches!(
            artifact_presentation(Some(&dataset)),
            Presentation::Determinate {
                completed: 12,
                total: 40,
                ..
            }
        ));
        dataset.progress.total = 0;
        assert!(matches!(
            artifact_presentation(Some(&dataset)),
            Presentation::OpenEnded { completed: 12, .. }
        ));
    }

    #[test]
    fn artifact_metrics_preserve_eta_and_all_compile_quality_facts_together() {
        let mut dataset = snapshots()
            .into_iter()
            .find_map(|snapshot| match snapshot {
                crate::generated::ApplicationSnapshot::Dataset(value) => Some(value),
                _ => None,
            })
            .unwrap();
        dataset.progress.elapsedseconds = 12;
        dataset.progress.remainingseconds = 34;
        dataset.progress.throughputpersecond = 5;
        dataset.progress.projectedoutputbytes = 6;
        dataset.progress.droppedinstances = 7;
        dataset.progress.quarantinedimages = 8;
        assert_eq!(
            artifact_metrics(&dataset.progress)
                .into_iter()
                .flatten()
                .collect::<Vec<_>>(),
            [
                "12s elapsed",
                "34s remaining",
                "5/s",
                "6 bytes projected",
                "7 instances dropped",
                "8 images quarantined",
            ]
        );
    }

    #[test]
    fn prediction_progress_preserves_processed_count_without_a_declared_total() {
        let mut predict = snapshots()
            .into_iter()
            .find_map(|snapshot| match snapshot {
                crate::generated::ApplicationSnapshot::Predict(value) => Some(value),
                _ => None,
            })
            .unwrap();
        predict.operation.active = true;
        predict.operation.progress.sequence = 1;
        predict.operation.progress.completed = 7;
        predict.operation.progress.total = 0;
        predict.operation.progress.status = "Processed".into();
        assert!(matches!(
            compute_presentation(Some(&predict.operation)),
            Presentation::OpenEnded { completed: 7, .. }
        ));
        predict.operation.progress.total = 10;
        assert!(matches!(
            compute_presentation(Some(&predict.operation)),
            Presentation::Determinate {
                completed: 7,
                total: 10,
                ..
            }
        ));
    }

    #[test]
    fn typed_compute_completion_and_failure_remain_visible() {
        let mut validation = snapshots()
            .into_iter()
            .find_map(|snapshot| match snapshot {
                crate::generated::ApplicationSnapshot::Validation(value) => Some(value),
                _ => None,
            })
            .unwrap();
        validation.operation.active = false;
        validation.operation.terminal.outcome =
            crate::generated::ComputeOperationOutcome::Succeeded;
        validation.operation.terminal.output = "metrics.json".into();
        assert!(matches!(
            compute_presentation(Some(&validation.operation)),
            Presentation::Terminal { ref outcome, ref detail }
                if outcome == "Succeeded" && detail == "metrics.json"
        ));
        validation.operation.terminal.outcome = crate::generated::ComputeOperationOutcome::Failed;
        validation.operation.terminal.detail = "device unavailable".into();
        assert!(matches!(
            compute_presentation(Some(&validation.operation)),
            Presentation::Terminal { ref outcome, ref detail }
                if outcome == "Failed" && detail == "device unavailable"
        ));
    }

    #[test]
    fn model_progress_covers_initial_known_unknown_and_terminal_states() {
        let mut model = snapshots()
            .into_iter()
            .find_map(|snapshot| match snapshot {
                crate::generated::ApplicationSnapshot::Model(value) => Some(value),
                _ => None,
            })
            .unwrap();
        model.generation = 1;
        model.active = true;
        model.progress = crate::generated::ModelProgress {
            stage: crate::generated::ModelProgressStage::Idle,
            activity: String::new(),
            completed: 0,
            total: 0,
            totalknown: false,
        };
        assert_eq!(model_presentation(Some(&model)), Presentation::Active);

        model.progress = crate::generated::ModelProgress {
            stage: crate::generated::ModelProgressStage::Downloading,
            activity: "Downloading model weights".into(),
            completed: 9000,
            total: 8192,
            totalknown: true,
        };
        assert_eq!(
            model_presentation(Some(&model)),
            Presentation::Determinate {
                stage: "Downloading".into(),
                activity: "Downloading model weights".into(),
                completed: 8192,
                total: 8192,
            }
        );

        model.progress.total = 0;
        model.progress.totalknown = false;
        model.progress.completed = 4096;
        assert_eq!(
            model_presentation(Some(&model)),
            Presentation::OpenEnded {
                stage: "Downloading".into(),
                activity: "Downloading model weights".into(),
                completed: 4096,
            }
        );

        model.active = false;
        for (outcome, expected) in [
            (
                crate::generated::ModelSelectionOutcome::Accepted,
                "Accepted",
            ),
            (
                crate::generated::ModelSelectionOutcome::Rejected,
                "Rejected",
            ),
            (
                crate::generated::ModelSelectionOutcome::Cancelled,
                "Cancelled",
            ),
        ] {
            model.terminal.outcome = outcome;
            assert!(matches!(
                model_presentation(Some(&model)),
                Presentation::Terminal { outcome, .. } if outcome == expected
            ));
        }
        model.active = true;
        model.terminal.outcome = crate::generated::ModelSelectionOutcome::CancellationRequested;
        assert!(matches!(
            model_presentation(Some(&model)),
            Presentation::Terminal { ref outcome, .. }
                if outcome == "CancellationRequested"
        ));
    }
}
