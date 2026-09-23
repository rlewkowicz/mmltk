use super::*;
use crate::generated::{BenchmarkTransferProgress, DatasetCompilePhase};

fn dataset() -> ArtifactUiState {
    crate::generated::application_snapshot_defaults().unwrap().into_iter()
        .find_map(|fact| match fact.value {
            crate::generated::ApplicationSnapshot::Dataset(value) => Some(value),
            _ => None,
        }).unwrap()
}

fn source() -> BenchmarkSourceProgress {
    BenchmarkSourceProgress {
        source: BenchmarkDatasetSource::KObjects365V2,
        activity: "Cache hit".into(), transfer: None,
        completedbytes: 85 * 1024 * 1024 * 1024, totalbytes: 85 * 1024 * 1024 * 1024,
        completedimages: 345491, totalimages: 345491, invalidatedimages: 0,
        retrycount: 0, cachehit: true, resumed: false, complete: true, bytetotalknown: true,
    }
}

#[test]
fn display_quantities_preserve_zero_tiny_values_and_all_integer_digits() {
    for (bytes, expected) in [(0, "0 KiB"), (1, "0.001 KiB"), (6, "0.006 KiB"),
        (1024, "1.0 KiB"), (1024 * 1024, "1.0 MiB"),
        (85 * 1024 * 1024 * 1024, "85.0 GiB"), (1024_u64.pow(4), "1.0 TiB"),
        (u64::MAX, "16,777,216.0 TiB")] {
        assert_eq!(size(bytes), expected);
    }
    assert_eq!(count(0), "0");
    assert_eq!(count(u64::MAX), "18,446,744,073,709,551,615");
    assert_eq!(quantity(0, None, false, true), "0 KiB / ?");
    assert_eq!(quantity(0, Some(0), false, true), "0 KiB / 0 KiB");
}

#[test]
fn cached_and_downloaded_sources_keep_concise_independent_transfer_facts() {
    let mut source = source();
    assert_eq!(source_heading(&source), "Objects365 v2 · Cached");
    assert_eq!(source_summary(&source), "85.0 GiB · 345,491 images");
    assert!(source_details(&source).is_empty());
    source.complete = false;
    source.cachehit = false;
    source.bytetotalknown = false;
    source.activity = "Resuming train-patch".into();
    source.retrycount = 2;
    source.resumed = true;
    source.invalidatedimages = 1234;
    source.transfer = Some(BenchmarkTransferProgress {
        completedbytes: 4096, totalbytes: 0, retainedbytes: 1024,
        attempt: 3, cachehit: false, resumed: true,
    });
    assert_eq!(source_heading(&source), "Objects365 v2 · Active");
    assert!(source_summary(&source).starts_with("85.0 GiB / ?"));
    assert_eq!(source_details(&source),
        "4.0 KiB / ? · attempt 3 · 1.0 KiB retained · 2 retries · 1,234 images invalidated");
    let transfer = source.transfer.as_mut().unwrap();
    transfer.totalbytes = 4096;
    transfer.cachehit = true;
    assert!(source_details(&source).starts_with("4.0 KiB / 4.0 KiB reused"));
    // A new, unresumed artifact must not hide earlier aggregate resumption.
    source.transfer.as_mut().unwrap().resumed = false;
    source.transfer.as_mut().unwrap().retainedbytes = 0;
    assert_eq!(source_details(&source),
        "4.0 KiB / 4.0 KiB reused · 2 retries · Resumed · 1,234 images invalidated");
    source.transfer = None;
    assert_eq!(source_details(&source), "2 retries · Resumed · 1,234 images invalidated");
    source.complete = true;
    assert_eq!(source_heading(&source), "Objects365 v2 · Complete");
    assert_eq!(source_details(&source), "2 retries · Resumed · 1,234 images invalidated");
}

