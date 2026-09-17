//! Live operation facts remain independent of the chart's selected saved run.
use crate::{
    fluent_theme::Element,
    generated::{ComputeOperationOutcome, TrainingPhase, TrainingSnapshot},
    view_model::ApplicationModel,
};
use iced::widget::{column, container, progress_bar, text};

pub(super) fn active(model: &ApplicationModel) -> bool {
    model.workflow.training.as_ref().is_some_and(|s| s.local.active)
}
fn duration(seconds: f64) -> String {
    if !seconds.is_finite() || seconds < 0.0 { return "—".into(); }
    let seconds = seconds as u64;
    format!("{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60)
}

// Formatted presentation only; the borrowed native snapshot remains authoritative.
struct Presentation {
    heading: String,
    images: Option<String>,
    fraction: Option<f32>,
    timing: Option<String>,
    rate: Option<String>,
    losses: Option<String>,
}
impl Presentation {
    fn from_snapshot(snapshot: &TrainingSnapshot) -> Option<Self> {
        if !snapshot.local.active { return None; }
        let stopping = snapshot.local.terminal.outcome == ComputeOperationOutcome::CancellationRequested;
        let current = snapshot.metrics.as_ref().filter(|_| snapshot.local.progress.sequence != 0);
        let mut result = Self {
            heading: String::new(),
            images: None,
            fraction: None,
            timing: None,
            rate: None,
            losses: None,
        };
        let Some(record) = current else {
            result.heading = if stopping { "Stopping" } else { "Preparing · awaiting current-run measurements" }.into();
            return Some(result);
        };
        let p = &record.progress;
        let phase = if stopping { "Stopping" } else { match p.phase {
            TrainingPhase::Starting => "Preparing",
            TrainingPhase::Train => "Training",
            TrainingPhase::Validate => "Validating",
            TrainingPhase::EpochComplete => "Epoch complete",
            TrainingPhase::Completed => "Finishing",
            TrainingPhase::Error => "Failed",
        }};
        result.heading = format!("{phase} · Epoch {} / {}", (p.epoch + 1).min(p.totalepochs).max(1), p.totalepochs);
        // These native counts/rates describe the training pass even when copied
        // into a validation boundary. Only the active Train phase can expose them.
        let training = !stopping && p.phase == TrainingPhase::Train;
        result.images = Some(if training && p.totalimages > 0 {
            let fraction = (p.completedimages as f64 / p.totalimages as f64).clamp(0.0, 1.0);
            result.fraction = Some(fraction as f32);
            format!("{} / {} images · {:.1}%", p.completedimages, p.totalimages, fraction * 100.0)
        } else if training {
            format!("{} images · image total unavailable", p.completedimages)
        } else {
            "Image progress unavailable in this phase".into()
        });
        // An accumulated batch can precede the first reported optimizer result.
        // Optional total, not numeric default storage or batch count, proves loss
        // availability. A measured but nonfinite value remains explicitly invalid.
        let observed = training && p.scalars.total.is_some();
        let measured_rate = observed && p.completedimages > 0
            && p.elapsedseconds.is_finite() && p.elapsedseconds > 0.0
            && p.imagespersecond.is_finite() && p.imagespersecond > 0.0;
        let eta = if measured_rate && p.totalimages > 0 {
            duration(p.totalimages.saturating_sub(p.completedimages) as f64 / p.imagespersecond)
        } else { "Unavailable".into() };
        result.timing = Some(format!("Elapsed {} · Epoch ETA {eta}", duration(p.elapsedseconds)));
        result.rate = Some(if training && !observed {
            "Warming up".into()
        } else if measured_rate {
            format!("{:.1} images/sec", p.imagespersecond)
        } else if training {
            "Rate unavailable".into()
        } else {
            "Rate unavailable in this phase".into()
        });
        if let Some(total) = p.scalars.total.filter(|_| training) {
            let value = |v: f64| if v.is_finite() { format!("{v:.4}") } else { "Unavailable".into() };
            result.losses = Some(format!("Classification {} · Box {} · Total {}", value(p.classloss), value(p.boxloss), value(total)));
        }
        Some(result)
    }
}

pub(super) fn view(model: &ApplicationModel) -> Element<'_, super::Message> {
    let Some(facts) = model.workflow.training.as_ref().and_then(Presentation::from_snapshot) else {
        return column![].into();
    };
    let mut content = column![text(facts.heading)].spacing(crate::view::workflow::FIELD_SPACING);
    if let Some(images) = facts.images { content = content.push(text(images)); }
    if let Some(fraction) = facts.fraction { content = content.push(progress_bar(0.0..=1.0, fraction)); }
    for label in [facts.timing, facts.rate, facts.losses].into_iter().flatten() {
        content = content.push(text(label));
    }
    container(crate::view::shared::card("Training progress", "", content)).id("train.card.progress").into()
}
#[cfg(test)]
mod tests {
    use super::*;

    fn snapshot() -> TrainingSnapshot {
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut snapshot = model.workflow.training.take().unwrap();
        snapshot.local.active = true;
        snapshot.local.progress.sequence = 1;
        snapshot.metrics = Some(crate::view::metrics::tests::record());
        let p = &mut snapshot.metrics.as_mut().unwrap().progress;
        p.imagespersecond = 2.0;
        p.elapsedseconds = 5.0;
        p.classloss = 0.25;
        p.boxloss = 0.5;
        snapshot
    }

