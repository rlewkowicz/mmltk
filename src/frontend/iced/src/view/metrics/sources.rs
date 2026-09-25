use super::{catalog::Metric, history::History};
use crate::generated::{TrainingHistoryPage, TrainingMetricSource, TrainingOpenedRun, TrainingRecord, TrainingSelection, TrainingSnapshot, TrainingSourceCatalog};

/// Run-scoped observations never move with the visible model or with plot storage.
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
    // Reopening the same run can replay its bounded summaries. Observe their
    // high-water count once, without counting the same omissions a second time.
    fn observe_omitted(&mut self, count: u64) {
        if count > self.omitted {
            self.omitted = count;
            self.chart_dirty = true;
        }
    }
}

/// One bounded run's source collection. The native catalog supplies order and
/// default policy; selection addresses a retained slot without moving its history.
/// Native TrainingOutput still owns file reads and the requested byte cursor.
pub(super) struct SourceHistories {
    catalog: TrainingSourceCatalog,
    histories: Vec<History>,
    selected: Option<usize>,
    pub(super) run: String,
    attempt: String,
    pub(super) sequence: Option<u64>,
    pub(super) dropped: u64,
    segment: u64,
    omitted: u64,
    pub(super) generation: u64,
    pub(super) page: Option<u64>,
    directory: String,
    // Pending admission exposes no old source. Only matching native run identity
    // may restore this one bounded presentation choice after admission finishes.
    remembered_source: Option<(String, TrainingMetricSource)>,
    pending_live: bool,
    live_generation: u64,
    selection: Option<TrainingSelection>,
    selected_output: bool,
    conditions: Conditions,
}
impl SourceHistories {
    pub(super) fn new(metrics: &[Metric]) -> Self {
        Self {
            catalog: TrainingSourceCatalog { available: Vec::new(), defaultsource: None },
            // The unbound slot supplies empty curves to retire existing geometry,
            // then becomes the first admitted source without another allocation.
            histories: vec![History::new(metrics)],
            selected: None,
            run: String::new(),
            attempt: String::new(),
            sequence: None,
            dropped: 0,
            segment: 0,
            omitted: 0,
            generation: 0,
            page: None,
            directory: String::new(),
            remembered_source: None,
            pending_live: false,
            live_generation: 0,
            selection: None,
            selected_output: false,
            conditions: Conditions::default(),
        }
    }
    pub(super) fn history(&self) -> &History {
        &self.histories[self.selected.unwrap_or(0)]
    }
    pub(super) fn sources(&self) -> &[TrainingMetricSource] {
        &self.catalog.available
    }
    pub(super) fn selected_source(&self) -> Option<&TrainingMetricSource> {
        self.selected.and_then(|index| self.histories[index].source.as_ref())
    }
    pub(super) fn selection(&self) -> Option<&TrainingSelection> {
        self.selection.as_ref()
    }
    pub(super) fn selected_output(&self) -> bool {
        self.selected_output
    }
    pub(super) fn show_selected_output(&mut self, selected: bool) {
        self.selected_output = selected && self.selection.is_some();
    }
    pub(super) fn select(&mut self, source: &TrainingMetricSource) -> bool {
        let Some(index) = self.histories.iter().position(|history| history.source.as_ref() == Some(source)) else { return false; };
        let changed = self.selected != Some(index);
        self.selected = Some(index);
        self.selected_output = false;
        changed
    }
    fn replace_selection(&mut self, selection: &Option<TrainingSelection>) {
        if &self.selection != selection {
            self.selection.clone_from(selection);
        }
        if self.selection.is_none() {
            self.selected_output = false;
        }
    }
    pub(super) fn clear(&mut self, metrics: &[Metric]) {
        for history in &mut self.histories { history.clear(metrics); }
        self.catalog.available.clear();
        self.catalog.defaultsource = None;
        self.selected = None;
        self.run.clear();
        self.attempt.clear();
        self.sequence = None;
        self.dropped = 0;
        self.segment = 0;
        self.omitted = 0;
        self.generation = 0;
        self.page = None;
        self.selection = None;
        self.selected_output = false;
    }
    pub(super) fn reconcile(&mut self, catalog: &TrainingSourceCatalog, metrics: &[Metric]) -> u16 {
        if self.catalog == *catalog { return 0; }
        let selected = self.selected_source().cloned();
        let mut changed = 0;
        for source in &catalog.available {
            if self.histories.iter().any(|history| history.source.as_ref() == Some(source)) { continue; }
            // Reuse a retired source's allocations. A weight-only correction may
            // retain its model's scalar samples; unrelated model data is cleared.
            let reusable = |history: &History| history.source.as_ref().is_none_or(|prior| !catalog.available.contains(prior));
            let index = self.histories.iter().position(|history| reusable(history)
                && history.source.as_ref().is_some_and(|prior| prior.scope == source.scope && prior.modelid == source.modelid))
                .or_else(|| self.histories.iter().position(reusable))
                .unwrap_or_else(|| { self.histories.push(History::new(metrics)); self.histories.len() - 1 });
            self.histories[index].bind(source, metrics);
            if self.selected == Some(index) { changed = u16::MAX; }
        }
        for history in &mut self.histories {
            if history.source.as_ref().is_some_and(|source| !catalog.available.contains(source)) {
                history.clear(metrics);
            }
        }
        self.omitted = self.histories.iter().fold(0_u64, |total, history| total.saturating_add(history.omitted));
        self.catalog.clone_from(catalog);
        let source = selected.as_ref().filter(|source| catalog.available.contains(source)).or(catalog.defaultsource.as_ref());
        self.selected = source.and_then(|source| self.histories.iter().position(|history| history.source.as_ref() == Some(source)));
        if selected.as_ref() != self.selected_source() { changed = u16::MAX; }
        changed
    }
    pub(super) fn rebase_live(&mut self, training: &TrainingSnapshot, metrics: &[Metric]) -> u16 {
        if training.local.progress.sequence == 0
            && (training.local.active || self.pending_live || self.live_generation != training.local.generationfrontier) {
            if self.pending_live && self.live_generation == training.local.generationfrontier { return 0; }
            if let Some(source) = self.selected_source().cloned() {
                self.remembered_source = Some((self.run.clone(), source));
            }
            self.clear(metrics);
            self.pending_live = true;
            self.live_generation = training.local.generationfrontier;
            return u16::MAX;
        }
        let Some(record) = &training.metrics else {
            if self.sequence.is_some() || !self.catalog.available.is_empty() || self.selection.is_some() {
                self.clear(metrics);
                return u16::MAX;
            }
            return 0;
        };
        self.pending_live = false;
        self.live_generation = training.local.generationfrontier;
        let mut changed = 0;
        if self.run != record.runid {
            self.clear(metrics);
            self.run.clone_from(&record.runid);
            self.conditions.set_run(&self.run);
            changed = u16::MAX;
        }
        changed |= self.reconcile(&training.sources.catalog, metrics);
        if let Some((run, source)) = self.remembered_source.take()
            && run == record.runid && self.select(&source) { changed = u16::MAX; }
        changed |= self.ingest(record, true, metrics);
        for observation in &training.sources.observations {
            changed |= self.ingest_observation(observation, metrics);
        }
        self.replace_selection(&training.sources.selected);
        changed
    }
    pub(super) fn rebase_saved(&mut self, directory: &str, opened: Option<&TrainingOpenedRun>, page: Option<&TrainingHistoryPage>, metrics: &[Metric]) -> u16 {
        let mut changed = 0;
        if self.directory != directory {
            self.clear(metrics);
            self.remembered_source = None;
            self.directory.clear();
            self.directory.push_str(directory);
            changed = u16::MAX;
        }
        let Some(opened) = opened else {
            if self.generation != 0 || !self.run.is_empty() || !self.catalog.available.is_empty() || self.selection.is_some() {
                self.remembered_source = self.selected_source().cloned().map(|source| (self.run.clone(), source));
                self.clear(metrics);
                changed = u16::MAX;
            }
            return changed;
        };
        let run = opened.run.as_ref().map_or("", |run| run.runid.as_str());
        if self.generation != opened.generation || self.run != run {
            let selected = (self.run == run).then(|| self.selected_source().cloned()).flatten()
                .or_else(|| self.remembered_source.take().filter(|(previous, _)| previous == run).map(|(_, source)| source));
            self.remembered_source = None;
            self.clear(metrics);
            self.run.push_str(run);
            self.generation = opened.generation;
            self.conditions.set_run(run);
            if let Some(run) = &opened.run { self.reconcile(&run.sources, metrics); }
            if let Some(source) = selected { self.select(&source); }
            changed = u16::MAX;
        } else if let Some(run) = &opened.run {
            changed |= self.reconcile(&run.sources, metrics);
        }
        if opened.run.is_none() {
            self.remembered_source = None;
            return changed;
        }
        self.replace_selection(&opened.selected);
        if let Some(page) = page
            && page.generation == self.generation
            && self.page.is_none_or(|cursor| page.nextcursor > cursor)
        {
            for record in &page.records { changed |= self.ingest(record, false, metrics); }
            self.page = Some(page.nextcursor);
        }
        changed
    }
    pub(super) fn ingest(&mut self, record: &TrainingRecord, live: bool, metrics: &[Metric]) -> u16 {
        if self.run != record.runid { return 0; }
        let attempt_changed = self.attempt != record.attemptid;
        if attempt_changed {
            self.sequence = None;
            self.dropped = 0;
            for history in &mut self.histories { history.begin_attempt(); }
        }
        if self.sequence.is_some_and(|sequence| record.sequence <= sequence) { return 0; }
        if attempt_changed || record.droppedbefore > self.dropped
            || self.sequence.is_some_and(|sequence| sequence.checked_add(1) != Some(record.sequence)) {
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
        let mut changed = 0;
        for (index, history) in self.histories.iter_mut().enumerate() {
            if !history.matches_scalars(record) && !history.matches_evaluation(record) { continue; }
            let omitted = history.omitted;
            let source_changed = history.ingest(record, live, self.segment, attempt_changed, metrics);
            self.omitted = self.omitted.saturating_add(history.omitted - omitted);
            self.conditions.observe_omitted(self.omitted);
            if self.selected == Some(index) { changed |= source_changed; }
        }
        changed
    }
    pub(super) fn ingest_observation(&mut self, record: &TrainingRecord, metrics: &[Metric]) -> u16 {
        if self.run != record.runid || self.attempt != record.attemptid { return 0; }
        let Some((index, history)) = self.histories.iter_mut().enumerate()
            .find(|(_, history)| history.matches_evaluation(record)) else { return 0; };
        let omitted = history.omitted;
        let changed = history.ingest_observation(record, self.segment, metrics);
        self.omitted = self.omitted.saturating_add(history.omitted - omitted);
        self.conditions.observe_omitted(self.omitted);
        if self.selected == Some(index) { changed } else { 0 }
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
    use crate::generated::{TrainingRecordScope, TRAINING_MODEL_CAPACITY};

    #[test]
    fn source_replacement_reuses_the_bounded_pool_and_curve_capacity() {
        let metrics = super::super::catalog::catalog();
        let mut histories = SourceHistories::new(&metrics);
        let mut catalog = super::super::tests::all_sources();
        histories.reconcile(&catalog, &metrics);
        let capacity: Vec<_> = histories.histories.iter().map(|history|
            history.curves.iter().map(|curve| curve.buckets.capacity()).collect::<Vec<_>>()).collect();
        for replacement in 1..4 {
            for source in &mut catalog.available {
                if source.scope == TrainingRecordScope::Model { source.modelid += 100; }
            }
            catalog.defaultsource = Some(catalog.available[replacement].clone());
            histories.clear(&metrics);
            histories.reconcile(&catalog, &metrics);
            assert_eq!(histories.histories.len(), TRAINING_MODEL_CAPACITY + 1);
            assert_eq!(histories.selected_source(), catalog.defaultsource.as_ref());
            for (history, capacities) in histories.histories.iter().zip(&capacity) {
                for (curve, capacity) in history.curves.iter().zip(capacities) {
                    assert_eq!(curve.buckets.capacity(), *capacity);
                    assert!(curve.buckets.is_empty());
                }
            }
        }
    }
}