#[test]
fn planning_sources_keep_unestablished_totals_open_until_known_or_complete() {
    let mut dataset = dataset();
    dataset.active = true;
    dataset.progress.phase = DatasetCompilePhase::Planning;
    let mut waiting = source();
    waiting.source = BenchmarkDatasetSource::KCoco2017;
    waiting.complete = false;
    waiting.cachehit = false;
    waiting.activity.clear();
    waiting.completedbytes = 0;
    waiting.totalbytes = 0;
    waiting.completedimages = 0;
    waiting.totalimages = 0;
    // byte-total-known starts neutral in the native source ledger.
    assert!(waiting.bytetotalknown);
    dataset.progress.sources = vec![waiting, source()];
    assert_eq!(heading(&dataset), "Planning");
    assert_eq!(source_heading(&dataset.progress.sources[0]), "COCO 2017 · Waiting");
    assert_eq!(source_summary(&dataset.progress.sources[0]), "0 KiB / ? · 0 / ? images");
    assert_eq!(source_summary(&dataset.progress.sources[1]), "85.0 GiB · 345,491 images");
    let waiting = &mut dataset.progress.sources[0];
    waiting.totalbytes = 1024;
    assert_eq!(source_summary(waiting), "0 KiB / 1.0 KiB · 0 / ? images");
    waiting.totalimages = 1234;
    assert_eq!(source_summary(waiting), "0 KiB / 1.0 KiB · 0 / 1,234 images");
    waiting.bytetotalknown = false;
    assert_eq!(source_summary(waiting), "0 KiB / ? · 0 / 1,234 images");
    waiting.bytetotalknown = true;
    waiting.totalbytes = 0;
    waiting.totalimages = 0;
    waiting.complete = true;
    assert_eq!(source_summary(waiting), "0 KiB · 0 images");
}

#[test]
fn tracks_preserve_independent_ratios_unknowns_zero_and_repair() {
    let mut dataset = dataset();
    let track = &mut dataset.progress.tracks.acquisition;
    track.completed = 4097;
    track.total = 8193;
    track.totalknown = true;
    track.active = true;
    track.activity = DatasetCompileActivity::Acquiring;
    assert_eq!(track_ratio(track), Some((4097_f64 / 8193_f64) as f32));
    track.totalknown = false;
    assert_eq!(track_ratio(track), None);
    assert!(track_caption(track, true).contains("4.0 KiB / ?"));
    track.totalknown = true;
    track.completed = 1024;
    track.invalidated = 3073;
    assert_eq!(track_ratio(track), Some((1024_f64 / 8193_f64) as f32));
    assert!(track_caption(track, true).contains("3,073 invalidated by repair"));
    track.completed = 0;
    track.total = 0;
    track.activity = DatasetCompileActivity::Unnecessary;
    assert_eq!(track_ratio(track), None);
    assert!(track_caption(track, true).starts_with("No acquisition needed"));
    dataset.progress.tracks.labels.total = 10;
    dataset.progress.tracks.labels.completed = 3;
    dataset.progress.tracks.labels.totalknown = true;
    dataset.progress.tracks.pixels.total = 20;
    dataset.progress.tracks.pixels.completed = 17;
    dataset.progress.tracks.pixels.totalknown = true;
    assert_eq!(track_ratio(&dataset.progress.tracks.labels), Some(0.3));
    assert_eq!(track_ratio(&dataset.progress.tracks.pixels), Some(0.85));
}

#[test]
fn metrics_keep_eta_output_and_quality_in_one_compact_group() {
    let mut dataset = dataset();
    dataset.progress.elapsedseconds = 12;
    dataset.progress.remainingseconds = 34;
    dataset.progress.throughputpersecond = 5;
    dataset.progress.projectedoutputbytes = 6;
    dataset.progress.droppedinstances = 7;
    dataset.progress.quarantinedimages = 8;
    assert_eq!(metrics(&dataset.progress),
        "12s elapsed · 34s remaining · 5/s · 0.006 KiB projected · 7 instances dropped · 8 images quarantined");
}

#[test]
fn native_lifecycle_controls_visibility_independently_of_retained_progress() {
    let mut dataset = dataset();
    assert_eq!(heading(&dataset), "");
    assert!(!show_tracks(&dataset));
    dataset.active = true;
    assert_eq!(heading(&dataset), "Preparing compilation");
    assert!(show_tracks(&dataset));
    dataset.progress.phase = DatasetCompilePhase::Planning;
    assert_eq!(heading(&dataset), "Planning");
    dataset.progress.phase = DatasetCompilePhase::Publishing;
    dataset.progress.sources.push(source());
    for track in [&mut dataset.progress.tracks.acquisition, &mut dataset.progress.tracks.labels,
        &mut dataset.progress.tracks.pixels] {
        track.complete = true;
    }
    // All tracks completed is still active publication, never success.
    assert!(show_tracks(&dataset));
    assert_eq!(heading(&dataset), "Publishing");
    dataset.terminal.outcome = ArtifactTerminalOutcome::CancellationRequested;
    assert!(cancelling(Some(&dataset)));
    assert!(!show_tracks(&dataset));
    assert_eq!(heading(&dataset), "Cancelling…");
    let retained = dataset.progress.clone();
    dataset.active = false;
    for (outcome, caption) in [(ArtifactTerminalOutcome::Cancelled, "Cancelled"),
        (ArtifactTerminalOutcome::Succeeded, "Completed"), (ArtifactTerminalOutcome::Failed, "Failed"),
        (ArtifactTerminalOutcome::Refused, "Compilation refused")] {
        dataset.terminal.outcome = outcome;
        assert_eq!(heading(&dataset), caption);
        assert!(!show_tracks(&dataset));
        assert!(!cancelling(Some(&dataset)));
        let _: Element<'static, ()> = view(Some(&dataset));
        assert_eq!(dataset.progress, retained);
    }
    dataset.generation += 1;
    dataset.active = true;
    dataset.terminal.outcome = ArtifactTerminalOutcome::Idle;
    assert!(show_tracks(&dataset));
}

