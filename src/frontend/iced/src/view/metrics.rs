//! Bounded visual summaries; native records remain authoritative history.
mod catalog;
mod chart;
mod history;
mod sources;
use crate::fluent_theme::Element;
pub use catalog::Chart;
use catalog::Metric;
use chart::RetainedChart;
use iced::widget::{button, checkbox, column, container, row, text};

#[derive(Debug, Clone)]
pub enum Message {
    Visible(Chart, bool),
    Source(crate::generated::TrainingMetricSource),
    SelectedOutput(bool),
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
    live: sources::SourceHistories,
    saved: sources::SourceHistories,
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
            live: sources::SourceHistories::new(&metrics),
            saved: sources::SourceHistories::new(&metrics),
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
    pub(crate) fn retained_source_facts(
        &self,
    ) -> Option<(&crate::generated::TrainingMetricSource, u64, u64, u64)> {
        let history = self.history();
        let observation = history
            .curves
            .iter()
            .zip(&self.metrics)
            .filter(|(_, metric)| matches!(metric.source, catalog::Source::Evaluation { .. }))
            .filter_map(|(curve, _)| curve.buckets.back().map(|bucket| bucket.last.order))
            .max()?;
        Some((
            history.source.as_ref()?,
            self.sources().sequence?,
            self.sources().dropped,
            observation,
        ))
    }
    pub(crate) fn retained_run_facts(&self) -> (bool, u64, Option<u64>) {
        (
            self.saved_selected,
            self.sources().generation,
            self.sources().page,
        )
    }
    fn invalidate(&mut self) {
        for chart in &mut self.charts {
            chart.dirty = true;
        }
    }
    fn sources(&self) -> &sources::SourceHistories {
        if self.saved_selected {
            &self.saved
        } else {
            &self.live
        }
    }
    fn sources_mut(&mut self) -> &mut sources::SourceHistories {
        if self.saved_selected {
            &mut self.saved
        } else {
            &mut self.live
        }
    }
    fn history(&self) -> &history::History {
        self.sources().history()
    }
    #[cfg(test)]
    fn clear(&mut self) {
        self.live.clear(&self.metrics);
        self.saved.clear(&self.metrics);
        self.invalidate();
    }
    pub fn reset(&mut self, visible: bool) {
        // Transport reconnection retires no run. Bootstrap/rebase decides whether
        // the authoritative run or selected directory actually changed.
        if visible {
            self.prepare();
        }
    }
    pub fn rebase(&mut self, model: &crate::view_model::ApplicationModel, visible: bool) {
        let live_changed = model
            .workflow
            .training
            .as_ref()
            .map_or(0, |training| self.live.rebase_live(training, &self.metrics));
        let selected = model.workflow.output.saved().is_some();
        let saved_changed = model.workflow.output.saved().map_or(0, |saved| {
            self.saved.rebase_saved(
                &saved.directory,
                saved.run.as_ref(),
                saved.page.as_ref(),
                &self.metrics,
            )
        });
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
            Message::Source(source) => {
                if self.sources_mut().select(&source) {
                    self.invalidate();
                }
            }
            Message::SelectedOutput(value) => self.sources_mut().show_selected_output(value),
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
        if history.selected_output() {
            return;
        }
        for chart in &mut self.charts {
            if self
                .expanded
                .map_or(chart.visible, |kind| kind == chart.kind)
            {
                chart.prepare(
                    &history.history().curves,
                    &self.metrics,
                    self.epoch,
                    self.log,
                );
            }
        }
    }
    pub fn controls_height(&self) -> f32 {
        (if self.selector { 274.0 } else { 84.0 })
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
        let histories = self.sources();
        let active = histories.selected_source();
        let mut source_controls = row![].spacing(6);
        for source in histories.sources() {
            let label =
                if source.scope == crate::generated::TrainingRecordScope::SynchronizedSession {
                    "Synchronized · Ordinary".into()
                } else {
                    format!("Model {} · {:?}", source.modelid, source.weights)
                };
            source_controls = source_controls.push(
                container(
                    button(text(label))
                        .on_press(Message::Source(source.clone()))
                        .style(if !histories.selected_output() && active == Some(source) {
                            crate::fluent_theme::button_selected
                        } else {
                            crate::fluent_theme::button_secondary
                        }),
                )
                .id(format!(
                    "train.metrics.source.{:?}.{}.{:?}",
                    source.scope, source.modelid, source.weights
                )),
            );
        }
        let selection = histories.selection();
        if selection.is_some() {
            source_controls = source_controls
                .push(button("Selected output").on_press(Message::SelectedOutput(true)));
        }
        let mut content = column![
            controls,
            iced::widget::scrollable(source_controls).direction(
                iced::widget::scrollable::Direction::Horizontal(
                    iced::widget::scrollable::Scrollbar::default()
                )
            )
        ]
        .spacing(6);
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
        let workspace: Element<'_, Message> =
            if histories.selected_output() && selection.is_some() {
                let selection = selection.unwrap();
                let mut facts = column![
                    text(format!(
                        "Selected output · {:?} · {:?}",
                        selection.method, selection.artifact.weights
                    )),
                    text(selection.artifact.path.clone()),
                    text(format!(
                        "Validation box AP: {:.4}",
                        selection.validation.bbox.ap
                    ))
                ]
                .spacing(6);
                if let Some(mask) = &selection.validation.mask {
                    facts = facts.push(text(format!("Validation mask AP: {:.4}", mask.ap)));
                }
                for ingredient in &selection.ingredients {
                    facts = facts.push(text(format!(
                        "Model {} · coefficient {:.4} · {}",
                        ingredient.modelid, ingredient.coefficient, ingredient.sha256
                    )));
                }
                container(facts).center(iced::Fill).into()
            } else if selected.is_empty() {
                container(text("No charts selected"))
                    .center(iced::Fill)
                    .into()
            } else if histories.sequence.is_none() {
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
                        let header = container(
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
    pub(super) fn source_catalog() -> TrainingSourceCatalog {
        let source = TrainingMetricSource {
            scope: TrainingRecordScope::Model,
            modelid: 0,
            weights: EvaluatedWeights::Ordinary,
        };
        TrainingSourceCatalog {
            defaultsource: Some(source.clone()),
            available: vec![source],
        }
    }
    fn metric_model() -> crate::view_model::ApplicationModel {
        let mut model = crate::view_model::test_support::bootstrapped();
        model.workflow.training.as_mut().unwrap().sources.catalog = source_catalog();
        model
    }
    pub(super) fn histories(metrics: &[Metric]) -> sources::SourceHistories {
        let mut histories = sources::SourceHistories::new(metrics);
        histories.run = "run".into();
        histories.reconcile(&source_catalog(), metrics);
        histories
    }
    pub(crate) fn record() -> TrainingRecord {
        use crate::generated::*;
        TrainingRecord {
            formatversion: 3,
            runid: "run".into(),
            attemptid: "attempt".into(),
            sequence: 1,
            droppedbefore: 0,
            role: TrainingRecordRole::Live,
            evaluatedweights: EvaluatedWeights::Ordinary,
            attemptconfiguration: None,
            progress: TrainingMetricProgress {
                phase: TrainingPhase::Train,
                scope: TrainingRecordScope::Model,
                sessionid: "session".into(),
                modelid: 0,
                round: 0,
                merge: 0,
                mergeboundary: None,
                artifact: Some(TrainingArtifact {
                    sessionid: "session".into(),
                    modelid: 0,
                    initialization: "init".into(),
                    configuration: "configuration".into(),
                    content: "content".into(),
                    sha256: "digest".into(),
                    path: "scheduled.pt".into(),
                    epoch: 0,
                    attempt: 0,
                    merge: 0,
                    weights: EvaluatedWeights::Ordinary,
                    selectionmetric: Some(0.42),
                    evaluation: Some(evaluation()),
                    validation: "validation".into(),
                }),
                failure: None,
                distribution: None,
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
        history: &'a sources::SourceHistories,
        metrics: &[Metric],
        chart: Chart,
    ) -> &'a history::Curve {
        &history.history().curves[metrics.iter().position(|m| m.chart == chart).unwrap()]
    }
    #[test]
    fn coalesced_live_projection_keeps_current_progress_and_original_ema_observation() {
        let mut model = metric_model();
        let mut observation = record();
        observation.sequence = 3;
        observation.role = TrainingRecordRole::Epoch;
        observation.evaluatedweights = EvaluatedWeights::Ema;
        observation.progress.phase = TrainingPhase::EpochComplete;
        observation.progress.modelid = 4;
        observation.progress.artifact.as_mut().unwrap().modelid = 4;
        observation.progress.artifact.as_mut().unwrap().weights = EvaluatedWeights::Ema;
        observation.progress.val = Some(evaluation());
        let mut current = record();
        current.sequence = 10;
        current.role = TrainingRecordRole::Epoch;
        current.progress.scope = TrainingRecordScope::Session;
        current.progress.artifact = None;
        current.progress.val = None;
        current.progress.scalars.total = None;
        current.progress.fullcheckpointpath = "session.json".into();
        let training = model.workflow.training.as_mut().unwrap();
        training.metrics = Some(current.clone());
        training.sources.catalog.defaultsource = Some(crate::generated::TrainingMetricSource {
            scope: observation.progress.scope,
            modelid: observation.progress.modelid,
            weights: observation.evaluatedweights,
        });
        training.sources.catalog.available =
            vec![training.sources.catalog.defaultsource.clone().unwrap()];
        training.sources.observations = vec![observation];
        let mut component = Component::default();
        component.rebase(&model, false);
        component.rebase(&model, false);
        assert_eq!(component.live.sequence, Some(10));
        assert_eq!(
            curve(&component.live, &component.metrics, Chart::Ap50)
                .buckets
                .len(),
            1
        );
        assert_eq!(
            curve(&component.live, &component.metrics, Chart::Ap50).buckets[0]
                .first
                .order,
            3
        );
        assert!(
            curve(&component.live, &component.metrics, Chart::Loss)
                .buckets
                .is_empty()
        );
        assert_eq!(
            model.workflow.training.as_ref().unwrap().metrics.as_ref(),
            Some(&current)
        );
    }
    #[test]
    fn late_native_observation_reconciles_provisional_live_source_once() {
        for synchronized in [false, true] {
            let mut model = metric_model();
            model
                .settings_snapshot
                .as_mut()
                .unwrap()
                .settingsstate
                .workflows
                .train
                .request
                .useema = synchronized;
            let mut current = record();
            current.sequence = 10;
            current.droppedbefore = 2;
            current.progress.artifact = None;
            model.workflow.training.as_mut().unwrap().metrics = Some(current.clone());
            let mut component = Component::default();
            component.rebase(&model, false);
            assert_eq!(
                curve(&component.live, &component.metrics, Chart::Loss)
                    .buckets
                    .len(),
                1
            );

            let mut observation = record();
            observation.sequence = 11;
            observation.droppedbefore = 2;
            observation.role = TrainingRecordRole::Epoch;
            observation.progress.phase = TrainingPhase::EpochComplete;
            observation.progress.scope = if synchronized {
                TrainingRecordScope::SynchronizedSession
            } else {
                TrainingRecordScope::Model
            };
            observation.progress.modelid = if synchronized { 0 } else { 4 };
            observation.evaluatedweights = if synchronized {
                EvaluatedWeights::Ordinary
            } else {
                EvaluatedWeights::Ema
            };
            let artifact = observation.progress.artifact.as_mut().unwrap();
            artifact.modelid = observation.progress.modelid;
            artifact.weights = observation.evaluatedweights;
            observation.progress.val = Some(evaluation());
            current.sequence = 12;
            current.droppedbefore = 3;
            current.progress.elapsedseconds = 3.0;
            current.progress.modelid = artifact.modelid;
            current.progress.scalars.total = Some(7.0);
            let training = model.workflow.training.as_mut().unwrap();
            training.metrics = Some(current.clone());
            training.sources.catalog.defaultsource = Some(crate::generated::TrainingMetricSource {
                scope: observation.progress.scope,
                modelid: observation.progress.modelid,
                weights: observation.evaluatedweights,
            });
            training.sources.catalog.available =
                vec![training.sources.catalog.defaultsource.clone().unwrap()];
            training.sources.observations = vec![observation];
            component.rebase(&model, false);
            component.rebase(&model, false);
            assert_eq!(component.live.sequence, Some(12));
            assert_eq!(component.live.dropped, 3);
            let ap = curve(&component.live, &component.metrics, Chart::Ap50);
            assert_eq!(ap.buckets.len(), 1);
            assert_eq!(ap.buckets[0].first.order, 11);
            let loss = curve(&component.live, &component.metrics, Chart::Loss);
            // Weight-kind correction leaves the same model's truthful losses;
            // a model correction discards the provisional model's points.
            assert_eq!(loss.buckets.len(), if synchronized { 0 } else { 1 });
            if !synchronized {
                assert_eq!(loss.buckets.back().unwrap().last.value, 7.0);
            }
            assert_eq!(
                model.workflow.training.as_ref().unwrap().metrics.as_ref(),
                Some(&current)
            );

            // Mutable settings and plot-storage reset cannot supersede the
            // native source already established for this attempt.
            component.clear();
            component.rebase(&model, false);
            assert_eq!(
                curve(&component.live, &component.metrics, Chart::Ap50)
                    .buckets
                    .len(),
                1
            );
            assert_eq!(component.live.sequence, Some(12));

            let training = model.workflow.training.as_mut().unwrap();
            let observation = training.sources.observations.first_mut().unwrap();
            observation.attemptid = "next-attempt".into();
            observation.sequence = 0;
            observation.progress.scope = TrainingRecordScope::Model;
            observation.progress.modelid = 5;
            observation.progress.artifact.as_mut().unwrap().modelid = 5;
            training.sources.catalog.defaultsource = Some(crate::generated::TrainingMetricSource {
                scope: observation.progress.scope,
                modelid: observation.progress.modelid,
                weights: observation.evaluatedweights,
            });
            training.sources.catalog.available =
                vec![training.sources.catalog.defaultsource.clone().unwrap()];
            let current = training.metrics.as_mut().unwrap();
            current.attemptid = "next-attempt".into();
            current.sequence = 1;
            current.droppedbefore = 0;
            current.progress.modelid = 5;
            component.rebase(&model, false);
            assert_eq!(component.live.sequence, Some(1));
            assert_eq!(component.live.dropped, 0);
            assert_eq!(
                curve(&component.live, &component.metrics, Chart::Loss)
                    .buckets
                    .len(),
                1
            );
            assert_eq!(
                curve(&component.live, &component.metrics, Chart::Ap50)
                    .buckets
                    .len(),
                1
            );
        }
    }
    #[test]
    fn saved_page_replay_selects_ema_without_session_gaps() {
        let mut model = metric_model();
        select_saved_run(&mut model);
        let run = model
            .workflow
            .output
            .saved_mut()
            .unwrap()
            .run
            .as_mut()
            .unwrap()
            .run
            .as_mut()
            .unwrap();
        run.configuration.useema = true;
        run.sources.defaultsource.as_mut().unwrap().weights = EvaluatedWeights::Ema;
        run.sources.available[0].weights = EvaluatedWeights::Ema;
        let mut ordinary = record();
        ordinary.runid = "saved".into();
        ordinary.sequence = 0;
        ordinary.role = TrainingRecordRole::Epoch;
        ordinary.progress.scope = TrainingRecordScope::SynchronizedSession;
        ordinary.progress.val = Some(evaluation());
        let mut ema = ordinary.clone();
        ema.sequence = 1;
        ema.progress.scope = TrainingRecordScope::Model;
        ema.evaluatedweights = EvaluatedWeights::Ema;
        ema.progress.artifact.as_mut().unwrap().weights = EvaluatedWeights::Ema;
        ema.progress.val.as_mut().unwrap().bbox.ap50 = 0.9;
        let mut session = ema.clone();
        session.sequence = 2;
        session.progress.scope = TrainingRecordScope::Session;
        session.progress.artifact = None;
        session.progress.val = None;
        session.progress.scalars.total = None;
        model.workflow.output.saved_mut().unwrap().page = Some(TrainingHistoryPage {
            generation: 3,
            nextcursor: 100,
            more: false,
            records: vec![ordinary, ema, session],
        });
        let mut component = Component::default();
        component.rebase(&model, false);
        component.rebase(&model, false);
        let ap = curve(&component.saved, &component.metrics, Chart::Ap50);
        assert_eq!(ap.buckets.len(), 1);
        assert_eq!(ap.buckets[0].first.value, 0.9);
        let loss = curve(&component.saved, &component.metrics, Chart::Loss);
        assert_eq!(loss.buckets.len(), 1);
        assert!(!loss.missing);
        assert_eq!(component.saved.sequence, Some(2));
    }
    #[test]
    fn sparse_validation_uses_completed_epochs_without_live_or_terminal_duplicates() {
        let metrics = catalog::catalog();
        let mut history = histories(&metrics);
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
        let mut history = histories(&metrics);
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
        let mut history = histories(&metrics);
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
            .zip(&history.history().curves)
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
        component.live = histories(&component.metrics);
        component.live.ingest(&sample, true, &component.metrics);
        component.prepare();
        let id = component.charts[0].shapes[0];
        assert!(id.is_some());
        assert_eq!(
            component.live.history().curves[0].buckets[0].first.epoch,
            1.1
        );
        sample.sequence += 1;
        sample.progress.elapsedseconds += 0.2;
        assert_eq!(component.live.ingest(&sample, true, &component.metrics), 0);
        sample.sequence += 1;
        sample.progress.phase = TrainingPhase::Validate;
        assert_eq!(component.live.ingest(&sample, true, &component.metrics), 0);
        component.update(Message::Expand(Some(Chart::Loss)));
        component.update(Message::Expand(None));
        assert_eq!(component.charts[0].shapes[0], id);
        for kind in Chart::ALL {
            component.update(Message::Visible(kind, false));
        }
        assert!(component.charts.iter().all(|c| !c.visible));
        component.update(Message::Reset);
        assert_eq!(component.charts.iter().filter(|c| c.visible).count(), 6);
        component.clear();
        component.prepare();
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
        let mut model = metric_model();
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
        // CLEANUP-IGNORE: One selected-request field access supplies this chart fixture, independent of the output-view fixture.
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
        model.workflow.output.saved_mut().unwrap().run = Some(
            crate::view_model::test_support::saved_training_run(configuration),
        );
    }
    fn publish(component: &mut Component, model: &mut crate::view_model::ApplicationModel) {
        component.rebase(model, false);
        component.publish_conditions(&mut model.notices);
    }

    fn install_history_bootstrap(
        model: &mut crate::view_model::ApplicationModel,
        sample: &TrainingRecord,
    ) {
        let mut snapshots: Vec<_> = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .map(|fact| fact.value)
            .collect();
        for snapshot in &mut snapshots {
            if let crate::generated::ApplicationSnapshot::Training(state) = snapshot {
                state.metrics = Some(sample.clone());
                state.sources.catalog = source_catalog();
            }
        }
        model
            .install_bootstrap(crate::generated::SCHEMA_FINGERPRINT, snapshots)
            .unwrap();
    }

    #[test]
    fn history_conditions_preserve_bootstrap_and_independent_acknowledgements_across_cache_reset() {
        use crate::view_model::notices::Origin;
        let mut component = Component::default();
        let mut model = crate::view_model::ApplicationModel::default();
        let mut sample = record();
        sample.droppedbefore = 4;
        install_history_bootstrap(&mut model, &sample);
        publish(&mut component, &mut model);
        assert!(
            model.notices.is_empty(),
            "first bootstrap seeds an active drop episode"
        );
        sample.sequence += 1;
        sample.droppedbefore = 0;
        model.workflow.training.as_mut().unwrap().metrics = Some(sample.clone());
        publish(&mut component, &mut model);
        sample.sequence += 1;
        sample.droppedbefore = 7;
        model.workflow.training.as_mut().unwrap().metrics = Some(sample.clone());
        publish(&mut component, &mut model);
        let live_id = model.notices.latest().unwrap().id;
        assert_eq!(
            model.notices.latest().unwrap().origin,
            Origin::HistoryDropped
        );
        model.notices.dismiss(live_id);

        select_saved_run(&mut model);
        let mut saved = sample.clone();
        saved.runid = "saved".into();
        saved.droppedbefore = 11;
        model.workflow.output.saved_mut().unwrap().page = Some(TrainingHistoryPage {
            generation: 3,
            nextcursor: 100,
            more: false,
            records: vec![saved.clone()],
        });
        publish(&mut component, &mut model);
        let saved_id = model.notices.latest().unwrap().id;
        assert_eq!(
            model.notices.latest().unwrap().origin,
            Origin::HistoryDroppedSaved
        );
        model.notices.dismiss(saved_id);
        component.reset(false);
        component.publish_conditions(&mut model.notices);
        publish(&mut component, &mut model);
        assert!(
            model.notices.is_empty(),
            "visual clear and restored same histories cannot recover native drops"
        );

        model.peer_disconnected(crate::view_model::UiError::transport("offline"));
        model.notices.dismiss_all();
        component.reset(false);
        install_history_bootstrap(&mut model, &sample);
        publish(&mut component, &mut model);
        assert!(
            model.notices.is_empty(),
            "reconnect keeps the live acknowledgement"
        );
        select_saved_run(&mut model);
        model.workflow.output.saved_mut().unwrap().page = Some(TrainingHistoryPage {
            generation: 3,
            nextcursor: 100,
            more: false,
            records: vec![saved],
        });
        publish(&mut component, &mut model);
        assert!(
            model.notices.is_empty(),
            "reselection keeps the independent saved acknowledgement"
        );

        sample.runid = "new native run".into();
        model.workflow.training.as_mut().unwrap().metrics = Some(sample);
        publish(&mut component, &mut model);
        assert_eq!(model.notices.len(), 1);
        assert_eq!(
            model.notices.latest().unwrap().origin,
            Origin::HistoryDropped
        );
        assert_ne!(model.notices.latest().unwrap().id, live_id);
    }

    #[test]
    fn reconnect_observes_same_run_drops_and_seeds_replacement_run_history() {
        use crate::view_model::notices::Origin;
        let mut component = Component::default();
        let mut model = crate::view_model::ApplicationModel::default();
        let mut sample = record();
        install_history_bootstrap(&mut model, &sample);
        publish(&mut component, &mut model);
        component.reset(false);
        sample.sequence += 1;
        sample.droppedbefore = 5;
        install_history_bootstrap(&mut model, &sample);
        publish(&mut component, &mut model);
        assert_eq!(
            model.notices.len(),
            1,
            "same-run drops completed while offline are new"
        );
        assert_eq!(
            model.notices.latest().unwrap().origin,
            Origin::HistoryDropped
        );
        model.notices.dismiss_all();
        component.reset(false);
        sample.runid = "replacement retained run".into();
        sample.sequence = 1;
        sample.droppedbefore = 2;
        install_history_bootstrap(&mut model, &sample);
        publish(&mut component, &mut model);
        assert!(
            model.notices.is_empty(),
            "replacement bootstrap seeds its retained condition"
        );
        for dropped in [0, 4] {
            sample.sequence += 1;
            sample.droppedbefore = dropped;
            model.workflow.training.as_mut().unwrap().metrics = Some(sample.clone());
            publish(&mut component, &mut model);
        }
        assert_eq!(
            model.notices.len(),
            1,
            "a real recovery rearms the replacement run"
        );
        assert!(
            model
                .notices
                .latest()
                .unwrap()
                .detail
                .starts_with("4 training records")
        );
    }

    #[test]
    fn metrics_publish_only_changed_conditions_and_preserve_retained_notice_identity() {
        use crate::view_model::notices::Origin;
        let mut component = Component::default();
        let mut model = metric_model();
        let mut sample = record();
        sample.droppedbefore = 2;
        model.workflow.training.as_mut().unwrap().metrics = Some(sample.clone());
        publish(&mut component, &mut model);
        let id = model.notices.latest().unwrap().id;
        let stable = model.notices.presentation().clone();
        for _ in 0..3 {
            publish(&mut component, &mut model);
        }
        sample.sequence += 1;
        sample.progress.elapsedseconds += 0.1; // A coalesced live record still observes native drops.
        model.workflow.training.as_mut().unwrap().metrics = Some(sample.clone());
        publish(&mut component, &mut model);
        assert_eq!(model.notices.presentation(), &stable);
        sample.sequence += 1;
        sample.droppedbefore = 3;
        model.workflow.training.as_mut().unwrap().metrics = Some(sample.clone());
        publish(&mut component, &mut model);
        assert_eq!(model.notices.latest().unwrap().id, id);
        assert_eq!(model.notices.latest().unwrap().content_version, 2);
        assert_ne!(model.notices.presentation(), &stable);

        model.notices.dismiss(id);
        for index in 0..=history::BUCKETS {
            sample.sequence += 1;
            sample.attemptid = format!("attempt {index}");
            component.live.ingest(&sample, false, &component.metrics);
        }
        component.publish_conditions(&mut model.notices);
        let chart = model.notices.latest().unwrap();
        assert_eq!(chart.origin, Origin::Chart);
        assert!(chart.detail.contains("older disconnected summaries"));
        let chart_id = chart.id;
        let unchanged = model.notices.presentation().clone();
        component.publish_conditions(&mut model.notices);
        assert_eq!(model.notices.presentation(), &unchanged);
        model.notices.dismiss(chart_id);
        component.reset(false);
        component.publish_conditions(&mut model.notices);
        assert!(model.notices.is_empty());
        model.workflow.training.as_mut().unwrap().metrics = Some(sample);
        publish(&mut component, &mut model);
        assert!(
            model.notices.is_empty(),
            "cache reset did not rearm the dismissed drop"
        );
    }

    #[test]
    fn hidden_navigation_saved_selection_and_preparation_preserve_independent_live_history() {
        let mut component = Component::default();
        let mut model = metric_model();
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
        assert_eq!(component.live.history().curves[0].buckets.len(), 2);
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
        assert_eq!(
            component.saved.history().curves[0].buckets[0].first.value,
            77.0
        );
        assert_eq!(component.charts[0].shapes[0], Some(shape));
        component.charts[0]
            .plot
            .update_series(&shape, |series| {
                assert!(series.positions.is_empty());
            })
            .unwrap();
    }
    #[test]
    fn every_admitted_source_retains_original_observations_across_switches_and_late_snapshots() {
        let mut model = metric_model();
        let training = model.workflow.training.as_mut().unwrap();
        training.sources.catalog.available.clear();
        let mut current = record();
        current.sequence = 100;
        current.droppedbefore = 9;
        current.progress.scope = TrainingRecordScope::Session;
        current.progress.val = None;
        current.progress.artifact = None;
        training.metrics = Some(current.clone());
        for modelid in 0..=TRAINING_MODEL_CAPACITY as u64 {
            let source = TrainingMetricSource {
                scope: if modelid == 0 {
                    TrainingRecordScope::SynchronizedSession
                } else {
                    TrainingRecordScope::Model
                },
                modelid,
                weights: if modelid == 0 {
                    EvaluatedWeights::Ordinary
                } else {
                    EvaluatedWeights::Ema
                },
            };
            let mut observation = record();
            observation.sequence = modelid + 1;
            observation.role = TrainingRecordRole::Epoch;
            observation.progress.phase = TrainingPhase::EpochComplete;
            observation.progress.scope = source.scope;
            observation.progress.modelid = modelid;
            observation.evaluatedweights = source.weights;
            observation.progress.val = Some(evaluation());
            let artifact = observation.progress.artifact.as_mut().unwrap();
            artifact.modelid = modelid;
            artifact.weights = source.weights;
            artifact.epoch = observation.progress.epoch as u64;
            training.sources.catalog.available.push(source);
            training.sources.observations.push(observation);
        }
        training.sources.catalog.defaultsource =
            Some(training.sources.catalog.available[1].clone());
        let catalog = training.sources.catalog.clone();
        let mut component = Component::default();
        component.rebase(&model, true);
        assert_eq!(component.live.sources().len(), TRAINING_MODEL_CAPACITY + 1);
        for source in catalog.available.iter().rev() {
            component.update(Message::Source(source.clone()));
            component.rebase(&model, true);
            let (actual, sequence, dropped, observation) =
                component.retained_source_facts().unwrap();
            assert_eq!(actual, source);
            assert_eq!(
                (sequence, dropped, observation),
                (100, 9, source.modelid + 1)
            );
            assert_eq!(
                curve(&component.live, &component.metrics, Chart::Ap)
                    .buckets
                    .len(),
                1
            );
            assert!(
                curve(&component.live, &component.metrics, Chart::Loss)
                    .buckets
                    .is_empty()
            );
            component.update(Message::SelectedOutput(true));
            component.update(Message::Source(source.clone()));
            assert!(!component.sources().selected_output());
            component.rebase(&model, true);
            assert_eq!(component.live.selected_source(), Some(source));
        }
        assert_eq!(
            model.workflow.training.as_ref().unwrap().metrics.as_ref(),
            Some(&current)
        );
        assert_eq!(component.live.sources().len(), catalog.available.len());
    }

    #[test]
    fn periodic_ordinary_epoch_records_replay_separate_losses_without_model_evaluations() {
        let catalog = TrainingSourceCatalog {
            defaultsource: Some(TrainingMetricSource {
                scope: TrainingRecordScope::SynchronizedSession,
                modelid: 0,
                weights: EvaluatedWeights::Ordinary,
            }),
            available: (0..=TRAINING_MODEL_CAPACITY as u64)
                .map(|modelid| TrainingMetricSource {
                    scope: if modelid == 0 {
                        TrainingRecordScope::SynchronizedSession
                    } else {
                        TrainingRecordScope::Model
                    },
                    modelid,
                    weights: EvaluatedWeights::Ordinary,
                })
                .collect(),
        };
        let mut records = Vec::new();
        for modelid in 1..=TRAINING_MODEL_CAPACITY as u64 {
            let mut sample = record();
            sample.runid = "saved".into();
            sample.attemptid = "saved-attempt".into();
            sample.sequence = modelid;
            sample.droppedbefore = 7;
            sample.role = TrainingRecordRole::Epoch;
            sample.progress.phase = TrainingPhase::EpochComplete;
            sample.progress.modelid = modelid;
            sample.progress.scalars.total = Some(modelid as f64);
            sample.progress.val = None;
            sample.progress.artifact = None;
            records.push(sample);
        }
        let mut evaluated = records[0].clone();
        evaluated.sequence = TRAINING_MODEL_CAPACITY as u64 + 1;
        evaluated.progress.scope = TrainingRecordScope::SynchronizedSession;
        evaluated.progress.modelid = 0;
        evaluated.progress.val = Some(evaluation());
        evaluated.progress.artifact = record().progress.artifact;
        records.push(evaluated);
        let mut terminal = records[0].clone();
        terminal.sequence = TRAINING_MODEL_CAPACITY as u64 + 2;
        terminal.role = TrainingRecordRole::Terminal;
        terminal.progress.phase = TrainingPhase::Completed;
        terminal.progress.scope = TrainingRecordScope::Session;
        records.push(terminal);
        for saved in [false, true] {
            let mut model = metric_model();
            let mut component = Component::default();
            if saved {
                select_saved_run(&mut model);
                let selected = model.workflow.output.saved_mut().unwrap();
                selected.run.as_mut().unwrap().run.as_mut().unwrap().sources = catalog.clone();
                selected.page = Some(TrainingHistoryPage {
                    generation: 3,
                    nextcursor: 100,
                    more: false,
                    records: records.clone(),
                });
                component.rebase(&model, false);
            } else {
                model.workflow.training.as_mut().unwrap().sources.catalog = catalog.clone();
                for sample in &records {
                    model.workflow.training.as_mut().unwrap().metrics = Some(sample.clone());
                    component.rebase(&model, false);
                }
            }
            for source in catalog.available.iter().rev() {
                component.update(Message::Source(source.clone()));
                component.rebase(&model, false);
                let history = component.sources();
                assert_eq!(history.sequence, Some(TRAINING_MODEL_CAPACITY as u64 + 2));
                assert_eq!(history.dropped, 7);
                let loss = curve(history, &component.metrics, Chart::Loss);
                let ap = curve(history, &component.metrics, Chart::Ap);
                if source.modelid == 0 {
                    assert!(loss.buckets.is_empty());
                    assert_eq!(ap.buckets.len(), 1);
                } else {
                    assert_eq!(loss.buckets.len(), 1);
                    assert_eq!(loss.buckets[0].first.value, source.modelid as f64);
                    assert_eq!(loss.buckets[0].first.order, source.modelid);
                    assert!(ap.buckets.is_empty());
                }
            }
        }
    }
    pub(super) fn all_sources() -> TrainingSourceCatalog {
        let available: Vec<_> = (0..=TRAINING_MODEL_CAPACITY as u64)
            .map(|modelid| TrainingMetricSource {
                scope: if modelid == 0 {
                    TrainingRecordScope::SynchronizedSession
                } else {
                    TrainingRecordScope::Model
                },
                modelid,
                weights: if modelid == 0 {
                    EvaluatedWeights::Ordinary
                } else {
                    EvaluatedWeights::Ema
                },
            })
            .collect();
        TrainingSourceCatalog {
            defaultsource: Some(available[3].clone()),
            available,
        }
    }
    fn source_page(catalog: &TrainingSourceCatalog, epoch: u64) -> TrainingHistoryPage {
        let count = catalog.available.len() as u64;
        let records = catalog
            .available
            .iter()
            .enumerate()
            .map(|(index, source)| {
                let mut sample = record();
                sample.runid = "saved".into();
                sample.attemptid = "saved-attempt".into();
                sample.sequence = epoch * count + index as u64 + 1;
                sample.droppedbefore = 7;
                sample.role = TrainingRecordRole::Epoch;
                sample.evaluatedweights = source.weights;
                sample.progress.phase = TrainingPhase::EpochComplete;
                sample.progress.epoch = epoch as i32;
                sample.progress.globaloptimizerstep = epoch as i64 * 10;
                sample.progress.scope = source.scope;
                sample.progress.modelid = source.modelid;
                sample.progress.scalars.total = Some(source.modelid as f64 + epoch as f64);
                sample.progress.val = Some(evaluation());
                sample.progress.val.as_mut().unwrap().bbox.ap =
                    0.1 + index as f64 * 0.02 + epoch as f64 * 0.01;
                let artifact = sample.progress.artifact.as_mut().unwrap();
                artifact.epoch = epoch;
                artifact.modelid = source.modelid;
                artifact.weights = source.weights;
                artifact.path = format!("source-{}-epoch-{epoch}.pt", source.modelid);
                sample
            })
            .collect();
        TrainingHistoryPage {
            generation: 3,
            nextcursor: (epoch + 1) * 1000,
            more: epoch == 0,
            records,
        }
    }
    fn selection(path: &str) -> TrainingSelection {
        let mut artifact = record().progress.artifact.unwrap();
        artifact.path = path.into();
        TrainingSelection {
            formatversion: 1,
            method: TrainFinalPolicy::Off,
            artifact,
            ingredients: vec![TrainingIngredient {
                modelid: 0,
                sha256: "digest".into(),
                coefficient: 1.0,
            }],
            validation: evaluation(),
            bestindividualmetric: 0.42,
        }
    }

    #[test]
    fn saved_pages_and_every_source_keep_run_frontiers_selection_and_geometry() {
        let catalog = all_sources();
        let count = catalog.available.len() as u64;
        let mut model = metric_model();
        let mut component = Component::default();
        // Independent live observations remain available while browsing saved data.
        let training = model.workflow.training.as_mut().unwrap();
        training.sources.catalog = catalog.clone();
        training.sources.selected = Some(selection("live-selected.pt"));
        training.sources.observations = source_page(&catalog, 0)
            .records
            .into_iter()
            .map(|mut sample| {
                sample.runid = "run".into();
                sample.attemptid = "attempt".into();
                sample
            })
            .collect();
        let mut current = record();
        current.sequence = 100;
        current.progress.scope = TrainingRecordScope::Session;
        training.metrics = Some(current);
        component.rebase(&model, true);
        let live_source = catalog.available[1].clone();
        component.update(Message::Source(live_source.clone()));
        component.update(Message::SelectedOutput(true));
        select_saved_run(&mut model);
        model
            .workflow
            .output
            .saved_mut()
            .unwrap()
            .run
            .as_mut()
            .unwrap()
            .run
            .as_mut()
            .unwrap()
            .sources = catalog.clone();
        let first = source_page(&catalog, 0);
        let second = source_page(&catalog, 1);
        for (page, points) in [(first.clone(), 1), (second.clone(), 2)] {
            let cursor = page.nextcursor;
            let sequence = page.records.last().unwrap().sequence;
            model.workflow.output.saved_mut().unwrap().page = Some(page);
            component.rebase(&model, true);
            for source in catalog.available.iter().rev() {
                component.update(Message::Source(source.clone()));
                let buckets = &curve(&component.saved, &component.metrics, Chart::Ap).buckets;
                let storage = buckets.as_slices().0.as_ptr();
                let shape = component.charts[Chart::Ap as usize].shapes.clone();
                let mut plot = PlotState::default();
                redraw(&mut component, Chart::Ap, &mut plot);
                for _ in 0..3 {
                    component.rebase(&model, true);
                    component.update(Message::Source(source.clone()));
                    assert!(
                        !redraw(&mut component, Chart::Ap, &mut plot),
                        "unchanged rebase must not project curves"
                    );
                }
                assert_eq!(component.saved.selected_source(), Some(source));
                assert_eq!(component.saved.run, "saved");
                assert_eq!(component.retained_run_facts(), (true, 3, Some(cursor)));
                assert_eq!(component.saved.sequence, Some(sequence));
                assert_eq!(component.saved.dropped, 7);
                let ap = curve(&component.saved, &component.metrics, Chart::Ap);
                assert_eq!(ap.buckets.len(), points);
                assert_eq!(
                    ap.buckets.as_slices().0.as_ptr(),
                    storage,
                    "selection never copies history"
                );
                assert_eq!(
                    ap.buckets.back().unwrap().last.order,
                    (points as u64 - 1) * count + source.modelid + 1
                );
                assert_eq!(component.charts[Chart::Ap as usize].shapes, shape);
                let loss = curve(&component.saved, &component.metrics, Chart::Loss);
                assert_eq!(
                    loss.buckets.len(),
                    if source.modelid == 0 { 0 } else { points }
                );
                if source.modelid != 0 {
                    assert_eq!(
                        loss.buckets.back().unwrap().last.value,
                        source.modelid as f64 + points as f64 - 1.0
                    );
                }
            }
        }
        let source = catalog.available.last().unwrap().clone();
        component.update(Message::Source(source.clone()));
        let saved_output = model.workflow.output.clone();
        model.workflow.output.live();
        component.rebase(&model, true);
        assert_eq!(component.live.selected_source(), Some(&live_source));
        assert!(component.live.selected_output());
        assert!(!component.saved.selected_output());
        assert_eq!(
            component.live.selection().unwrap().artifact.path,
            "live-selected.pt"
        );
        assert_eq!(
            curve(&component.live, &component.metrics, Chart::Ap)
                .buckets
                .len(),
            1
        );
        model.workflow.output = saved_output;
        component.rebase(&model, true);
        assert_eq!(component.saved.selected_source(), Some(&source));
        // A stale or duplicated page cannot replay an older attempt or move the frontier.
        model.workflow.output.saved_mut().unwrap().page = Some(first);
        component.rebase(&model, true);
        assert_eq!(component.saved.page, Some(2000));
        assert_eq!(component.saved.sequence, Some(count * 2));
        assert_eq!(
            curve(&component.saved, &component.metrics, Chart::Ap)
                .buckets
                .len(),
            2
        );
        model.workflow.output.saved_mut().unwrap().page = Some(second);
        component.reset(true);
        component.rebase(&model, true);
        assert_eq!(component.saved.selected_source(), Some(&source));
        assert_eq!(
            curve(&component.saved, &component.metrics, Chart::Ap)
                .buckets
                .len(),
            2
        );
    }

    #[test]
    fn replacement_pending_empty_and_failed_owners_expose_no_old_sources_or_selection() {
        let catalog = all_sources();
        let mut model = metric_model();
        select_saved_run(&mut model);
        let saved = model.workflow.output.saved_mut().unwrap();
        saved.run.as_mut().unwrap().run.as_mut().unwrap().sources = catalog.clone();
        saved.run.as_mut().unwrap().selected = Some(selection("old-selected.pt"));
        saved.page = Some(source_page(&catalog, 0));
        let mut component = Component::default();
        component.rebase(&model, true);
        let source = catalog.available.last().unwrap().clone();
        component.update(Message::Source(source.clone()));
        component.update(Message::SelectedOutput(true));
        assert!(component.saved.selected_output());
        model
            .workflow
            .output
            .saved_mut()
            .unwrap()
            .run
            .as_mut()
            .unwrap()
            .selected = Some(selection("replaced-selected.pt"));
        component.rebase(&model, true);
        assert!(component.saved.selected_output());
        assert_eq!(
            component.saved.selection().unwrap().artifact.path,
            "replaced-selected.pt"
        );
        let original = model.workflow.output.clone();
        // The same directory's reconnect opens pending, then binds the remembered
        // source only after the authoritative run matches; no old artifact is shown.
        model.peer_disconnected(crate::view_model::UiError::transport("offline"));
        component.reset(true);
        model.workflow.output.select_saved("saved-output".into());
        for _ in 0..2 {
            component.rebase(&model, true);
            assert!(component.saved.sources().is_empty());
            assert!(component.saved.selection().is_none());
            assert!(!component.saved.selected_output());
            assert!(
                component
                    .history()
                    .curves
                    .iter()
                    .all(|curve| curve.buckets.is_empty())
            );
        }
        model.workflow.output = original;
        model
            .workflow
            .output
            .saved_mut()
            .unwrap()
            .run
            .as_mut()
            .unwrap()
            .generation = 4;
        model
            .workflow
            .output
            .saved_mut()
            .unwrap()
            .page
            .as_mut()
            .unwrap()
            .generation = 4;
        component.rebase(&model, true);
        assert_eq!(component.saved.selected_source(), Some(&source));
        assert_eq!(component.saved.generation, 4);
        assert_eq!(
            curve(&component.saved, &component.metrics, Chart::Ap)
                .buckets
                .len(),
            1
        );

        for empty_open in [false, true] {
            model
                .workflow
                .output
                .select_saved(format!("replacement-{empty_open}"));
            if empty_open {
                model.workflow.output.saved_mut().unwrap().run = Some(TrainingOpenedRun {
                    directory: "replacement-true".into(),
                    generation: 5,
                    run: None,
                    selected: None,
                });
            } else {
                model.workflow.output.saved_mut().unwrap().load =
                    crate::view_model::HistoryLoad::Opening(9);
                assert!(model.workflow.output.fail(9));
            }
            component.rebase(&model, true);
            assert!(component.saved.sources().is_empty());
            assert!(component.saved.selection().is_none());
            assert!(component.saved.sequence.is_none());
            assert!(
                component
                    .history()
                    .curves
                    .iter()
                    .all(|curve| curve.buckets.is_empty())
            );
        }
        select_saved_run(&mut model);
        let saved = model.workflow.output.saved_mut().unwrap();
        let run = saved.run.as_mut().unwrap();
        run.generation = 6;
        run.run.as_mut().unwrap().runid = "replacement-run".into();
        saved.page = None;
        component.rebase(&model, true);
        assert_eq!(
            component.saved.selected_source(),
            source_catalog().defaultsource.as_ref()
        );
        assert!(component.saved.selection().is_none());
        assert!(
            component
                .history()
                .curves
                .iter()
                .all(|curve| curve.buckets.is_empty())
        );
    }

    #[test]
    fn new_live_admission_clears_all_sources_before_records_and_continuation_preserves_them() {
        let catalog = all_sources();
        let mut model = metric_model();
        let mut component = Component::default();
        let training = model.workflow.training.as_mut().unwrap();
        training.sources.catalog = catalog.clone();
        training.sources.selected = Some(selection("old-live.pt"));
        for record in source_page(&catalog, 0).records {
            model.workflow.training.as_mut().unwrap().metrics = Some(record);
            component.rebase(&model, false);
        }
        let source = catalog.available[2].clone();
        component.update(Message::Source(source.clone()));
        // A compatible next attempt appends with a truthful disconnected segment.
        let mut resumed = source_page(&catalog, 1).records[2].clone();
        resumed.attemptid = "continued".into();
        resumed.sequence = 0;
        let old_segment = curve(&component.live, &component.metrics, Chart::Ap)
            .buckets
            .back()
            .unwrap()
            .segment;
        model.workflow.training.as_mut().unwrap().metrics = Some(resumed);
        component.rebase(&model, true);
        assert_eq!(component.live.selected_source(), Some(&source));
        let ap = curve(&component.live, &component.metrics, Chart::Ap);
        assert_eq!(ap.buckets.len(), 2);
        assert_ne!(ap.buckets.back().unwrap().segment, old_segment);
        // The native pending-admission state hides old sources; a confirmed
        // continuation of the same run restores only its compatible source choice.
        let training = model.workflow.training.as_mut().unwrap();
        training.local.generationfrontier += 1;
        training.local.active = true;
        training.local.progress.sequence = 0;
        component.rebase(&model, true);
        assert!(component.live.sources().is_empty());
        let training = model.workflow.training.as_mut().unwrap();
        training.local.progress.sequence = 1;
        training.metrics.as_mut().unwrap().sequence = 1;
        component.rebase(&model, true);
        assert_eq!(component.live.selected_source(), Some(&source));
        component.update(Message::SelectedOutput(true));
        let training = model.workflow.training.as_mut().unwrap();
        training.local.generationfrontier += 1;
        training.local.active = true;
        training.local.progress.sequence = 0;
        component.rebase(&model, true);
        assert!(component.live.sources().is_empty());
        assert!(component.live.selection().is_none());
        assert!(!component.live.selected_output());
        assert!(component.live.sequence.is_none());
        // A failed admission with no progress must not resurrect the old snapshot.
        model.workflow.training.as_mut().unwrap().local.active = false;
        component.rebase(&model, true);
        assert!(component.live.sources().is_empty());
        let training = model.workflow.training.as_mut().unwrap();
        training.local.progress.sequence = 1;
        training.sources.catalog = source_catalog();
        training.sources.selected = None;
        let mut sample = record();
        sample.runid = "new-live".into();
        training.metrics = Some(sample);
        component.rebase(&model, true);
        assert_eq!(component.live.run, "new-live");
        assert_eq!(
            component.live.selected_source(),
            source_catalog().defaultsource.as_ref()
        );
        assert_eq!(component.live.sources().len(), 1);
        assert!(component.live.selection().is_none());
        assert_eq!(
            curve(&component.live, &component.metrics, Chart::Loss)
                .buckets
                .len(),
            1
        );
        assert!(
            curve(&component.live, &component.metrics, Chart::Ap)
                .buckets
                .is_empty()
        );
    }

    #[test]
    fn all_source_storage_is_bounded_and_run_condition_dismissal_survives_switches() {
        let catalog = all_sources();
        let mut model = metric_model();
        select_saved_run(&mut model);
        model
            .workflow
            .output
            .saved_mut()
            .unwrap()
            .run
            .as_mut()
            .unwrap()
            .run
            .as_mut()
            .unwrap()
            .sources = catalog.clone();
        let mut component = Component::default();
        for epoch in 0..=history::BUCKETS as u64 {
            let mut page = source_page(&catalog, epoch);
            for record in &mut page.records {
                record.attemptid = format!("attempt-{epoch}");
            }
            model.workflow.output.saved_mut().unwrap().page = Some(page);
            publish(&mut component, &mut model);
        }
        assert!(
            model
                .notices
                .rows()
                .any(|notice| notice.origin == crate::view_model::notices::Origin::ChartSaved)
        );
        assert!(
            model
                .notices
                .rows()
                .any(|notice| notice.origin
                    == crate::view_model::notices::Origin::HistoryDroppedSaved)
        );
        model.notices.dismiss_all();
        for source in &catalog.available {
            component.update(Message::Source(source.clone()));
            publish(&mut component, &mut model);
            assert!(model.notices.is_empty());
            assert_eq!(component.saved.sources().len(), catalog.available.len());
            assert!(
                component
                    .history()
                    .curves
                    .iter()
                    .all(|curve| curve.buckets.len() <= history::BUCKETS)
            );
            assert!(curve(&component.saved, &component.metrics, Chart::Ap).omitted > 0);
        }
        component.reset(true);
        publish(&mut component, &mut model);
        assert!(model.notices.is_empty());
    }
}

impl Component {
    pub(crate) fn publish_conditions(
        &mut self,
        notices: &mut crate::view_model::notices::NoticeStore,
    ) {
        self.live.publish_conditions(notices, false);
        self.saved.publish_conditions(notices, true);
    }
}
