//! Dataset compilation presentation; native state alone decides settlement.
use crate::fluent_theme::Element;
use crate::generated::{ArtifactTerminalOutcome, ArtifactUiState, BenchmarkDatasetSource,
    BenchmarkSourceProgress, DatasetCompileActivity, DatasetCompileTrack};
use iced::Fill;
use iced::widget::{column, container, progress_bar, text, Space};

pub(crate) const AREA_ID: &str = "train.dataset.progress.area";
pub(crate) const TRACK_IDS: [&str; 3] = [
    "train.dataset.progress.acquisition", "train.dataset.progress.labels", "train.dataset.progress.pixels",
];

pub(super) fn cancelling(state: Option<&ArtifactUiState>) -> bool {
    state.is_some_and(|state| state.active
        && state.terminal.outcome == ArtifactTerminalOutcome::CancellationRequested)
}

fn heading(state: &ArtifactUiState) -> String {
    if cancelling(Some(state)) {
        return "Cancelling…".into();
    }
    if state.active {
        return match state.progress.phase {
            crate::generated::DatasetCompilePhase::Idle => "Preparing compilation".into(),
            phase => format!("{phase:?}"),
        };
    }
    match state.terminal.outcome {
        ArtifactTerminalOutcome::Idle => "",
        ArtifactTerminalOutcome::Succeeded => "Completed",
        ArtifactTerminalOutcome::Failed => "Failed",
        ArtifactTerminalOutcome::Cancelled => "Cancelled",
        ArtifactTerminalOutcome::Refused => "Compilation refused",
        ArtifactTerminalOutcome::CancellationRequested => "Cancelling…",
    }.into()
}

fn show_tracks(state: &ArtifactUiState) -> bool {
    state.active && !cancelling(Some(state))
}

// Integer formatting preserves all u64 digits, independently of bar precision.
fn count(value: u64) -> String {
    let digits = value.to_string();
    let mut grouped = String::with_capacity(digits.len() + digits.len() / 3);
    for (index, digit) in digits.chars().enumerate() {
        if index != 0 && (digits.len() - index).is_multiple_of(3) {
            grouped.push(',');
        }
        grouped.push(digit);
    }
    grouped
}

fn size(bytes: u64) -> String {
    if bytes == 0 { return "0 KiB".into(); }
    let mut divisor = 1024_u64;
    let mut unit = "KiB";
    for candidate in ["MiB", "GiB", "TiB"] {
        if bytes < divisor * 1024 { break; }
        divisor *= 1024;
        unit = candidate;
    }
    // Sub-KiB values retain three decimal places, so even one byte is nonzero.
    let precision = if bytes < 1024 { 1000_u128 } else { 10 };
    let rounded = (u128::from(bytes) * precision + u128::from(divisor) / 2)
        / u128::from(divisor);
    let whole = count((rounded / precision) as u64);
    let fraction = rounded % precision;
    if precision == 1000 { format!("{whole}.{fraction:03} {unit}") }
    else { format!("{whole}.{fraction} {unit}") }
}

fn quantity(completed: u64, total: Option<u64>, complete: bool, bytes: bool) -> String {
    let format = if bytes { size } else { count };
    match total {
        Some(total) if complete && completed == total => format(completed),
        Some(total) => format!("{} / {}", format(completed), format(total)),
        None => format!("{} / ?", format(completed)),
    }
}

fn track_caption(track: &DatasetCompileTrack, bytes: bool) -> String {
    let activity = match track.activity {
        DatasetCompileActivity::Waiting => "Waiting",
        DatasetCompileActivity::Unnecessary => "No acquisition needed",
        DatasetCompileActivity::Acquiring => "Acquiring sources",
        DatasetCompileActivity::Normalizing => "Normalizing annotations",
        DatasetCompileActivity::Preparing => "Preparing labels and masks",
        DatasetCompileActivity::Compiling => "Compiling image pixels",
        DatasetCompileActivity::Complete => "Complete",
    };
    let mut caption = format!("{activity} · {}", quantity(track.completed,
        track.totalknown.then_some(track.total), track.complete, bytes));
    if track.invalidated != 0 {
        caption.push_str(&format!(" · {} invalidated by repair", count(track.invalidated)));
    }
    caption
}

fn track_ratio(track: &DatasetCompileTrack) -> Option<f32> {
    (track.totalknown && track.total != 0
        && track.activity != DatasetCompileActivity::Unnecessary)
        .then(|| (track.completed as f64 / track.total as f64) as f32)
}

fn track<Message: 'static>(id: &'static str, name: &'static str,
    fact: &DatasetCompileTrack, bytes: bool) -> Element<'static, Message> {
    let bar: Element<'static, Message> = match track_ratio(fact) {
        Some(ratio) => progress_bar(0.0..=1.0, ratio).height(4).into(),
        None => Space::new().height(4).into(),
    };
    container(column![text(name).size(12), text(track_caption(fact, bytes)).size(12), bar]
        .spacing(4).width(Fill)).id(id).width(Fill).into()
}

