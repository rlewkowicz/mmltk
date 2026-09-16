//! Bounded visual summaries. Native records remain the authoritative history.
use crate::fluent_theme::Element;
use crate::generated::{TrainingRecord, TrainingRecordRole, TrainingScalars, TrainingScalarsField};
use iced::widget::{button, checkbox, column, container, row, text};
use iced_plot::{AxisScale, LineStyle, PlotWidget, PlotWidgetBuilder, Series, ShapeId};
use std::collections::VecDeque;

const BUCKETS: usize = 128;
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Group {
    Loss,
    Ap,
    Confidence,
    LearningRate,
    Throughput,
}
impl Group {
    const ALL: [Self; 5] = [
        Self::Loss,
        Self::Ap,
        Self::Confidence,
        Self::LearningRate,
        Self::Throughput,
    ];
    fn label(self) -> &'static str {
        match self {
            Self::Loss => "Losses",
            Self::Ap => "AP",
            Self::Confidence => "Precision / recall / F1",
            Self::LearningRate => "Learning rates",
            Self::Throughput => "Throughput",
        }
    }
}
#[derive(Debug, Clone)]
pub enum Message {
    Group(Group),
    Epoch(bool),
    Log(bool),
    Plot(iced_plot::PlotUiMessage),
}
#[derive(Clone, Copy, Debug)]
struct Point {
    step: f64,
    epoch: f64,
    value: f64,
    order: u64,
}
#[derive(Clone, Debug)]
struct Bucket {
    segment: u64,
    first: Point,
    min: Point,
    max: Point,
    last: Point,
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
struct Curve {
    name: String,
    group: Group,
    buckets: VecDeque<Bucket>,
    omitted: u64,
    omitted_extrema: Option<(f64, f64)>,
    missing: bool,
    available: bool,
    segment: u64,
    external: Option<u64>,
    scratch: VecDeque<Bucket>,
}
impl Curve {
    fn new(name: impl Into<String>, group: Group) -> Self {
        Self {
            name: name.into(),
            group,
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
    fn push(&mut self, segment: u64, point: Option<Point>) {
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
fn scalar_group(field: TrainingScalarsField) -> Group {
    use TrainingScalarsField::*;
    match field {
        LearningRate | LearningRateMin | LearningRateMax => Group::LearningRate,
        ImagesPerSecond => Group::Throughput,
        Total | Classification | L1 | Giou | MaskCe | MaskDice | AuxiliaryWeighted
        | DenoisingWeighted => Group::Loss,
        CorrespondenceWeighted | ClassError | CardinalityError => Group::Loss,
    }
}

pub struct Component {
    plot: PlotWidget,
    shapes: Vec<Option<ShapeId>>,
    prepared_group: Option<Group>,
    curves: Vec<Curve>,
    group: Group,
    epoch: bool,
    log: bool,
    run: String,
    attempt: String,
    sequence: Option<u64>,
    dropped: u64,
    segment: u64,
    last_live: Option<f64>,
    page: Option<(u64, u64)>,
    generation: u64,
    dirty: bool,
    notice: String,
    convention: Option<crate::generated::TrainAssignmentKind>,
    positions: Vec<[f64; 2]>,
}
impl Default for Component {
    fn default() -> Self {
        let mut plot = PlotWidgetBuilder::new()
            .with_autoscale_on_updates(true)
            .build()
            .expect("valid empty plot");
        plot.set_cursor_overlay(true);
        plot.set_crosshairs(true);
        let mut result = Self {
            plot,
            shapes: Vec::new(),
            prepared_group: None,
            curves: Vec::new(),
            group: Group::Loss,
            epoch: false,
            log: false,
            run: String::new(),
            attempt: String::new(),
            sequence: None,
            dropped: 0,
            segment: 0,
            last_live: None,
            page: None,
            generation: 0,
            dirty: true,
            notice: String::new(),
            convention: None,
            positions: Vec::with_capacity(BUCKETS * 5),
        };
        result.clear();
        result
    }
}
impl Component {
    pub fn clear(&mut self) {
        self.run.clear();
        self.attempt.clear();
        self.sequence = None;
        self.dropped = 0;
        self.segment = 0;
        self.last_live = None;
        self.page = None;
        self.generation = 0;
        self.convention = None;
        self.notice.clear();
        self.curves = TrainingScalars::FIELDS
            .iter()
            .map(|(field, name)| Curve::new(name.replace('_', " "), scalar_group(*field)))
            .collect();
        for (name, group) in [
            ("Validation loss", Group::Loss),
            ("Box AP", Group::Ap),
            ("Box AP50", Group::Ap),
            ("Box AP75", Group::Ap),
            ("Box precision", Group::Confidence),
            ("Box recall", Group::Confidence),
            ("Box F1", Group::Confidence),
            ("Mask AP", Group::Ap),
            ("Mask AP50", Group::Ap),
            ("Mask AP75", Group::Ap),
            ("Mask precision", Group::Confidence),
            ("Mask recall", Group::Confidence),
            ("Mask F1", Group::Confidence),
        ] {
            self.curves.push(Curve::new(name, group));
        }
        self.dirty = true;
    }
    fn ingest(&mut self, record: &TrainingRecord, live: bool) {
        if self.run != record.runid {
            self.clear();
            self.run.clone_from(&record.runid);
        }
        let attempt_changed = self.attempt != record.attemptid;
        if attempt_changed {
            self.sequence = None;
            self.dropped = 0;
            self.last_live = None;
        }
        if self
            .sequence
            .is_some_and(|sequence| record.sequence <= sequence)
        {
            return;
        }
        let gap = attempt_changed
            || record.droppedbefore > self.dropped
            || self
                .sequence
                .is_some_and(|sequence| sequence.checked_add(1) != Some(record.sequence));
        // Observation advances even when display cadence coalesces this sample.
        // The segment and each curve's missing flag retain pending boundaries.
        if gap {
            self.segment = self.segment.wrapping_add(1);
        }
        self.attempt.clone_from(&record.attemptid);
        self.sequence = Some(record.sequence);
        self.dropped = record.droppedbefore;
        let admit = !live
            || record.role != TrainingRecordRole::Live
            || attempt_changed
            || !self.last_live.is_some_and(|last| {
                record.progress.elapsedseconds >= last
                    && record.progress.elapsedseconds - last < 1.0
            });
        if admit && record.role == TrainingRecordRole::Live {
            self.last_live = Some(record.progress.elapsedseconds);
        }
        if let Some(configuration) = &record.attemptconfiguration {
            self.convention = Some(configuration.trainingsupervision.assignment);
        }
        let progress = &record.progress;
        let summary_values = |summary: Option<&crate::generated::MetricSummary>| {
            let summary = summary.filter(|value| value.available);
            [
                summary.map(|v| v.ap),
                summary.map(|v| v.ap50),
                summary.map(|v| v.ap75),
                summary.map(|v| v.confidence.precision),
                summary.map(|v| v.confidence.recall),
                summary.map(|v| v.confidence.f1),
            ]
        };
        let values = progress
            .scalars
            .values()
            .into_iter()
            .chain([progress.valloss])
            .chain(summary_values(
                progress.val.as_ref().map(|value| &value.bbox),
            ))
            .chain(summary_values(
                progress.val.as_ref().and_then(|value| value.mask.as_ref()),
            ));
        for (curve, value) in self.curves.iter_mut().zip(values) {
            // An unavailable interval terminates just this curve, not unrelated metrics.
            let value = value.filter(|value| value.is_finite());
            curve.available = value.is_some();
            if !admit {
                if value.is_none() {
                    curve.missing = true;
                }
                continue;
            }
            curve.push(
                self.segment,
                value.map(|value| Point {
                    step: progress.globaloptimizerstep as f64,
                    epoch: progress.epoch as f64,
                    value,
                    order: record.sequence,
                }),
            );
        }
        if admit {
            self.notice = format!(
                "Run {} · attempt {} · {:?} weights · gaps remain disconnected",
                record.runid, record.attemptid, record.evaluatedweights
            );
            self.dirty = true;
        }
    }
    pub fn reset(&mut self, visible: bool) {
        self.clear();
        if visible {
            self.prepare();
        }
    }
    pub fn rebase(&mut self, model: &crate::view_model::ApplicationModel, visible: bool) {
        if let Some(opened) = &model.workflow.training_run {
            if self.generation != opened.generation {
                self.clear();
                self.run.clone_from(&opened.run.runid);
                self.generation = opened.generation;
                self.convention = Some(opened.run.configuration.trainingsupervision.assignment);
                self.notice = format!(
                    "Run {} · {:?} weights",
                    opened.run.runid, opened.run.evaluatedweights
                );
            }
            if let Some(page) = &model.workflow.training_history {
                let key = (page.generation, page.nextcursor);
                if page.generation == self.generation && self.page != Some(key) {
                    for record in &page.records {
                        self.ingest(record, false);
                    }
                    self.page = Some(key);
                }
            }
        } else {
            if self.generation != 0 {
                self.clear();
            }
            if let Some(record) = model
                .workflow
                .training
                .as_ref()
                .and_then(|value| value.metrics.as_ref())
            {
                self.ingest(record, true);
            }
        }
        if visible {
            self.prepare();
        }
    }
    pub fn update(&mut self, message: Message) {
        match message {
            Message::Group(group) => {
                self.group = group;
                self.dirty = true;
            }
            Message::Epoch(epoch) => {
                self.epoch = epoch;
                self.dirty = true;
            }
            Message::Log(log) => {
                self.log = log;
                self.dirty = true;
            }
            Message::Plot(message) => self.plot.update(message),
        }
        self.prepare();
    }
    fn prepare(&mut self) {
        if !self.dirty {
            return;
        }
        if self.prepared_group != Some(self.group) {
            for id in self.shapes.drain(..).flatten() {
                let _ = self.plot.remove_series(&id);
            }
            self.prepared_group = Some(self.group);
        }
        self.shapes.resize(self.curves.len(), None);
        self.plot.set_x_axis_label(if self.epoch {
            "Epoch"
        } else {
            "Global optimizer step"
        });
        self.plot.set_y_axis_label(self.group.label());
        self.plot
            .set_y_axis_scale(if self.log && self.group == Group::Loss {
                AxisScale::Log { base: 10.0 }
            } else {
                AxisScale::Linear
            });
        for (index, curve) in self
            .curves
            .iter()
            .enumerate()
            .filter(|(_, curve)| curve.group == self.group)
        {
            let positions = &mut self.positions;
            positions.clear();
            let mut segment = None;
            for bucket in &curve.buckets {
                if segment != Some(bucket.segment) && !positions.is_empty() {
                    positions.push([f64::NAN; 2]);
                }
                segment = Some(bucket.segment);
                let mut points = [bucket.first, bucket.min, bucket.max, bucket.last];
                points.sort_by_key(|point| point.order);
                let mut previous = None;
                for point in points {
                    if previous == Some(point.order) {
                        continue;
                    }
                    positions.push([
                        if self.epoch { point.epoch } else { point.step },
                        point.value,
                    ]);
                    previous = Some(point.order);
                }
            }
            if let Some(id) = self.shapes[index] {
                self.plot.set_series_positions(&id, positions);
                continue;
            }
            if positions.is_empty() {
                continue;
            }
            let hue = index as f32 * 137.5;
            let color = crate::presentation_surface::labels::class_color(
                &crate::generated::AnnotationColor {
                    hue,
                    saturation: 0.8,
                    value: 0.8,
                },
            );
            let series =
                Series::line_only(positions.clone(), LineStyle::solid().with_pixel_width(1.5))
                    .with_label(curve.name.clone())
                    .with_color(color);
            self.shapes[index] = Some(series.id);
            let _ = self.plot.add_series(series);
        }
        self.dirty = false;
    }
    pub fn view(&self, width: f32) -> Element<'_, Message> {
        let groups = Group::ALL
            .into_iter()
            .fold(row![].spacing(4), |row, group| {
                row.push(button(group.label()).on_press(Message::Group(group)))
            });
        let missing = self
            .curves
            .iter()
            .filter(|curve| curve.group == self.group && !curve.available)
            .map(|curve| curve.name.as_str())
            .collect::<Vec<_>>()
            .join(", ");
        let omitted = self
            .curves
            .iter()
            .filter(|curve| curve.group == self.group)
            .filter_map(|curve| {
                curve.omitted_extrema.map(|(min, max)| {
                    format!("{}: older disconnected extrema {min} … {max}", curve.name)
                })
            })
            .collect::<Vec<_>>()
            .join("; ");
        column![groups, row![checkbox(self.epoch).label("Epoch axis").on_toggle(Message::Epoch),
            checkbox(self.log).label("Log loss scale").on_toggle(Message::Log)].spacing(10),
            container(self.plot.view().map(Message::Plot)).id("train.metrics.plot").width(iced::Fill).height(width.max(1.0) * 9.0 / 16.0),
            text(&self.notice).size(12), text(format!("Unavailable: {missing}")).size(12), text(omitted).size(12),
            text(format!("Main components: {}. Correspondence overlaps total; never add it again. {} old disconnected summaries omitted.",
                match self.convention { Some(crate::generated::TrainAssignmentKind::Hungarian) => "raw losses", Some(crate::generated::TrainAssignmentKind::MatchFree) => "weighted losses", None => "saved convention unavailable" },
                self.curves.iter().map(|curve| curve.omitted).sum::<u64>())).size(12),
        ].spacing(6).into()
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn record() -> TrainingRecord {
        use crate::generated::*;
        TrainingRecord {
            formatversion: 1,
            runid: "run".into(),
            attemptid: "attempt".into(),
            sequence: 1,
            droppedbefore: 0,
            role: TrainingRecordRole::Live,
            evaluatedweights: EvaluatedWeights::Ordinary,
            attemptconfiguration: None,
            progress: TrainingMetricProgress {
                phase: TrainingPhase::Train,
                epoch: 1,
                totalepochs: 3,
                completedbatches: 1,
                totalbatches: 10,
                completedwaves: 1,
                optimizersteps: 1,
                globaloptimizerstep: 1,
                stepsperepoch: 10,
                trainlanes: 1,
                ranklocal: false,
                trainloss: 1.0,
                classloss: 0.0,
                boxloss: 0.0,
                steploss: 1.0,
                stepclassloss: 0.0,
                stepboxloss: 0.0,
                batchespersecond: 0.0,
                imagespersecond: 0.0,
                elapsedseconds: 1.0,
                scalars: TrainingScalars {
                    total: Some(1.0),
                    classification: None,
                    l1: None,
                    giou: None,
                    maskce: None,
                    maskdice: None,
                    auxiliaryweighted: None,
                    denoisingweighted: None,
                    correspondenceweighted: None,
                    classerror: None,
                    cardinalityerror: None,
                    learningrate: None,
                    learningratemin: None,
                    learningratemax: None,
                    imagespersecond: None,
                },
                epochgloballoss: None,
                valloss: None,
                val: None,
                test: None,
                checkpointpath: String::new(),
                fullcheckpointpath: String::new(),
            },
        }
    }
    #[test]
    fn hidden_updates_preserve_immutable_run_meaning_without_preparing_plot_data() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut record = record();
        let mut configuration = model
            .settings_snapshot
            .as_ref()
            .unwrap()
            .settingsstate
            .workflows
            .train
            .request
            .clone();
        configuration.trainingsupervision.assignment =
            crate::generated::TrainAssignmentKind::Hungarian;
        configuration.useema = true;
        record.attemptconfiguration = Some(configuration);
        record.evaluatedweights = crate::generated::EvaluatedWeights::Ema;
        model.workflow.training.as_mut().unwrap().metrics = Some(record);
        let mut component = Component::default();
        component.rebase(&model, false);
        assert!(component.shapes.is_empty());
        model
            .settings_snapshot
            .as_mut()
            .unwrap()
            .settingsstate
            .workflows
            .train
            .request
            .trainingsupervision
            .assignment = crate::generated::TrainAssignmentKind::MatchFree;
        component.rebase(&model, false);
        assert_eq!(
            component.convention,
            Some(crate::generated::TrainAssignmentKind::Hungarian)
        );
        assert!(component.notice.contains("Ema"));
        component.rebase(&model, true);
        assert!(component.shapes[0].is_some());
    }

    #[test]
    fn live_cadence_boundaries_and_series_identity_follow_updates() {
        let mut component = Component::default();
        let mut record = record();
        component.ingest(&record, true);
        component.prepare();
        let shape = component.shapes[0];
        assert!(shape.is_some());
        component.prepare();
        assert_eq!(component.shapes[0], shape);
        record.sequence += 1;
        record.progress.elapsedseconds = 1.5;
        component.ingest(&record, true);
        assert!(!component.dirty);
        record.sequence += 1;
        record.role = TrainingRecordRole::Epoch;
        component.ingest(&record, true);
        assert_eq!(component.curves[0].buckets.len(), 2);
        component.prepare();
        assert_eq!(component.shapes[0], shape);
        record.attemptid = "resumed".into();
        record.sequence = 1;
        component.ingest(&record, true);
        assert_ne!(
            component.curves[0].buckets.front().unwrap().segment,
            component.curves[0].buckets.back().unwrap().segment
        );
        component.clear();
        component.prepare();
        assert_eq!(component.shapes[0], shape);
        component
            .plot
            .update_series(&shape.unwrap(), |series| {
                assert!(series.positions.is_empty())
            })
            .unwrap();
    }
    #[test]
    fn observed_sequences_and_cumulative_drops_survive_display_coalescing() {
        let mut component = Component::default();
        let mut record = record();
        record.sequence = 0;
        component.ingest(&record, true);
        assert_eq!(component.curves[0].buckets.len(), 1);
        let first = component.curves[0].segment;
        record.sequence = 1;
        record.progress.elapsedseconds = 1.5;
        component.ingest(&record, true);
        assert_eq!(component.sequence, Some(1));
        record.sequence = 2;
        record.role = TrainingRecordRole::Epoch;
        component.ingest(&record, true);
        assert_eq!(component.curves[0].segment, first);
        component.ingest(&record, true);
        assert_eq!(component.curves[0].buckets.len(), 2);
        record.sequence = 3;
        record.droppedbefore = 1;
        component.ingest(&record, true);
        let dropped = component.curves[0].segment;
        assert_ne!(first, dropped);
        record.sequence = 4;
        component.ingest(&record, true);
        assert_eq!(component.curves[0].segment, dropped);
        record.sequence = 5;
        record.droppedbefore = 2;
        component.ingest(&record, true);
        assert_eq!(component.curves[0].segment, dropped + 1);
        record.attemptid = "next".into();
        record.sequence = 0;
        record.droppedbefore = 0;
        component.ingest(&record, true);
        assert_eq!(component.sequence, Some(0));
        assert_eq!(component.dropped, 0);
    }

    #[test]
    fn coalesced_actual_holes_and_unavailability_remain_pending() {
        let mut component = Component::default();
        let mut record = record();
        record.sequence = 0;
        component.ingest(&record, true);
        let first = component.curves[0].segment;
        record.sequence = 2;
        record.progress.elapsedseconds = 1.2;
        record.progress.scalars.total = None;
        component.ingest(&record, true);
        assert!(!component.curves[0].available);
        assert_eq!(component.curves[0].buckets.len(), 1);
        record.sequence = 3;
        record.progress.elapsedseconds = 1.4;
        record.progress.scalars.total = Some(3.0);
        component.ingest(&record, true);
        assert!(component.curves[0].available);
        assert!(component.curves[0].missing);
        record.sequence = 4;
        record.progress.elapsedseconds = 2.0;
        component.ingest(&record, true);
        assert_ne!(component.curves[0].segment, first);
        let resumed = component.curves[0].segment;
        record.sequence = 5;
        record.progress.elapsedseconds = 3.0;
        component.ingest(&record, true);
        assert_eq!(component.curves[0].segment, resumed);
        record.sequence = 6;
        record.progress.scalars.total = None;
        component.ingest(&record, true);
        assert!(!component.curves[0].available);
        assert!(!component.curves[0].buckets.is_empty());
    }

    #[test]
    fn coalesced_missing_and_drop_boundaries_break_once_without_sequence_holes() {
        let mut component = Component::default();
        let mut sample = record();
        sample.sequence = 0;
        component.ingest(&sample, true);
        for drop in [false, true] {
            let segment = component.curves[0].segment;
            sample.sequence += 1;
            sample.progress.elapsedseconds += 0.2;
            if drop {
                sample.droppedbefore += 1;
            } else {
                sample.progress.scalars.total = None;
            }
            component.ingest(&sample, true);
            sample.sequence += 1;
            sample.progress.elapsedseconds += 0.2;
            sample.progress.scalars.total = Some(2.0);
            component.ingest(&sample, true);
            assert_eq!(component.curves[0].segment, segment);
            sample.sequence += 1;
            sample.role = TrainingRecordRole::Epoch;
            component.ingest(&sample, true);
            assert_eq!(component.curves[0].segment, segment + 1);
            sample.sequence += 1;
            component.ingest(&sample, true);
            assert_eq!(component.curves[0].segment, segment + 1);
            sample.role = TrainingRecordRole::Live;
        }
    }

    #[test]
    fn hidden_reveal_departure_and_history_invalidation_retain_series() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut sample = record();
        model.workflow.training.as_mut().unwrap().metrics = Some(sample.clone());
        let mut component = Component::default();
        component.rebase(&model, true);
        let id = component.shapes[0].unwrap();
        sample.sequence += 1;
        sample.progress.elapsedseconds += 1.0;
        model.workflow.training.as_mut().unwrap().metrics = Some(sample);
        component.rebase(&model, false);
        component
            .plot
            .update_series(&id, |series| assert_eq!(series.positions.len(), 1))
            .unwrap();
        component.rebase(&model, true);
        component
            .plot
            .update_series(&id, |series| assert_eq!(series.positions.len(), 2))
            .unwrap();
        component.rebase(&model, false);
        assert!(!component.dirty);
        // Start/resume clear saved selection before rebase; no live metrics yet.
        component.generation = 7;
        model.workflow.training.as_mut().unwrap().metrics = None;
        component.rebase(&model, true);
        component
            .plot
            .update_series(&id, |series| assert!(series.positions.is_empty()))
            .unwrap();
        assert_eq!(component.shapes[0], Some(id));
        component.ingest(&record(), true);
        component.prepare();
        component.reset(true);
        component
            .plot
            .update_series(&id, |series| assert!(series.positions.is_empty()))
            .unwrap();
        assert_eq!(component.shapes[0], Some(id));
    }

    #[test]
    fn missing_values_form_gaps_and_disconnected_capacity_is_bounded() {
        let mut curve = Curve::new("loss", Group::Loss);
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
        let mut curve = Curve::new("loss", Group::Loss);
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
