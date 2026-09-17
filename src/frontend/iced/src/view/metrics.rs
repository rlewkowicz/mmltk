//! Bounded visual summaries; native records remain authoritative history.
mod catalog;
mod chart;
mod history;
pub use catalog::Chart;
use catalog::Metric;
use chart::RetainedChart;
use crate::fluent_theme::Element;
use iced::widget::{button, checkbox, column, container, row, text};

#[derive(Debug, Clone)]
pub enum Message {
    Visible(Chart, bool), Reset, Selector, Expand(Option<Chart>), Epoch(bool), Log(bool),
    Plot(Chart, iced_plot::PlotUiMessage),
}
pub struct Component {
    charts: Vec<RetainedChart>,
    metrics: Vec<Metric>,
    epoch: bool,
    log: bool,
    selector: bool,
    expanded: Option<Chart>,
    live: history::History,
    saved: history::History,
    saved_selected: bool,
}
impl Default for Component {
    fn default() -> Self {
        let metrics = catalog::catalog();
        Self {
            charts: Chart::ALL.into_iter().map(|kind| RetainedChart::new(kind, metrics.len())).collect(),
            live: history::History::new(&metrics), saved: history::History::new(&metrics), saved_selected: false,
            metrics, epoch: true, log: false, selector: false, expanded: None,
        }
    }
}
impl Component {
    fn invalidate(&mut self) { for chart in &mut self.charts { chart.dirty = true; } }
    fn history(&self) -> &history::History { if self.saved_selected { &self.saved } else { &self.live } }
    pub fn clear(&mut self) { self.live.clear(&self.metrics); self.saved.clear(&self.metrics); self.invalidate(); }
    pub fn reset(&mut self, visible: bool) { self.clear(); if visible { self.prepare(); } }
    pub fn rebase(&mut self, model: &crate::view_model::ApplicationModel, visible: bool) {
        let mut live_changed = 0;
        if let Some(training) = &model.workflow.training {
            if training.local.active && training.local.progress.sequence == 0 {
                if self.live.sequence.is_some() { self.live.clear(&self.metrics); live_changed = u16::MAX; }
            } else if let Some(record) = &training.metrics {
                live_changed = self.live.ingest(record, true, &self.metrics);
            }
        }
        let selected = model.workflow.training_run.is_some();
        let mut saved_changed = 0;
        if let Some(opened) = &model.workflow.training_run {
            if self.saved.generation != opened.generation {
                self.saved.clear(&self.metrics);
                self.saved.set_run(&opened.run.runid, opened.generation);
                saved_changed = u16::MAX;
            }
            if let Some(page) = &model.workflow.training_history {
                let key = (page.generation, page.nextcursor);
                if page.generation == self.saved.generation && self.saved.page != Some(key) {
                    for record in &page.records { saved_changed |= self.saved.ingest(record, false, &self.metrics); }
                    self.saved.page = Some(key);
                }
            }
        }
        let changed = if selected != self.saved_selected { u16::MAX } else if selected { saved_changed } else { live_changed };
        for chart in &mut self.charts { chart.dirty |= changed & (1 << chart.kind as usize) != 0; }
        self.saved_selected = selected;
        if visible { self.prepare(); }
    }
    pub fn update(&mut self, message: Message) {
        match message {
            Message::Visible(kind, visible) => { if let Some(chart) = self.charts.iter_mut().find(|c| c.kind == kind) { chart.visible = visible; } }
            Message::Reset => { for chart in &mut self.charts { chart.visible = chart.kind.main(); } self.expanded = None; }
            Message::Selector => self.selector = !self.selector,
            Message::Expand(chart) => self.expanded = chart,
            Message::Epoch(epoch) => { self.epoch = epoch; self.invalidate(); }
            Message::Log(log) => { self.log = log; self.invalidate(); }
            Message::Plot(kind, message) => { if let Some(chart) = self.charts.iter_mut().find(|c| c.kind == kind) { chart.plot.update(message); } }
        }
        self.prepare();
    }
    fn prepare(&mut self) {
        let history = if self.saved_selected { &self.saved } else { &self.live };
        for chart in &mut self.charts {
            if self.expanded.map_or(chart.visible, |kind| kind == chart.kind) {
                chart.prepare(&history.curves, &self.metrics, self.epoch, self.log);
            }
        }
    }
    pub fn omitted_summaries(&self) -> u64 { self.history().curves.iter().map(|curve| curve.omitted).sum() }
    pub fn controls_height(&self) -> f32 { (if self.selector { 230.0 } else { 40.0 }) + if self.expanded.is_some() { 34.0 } else { 0.0 } }
    pub fn view(&self, width: f32, height: f32) -> Element<'_, Message> {
        let controls = row![container(button("Charts").on_press(Message::Selector)).id("train.metrics.charts"),
            checkbox(self.epoch).label("Epoch axis").on_toggle(Message::Epoch),
            checkbox(self.log).label("Log loss scale").on_toggle(Message::Log)].spacing(8);
        let mut content = column![controls].spacing(6);
        if self.selector {
            let mut choices = column![button("Reset to main charts").on_press(Message::Reset)].spacing(4);
            for chunk in self.charts.chunks(2) {
                let mut line = row![].spacing(8);
                for chart in chunk { let kind = chart.kind; line = line.push(container(checkbox(chart.visible).label(kind.label()).on_toggle(move |v| Message::Visible(kind, v))).id(format!("train.metrics.visible.{kind:?}")).width(iced::FillPortion(1))); }
                choices = choices.push(line);
            }
            content = content.push(container(choices).id("train.metrics.selector"));
        }
        let selected: Vec<_> = self.charts.iter().filter(|c| self.expanded.map_or(c.visible, |kind| kind == c.kind)).collect();
        let workspace: Element<'_, Message> = if selected.is_empty() {
            container(text("No charts selected")).center(iced::Fill).into()
        } else if self.history().sequence.is_none() {
            container(text("No training data yet")).center(iced::Fill).into()
        } else {
            let columns = if self.expanded.is_some() { 1 } else { ((selected.len() as f32 * width / height.max(1.0) / 1.5).sqrt().round() as usize).clamp(1, selected.len()) };
            let rows = selected.len().div_ceil(columns);
            let gap = 6.0_f32.min(height / (rows * 2) as f32);
            let tile_height = (height - gap * (rows - 1) as f32) / rows as f32;
            let mut grid = column![].spacing(gap);
            for chunk in selected.chunks(columns) {
                let mut line = row![].spacing(6).height(tile_height);
                for chart in chunk {
                    let kind = chart.kind;
                    let header = container(container(button(text(kind.label()).size(12)).on_press(Message::Expand(if self.expanded.is_some() { None } else { Some(kind) }))).id(format!("train.metrics.expand.{kind:?}"))).center_x(iced::Fill);
                    let has_data = self.history().curves.iter().zip(&self.metrics).any(|(c, m)| m.chart == kind && !c.buckets.is_empty());
                    let body: Element<'_, Message> = if has_data { chart.plot.view().map(move |m| Message::Plot(kind, m)) }
                        else { container(text(if kind.loss() || kind == Chart::LearningRate { "No recorded measurements" } else { "Waiting for validation" }).size(12)).center(iced::Fill).into() };
                    line = line.push(container(column![header, body].spacing(2)).clip(true).id(format!("train.metrics.chart.{kind:?}")).width(iced::FillPortion(1)).height(iced::Fill).style(crate::fluent_theme::container_workspace));
                }
                grid = grid.push(line);
            }
            grid.into()
        };
        if self.expanded.is_some() { content = content.push(container(button("Back to charts").on_press(Message::Expand(None))).id("train.metrics.back")); }
        content.push(container(workspace).id("train.metrics.plot").width(width).height(height)).into()
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::generated::*;
    pub(crate) fn record() -> TrainingRecord {
        use crate::generated::*;
        TrainingRecord {
            formatversion: 2,
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
                completedimages: 4,
                totalimages: 40,
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

    pub(super) fn evaluation() -> EvalSummary {
        EvalSummary { bbox: MetricSummary {
            ap: 0.42, ap50: 0.65, ap75: 0.37, available: true,
            averagerecall: [Some(0.2), Some(0.4), Some(0.6)],
            detectionlimits: [1, 10, 300], areaap: [Some(0.1), None, Some(0.7)],
            areaar: [Some(0.2), None, Some(0.8)],
            confidence: ConfidenceMetrics { precision: 0.8, recall: 0.5, f1: 0.615 },
            confidencethreshold: 0.5,
        }, mask: None, modeldetectionbudget: 300 }
    }
    fn curve<'a>(history: &'a history::History, metrics: &[Metric], chart: Chart) -> &'a history::Curve {
        &history.curves[metrics.iter().position(|m| m.chart == chart).unwrap()]
    }
    #[test]
    fn sparse_validation_uses_completed_epochs_without_live_or_terminal_duplicates() {
        let metrics = catalog::catalog();
        let mut history = history::History::new(&metrics);
        let mut sample = record();
        history.ingest(&sample, true, &metrics);
        assert!(curve(&history, &metrics, Chart::Ap50).buckets.is_empty());
        for epoch in 0..2 {
            sample.sequence += 1; sample.role = TrainingRecordRole::Epoch;
            sample.progress.epoch = epoch; sample.progress.val = Some(evaluation());
            history.ingest(&sample, true, &metrics);
            sample.sequence += 1; sample.role = TrainingRecordRole::Live;
            sample.progress.val = None;
            history.ingest(&sample, true, &metrics);
        }
        let ap = curve(&history, &metrics, Chart::Ap50);
        assert_eq!(ap.buckets.len(), 2);
        assert_eq!(ap.buckets[0].first.value, 0.65);
        assert_eq!(ap.buckets[0].first.epoch, 1.0);
        assert_eq!(ap.buckets[1].first.epoch, 2.0);
        assert_eq!(ap.buckets[0].segment, ap.buckets[1].segment);
        sample.sequence += 1; sample.role = TrainingRecordRole::Terminal;
        sample.progress.val = Some(evaluation()); sample.progress.test = Some(evaluation());
        history.ingest(&sample, true, &metrics);
        assert_eq!(curve(&history, &metrics, Chart::Ap50).buckets.len(), 2);
    }
    #[test]
    fn gaps_invalid_evaluations_and_resumed_attempts_never_join() {
        let metrics = catalog::catalog();
        let mut history = history::History::new(&metrics);
        let mut sample = record(); sample.role = TrainingRecordRole::Epoch;
        sample.progress.val = Some(evaluation());
        history.ingest(&sample, true, &metrics);
        for missing in [true, false] {
            sample.sequence += if missing { 1 } else { 2 };
            sample.progress.epoch += 1;
            sample.progress.val.as_mut().unwrap().bbox.ap50 = f64::NAN;
            history.ingest(&sample, true, &metrics);
            sample.sequence += 1; sample.progress.epoch += 1;
            sample.progress.val = Some(evaluation());
            history.ingest(&sample, true, &metrics);
        }
        sample.attemptid = "resume".into(); sample.sequence = 0;
        history.ingest(&sample, true, &metrics);
        let ap = curve(&history, &metrics, Chart::Ap50);
        assert_eq!(ap.buckets.len(), 4);
        assert!(ap.buckets.iter().zip(ap.buckets.iter().skip(1)).all(|(a,b)| a.segment != b.segment));
    }
    #[test]
    fn six_main_charts_retention_controls_and_live_epoch_coordinates() {
        let mut component = Component::default();
        assert_eq!(component.charts.iter().filter(|c| c.visible).count(), 6);
        assert!(component.epoch);
        let mut sample = record();
        component.live.ingest(&sample, true, &component.metrics);
        component.prepare();
        let id = component.charts[0].shapes[0];
        assert!(id.is_some());
        assert_eq!(component.live.curves[0].buckets[0].first.epoch, 1.1);
        sample.sequence += 1; sample.progress.elapsedseconds += 0.2;
        assert_eq!(component.live.ingest(&sample, true, &component.metrics), 0);
        sample.sequence += 1; sample.progress.phase = TrainingPhase::Validate;
        assert_ne!(component.live.ingest(&sample, true, &component.metrics), 0);
        component.update(Message::Expand(Some(Chart::Loss)));
        component.update(Message::Expand(None));
        assert_eq!(component.charts[0].shapes[0], id);
        for kind in Chart::ALL { component.update(Message::Visible(kind, false)); }
        assert!(component.charts.iter().all(|c| !c.visible));
        component.update(Message::Reset);
        assert_eq!(component.charts.iter().filter(|c| c.visible).count(), 6);
        component.reset(true);
        assert_eq!(component.charts[0].shapes[0], id);
        component.charts[0].plot.update_series(&id.unwrap(), |s| assert!(s.positions.is_empty())).unwrap();
    }
    #[test]
    fn hidden_navigation_saved_selection_and_preparation_preserve_independent_live_history() {
        let mut component = Component::default();
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut sample = record();
        model.workflow.training.as_mut().unwrap().metrics = Some(sample.clone());
        component.rebase(&model, false);
        assert!(component.charts.iter().all(|c| c.shapes.iter().all(Option::is_none)));
        component.rebase(&model, true);
        let shape = component.charts[0].shapes[0].unwrap();
        component.charts[0].plot.update_series(&shape, |series| {
            assert_eq!(series.positions, vec![[1.1, 1.0]]);
        }).unwrap();

        let configuration = model.settings_snapshot.as_ref().unwrap()
            .settingsstate.workflows.train.request.clone();
        model.workflow.training_run = Some(TrainingOpenedRun {
            generation: 3,
            directory: "saved-output".into(),
            run: TrainingRun {
                formatversion: 2,
                runid: "saved".into(),
                attemptid: "saved-attempt".into(),
                checkpointattemptid: String::new(),
                sourcecheckpointattemptid: String::new(),
                configuration,
                execution: TrainingExecutionFacts {
                    evallanes: 1,
                    effectivebatchperrank: 4,
                    effectivebatchglobal: 4,
                    datasetlimits: TrainingDatasetLimits {
                        trainmaxinstances: 1,
                        valmaxinstances: 1,
                        testmaxinstances: None,
                        largestmaxinstances: 1,
                        resolvednumqueries: 6,
                        requirednumqueries: 1,
                        automaticnumqueriescap: 6,
                        querysource: "fixture".into(),
                        requestedoverride: false,
                        automatic: false,
                    },
                },
                originalweights: "fixture.pt".into(),
                originalclassdescriptor: String::new(),
                evaluatedweights: EvaluatedWeights::Ordinary,
                classlayout: ModelClassLayout {
                    version: 1,
                    foreground: OrderedClassCatalog { names: Vec::new() },
                    classnameevidence: OrderedClassCatalog { names: Vec::new() },
                    slots: Vec::new(),
                    scores: ClassScoreEncoding::SigmoidLogits,
                    noobject: NoObjectEncoding::AllNegative,
                    provenance: ClassLayoutProvenance {
                        origin: ClassLayoutOrigin::Unresolved,
                        producer: "fixture".into(),
                        artifactsha256: String::new(),
                    },
                    supervisioninforegroundorder: false,
                },
                resumeepoch: -1,
                resumeoptimizerstep: 0,
            },
        });
        let mut saved = record();
        saved.runid = "saved".into();
        saved.attemptid = "saved-attempt".into();
        saved.progress.epoch = 9;
        saved.progress.scalars.total = Some(77.0);
        model.workflow.training_history = Some(TrainingHistoryPage {
            generation: 3,
            nextcursor: 100,
            more: false,
            records: vec![saved],
        });
        component.rebase(&model, true);
        assert!(component.saved_selected);
        assert_eq!(component.history().curves[0].buckets[0].first.value, 77.0);
        assert_eq!(component.charts[0].shapes[0], Some(shape));
        component.charts[0].plot.update_series(&shape, |series| {
            assert_eq!(series.positions, vec![[9.1, 77.0]]);
        }).unwrap();

        // Live delivery continues while saved charts are selected and hidden.
        sample.sequence += 1;
        sample.progress.elapsedseconds += 1.0;
        sample.progress.completedbatches = 2;
        sample.progress.scalars.total = Some(2.0);
        model.workflow.training.as_mut().unwrap().metrics = Some(sample);
        component.rebase(&model, false);
        assert!(component.saved_selected);
        assert_eq!(component.live.curves[0].buckets.len(), 2);
        assert_eq!(component.history().curves[0].buckets[0].first.value, 77.0);
        component.rebase(&model, true);
        component.charts[0].plot.update_series(&shape, |series| {
            assert_eq!(series.positions, vec![[9.1, 77.0]]);
        }).unwrap();

        // This is the same explicit selection clearing used by Current live run.
        model.workflow.training_run = None;
        model.workflow.training_history = None;
        component.rebase(&model, true);
        assert!(!component.saved_selected);
        assert_eq!(component.charts[0].shapes[0], Some(shape));
        component.charts[0].plot.update_series(&shape, |series| {
            assert_eq!(series.positions, vec![[1.1, 1.0], [1.2, 2.0]]);
        }).unwrap();

        // Old metric storage can still be present during new-run inspection.
        let snapshot = model.workflow.training.as_mut().unwrap();
        snapshot.local.active = true;
        snapshot.local.progress.sequence = 0;
        component.rebase(&model, true);
        assert!(component.live.sequence.is_none());
        assert_eq!(component.saved.curves[0].buckets[0].first.value, 77.0);
        assert_eq!(component.charts[0].shapes[0], Some(shape));
        component.charts[0].plot.update_series(&shape, |series| {
            assert!(series.positions.is_empty());
        }).unwrap();
    }
}