fn source_identity(source: BenchmarkDatasetSource) -> (&'static str, &'static str) {
    match source {
        BenchmarkDatasetSource::KCoco2017 => ("COCO 2017", "train.dataset.progress.source.coco2017"),
        BenchmarkDatasetSource::KObjects365V2 => ("Objects365 v2", "train.dataset.progress.source.objects365v2"),
        BenchmarkDatasetSource::KOpenImagesV7 => ("Open Images v7", "train.dataset.progress.source.openimagesv7"),
        BenchmarkDatasetSource::KCoconut => ("COCONut", "train.dataset.progress.source.coconut"),
        BenchmarkDatasetSource::KObjects365V1 => ("Objects365 v1", "train.dataset.progress.source.objects365v1"),
    }
}

fn source_heading(source: &BenchmarkSourceProgress) -> String {
    let status = if source.complete {
        if source.cachehit { "Cached" } else { "Complete" }
    } else if source.activity.is_empty() { "Waiting" } else { "Active" };
    format!("{} · {status}", source_identity(source.source).0)
}

fn source_summary(source: &BenchmarkSourceProgress) -> String {
    // Initial rows have neutral zero totals. A nonzero total establishes a
    // quantity; source completion also establishes an actually empty source.
    let bytes_known = source.bytetotalknown && (source.totalbytes != 0 || source.complete);
    let images_known = source.totalimages != 0 || source.complete;
    format!("{} · {} images",
        quantity(source.completedbytes, bytes_known.then_some(source.totalbytes), source.complete, true),
        quantity(source.completedimages, images_known.then_some(source.totalimages), source.complete, false))
}

fn source_details(source: &BenchmarkSourceProgress) -> String {
    let mut details = Vec::with_capacity(4);
    let transfer = source.transfer.as_ref().filter(|_| !source.complete);
    if let Some(transfer) = transfer {
        let mut current = quantity(transfer.completedbytes,
            (transfer.totalbytes != 0).then_some(transfer.totalbytes), false, true);
        if transfer.cachehit { current.push_str(" reused"); }
        if transfer.attempt != 0 && !transfer.cachehit {
            current.push_str(&format!(" · attempt {}", count(u64::from(transfer.attempt))));
        }
        if transfer.resumed {
            current.push_str(&format!(" · {} retained", size(transfer.retainedbytes)));
        }
        details.push(current);
    }
    if source.retrycount != 0 { details.push(format!("{} retries", count(source.retrycount))); }
    if source.resumed && !transfer.is_some_and(|transfer| transfer.resumed) {
        details.push("Resumed".into());
    }
    if source.invalidatedimages != 0 {
        details.push(format!("{} images invalidated", count(source.invalidatedimages)));
    }
    details.join(" · ")
}

fn metrics(progress: &crate::generated::ArtifactProgress) -> String {
    [
        (progress.elapsedseconds != 0).then(|| format!("{}s elapsed", count(progress.elapsedseconds))),
        (progress.remainingseconds != 0).then(|| format!("{}s remaining", count(progress.remainingseconds))),
        (progress.throughputpersecond != 0).then(|| format!("{}/s", count(progress.throughputpersecond))),
        (progress.projectedoutputbytes != 0).then(|| format!("{} projected", size(progress.projectedoutputbytes))),
        (progress.droppedinstances != 0).then(|| format!("{} instances dropped", count(progress.droppedinstances))),
        (progress.quarantinedimages != 0).then(|| format!("{} images quarantined", count(progress.quarantinedimages))),
    ].into_iter().flatten().collect::<Vec<_>>().join(" · ")
}

pub(crate) fn view<Message: 'static>(state: Option<&ArtifactUiState>) -> Element<'static, Message> {
    let mut body = column![].spacing(6).width(Fill);
    if let Some(state) = state {
        let title = heading(state);
        if !title.is_empty() { body = body.push(text(title).size(12)); }
        if show_tracks(state) {
            let progress = &state.progress;
            if !progress.activity.is_empty() { body = body.push(text(progress.activity.clone()).size(12)); }
            let facts = metrics(progress);
            if !facts.is_empty() { body = body.push(text(facts).size(12)); }
            for (index, name, fact) in [
                (0, "Acquisition", &progress.tracks.acquisition),
                (1, "Labels/masks", &progress.tracks.labels),
                (2, "Pixels", &progress.tracks.pixels),
            ] {
                body = body.push(track(TRACK_IDS[index], name, fact, index == 0));
            }
            for source in &progress.sources {
                let mut row = column![text(source_heading(source)).size(12), text(source_summary(source)).size(12)]
                    .spacing(2).width(Fill);
                if !source.complete && !source.activity.is_empty() {
                    row = row.push(text(source.activity.clone()).size(12));
                }
                let detail = source_details(source);
                if !detail.is_empty() { row = row.push(text(detail).size(12)); }
                body = body.push(container(row).id(source_identity(source.source).1).width(Fill));
            }
        } else if !state.active {
            for detail in [&state.terminal.detail, &state.terminal.artifact] {
                if !detail.is_empty() { body = body.push(text(detail.clone()).size(12)); }
            }
        }
    }
    crate::view::shared::disclosure(AREA_ID, true, body)
        .reserve(state.filter(|state| state.active).map(|state| state.generation)).into()
}

#[cfg(test)]
mod tests;
