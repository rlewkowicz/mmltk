use std::collections::VecDeque;
pub(super) const BUCKETS: usize = 128;
#[derive(Clone, Copy, Debug)]
pub(super) struct Point {
    pub(super) step: f64,
    pub(super) epoch: f64,
    pub(super) value: f64,
    pub(super) order: u64,
}
#[derive(Clone, Debug)]
pub(super) struct Bucket {
    pub(super) segment: u64,
    pub(super) first: Point,
    pub(super) min: Point,
    pub(super) max: Point,
    pub(super) last: Point,
}
impl Bucket {
    fn merge(&mut self, other: Self) {
        if other.min.value < self.min.value {
            self.min = other.min;
        }
        if other.max.value > self.max.value {
            self.max = other.max;
        }
        self.last = other.last;
    }
}
pub(super) struct Curve {
    pub(super) name: String,
    pub(super) buckets: VecDeque<Bucket>,
    pub(super) omitted: u64,
    pub(super) missing: bool,
    pub(super) segment: u64,
    external: Option<u64>,
    scratch: VecDeque<Bucket>,
}
impl Curve {
    pub(super) fn new(name: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            buckets: VecDeque::with_capacity(BUCKETS),
            omitted: 0,
            missing: false,
            segment: 0,
            external: None,
            scratch: VecDeque::with_capacity(BUCKETS),
        }
    }
    pub(super) fn clear(&mut self, name: &str) {
        self.name.clear();
        self.name.push_str(name);
        self.buckets.clear();
        self.scratch.clear();
        self.omitted = 0;
        self.missing = false;
        self.segment = 0;
        self.external = None;
    }
    pub(super) fn push(&mut self, segment: u64, point: Option<Point>) {
        let Some(point) = point else {
            self.missing = true;
            return;
        };
        if self.external != Some(segment) || self.missing {
            self.segment = self.segment.wrapping_add(1);
        }
        self.external = Some(segment);
        self.missing = false;
        let segment = self.segment;
        // At capacity merge adjacent same-segment buckets, preserving their exact
        // extrema and endpoints. Never bridge an attempt, drop or unavailable gap.
        if self.buckets.len() == BUCKETS {
            self.scratch.clear();
            while let Some(mut bucket) = self.buckets.pop_front() {
                if self
                    .buckets
                    .front()
                    .is_some_and(|next| next.segment == bucket.segment)
                {
                    bucket.merge(self.buckets.pop_front().unwrap());
                }
                self.scratch.push_back(bucket);
            }
            std::mem::swap(&mut self.buckets, &mut self.scratch);
            if self.buckets.len() == BUCKETS {
                self.buckets.pop_front();
                self.omitted = self.omitted.saturating_add(1);
            }
        }
        self.buckets.push_back(Bucket {
            segment,
            first: point,
            min: point,
            max: point,
            last: point,
        });
    }
}

use super::catalog::{Metric, Source};
use crate::generated::{EvaluatedWeights, TrainingMetricSource, TrainingSourceCatalog, TrainingPhase, TrainingRecord, TrainingRecordRole, TrainingRecordScope};

/// The observed source survives clearing plot storage. In particular a cache reset
/// cannot claim native history recovered or rearm a dismissed active drop episode.
#[derive(Default)]
struct Conditions {
    run: String,
    omitted: u64,
    dropped: u64,
    chart_dirty: bool,
    dropped_dirty: bool,
}
impl Conditions {
    fn set_run(&mut self, run: &str) {
        if self.run != run {
            self.run.clear();
            self.run.push_str(run);
            self.omitted = 0;
            self.dropped = 0;
            self.chart_dirty = true;
            self.dropped_dirty = true;
        }
    }
}