    #[test]
    fn preparation_and_inactive_states_do_not_expose_old_measurements() {
        let mut snapshot = snapshot();
        snapshot.local.progress.sequence = 0;
        let facts = Presentation::from_snapshot(&snapshot).unwrap();
        assert_eq!(facts.heading, "Preparing · awaiting current-run measurements");
        assert!(facts.images.is_none() && facts.fraction.is_none());
        assert!(facts.rate.is_none() && facts.losses.is_none() && facts.timing.is_none());
        snapshot.local.progress.sequence = 1;
        snapshot.metrics.as_mut().unwrap().progress.phase = TrainingPhase::Starting;
        let facts = Presentation::from_snapshot(&snapshot).unwrap();
        assert_eq!(facts.heading, "Preparing · Epoch 2 / 3");
        assert!(facts.fraction.is_none() && facts.losses.is_none());
        assert_eq!(facts.images.as_deref(), Some("Image progress unavailable in this phase"));
        for outcome in [ComputeOperationOutcome::Idle, ComputeOperationOutcome::Succeeded,
            ComputeOperationOutcome::Cancelled, ComputeOperationOutcome::Failed] {
            snapshot.local.active = false;
            snapshot.local.terminal.outcome = outcome;
            assert!(Presentation::from_snapshot(&snapshot).is_none());
        }
    }

    #[test]
    fn accumulated_training_batches_do_not_invent_optimizer_measurements() {
        let mut snapshot = snapshot();
        let p = &mut snapshot.metrics.as_mut().unwrap().progress;
        p.optimizersteps = 0;
        p.scalars.total = None;
        p.trainloss = 0.0;
        p.classloss = 0.0;
        p.boxloss = 0.0;
        let facts = Presentation::from_snapshot(&snapshot).unwrap();
        assert_eq!(facts.images.as_deref(), Some("4 / 40 images · 10.0%"));
        assert_eq!(facts.fraction, Some(0.1));
        assert_eq!(facts.rate.as_deref(), Some("Warming up"));
        assert!(facts.losses.is_none());
        assert_eq!(facts.timing.as_deref(), Some("Elapsed 0:00:05 · Epoch ETA Unavailable"));
    }

    #[test]
    fn measured_progress_and_phase_changes_use_only_current_phase_facts() {
        let mut snapshot = snapshot();
        let facts = Presentation::from_snapshot(&snapshot).unwrap();
        assert_eq!(facts.heading, "Training · Epoch 2 / 3");
        assert_eq!(facts.images.as_deref(), Some("4 / 40 images · 10.0%"));
        assert_eq!(facts.rate.as_deref(), Some("2.0 images/sec"));
        assert_eq!(facts.timing.as_deref(), Some("Elapsed 0:00:05 · Epoch ETA 0:00:18"));
        assert_eq!(facts.losses.as_deref(), Some("Classification 0.2500 · Box 0.5000 · Total 1.0000"));
        for phase in [TrainingPhase::Validate, TrainingPhase::EpochComplete, TrainingPhase::Completed, TrainingPhase::Error] {
            let p = &mut snapshot.metrics.as_mut().unwrap().progress;
            p.phase = phase;
            p.completedimages = p.totalimages;
            let facts = Presentation::from_snapshot(&snapshot).unwrap();
            assert_eq!(facts.images.as_deref(), Some("Image progress unavailable in this phase"));
            assert_eq!(facts.rate.as_deref(), Some("Rate unavailable in this phase"));
            assert_eq!(facts.timing.as_deref(), Some("Elapsed 0:00:05 · Epoch ETA Unavailable"));
            assert!(facts.fraction.is_none() && facts.losses.is_none());
        }
        snapshot.metrics.as_mut().unwrap().progress.phase = TrainingPhase::Train;
        snapshot.local.terminal.outcome = ComputeOperationOutcome::CancellationRequested;
        let facts = Presentation::from_snapshot(&snapshot).unwrap();
        assert_eq!(facts.heading, "Stopping · Epoch 2 / 3");
        assert_eq!(facts.images.as_deref(), Some("Image progress unavailable in this phase"));
        assert_eq!(facts.rate.as_deref(), Some("Rate unavailable in this phase"));
        assert!(facts.fraction.is_none() && facts.losses.is_none());
        assert_eq!(facts.timing.as_deref(), Some("Elapsed 0:00:05 · Epoch ETA Unavailable"));
    }

    #[test]
    fn invalid_or_unknown_measurements_do_not_produce_a_rate_or_eta() {
        for (rate, elapsed) in [(f64::NAN, 5.0), (-1.0, 5.0), (0.0, 5.0), (2.0, f64::INFINITY), (2.0, -1.0), (2.0, 0.0)] {
            let mut snapshot = snapshot();
            let p = &mut snapshot.metrics.as_mut().unwrap().progress;
            p.imagespersecond = rate;
            p.elapsedseconds = elapsed;
            let facts = Presentation::from_snapshot(&snapshot).unwrap();
            assert_eq!(facts.rate.as_deref(), Some("Rate unavailable"));
            assert!(facts.timing.as_ref().unwrap().ends_with("Epoch ETA Unavailable"));
        }
        let mut snapshot = snapshot();
        let p = &mut snapshot.metrics.as_mut().unwrap().progress;
        p.totalimages = 0;
        p.scalars.total = Some(f64::NAN);
        p.classloss = f64::INFINITY;
        let facts = Presentation::from_snapshot(&snapshot).unwrap();
        assert_eq!(facts.images.as_deref(), Some("4 images · image total unavailable"));
        assert!(facts.fraction.is_none());
        assert!(facts.timing.as_ref().unwrap().ends_with("Epoch ETA Unavailable"));
        assert_eq!(facts.losses.as_deref(), Some("Classification Unavailable · Box 0.5000 · Total Unavailable"));
        assert_eq!(duration(f64::NAN), "—");
        assert_eq!(duration(-1.0), "—");
        assert_eq!(duration(3661.0), "1:01:01");
    }
}
