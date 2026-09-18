//! Bounded visual summaries; native records remain authoritative history.
mod catalog;
mod chart;
mod history;
use crate::fluent_theme::Element;
pub use catalog::Chart;
use catalog::Metric;
use chart::RetainedChart;
use iced::widget::{button, checkbox, column, container, row, text};

#[derive(Debug, Clone)]
pub enum Message {
    Visible(Chart, bool),
    Reset,
    Selector,
    Expand(Option<Chart>),
    Epoch(bool),
    Log(bool),
    Plot(Chart, iced_plot::PlotUiMessage),
}
/// Read-only rendered-view facts, collected only by enabled acceptance workflows.
#[derive(Debug, Clone)]
pub(crate) struct ChartView {
    pub ranges: [[f64; 2]; 2],
    pub plot_bounds: iced::Rectangle,
    pub legend_collapsed: bool,
    pub legend_control: String,
    pub visible: bool,
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
            charts: Chart::ALL
                .into_iter()
                .map(|kind| RetainedChart::new(kind, metrics.len()))
                .collect(),
            live: history::History::new(&metrics),
            saved: history::History::new(&metrics),
            saved_selected: false,
            metrics,
            epoch: true,
            log: false,
            selector: false,
            expanded: None,
        }
    }
}
impl Component {
    pub(crate) fn chart_view(&self, kind: Chart) -> Option<ChartView> {
        let chart = self.charts.iter().find(|chart| chart.kind == kind)?;
        Some(ChartView {
            ranges: chart.plot.view_ranges()?,
            plot_bounds: chart.plot.view_bounds()?,
            legend_collapsed: chart.plot.legend_collapsed(),
            legend_control: chart.plot.legend_control_id(),
            visible: self
                .expanded
                .map_or(chart.visible, |expanded| expanded == kind),
        })
    }
    fn invalidate(&mut self) {
        for chart in &mut self.charts {
            chart.dirty = true;
        }
    }
    fn history(&self) -> &history::History {
        if self.saved_selected {
            &self.saved
        } else {
            &self.live
        }
    }
    pub fn clear(&mut self) {
        self.live.clear(&self.metrics);
        self.saved.clear(&self.metrics);
        self.invalidate();
    }
    pub fn reset(&mut self, visible: bool) {
        self.clear();
        if visible {
            self.prepare();
        }
    }
    pub fn rebase(&mut self, model: &crate::view_model::ApplicationModel, visible: bool) {
        let mut live_changed = 0;
        if let Some(training) = &model.workflow.training {
            if training.local.active && training.local.progress.sequence == 0 {
                if self.live.sequence.is_some() {
                    self.live.clear(&self.metrics);
                    live_changed = u16::MAX;
                }
            } else if let Some(record) = &training.metrics {
                live_changed = self.live.ingest(record, true, &self.metrics);
            }
        }
        let selected = model.workflow.output.saved().is_some();
        let mut saved_changed = 0;
        if let Some(opened) = model.workflow.output.run() {
            if self.saved.generation != opened.generation {
                self.saved.clear(&self.metrics);
                if let Some(run) = &opened.run {
                    self.saved.set_run(&run.runid, opened.generation);
                } else { self.saved.generation = opened.generation; }
                saved_changed = u16::MAX;
            }
            if let Some(page) = model.workflow.output.page() {
                let key = (page.generation, page.nextcursor);
                if page.generation == self.saved.generation && self.saved.page != Some(key) {
                    for record in &page.records {
                        saved_changed |= self.saved.ingest(record, false, &self.metrics);
                    }
                    self.saved.page = Some(key);
                }
            }
        }
        if selected && model.workflow.output.run().is_none() && (self.saved.generation != 0 || !self.saved_selected) {
            self.saved.clear(&self.metrics);
            saved_changed = u16::MAX;
        }
        let changed = if selected != self.saved_selected {
            u16::MAX
        } else if selected {
            saved_changed
        } else {
            live_changed
        };
        for chart in &mut self.charts {
            chart.dirty |= changed & (1 << chart.kind as usize) != 0;
        }
        self.saved_selected = selected;
        if visible {
            self.prepare();
        }
    }
    pub fn update(&mut self, message: Message) {
        match message {
            Message::Visible(kind, visible) => {
                if let Some(chart) = self.charts.iter_mut().find(|c| c.kind == kind) {
                    chart.visible = visible;
                }
            }
            Message::Reset => {
                for chart in &mut self.charts {
                    chart.visible = chart.kind.main();
                }
                self.expanded = None;
            }
            Message::Selector => self.selector = !self.selector,
            Message::Expand(chart) => self.expanded = chart,
            Message::Epoch(epoch) => {
                self.epoch = epoch;
                self.invalidate();
            }
            Message::Log(log) => {
                if self.log != log {
                    self.log = log;
                    for chart in &mut self.charts {
                        chart.dirty |= chart.kind.loss();
                    }
                }
            }
            Message::Plot(kind, message) => {
                if let Some(chart) = self.charts.iter_mut().find(|c| c.kind == kind) {
                    chart.plot.update(message);
                }
            }
        }
        self.prepare();
    }
    fn prepare(&mut self) {
        let history = if self.saved_selected {
            &self.saved
        } else {
            &self.live
        };
        for chart in &mut self.charts {
            if self
                .expanded
                .map_or(chart.visible, |kind| kind == chart.kind)
            {
                chart.prepare(&history.curves, &self.metrics, self.epoch, self.log);
            }
        }
    }
    pub fn omitted_summaries(&self) -> u64 {
        self.history()
            .curves
            .iter()
            .map(|curve| curve.omitted)
            .sum()
    }
    pub fn controls_height(&self) -> f32 {
        (if self.selector { 230.0 } else { 40.0 })
            + if self.expanded.is_some() { 34.0 } else { 0.0 }
    }
    pub fn view(&self, width: f32, height: f32) -> Element<'_, Message> {
        let controls = row![
            container(button("Charts").on_press(Message::Selector)).id("train.metrics.charts"),
            checkbox(self.epoch)
                .label("Epoch axis")
                .on_toggle(Message::Epoch),
            checkbox(self.log)
                .label("Log loss scale")
                .on_toggle(Message::Log)
        ]
        .spacing(8);
        let mut content = column![controls].spacing(6);
        if self.selector {
            let mut choices =
                column![button("Reset to main charts").on_press(Message::Reset)].spacing(4);
            for chunk in self.charts.chunks(2) {
                let mut line = row![].spacing(8);
                for chart in chunk {
                    let kind = chart.kind;
                    line = line.push(
                        container(
                            container(
                                checkbox(chart.visible)
                                    .label(kind.label())
                                    .on_toggle(move |v| Message::Visible(kind, v)),
                            )
                            .id(format!("train.metrics.visible.{kind:?}")),
                        )
                        .width(iced::FillPortion(1)),
                    );
                }
                choices = choices.push(line);
            }
            content = content.push(container(choices).id("train.metrics.selector"));
        }
        let selected: Vec<_> = self
            .charts
            .iter()
            .filter(|c| self.expanded.map_or(c.visible, |kind| kind == c.kind))
            .collect();
        let workspace: Element<'_, Message> = if selected.is_empty() {
            container(text("No charts selected"))
                .center(iced::Fill)
                .into()
        } else if self.history().sequence.is_none() {
            container(text("No training data yet"))
                .center(iced::Fill)
                .into()
        } else {
            let columns = if self.expanded.is_some() {
                1
            } else {
                ((selected.len() as f32 * width / height.max(1.0) / 1.5)
                    .sqrt()
                    .round() as usize)
                    .clamp(1, selected.len())
            };
            let rows = selected.len().div_ceil(columns);
            let gap = 6.0_f32.min(height / (rows * 2) as f32);
            let tile_height = (height - gap * (rows - 1) as f32) / rows as f32;
            let mut grid = column![].spacing(gap);
            for chunk in selected.chunks(columns) {
                let mut line = row![].spacing(6).height(tile_height);
                for chart in chunk {
                    let kind = chart.kind;
                    let header =
                        container(
                            container(button(text(kind.label()).size(12)).on_press(
                                Message::Expand(if self.expanded.is_some() {
                                    None
                                } else {
                                    Some(kind)
                                }),
                            ))
                            .id(format!("train.metrics.expand.{kind:?}")),
                        )
                        .center_x(iced::Fill);
                    let has_data = self
                        .history()
                        .curves
                        .iter()
                        .zip(&self.metrics)
                        .any(|(c, m)| m.chart == kind && !c.buckets.is_empty());
                    let body: Element<'_, Message> = if has_data {
                        chart.plot.view().map(move |m| Message::Plot(kind, m))
                    } else {
                        container(
                            text(if kind.loss() || kind == Chart::LearningRate {
                                "No recorded measurements"
                            } else {
                                "Waiting for validation"
                            })
                            .size(12),
                        )
                        .center(iced::Fill)
                        .into()
                    };
                    line = line.push(
                        container(column![header, body].spacing(2))
                            .clip(true)
                            .id(format!("train.metrics.chart.{kind:?}"))
                            .width(iced::FillPortion(1))
                            .height(iced::Fill)
                            .style(crate::fluent_theme::container_workspace),
                    );
                }
                grid = grid.push(line);
            }
            grid.into()
        };
        if self.expanded.is_some() {
            content = content.push(
                container(button("Back to charts").on_press(Message::Expand(None)))
                    .id("train.metrics.back"),
            );
        }
        content
            .push(
                container(workspace)
                    .id("train.metrics.plot")
                    .width(width)
                    .height(height),
            )
            .into()
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::generated::*;
    use iced::widget::shader::Program;
    use iced::{Event, Point, Rectangle, Size, mouse};
    use iced_plot::{PlotUiMessage, PlotWidget};
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
        EvalSummary {
            bbox: MetricSummary {
                ap: 0.42,
                ap50: 0.65,
                ap75: 0.37,
                available: true,
                averagerecall: [Some(0.2), Some(0.4), Some(0.6)],
                detectionlimits: [1, 10, 300],
                areaap: [Some(0.1), None, Some(0.7)],
                areaar: [Some(0.2), None, Some(0.8)],
                confidence: ConfidenceMetrics {
                    precision: 0.8,
                    recall: 0.5,
                    f1: 0.615,
                },
                confidencethreshold: 0.5,
            },
            mask: None,
            modeldetectionbudget: 300,
        }
    }
    fn curve<'a>(
        history: &'a history::History,
        metrics: &[Metric],
        chart: Chart,
    ) -> &'a history::Curve {
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
            sample.sequence += 1;
            sample.role = TrainingRecordRole::Epoch;
            sample.progress.epoch = epoch;
            sample.progress.val = Some(evaluation());
            history.ingest(&sample, true, &metrics);
            sample.sequence += 1;
            sample.role = TrainingRecordRole::Live;
            sample.progress.val = None;
            history.ingest(&sample, true, &metrics);
        }
        let ap = curve(&history, &metrics, Chart::Ap50);
        assert_eq!(ap.buckets.len(), 2);
        assert_eq!(ap.buckets[0].first.value, 0.65);
        assert_eq!(ap.buckets[0].first.epoch, 1.0);
        assert_eq!(ap.buckets[1].first.epoch, 2.0);
        assert_eq!(ap.buckets[0].segment, ap.buckets[1].segment);
        sample.sequence += 1;
        sample.role = TrainingRecordRole::Terminal;
        sample.progress.val = Some(evaluation());
        sample.progress.test = Some(evaluation());
        history.ingest(&sample, true, &metrics);
        assert_eq!(curve(&history, &metrics, Chart::Ap50).buckets.len(), 2);
    }
    #[test]
    fn gaps_invalid_evaluations_and_resumed_attempts_never_join() {
        let metrics = catalog::catalog();
        let mut history = history::History::new(&metrics);
        let mut sample = record();
        sample.role = TrainingRecordRole::Epoch;
        sample.progress.val = Some(evaluation());
        history.ingest(&sample, true, &metrics);
        for missing in [true, false] {
            sample.sequence += if missing { 1 } else { 2 };
            sample.progress.epoch += 1;
            sample.progress.val.as_mut().unwrap().bbox.ap50 = f64::NAN;
            history.ingest(&sample, true, &metrics);
            sample.sequence += 1;
            sample.progress.epoch += 1;
            sample.progress.val = Some(evaluation());
            history.ingest(&sample, true, &metrics);
        }
        sample.attemptid = "resume".into();
        sample.sequence = 0;
        history.ingest(&sample, true, &metrics);
        let ap = curve(&history, &metrics, Chart::Ap50);
        assert_eq!(ap.buckets.len(), 4);
        assert!(
            ap.buckets
                .iter()
                .zip(ap.buckets.iter().skip(1))
                .all(|(a, b)| a.segment != b.segment)
        );
    }
    #[test]
    fn evaluation_availability_caps_and_nonfinite_leaves_are_honest() {
        let metrics = catalog::catalog();
        let mut history = history::History::new(&metrics);
        let mut sample = record();
        sample.role = TrainingRecordRole::Epoch;
        sample.progress.val = Some(evaluation());
        sample.progress.val.as_mut().unwrap().bbox.available = false;
        history.ingest(&sample, false, &metrics);
        assert!(curve(&history, &metrics, Chart::Ap).buckets.is_empty());
        sample.sequence += 1;
        sample.progress.epoch += 1;
        let bbox = &mut sample.progress.val.as_mut().unwrap().bbox;
        bbox.available = true;
        bbox.detectionlimits = [2, 23, 456];
        bbox.confidence.precision = f64::INFINITY;
        bbox.areaap[0] = Some(f64::NEG_INFINITY);
        history.ingest(&sample, false, &metrics);
        assert_eq!(
            curve(&history, &metrics, Chart::Ap).buckets[0].first.value,
            0.42
        );
        assert!(
            curve(&history, &metrics, Chart::Confidence)
                .buckets
                .is_empty()
        );
        assert!(curve(&history, &metrics, Chart::Area).buckets.is_empty());
        let recalls = metrics
            .iter()
            .zip(&history.curves)
            .filter(|(m, _)| m.chart == Chart::AverageRecall)
            .map(|(_, c)| c.name.as_str())
            .collect::<Vec<_>>();
        assert_eq!(recalls, ["Box AR@2", "Box AR@23", "Box AR@456"]);
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
        sample.sequence += 1;
        sample.progress.elapsedseconds += 0.2;
        assert_eq!(component.live.ingest(&sample, true, &component.metrics), 0);
        sample.sequence += 1;
        sample.progress.phase = TrainingPhase::Validate;
        assert_ne!(component.live.ingest(&sample, true, &component.metrics), 0);
        component.update(Message::Expand(Some(Chart::Loss)));
        component.update(Message::Expand(None));
        assert_eq!(component.charts[0].shapes[0], id);
        for kind in Chart::ALL {
            component.update(Message::Visible(kind, false));
        }
        assert!(component.charts.iter().all(|c| !c.visible));
        component.update(Message::Reset);
        assert_eq!(component.charts.iter().filter(|c| c.visible).count(), 6);
        component.reset(true);
        assert_eq!(component.charts[0].shapes[0], id);
        component.charts[0]
            .plot
            .update_series(&id.unwrap(), |s| assert!(s.positions.is_empty()))
            .unwrap();
    }
    type PlotState = <PlotWidget as Program<PlotUiMessage>>::State;

    fn plot_event(
        component: &mut Component,
        kind: Chart,
        state: &mut PlotState,
        event: Event,
        cursor: mouse::Cursor,
    ) -> bool {
        let chart = component
            .charts
            .iter()
            .find(|chart| chart.kind == kind)
            .unwrap();
        let message = Program::update(
            &chart.plot,
            state,
            &event,
            Rectangle::with_size(Size::new(640.0, 360.0)),
            cursor,
        )
        .and_then(|action| action.into_inner().0);
        let published = message.is_some();
        if let Some(message) = message {
            component.update(Message::Plot(kind, message));
        }
        published
    }

    fn redraw(component: &mut Component, kind: Chart, state: &mut PlotState) -> bool {
        plot_event(
            component,
            kind,
            state,
            Event::Window(iced::window::Event::RedrawRequested(
                iced::time::Instant::now(),
            )),
            mouse::Cursor::Unavailable,
        )
    }

    fn pan(component: &mut Component, kind: Chart, state: &mut PlotState) -> [[f64; 2]; 2] {
        let initial = component.chart_view(kind).unwrap().ranges;
        for (event, position) in [
            (
                mouse::Event::ButtonPressed(mouse::Button::Left),
                Point::new(320.0, 180.0),
            ),
            (
                mouse::Event::CursorMoved {
                    position: Point::new(380.0, 200.0),
                },
                Point::new(380.0, 200.0),
            ),
            (
                mouse::Event::ButtonReleased(mouse::Button::Left),
                Point::new(380.0, 200.0),
            ),
        ] {
            plot_event(
                component,
                kind,
                state,
                Event::Mouse(event),
                mouse::Cursor::Available(position),
            );
        }
        plot_event(
            component,
            kind,
            state,
            Event::Mouse(mouse::Event::CursorMoved {
                position: Point::new(-1.0, -1.0),
            }),
            mouse::Cursor::Unavailable,
        );
        redraw(component, kind, state);
        let panned = component.chart_view(kind).unwrap().ranges;
        assert_ne!(panned, initial, "{kind:?} must actually pan");
        panned
    }

    fn dashboard_records(saved: bool) -> Vec<TrainingRecord> {
        [0, 1]
            .into_iter()
            .map(|index| {
                let mut sample = record();
                sample.sequence = index + 1;
                sample.role = TrainingRecordRole::Epoch;
                sample.progress.epoch = index as i32 * 2;
                sample.progress.globaloptimizerstep = 10 + index as i64 * 20;
                sample.progress.scalars.total = Some(if index == 0 { 1.0 } else { 100.0 });
                sample.progress.scalars.classification = sample.progress.scalars.total;
                sample.progress.scalars.learningrate = Some(0.001 * (index + 1) as f64);
                sample.progress.scalars.classerror = Some((index + 1) as f64);
                let mut summary = evaluation();
                summary.bbox.ap50 = if index == 0 { 0.2 } else { 0.8 };
                summary.mask = Some(summary.bbox.clone());
                sample.progress.val = Some(summary);
                if saved {
                    sample.runid = "saved".into();
                    sample.attemptid = "saved-attempt".into();
                    sample.progress.epoch += 10;
                    sample.progress.globaloptimizerstep += 100;
                }
                sample
            })
            .collect()
    }

    fn dashboard(saved: bool) -> (Component, crate::view_model::ApplicationModel) {
        let mut component = Component::default();
        let mut model = crate::view_model::test_support::bootstrapped();
        for sample in dashboard_records(false) {
            model.workflow.training.as_mut().unwrap().metrics = Some(sample);
            component.rebase(&model, true);
        }
        if saved {
            select_saved_run(&mut model);
            model.workflow.output.saved_mut().unwrap().page = Some(TrainingHistoryPage {
                generation: 3,
                nextcursor: 100,
                more: false,
                records: dashboard_records(true),
            });
            component.rebase(&model, true);
        }
        assert_eq!(component.saved_selected, saved);
        for kind in Chart::ALL {
            component.update(Message::Visible(kind, true));
        }
        assert!(component.charts.iter().all(|chart| !chart.dirty));
        (component, model)
    }

    #[test]
    fn loss_scale_preserves_non_loss_camera_and_legend_through_remount() {
        for saved in [false, true] {
            for hidden in [false, true] {
                for kind in Chart::ALL.into_iter().filter(|kind| !kind.loss()) {
                    let (mut component, _) = dashboard(saved);
                    let mut state = PlotState::default();
                    assert!(redraw(&mut component, kind, &mut state));
                    let camera = pan(&mut component, kind, &mut state);
                    component.update(Message::Plot(kind, PlotUiMessage::ToggleLegend));
                    let legend = component.chart_view(kind).unwrap().legend_collapsed;
                    assert!(!legend);
                    if hidden {
                        component.update(Message::Visible(kind, false));
                    }
                    component.update(Message::Log(true));
                    if hidden {
                        component.update(Message::Visible(kind, true));
                    }
                    let mut remount = PlotState::default();
                    assert!(redraw(&mut component, kind, &mut remount));
                    let view = component.chart_view(kind).unwrap();
                    assert_eq!(
                        view.ranges, camera,
                        "{kind:?}, saved={saved}, hidden={hidden}"
                    );
                    assert_eq!(view.legend_collapsed, legend);
                }
            }
        }
    }

    #[test]
    fn loss_scale_does_not_prepare_unrelated_visible_or_hidden_charts() {
        for saved in [false, true] {
            let (mut component, _) = dashboard(saved);
            let mut state = PlotState::default();
            redraw(&mut component, Chart::Ap50, &mut state);
            assert!(!redraw(&mut component, Chart::Ap50, &mut state));
            component.update(Message::Visible(Chart::LearningRate, false));
            component.update(Message::Log(true));
            // A prepared data version publishes even if its numerical camera
            // happens to be unchanged. This observes the ordinary program path.
            assert!(!redraw(&mut component, Chart::Ap50, &mut state));
            assert!(
                component
                    .charts
                    .iter()
                    .filter(|chart| !chart.kind.loss())
                    .all(|chart| !chart.dirty)
            );
        }
    }

    #[test]
    fn unchanged_loss_scale_selection_preserves_settled_views() {
        for saved in [false, true] {
            for log in [false, true] {
                for kind in Chart::ALL {
                    let (mut component, _) = dashboard(saved);
                    component.update(Message::Log(log));
                    let mut state = PlotState::default();
                    redraw(&mut component, kind, &mut state);
                    let camera = pan(&mut component, kind, &mut state);
                    component.update(Message::Log(log));
                    redraw(&mut component, kind, &mut PlotState::default());
                    assert_eq!(
                        component.chart_view(kind).unwrap().ranges,
                        camera,
                        "{kind:?}, saved={saved}, log={log}"
                    );
                }
            }
        }
    }

    fn assert_center(component: &Component, kind: Chart, expected: [f64; 2]) {
        let ranges = component.chart_view(kind).unwrap().ranges;
        for (range, expected) in ranges.into_iter().zip(expected) {
            assert!(
                ((range[0] + range[1]) / 2.0 - expected).abs() < 1e-9,
                "{kind:?}: {ranges:?}, expected center {expected}"
            );
        }
    }

    #[test]
    fn loss_and_components_change_effective_scale_for_live_and_saved_histories() {
        for saved in [false, true] {
            for kind in [Chart::Loss, Chart::Components] {
                let (mut component, _) = dashboard(saved);
                let mut state = PlotState::default();
                redraw(&mut component, kind, &mut state);
                let x = if saved { 12.0 } else { 2.0 };
                assert_center(&component, kind, [x, 50.5]);
                pan(&mut component, kind, &mut state);
                component.update(Message::Log(true));
                redraw(&mut component, kind, &mut PlotState::default());
                assert_center(&component, kind, [x, 1.0]);
                component.update(Message::Log(false));
                redraw(&mut component, kind, &mut PlotState::default());
                assert_center(&component, kind, [x, 50.5]);
            }
        }
    }

    #[test]
    fn epoch_data_and_explicit_history_changes_still_autoscale_retained_charts() {
        let (mut component, mut model) = dashboard(true);
        let kind = Chart::Ap50;
        let mut state = PlotState::default();
        redraw(&mut component, kind, &mut state);
        assert_center(&component, kind, [12.0, 0.5]);
        pan(&mut component, kind, &mut state);
        component.update(Message::Epoch(false));
        let mut state = PlotState::default();
        redraw(&mut component, kind, &mut state);
        assert_center(&component, kind, [120.0, 0.5]);
        pan(&mut component, kind, &mut state);
        // Current live run is an explicit source transition with its own bounds.
        model.workflow.output.live();
        component.rebase(&model, true);
        let mut state = PlotState::default();
        redraw(&mut component, kind, &mut state);
        assert_center(&component, kind, [20.0, 0.5]);
        pan(&mut component, kind, &mut state);
        let sample = model
            .workflow
            .training
            .as_mut()
            .unwrap()
            .metrics
            .as_mut()
            .unwrap();
        sample.sequence += 1;
        sample.progress.epoch = 9;
        sample.progress.globaloptimizerstep = 80;
        sample.progress.val.as_mut().unwrap().bbox.ap50 = 0.95;
        component.rebase(&model, false);
        component.rebase(&model, true);
        redraw(&mut component, kind, &mut PlotState::default());
        assert_center(&component, kind, [45.0, 0.575]);
    }

    fn select_saved_run(model: &mut crate::view_model::ApplicationModel) {
        let configuration = model
            .settings_snapshot
            .as_ref()
            .unwrap()
            .settingsstate
            .workflows
            .train
            .request
            .clone();
        model.workflow.output.select_saved("saved-output".into());
        model.workflow.output.saved_mut().unwrap().run = Some(crate::view_model::test_support::saved_training_run(configuration));
    }
    #[test]
    fn hidden_navigation_saved_selection_and_preparation_preserve_independent_live_history() {
        let mut component = Component::default();
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut sample = record();
        model.workflow.training.as_mut().unwrap().metrics = Some(sample.clone());
        component.rebase(&model, false);
        assert!(
            component
                .charts
                .iter()
                .all(|c| c.shapes.iter().all(Option::is_none))
        );
        component.rebase(&model, true);
        let shape = component.charts[0].shapes[0].unwrap();
        component.charts[0]
            .plot
            .update_series(&shape, |series| {
                assert_eq!(series.positions, vec![[1.1, 1.0]]);
            })
            .unwrap();

        select_saved_run(&mut model);
        let mut saved = record();
        saved.runid = "saved".into();
        saved.attemptid = "saved-attempt".into();
        saved.progress.epoch = 9;
        saved.progress.scalars.total = Some(77.0);
        model.workflow.output.saved_mut().unwrap().page = Some(TrainingHistoryPage {
            generation: 3,
            nextcursor: 100,
            more: false,
            records: vec![saved],
        });
        component.rebase(&model, true);
        assert!(component.saved_selected);
        assert_eq!(component.history().curves[0].buckets[0].first.value, 77.0);
        assert_eq!(component.charts[0].shapes[0], Some(shape));
        component.charts[0]
            .plot
            .update_series(&shape, |series| {
                assert_eq!(series.positions, vec![[9.1, 77.0]]);
            })
            .unwrap();

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
        component.charts[0]
            .plot
            .update_series(&shape, |series| {
                assert_eq!(series.positions, vec![[9.1, 77.0]]);
            })
            .unwrap();

        // This is the same explicit selection clearing used by Current live run.
        model.workflow.output.live();
        component.rebase(&model, true);
        assert!(!component.saved_selected);
        assert_eq!(component.charts[0].shapes[0], Some(shape));
        component.charts[0]
            .plot
            .update_series(&shape, |series| {
                assert_eq!(series.positions, vec![[1.1, 1.0], [1.2, 2.0]]);
            })
            .unwrap();

        // Old metric storage can still be present during new-run inspection.
        let snapshot = model.workflow.training.as_mut().unwrap();
        snapshot.local.active = true;
        snapshot.local.progress.sequence = 0;
        component.rebase(&model, true);
        assert!(component.live.sequence.is_none());
        assert_eq!(component.saved.curves[0].buckets[0].first.value, 77.0);
        assert_eq!(component.charts[0].shapes[0], Some(shape));
        component.charts[0]
            .plot
            .update_series(&shape, |series| {
                assert!(series.positions.is_empty());
            })
            .unwrap();
    }
}