pub(super) struct History {
    conditions: Conditions,
    pub(super) curves: Vec<Curve>,
    pub(super) run: String,
    attempt: String,
    pub(super) sequence: Option<u64>,
    pub(super) dropped: u64,
    segment: u64,
    last_live: Option<f64>,
    last_phase_epoch: Option<(TrainingPhase, i32)>,
    last_evaluation: Option<(String, i32, TrainingRecordScope, u64, EvaluatedWeights, String, String)>,
    observed_source: Option<(String, String)>,
    pub(super) source: Option<TrainingMetricSource>,
    pub(super) page: Option<(u64, u64)>,
    pub(super) generation: u64,
}
impl History {
    pub(super) fn new(metrics: &[Metric]) -> Self {
        Self {
            conditions: Conditions::default(),
            curves: metrics.iter().map(|m| Curve::new(&m.label)).collect(),
            run: String::new(),
            attempt: String::new(),
            sequence: None,
            dropped: 0,
            segment: 0,
            last_live: None,
            last_phase_epoch: None,
            last_evaluation: None,
            observed_source: None,
            source: None,
            page: None,
            generation: 0,
        }
    }
    pub(super) fn set_run(&mut self, run: &str, generation: u64) {
        self.conditions.set_run(run);
        self.run.clear();
        self.run.push_str(run);
        self.generation = generation;
    }
    pub(super) fn clear(&mut self, metrics: &[Metric]) {
        self.clear_curves(metrics, true);
        self.run.clear();
        self.attempt.clear();
        self.sequence = None;
        self.dropped = 0;
        self.segment = 0;
        self.page = None;
        self.generation = 0;
    }
    fn clear_curves(&mut self, metrics: &[Metric], scalars: bool) {
        if scalars {
            self.last_live = None;
            self.last_phase_epoch = None;
        }
        self.last_evaluation = None;
        // Keep chart/series identities and allocated bucket storage through source changes.
        for (curve, metric) in self.curves.iter_mut().zip(metrics) {
            if scalars || matches!(metric.source, Source::Evaluation { .. }) { curve.clear(&metric.label); }
        }
        let omitted = self.curves.iter().fold(0_u64, |total, curve| total.saturating_add(curve.omitted));
        if self.conditions.omitted != omitted {
            self.conditions.omitted = omitted;
            self.conditions.chart_dirty = true;
        }
    }
    pub(super) fn select_source(&mut self, catalog: &TrainingSourceCatalog) {
        if self.source.as_ref().is_none_or(|source| !catalog.available.contains(source)) {
            self.source.clone_from(&catalog.defaultsource);
        }
    }
    fn has_observed_source(&self, record: &TrainingRecord) -> bool {
        self.observed_source.as_ref().is_some_and(|(run, attempt)| run == &record.runid && attempt == &record.attemptid)
    }
    pub(super) fn reconcile_live_source(&mut self, current: &TrainingRecord, catalog: &TrainingSourceCatalog, metrics: &[Metric]) -> u16 {
        if catalog.defaultsource.is_none() { return 0; }
        let previous = self.source.clone();
        if !self.has_observed_source(current) {
            self.select_source(catalog);
            self.observed_source = Some((current.runid.clone(), current.attemptid.clone()));
        } else { self.select_source(catalog); }
        if previous != self.source {
            self.clear_curves(metrics, previous.as_ref().map(|source| (source.scope, source.modelid)) != self.source.as_ref().map(|source| (source.scope, source.modelid)));
            u16::MAX
        } else { 0 }
    }
    fn evaluation_identity(&mut self, record: &TrainingRecord) -> bool {
        let progress = &record.progress;
        let Some(artifact) = &progress.artifact else { return false; };
        if record.role != TrainingRecordRole::Epoch || progress.val.is_none() || artifact.weights != record.evaluatedweights
            || self.source.as_ref().is_none_or(|source| source.weights != artifact.weights || source.scope != progress.scope || source.modelid != progress.modelid) {
            return false;
        }
        if self.last_evaluation.as_ref().is_some_and(|(attempt, epoch, scope, model, weights, path, digest)|
            (attempt.as_str(), *epoch, *scope, *model, *weights, path.as_str(), digest.as_str()) ==
            (record.attemptid.as_str(), progress.epoch, progress.scope, progress.modelid, artifact.weights, artifact.path.as_str(), artifact.sha256.as_str())) { return false; }
        self.last_evaluation = Some((record.attemptid.clone(), progress.epoch, progress.scope, progress.modelid,
            artifact.weights, artifact.path.clone(), artifact.sha256.clone()));
        true
    }
    // A retained scheduled record may precede current progress. Replay only its
    // evaluation; it does not advance sequence/drop accounting or scalar curves.
    pub(super) fn ingest_observation(&mut self, record: &TrainingRecord, metrics: &[Metric]) -> u16 {
        if self.run != record.runid || self.attempt != record.attemptid { return 0; }
        let evaluation = self.evaluation_identity(record);
        self.plot(record, true, evaluation, false, metrics)
    }
    pub(super) fn ingest(
        &mut self,
        record: &TrainingRecord,
        live: bool,
        metrics: &[Metric],
    ) -> u16 {
        let mut changed = 0;
        if self.run != record.runid {
            self.clear(metrics);
            self.run.clone_from(&record.runid);
            changed = u16::MAX;
        }
        let attempt_changed = self.attempt != record.attemptid;
        if attempt_changed {
            self.sequence = None;
            self.dropped = 0;
            self.last_live = None;
            self.last_evaluation = None;
        }
        if self
            .sequence
            .is_some_and(|sequence| record.sequence <= sequence)
        {
            return 0;
        }
        if attempt_changed
            || record.droppedbefore > self.dropped
            || self
                .sequence
                .is_some_and(|sequence| sequence.checked_add(1) != Some(record.sequence))
        {
            self.segment = self.segment.wrapping_add(1);
        }
        self.attempt.clone_from(&record.attemptid);
        self.sequence = Some(record.sequence);
        self.dropped = record.droppedbefore;
        self.conditions.set_run(&record.runid);
        if self.conditions.dropped != record.droppedbefore {
            self.conditions.dropped = record.droppedbefore;
            self.conditions.dropped_dirty = true;
        }
        let progress = &record.progress;
        if self.source.is_none() && progress.scope == TrainingRecordScope::Model {
            self.source = Some(TrainingMetricSource { scope: progress.scope, modelid: progress.modelid, weights: record.evaluatedweights });
        }
        let evaluation = self.evaluation_identity(record);
        let scalars = self.source.as_ref().is_some_and(|source| source.scope == TrainingRecordScope::Model && progress.scope == TrainingRecordScope::Model && source.modelid == progress.modelid)
            && matches!(record.role, TrainingRecordRole::Live | TrainingRecordRole::Epoch)
            && matches!(progress.phase, TrainingPhase::Train | TrainingPhase::EpochComplete);
        let phase_epoch = (progress.phase, progress.epoch);
        let admit = !live
            || record.role != TrainingRecordRole::Live
            || attempt_changed
            || self.last_phase_epoch != Some(phase_epoch)
            || !self.last_live.is_some_and(|last| {
                progress.elapsedseconds >= last && progress.elapsedseconds - last < 1.0
            });
        if scalars {
            self.last_phase_epoch = Some(phase_epoch);
            if admit && record.role == TrainingRecordRole::Live { self.last_live = Some(progress.elapsedseconds); }
        }
        changed | self.plot(record, admit, evaluation, scalars, metrics)
    }
    fn plot(&mut self, record: &TrainingRecord, admit: bool, evaluation: bool, scalars: bool, metrics: &[Metric]) -> u16 {
        let mut changed = 0;
        let progress = &record.progress;
        for (metric, curve) in metrics.iter().zip(&mut self.curves) {
            let (value, is_evaluation) = match metric.source {
                Source::Scalar(field) => {
                    if !scalars { continue; }
                    (progress.scalars.value(field), false)
                },
                Source::Evaluation { mask, field } => {
                    if !evaluation {
                        continue;
                    }
                    let summary = progress
                        .val
                        .as_ref()
                        .and_then(|v| if mask { v.mask.as_ref() } else { Some(&v.bbox) });
                    if let crate::generated::MetricSummaryField::AverageRecall(index) = field {
                        if let Some(cap) = summary.and_then(|s| s.detectionlimits.get(index)) {
                            curve.name =
                                format!("{} AR@{}", if mask { "Mask" } else { "Box" }, cap);
                        }
                    }
                    (
                        summary.filter(|s| s.available).and_then(|s| s.value(field)),
                        true,
                    )
                }
            };
            let value = value.filter(|v| v.is_finite());
            if !admit {
                if value.is_none() {
                    curve.missing = true;
                }
                continue;
            }
            let fraction = if is_evaluation || record.role == TrainingRecordRole::Epoch {
                1.0
            } else if progress.totalbatches > 0 {
                (progress.completedbatches as f64 / progress.totalbatches as f64).clamp(0.0, 1.0)
            } else {
                0.0
            };
            if value.is_some() {
                changed |= 1 << metric.chart as usize;
            }
            let omitted = curve.omitted;
            curve.push(
                self.segment,
                value.map(|value| Point {
                    step: progress.globaloptimizerstep as f64,
                    epoch: progress.epoch as f64 + fraction,
                    value,
                    order: record.sequence,
                }),
            );
            if curve.omitted != omitted {
                self.conditions.omitted = self.conditions.omitted.saturating_add(curve.omitted - omitted);
                self.conditions.chart_dirty = true;
            }
        }
        changed
    }
    pub(super) fn publish_conditions(&mut self, notices: &mut crate::view_model::notices::NoticeStore, saved: bool) {
        use crate::view_model::notices::{Origin, warning};
        let conditions = &mut self.conditions;
        if std::mem::take(&mut conditions.chart_dirty) {
            let omitted = conditions.omitted;
            notices.run_condition(if saved { Origin::ChartSaved } else { Origin::Chart }, &conditions.run, omitted > 0,
                || warning("Chart history incomplete", format!("Charts omit {omitted} older disconnected summaries; saved history remains unchanged.")));
        }
        if std::mem::take(&mut conditions.dropped_dirty) {
            let dropped = conditions.dropped;
            notices.run_condition(if saved { Origin::HistoryDroppedSaved } else { Origin::HistoryDropped }, &conditions.run, dropped > 0,
                || warning("History incomplete", format!("{dropped} training records were dropped.")));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn bookkeeping_and_other_models_never_change_the_selected_curves() {
        let metrics = super::super::catalog::catalog();
        let mut component = History::new(&metrics);
        component.source = Some(TrainingMetricSource { scope: TrainingRecordScope::Model, modelid: 1, weights: EvaluatedWeights::Ema });
        let mut record = super::super::tests::record();
        record.sequence = 0;
        record.progress.modelid = 1;
        component.ingest(&record, false, &metrics);
        let segment = component.curves[0].segment;
        record.sequence += 1;
        record.progress.modelid = 2;
        record.progress.scalars.total = Some(99.0);
        component.ingest(&record, false, &metrics);
        record.sequence += 1;
        record.role = TrainingRecordRole::Epoch;
        record.progress.scope = TrainingRecordScope::SynchronizedSession;
        record.progress.modelid = 0;
        record.progress.val = Some(super::super::tests::evaluation());
        component.ingest(&record, false, &metrics);
        assert_eq!(component.curves[0].buckets.len(), 1);
        let evaluation_curve = metrics.iter().position(|m| matches!(m.source, Source::Evaluation { .. })).unwrap();
        assert!(component.curves[evaluation_curve].buckets.is_empty());
        record.sequence += 1;
        record.progress.scope = TrainingRecordScope::Model;
        record.progress.modelid = 1;
        record.progress.artifact.as_mut().unwrap().weights = EvaluatedWeights::Ema;
        record.evaluatedweights = EvaluatedWeights::Ema;
        record.progress.artifact.as_mut().unwrap().modelid = 1;
        record.progress.scalars.total = Some(2.0);
        component.ingest(&record, false, &metrics);
        let observation = record.clone();
        record.sequence += 1;
        record.progress.scope = TrainingRecordScope::Session;
        record.progress.artifact = None;
        record.progress.val = None;
        record.progress.scalars.total = None;
        component.ingest(&record, false, &metrics);
        assert!(!component.curves[0].missing);
        assert_eq!(component.curves[0].segment, segment);
        assert_eq!(component.curves[0].buckets.len(), 2);
        assert_eq!(component.curves[evaluation_curve].buckets.len(), 1);
        let sequence = component.sequence;
        component.ingest_observation(&observation, &metrics);
        assert_eq!(component.sequence, sequence);
        assert_eq!(component.curves[evaluation_curve].buckets.len(), 1);
        let mut coalesced = History::new(&metrics);
        coalesced.source = component.source.clone();
        coalesced.ingest(&record, true, &metrics);
        coalesced.ingest_observation(&observation, &metrics);
        coalesced.ingest_observation(&observation, &metrics);
        assert_eq!(coalesced.sequence, sequence);
        assert_eq!(coalesced.curves[evaluation_curve].buckets.len(), 1);
        assert!(coalesced.curves[0].buckets.is_empty());
    }
    #[test]
    fn synchronized_ordinary_observation_and_session_epoch_keep_one_model_continuous() {
        let metrics = super::super::catalog::catalog();
        let mut component = History::new(&metrics);
        let mut record = super::super::tests::record();
        record.sequence = 0;
        component.ingest(&record, false, &metrics);
        let segment = component.curves[0].segment;
        record.sequence = 1;
        record.role = TrainingRecordRole::Epoch;
        record.progress.scope = TrainingRecordScope::SynchronizedSession;
        record.progress.val = Some(super::super::tests::evaluation());
        component.ingest(&record, false, &metrics);
        record.sequence = 2;
        record.progress.scope = TrainingRecordScope::Session;
        record.progress.artifact = None;
        record.progress.val = None;
        record.progress.scalars.total = None;
        component.ingest(&record, false, &metrics);
        record.sequence = 3;
        record.role = TrainingRecordRole::Live;
        record.progress.scope = TrainingRecordScope::Model;
        record.progress.scalars.total = Some(2.0);
        component.ingest(&record, false, &metrics);
        assert_eq!(component.curves[0].buckets.len(), 2);
        assert_eq!(component.curves[0].segment, segment);
        assert!(!component.curves[0].missing);
    }
    #[test]
    fn observed_sequences_and_cumulative_drops_survive_display_coalescing() {
        let metrics = super::super::catalog::catalog();
        let mut component = History::new(&metrics);
        let mut record = super::super::tests::record();
        record.sequence = 0;
        component.ingest(&record, true, &metrics);
        assert_eq!(component.curves[0].buckets.len(), 1);
        let first = component.curves[0].segment;
        record.sequence = 1;
        record.progress.elapsedseconds = 1.5;
        component.ingest(&record, true, &metrics);
        assert_eq!(component.sequence, Some(1));
        record.sequence = 2;
        record.role = TrainingRecordRole::Epoch;
        component.ingest(&record, true, &metrics);
        assert_eq!(component.curves[0].segment, first);
        component.ingest(&record, true, &metrics);
        assert_eq!(component.curves[0].buckets.len(), 2);
        record.sequence = 3;
        record.droppedbefore = 1;
        component.ingest(&record, true, &metrics);
        let dropped = component.curves[0].segment;
        assert_ne!(first, dropped);
        record.sequence = 4;
        component.ingest(&record, true, &metrics);
        assert_eq!(component.curves[0].segment, dropped);
        record.sequence = 5;
        record.droppedbefore = 2;
        component.ingest(&record, true, &metrics);
        assert_eq!(component.curves[0].segment, dropped + 1);
        record.attemptid = "next".into();
        record.sequence = 0;
        record.droppedbefore = 0;
        component.ingest(&record, true, &metrics);
        assert_eq!(component.sequence, Some(0));
        assert_eq!(component.dropped, 0);
    }

    #[test]
    fn coalesced_actual_holes_and_unavailability_remain_pending() {
        let metrics = super::super::catalog::catalog();
        let mut component = History::new(&metrics);
        let mut record = super::super::tests::record();
        record.sequence = 0;
        component.ingest(&record, true, &metrics);
        let first = component.curves[0].segment;
        record.sequence = 2;
        record.progress.elapsedseconds = 1.2;
        record.progress.scalars.total = None;
        component.ingest(&record, true, &metrics);
        assert!(component.curves[0].missing);
        assert_eq!(component.curves[0].buckets.len(), 1);
        record.sequence = 3;
        record.progress.elapsedseconds = 1.4;
        record.progress.scalars.total = Some(3.0);
        component.ingest(&record, true, &metrics);
        assert_eq!(component.curves[0].buckets.len(), 1);
        assert_eq!(component.curves[0].buckets[0].last.value, 1.0);
        assert!(component.curves[0].missing);
        record.sequence = 4;
        record.progress.elapsedseconds = 2.0;
        component.ingest(&record, true, &metrics);
        assert_ne!(component.curves[0].segment, first);
        let resumed = component.curves[0].segment;
        record.sequence = 5;
        record.progress.elapsedseconds = 3.0;
        component.ingest(&record, true, &metrics);
        assert_eq!(component.curves[0].segment, resumed);
        record.sequence = 6;
        record.progress.scalars.total = None;
        component.ingest(&record, true, &metrics);
        assert!(component.curves[0].missing);
        assert_eq!(component.curves[0].buckets.len(), 3);
        assert_eq!(component.curves[0].buckets.back().unwrap().last.value, 3.0);
    }

    #[test]
    fn coalesced_missing_and_drop_boundaries_break_once_without_sequence_holes() {
        let metrics = super::super::catalog::catalog();
        let mut component = History::new(&metrics);
        let mut sample = super::super::tests::record();
        sample.sequence = 0;
        component.ingest(&sample, true, &metrics);
        for drop in [false, true] {
            let segment = component.curves[0].segment;
            sample.sequence += 1;
            sample.progress.elapsedseconds += 0.2;
            if drop {
                sample.droppedbefore += 1;
            } else {
                sample.progress.scalars.total = None;
            }
            component.ingest(&sample, true, &metrics);
            sample.sequence += 1;
            sample.progress.elapsedseconds += 0.2;
            sample.progress.scalars.total = Some(2.0);
            component.ingest(&sample, true, &metrics);
            assert_eq!(component.curves[0].segment, segment);
            sample.sequence += 1;
            sample.role = TrainingRecordRole::Epoch;
            component.ingest(&sample, true, &metrics);
            assert_eq!(component.curves[0].segment, segment + 1);
            sample.sequence += 1;
            component.ingest(&sample, true, &metrics);
            assert_eq!(component.curves[0].segment, segment + 1);
            sample.role = TrainingRecordRole::Live;
        }
    }

    #[test]
    fn missing_values_form_gaps_and_disconnected_capacity_is_bounded() {
        let mut curve = Curve::new("loss");
        let point = Point {
            step: 1.0,
            epoch: 1.0,
            value: 2.0,
            order: 1,
        };
        curve.push(1, Some(point));
        curve.push(1, None);
        curve.push(1, Some(point));
        assert_ne!(
            curve.buckets.front().unwrap().segment,
            curve.buckets.back().unwrap().segment
        );
        for segment in 2..1024 {
            curve.push(segment, Some(point));
        }
        assert_eq!(curve.buckets.len(), BUCKETS);
        assert!(curve.omitted > 0);
    }
    #[test]
    fn bounded_summaries_preserve_extrema_and_never_merge_attempts() {
        let mut curve = Curve::new("loss");
        for order in 0..4096 {
            curve.push(
                7,
                Some(Point {
                    step: order as f64,
                    epoch: 0.0,
                    value: if order == 100 { -99.0 } else { order as f64 },
                    order,
                }),
            );
        }
        assert!(curve.buckets.len() <= BUCKETS);
        assert_eq!(
            curve
                .buckets
                .iter()
                .map(|b| b.min.value)
                .fold(f64::INFINITY, f64::min),
            -99.0
        );
        curve.push(
            8,
            Some(Point {
                step: 0.0,
                epoch: 0.0,
                value: 12.0,
                order: 0,
            }),
        );
        assert_ne!(
            curve.buckets.back().unwrap().segment,
            curve.buckets.front().unwrap().segment
        );
    }
}
