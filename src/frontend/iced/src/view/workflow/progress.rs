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
    content.into()
}

fn render<Message: 'static>(presentation: Presentation) -> Element<'static, Message> {
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
        } => facts(stage, activity, completed, total),
        Presentation::OpenEnded {
            stage,
            activity,
            completed,
        } => facts(stage, activity, completed, 0),
    }
}

pub fn presentation<Message: 'static>(presentation: Presentation) -> Element<'static, Message> {
    render(presentation)
}

pub fn compute<Message: 'static>(
    state: Option<&crate::generated::ComputeUiState>,
) -> Element<'static, Message> {
    presentation(compute_presentation(state))
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
