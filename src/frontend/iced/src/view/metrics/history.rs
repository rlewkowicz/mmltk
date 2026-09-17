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
    pub(super) omitted_extrema: Option<(f64, f64)>,
    pub(super) missing: bool,
    pub(super) available: bool,
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
            omitted_extrema: None,
            missing: false,
            available: false,
            segment: 0,
            external: None,
            scratch: VecDeque::with_capacity(BUCKETS),
        }
    }
    pub(super) fn clear(&mut self, name: &str) {
        self.name.clear(); self.name.push_str(name); self.buckets.clear(); self.scratch.clear();
        self.omitted = 0; self.omitted_extrema = None; self.missing = false;
        self.available = false; self.segment = 0; self.external = None;
    }
    pub(super) fn push(&mut self, segment: u64, point: Option<Point>) {
        self.available = point.is_some();
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
                let retired = self.buckets.pop_front().unwrap();
                self.omitted_extrema = Some(
                    self.omitted_extrema
                        .map_or((retired.min.value, retired.max.value), |(min, max)| {
                            (min.min(retired.min.value), max.max(retired.max.value))
                        }),
                );
                self.omitted += 1;
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
use crate::generated::{TrainingRecord, TrainingRecordRole, TrainingPhase};

pub(super) struct History {
    pub(super) curves: Vec<Curve>,
    run: String,
    attempt: String,
    pub(super) sequence: Option<u64>,
    dropped: u64,
    segment: u64,
    last_live: Option<f64>,
    last_phase_epoch: Option<(TrainingPhase, i32)>,
    last_evaluation: Option<(String, i32)>,
    pub(super) page: Option<(u64, u64)>,
    pub(super) generation: u64,
}
impl History {
    pub(super) fn new(metrics: &[Metric]) -> Self {
        Self { curves: metrics.iter().map(|m| Curve::new(&m.label)).collect(),
            run: String::new(), attempt: String::new(), sequence: None, dropped: 0, segment: 0,
            last_live: None, last_phase_epoch: None, last_evaluation: None,
            page: None, generation: 0 }
    }
    pub(super) fn set_run(&mut self, run: &str, generation: u64) { self.run.clear(); self.run.push_str(run); self.generation = generation; }
    pub(super) fn clear(&mut self, metrics: &[Metric]) {
        self.run.clear(); self.attempt.clear(); self.sequence = None; self.dropped = 0;
        self.segment = 0; self.last_live = None; self.last_phase_epoch = None;
        self.last_evaluation = None; self.page = None; self.generation = 0;
        // Keep chart/series identities and allocated bucket storage through source changes.
        for (curve, metric) in self.curves.iter_mut().zip(metrics) {
            curve.clear(&metric.label);
        }

    }
    pub(super) fn ingest(&mut self, record: &TrainingRecord, live: bool, metrics: &[Metric]) -> u16 {
        let mut changed = 0;
        if self.run != record.runid { self.clear(metrics); self.run.clone_from(&record.runid); changed = u16::MAX; }
        let attempt_changed = self.attempt != record.attemptid;
        if attempt_changed { self.sequence = None; self.dropped = 0; self.last_live = None; self.last_evaluation = None; }
        if self.sequence.is_some_and(|sequence| record.sequence <= sequence) { return 0; }
        if attempt_changed || record.droppedbefore > self.dropped || self.sequence.is_some_and(|sequence| sequence.checked_add(1) != Some(record.sequence)) {
            self.segment = self.segment.wrapping_add(1);
        }
        self.attempt.clone_from(&record.attemptid);
        self.sequence = Some(record.sequence); self.dropped = record.droppedbefore;
        let progress = &record.progress;
        let phase_epoch = (progress.phase, progress.epoch);
        let admit = !live || record.role != TrainingRecordRole::Live || attempt_changed
            || self.last_phase_epoch != Some(phase_epoch)
            || !self.last_live.is_some_and(|last| progress.elapsedseconds >= last && progress.elapsedseconds - last < 1.0);
        self.last_phase_epoch = Some(phase_epoch);
        if admit && record.role == TrainingRecordRole::Live { self.last_live = Some(progress.elapsedseconds); }
        // Only completed epoch observations contribute validation. Live and terminal
        // records can carry copied summaries, and neither is another evaluation.
        let evaluation = record.role == TrainingRecordRole::Epoch
            && self.last_evaluation.as_ref().is_none_or(|(attempt, epoch)| attempt != &record.attemptid || *epoch != progress.epoch);
        if evaluation { self.last_evaluation = Some((record.attemptid.clone(), progress.epoch)); }
        for (metric, curve) in metrics.iter().zip(&mut self.curves) {
            let (value, is_evaluation) = match metric.source {
                Source::Scalar(field) => (progress.scalars.value(field), false),
                Source::Evaluation { mask, field } => {
                    if !evaluation { continue; }
                    let summary = progress.val.as_ref().and_then(|v| if mask { v.mask.as_ref() } else { Some(&v.bbox) });
                    if let crate::generated::MetricSummaryField::AverageRecall(index) = field {
                        if let Some(cap) = summary.and_then(|s| s.detectionlimits.get(index)) {
                            curve.name = format!("{} AR@{}", if mask { "Mask" } else { "Box" }, cap);
                        }
                    }
                    (summary.filter(|s| s.available).and_then(|s| s.value(field)), true)
                }
            };
            let value = value.filter(|v| v.is_finite());
            curve.available = value.is_some();
            if !admit { if value.is_none() { curve.missing = true; } continue; }
            let fraction = if is_evaluation || record.role == TrainingRecordRole::Epoch { 1.0 }
                else if progress.totalbatches > 0 { (progress.completedbatches as f64 / progress.totalbatches as f64).clamp(0.0, 1.0) } else { 0.0 };
            if value.is_some() { changed |= 1 << metric.chart as usize; }
            curve.push(self.segment, value.map(|value| Point { step: progress.globaloptimizerstep as f64, epoch: progress.epoch as f64 + fraction, value, order: record.sequence }));

        }
        changed
    }
}

#[cfg(test)]
mod tests {
use super::*;
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
        assert!(!component.curves[0].available);
        assert_eq!(component.curves[0].buckets.len(), 1);
        record.sequence = 3;
        record.progress.elapsedseconds = 1.4;
        record.progress.scalars.total = Some(3.0);
        component.ingest(&record, true, &metrics);
        assert!(component.curves[0].available);
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
        assert!(!component.curves[0].available);
        assert!(!component.curves[0].buckets.is_empty());
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