#[test]
fn measured_active_area_holds_shorter_updates_and_cancellation_removes_rows() {
    use iced::advanced::{Layout, layout, renderer::Headless, widget};
    use iced::{Rectangle, Size};
    struct Rows(usize);
    impl widget::Operation for Rows {
        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn widget::Operation)) { operate(self); }
        fn container(&mut self, id: Option<&widget::Id>, _: Rectangle) {
            if TRACK_IDS.iter().any(|name| id == Some(&widget::Id::from(*name))) { self.0 += 1; }
        }
    }
    let renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
        Default::default(), Some("wgpu"),
    )).expect("dataset progress layout requires the container renderer");
    let limits = layout::Limits::new(Size::ZERO, Size::new(200.0, 4000.0));
    let mut dataset = dataset();
    dataset.active = true;
    dataset.generation = 1;
    dataset.progress.phase = DatasetCompilePhase::Downloading;
    dataset.progress.activity = "Downloading the current image archive".into();
    dataset.progress.sources.push(source());
    dataset.progress.projectedoutputbytes = 1024_u64.pow(4);
    let mut element: Element<'static, ()> = view(Some(&dataset));
    let mut tree = widget::Tree::new(&element);
    tree.diff(&mut element);
    let node = element.as_widget_mut().layout(&mut tree, &renderer, &limits);
    let active_height = node.size().height;
    let mut rows = Rows(0);
    element.as_widget_mut().operate(&mut tree, Layout::new(&node), &renderer, &mut rows);
    assert_eq!(rows.0, 3);
    dataset.progress.activity.clear();
    dataset.progress.projectedoutputbytes = 0;
    dataset.progress.sources.clear();
    for cancelling in [false, true] {
        if cancelling { dataset.terminal.outcome = ArtifactTerminalOutcome::CancellationRequested; }
        element = view(Some(&dataset));
        tree.diff(&mut element);
        let node = element.as_widget_mut().layout(&mut tree, &renderer, &limits);
        assert_eq!(node.size().height, active_height);
        let mut rows = Rows(0);
        element.as_widget_mut().operate(&mut tree, Layout::new(&node), &renderer, &mut rows);
        assert_eq!(rows.0, if cancelling { 0 } else { 3 });
    }
    dataset.active = false;
    dataset.terminal.outcome = ArtifactTerminalOutcome::Cancelled;
    // Reconnection installs the compact native terminal immediately; no source
    // or track is resurrected by the retained non-idle phase.
    element = view(Some(&dataset));
    let mut reconnected = widget::Tree::new(&element);
    let node = element.as_widget_mut().layout(&mut reconnected, &renderer, &limits);
    assert!(node.size().height < active_height);
    let mut rows = Rows(0);
    element.as_widget_mut().operate(&mut reconnected, Layout::new(&node), &renderer, &mut rows);
    assert_eq!(rows.0, 0);
    dataset.generation += 1;
    dataset.active = true;
    dataset.terminal.outcome = ArtifactTerminalOutcome::Idle;
    element = view(Some(&dataset));
    // A new generation uses current content rather than a prior run's tall
    // source panel; shared disclosure tests also exercise retained-tree reset.
    let mut restarted = widget::Tree::new(&element);
    let node = element.as_widget_mut().layout(&mut restarted, &renderer, &limits);
    assert!(node.size().height < active_height);
    let mut rows = Rows(0);
    element.as_widget_mut().operate(&mut restarted, Layout::new(&node), &renderer, &mut rows);
    assert_eq!(rows.0, 3);
}
