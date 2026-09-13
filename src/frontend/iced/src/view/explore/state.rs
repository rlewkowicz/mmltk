use crate::generated::{ExploreFilterUpdate, ExploreImageMetadata, ExploreSnapshot, ExploreViewportUpdate, VisualExtent};
use std::sync::{Arc, Mutex};

#[derive(Debug, Clone, PartialEq)]
pub(super) struct GallerySource {
    dataset: u64,
    frame: crate::generated::VisualFrame,
}

impl GallerySource {
    pub(super) fn current(snapshot: Option<&ExploreSnapshot>) -> Option<Self> {
        snapshot
            .filter(|value| value.ready && value.mode == crate::generated::ExploreMode::Gallery)
            .map(|value| Self {
                dataset: value.dataset.identity,
                frame: value.frame.clone(),
            })
    }

    fn hit(
        &self,
        shown: &ExploreImageMetadata,
        sample: crate::presentation_surface::SurfaceSample,
    ) -> Option<Option<u32>> {
        (shown.mode == crate::generated::ExploreMode::Gallery
            && shown.dataset.identity == self.dataset && shown.frame == self.frame)
            .then(|| selected_at(Some(shown), sample))
    }
}

#[derive(Debug, Default)]
pub(super) struct GalleryHover {
    focused: Mutex<Option<Option<u32>>>,
}

/// A hit stays attached to the source and component incarnation that sampled it.
#[derive(Debug, Clone)]
pub struct GalleryInput {
    owner: Arc<GalleryHover>,
    source: Option<GallerySource>,
    selected: Option<u32>,
    kind: crate::presentation_surface::SurfaceGestureKind,
    pressed: bool,
}

impl GalleryHover {
    pub(super) fn capture(
        self: &Arc<Self>,
        source: Option<&GallerySource>,
        shown: Option<&ExploreImageMetadata>,
        gesture: crate::presentation_surface::SurfaceGesture,
        focus_ready: bool,
    ) -> Option<GalleryInput> {
        let hit = source.and_then(|source| source.hit(shown?, gesture.sample));
        if gesture.kind == crate::presentation_surface::SurfaceGestureKind::Viewport {
            let selected = hit.filter(|_| focus_ready)?;
            let mut focused = self.focused.lock().expect("gallery local focus");
            if *focused == Some(selected) {
                return None;
            }
            *focused = Some(selected);
        }
        Some(GalleryInput {
            owner: self.clone(),
            source: hit.and(source.cloned()),
            selected: hit.flatten(),
            kind: gesture.kind,
            pressed: gesture.sample.pressed,
        })
    }
}

#[cfg(test)]
use crate::generated::EXPLORE_VISIBLE_ITEM_CAPACITY as VISIBLE_ITEM_CAPACITY;

#[derive(Debug, Clone)]
struct SubmittedFilter {
    request: ExploreFilterUpdate,
    admission_revision: Option<u64>,
}

#[derive(Debug, Clone, PartialEq)]
struct MeasuredGallery {
    logical_width: f32,
    logical_height: f32,
    maximum_extent: VisualExtent,
    columns: u32,
    logical_first_row: Option<u32>,
    row_fraction: f32,
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct LogicalGalleryGeometry {
    first_row: u32,
    row_count: u32,
    total_rows: u32,
    row_extent: f32,
    virtual_height: f32,
}

impl LogicalGalleryGeometry {
    fn new(width: f32, height: f32, columns: u32, matching_count: u32, first_row: u32) -> Self {
        Self::at_fraction(width, height, columns, matching_count, first_row, 0.0)
    }

    fn at_fraction(
        width: f32,
        height: f32,
        columns: u32,
        matching_count: u32,
        first_row: u32,
        fraction: f32,
    ) -> Self {
        let width = measured_dimension(width);
        let height = measured_dimension(height);
        let columns = columns.max(1);
        let total_rows = matching_count.div_ceil(columns).max(1);
        let first_row = first_row.min(total_rows - 1);
        let row_extent = width / columns as f32;
        let visible_rows = (height / row_extent + fraction).ceil().max(1.0) as u32;
        let row_count = visible_rows.max(1).min(total_rows - first_row).max(1);
        Self {
            first_row,
            row_count,
            total_rows,
            row_extent,
            virtual_height: matching_count.div_ceil(columns) as f32 * row_extent,
        }
    }

    pub(crate) fn virtual_height(&self) -> f32 {
        self.virtual_height
    }

    pub(crate) fn first_row_for_offset(&self, offset: f32) -> u32 {
        ((offset.max(0.0) / self.row_extent).floor() as u32).min(self.total_rows - 1)
    }

    pub(crate) fn row_extent(&self) -> f32 {
        self.row_extent
    }

    pub(crate) fn row_count(&self) -> u32 {
        self.row_count
    }
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct GalleryGeometry {
    viewport: crate::generated::ExploreViewport,
    card_extent: u32,
}

impl GalleryGeometry {
    #[cfg(test)]
    fn new(
        logical_width: f32,
        logical_height: f32,
        maximum_extent: VisualExtent,
        columns: u32,
        matching_count: u32,
        first_row: u32,
    ) -> Option<Self> {
        Self::at_fraction(
            logical_width,
            logical_height,
            maximum_extent,
            columns,
            matching_count,
            first_row,
            0.0,
        )
    }

    fn at_fraction(
        logical_width: f32,
        logical_height: f32,
        maximum_extent: VisualExtent,
        columns: u32,
        matching_count: u32,
        first_row: u32,
        fraction: f32,
    ) -> Option<Self> {
        if maximum_extent.width == 0 || maximum_extent.height == 0 {
            return None;
        }
        let logical_width = measured_dimension(logical_width);
        let logical_height = measured_dimension(logical_height);
        let columns = columns.max(1);
        let logical = LogicalGalleryGeometry::at_fraction(
            logical_width,
            logical_height,
            columns,
            matching_count,
            first_row,
            fraction,
        );
        let firstrow = logical.first_row;
        let rowcount = logical.row_count;
        // Reserve the possible partially visible row before choosing raster
        // resolution. Scrolling changes demand, not the size of cached pixels.
        let raster_rows = ((logical_height / logical.row_extent).ceil() as u32)
            .saturating_add(1)
            .min(logical.total_rows)
            .max(1);
        let logical_native_width = logical_width.floor() as u32;
        let card_extent = (logical_native_width / columns)
            .max(1)
            .min(maximum_extent.width / columns)
            .min(maximum_extent.height / raster_rows)
            .max(1);
        let width = card_extent.checked_mul(columns)?;
        let height = card_extent.checked_mul(rowcount)?;
        Some(Self {
            viewport: crate::generated::ExploreViewport {
                extent: VisualExtent { width, height },
                firstrow,
                rowcount,
                columns,
            },
            card_extent,
        })
    }

    pub(crate) fn viewport(&self) -> &crate::generated::ExploreViewport {
        &self.viewport
    }
}

#[derive(Debug, Default)]
pub struct State {
    dataset_identity: u64,
    pub(super) fit_revision: u64,
    detail_view: std::cell::Cell<Option<DetailView>>,
    submitted_filter: Option<SubmittedFilter>,
    sent_viewport: Option<ExploreViewportUpdate>,
    desired_viewport: Option<ExploreViewportUpdate>,
    viewport_writable_wait: bool,
    measured_gallery: Option<MeasuredGallery>,
    pressed_image: Option<u32>,
    pub(super) gallery_hover: Arc<GalleryHover>,
}

#[derive(Debug, Clone, Copy)]
struct DetailView {
    identity: Option<(u64, u32)>,
    original: bool,
    chosen: bool,
}

impl State {
    pub(crate) fn detail_original(&self, content: &crate::presentation_surface::DetailContent) -> bool {
        let identity = content.viewer_identity();
        let mut view = self.detail_view.get().filter(|view| view.identity == identity)
            .unwrap_or(DetailView { identity, original: content.original_dimensions(), chosen: false });
        if !view.chosen { view.original = content.original_dimensions(); }
        self.detail_view.set(Some(view));
        view.original
    }

    pub(crate) fn choose_detail_original(&mut self, original: bool) {
        if let Some(mut view) = self.detail_view.get() {
            view.original = original;
            view.chosen = true;
            self.detail_view.set(Some(view));
        }
    }

    pub fn abandon_detail(&mut self) {
        self.detail_view.set(None);
    }

    pub fn rebase(&mut self, snapshot: Option<&ExploreSnapshot>, bootstrap: bool) {
        if let Some(identity) = snapshot
            .map(|snapshot| snapshot.dataset.identity)
            .filter(|identity| *identity != 0 && *identity != self.dataset_identity)
        {
            self.dataset_identity = identity;
            self.measured_gallery = None;
            self.clear_viewport_admission();
        }
        if bootstrap {
            self.abandon_detail();
            self.submitted_filter = None;
            self.clear_viewport_admission();
            if let Some(measured) = self.measured_gallery.as_mut() {
                measured.logical_first_row = None;
                measured.row_fraction = 0.0;
            }
        } else if let Some(snapshot) = snapshot {
            if self.submitted_filter.as_ref().is_some_and(|submitted| {
                !snapshot.busy
                    && snapshot.filter == submitted.request.filter
                    && snapshot.overlay == submitted.request.overlay
                    && submitted
                        .admission_revision
                        .is_some_and(|revision| snapshot.revision >= revision)
            }) {
                self.submitted_filter = None;
            }
            let committed = snapshot
                .viewportresult
                .as_ref()
                .filter(|result| result.outcome != crate::generated::ExploreViewportOutcome::Ready)
                .map(|result| result.request.clone())
                .unwrap_or_else(|| Self::committed_viewport(snapshot));
            if self.sent_viewport.as_ref() == Some(&committed) {
                self.sent_viewport = None;
            }
            if self.sent_viewport.is_none() && Some(&committed) == self.desired_viewport.as_ref() {
                self.desired_viewport = None;
            }
        }
        if !bootstrap {
            self.reconcile_gallery_hover(snapshot);
        }
        self.pressed_image = None;
    }

    pub fn record_submission(&mut self, request: ExploreFilterUpdate) {
        self.submitted_filter = Some(SubmittedFilter {
            request,
            admission_revision: None,
        });
    }

    pub fn record_admission(&mut self, revision: u64) {
        if let Some(submitted) = self.submitted_filter.as_mut() {
            submitted.admission_revision = Some(revision);
        }
    }

    pub fn abandon_submission(&mut self) {
        self.submitted_filter = None;
    }

    pub fn presented_filter(
        &self,
        snapshot: Option<&ExploreSnapshot>,
    ) -> Option<ExploreFilterUpdate> {
        self.submitted_filter
            .as_ref()
            .map(|value| value.request.clone())
            .or_else(|| {
                snapshot
                    .filter(|value| value.ready)
                    .map(|value| ExploreFilterUpdate {
                        filter: value.filter.clone(),
                        overlay: value.overlay.clone(),
                    })
            })
    }

    fn committed_viewport(snapshot: &ExploreSnapshot) -> ExploreViewportUpdate {
        ExploreViewportUpdate {
            viewport: snapshot.viewport.clone(),
        }
    }

    pub fn request_viewport(
        &mut self,
        snapshot: Option<&ExploreSnapshot>,
        request: ExploreViewportUpdate,
    ) {
        if let Some(measured) = self.measured_gallery.as_mut() {
            measured.logical_first_row = Some(request.viewport.firstrow);
        }
        if self.sent_viewport.as_ref() == Some(&request)
            || (self.sent_viewport.is_none()
                && snapshot
                    .filter(|value| value.ready)
                    .map(|snapshot| {
                        snapshot
                            .viewportresult
                            .as_ref()
                            .filter(|result| {
                                result.outcome != crate::generated::ExploreViewportOutcome::Ready
                            })
                            .map(|result| result.request.clone())
                            .unwrap_or_else(|| Self::committed_viewport(snapshot))
                    })
                    .as_ref()
                    == Some(&request))
        {
            self.desired_viewport = None;
        } else {
            self.desired_viewport = Some(request);
        }
        self.reconcile_gallery_hover(snapshot);
    }

    pub fn dispatchable_viewport(&self) -> Option<ExploreViewportUpdate> {
        self.desired_viewport.clone()
    }

    pub fn viewport_queued(&mut self, request: ExploreViewportUpdate) {
        if self.desired_viewport.as_ref() == Some(&request) {
            self.desired_viewport = None;
        }
        self.sent_viewport = Some(request);
        self.viewport_writable_wait = false;
    }

    pub fn arm_viewport_writable_wait(&mut self) -> bool {
        if self.viewport_writable_wait {
            return false;
        }
        self.viewport_writable_wait = true;
        true
    }

    pub fn viewport_writable(&mut self) {
        self.viewport_writable_wait = false;
    }

    pub fn clear_viewport_admission(&mut self) {
        self.sent_viewport = None;
        self.desired_viewport = None;
        self.viewport_writable_wait = false;
        self.gallery_hover = Arc::default();
        self.pressed_image = None;
    }

    pub fn measure_gallery(
        &mut self,
        width: f32,
        height: f32,
        maximum_extent: VisualExtent,
        columns: u32,
    ) -> bool {
        let measurement = MeasuredGallery {
            logical_width: measured_dimension(width),
            logical_height: measured_dimension(height),
            maximum_extent,
            columns: columns.max(1),
            row_fraction: self
                .measured_gallery
                .as_ref()
                .map_or(0.0, |value| value.row_fraction),
            logical_first_row: self
                .measured_gallery
                .as_ref()
                .and_then(|measured| measured.logical_first_row),
        };
        if self.measured_gallery.as_ref() == Some(&measurement) {
            return false;
        }
        self.measured_gallery = Some(measurement);
        *self
            .gallery_hover
            .focused
            .lock()
            .expect("gallery local focus") = None;
        true
    }

    pub fn measured_viewport(
        &self,
        columns: u32,
        first_row: u32,
        matching_count: u32,
    ) -> Option<crate::generated::ExploreViewport> {
        let measured = self.measured_gallery.as_ref()?;
        GalleryGeometry::at_fraction(
            measured.logical_width,
            measured.logical_height,
            measured.maximum_extent.clone(),
            columns,
            matching_count,
            first_row,
            measured.row_fraction,
        )
        .map(|geometry| geometry.viewport)
    }

    pub fn measured_layout_request(
        &self,
        snapshot: Option<&ExploreSnapshot>,
        columns: u32,
        matching_count: u32,
    ) -> Option<ExploreViewportUpdate> {
        let latest = self
            .desired_viewport
            .clone()
            .or_else(|| self.sent_viewport.clone())
            .or_else(|| {
                snapshot
                    .filter(|value| value.ready)
                    .map(Self::committed_viewport)
            })?;
        let first_row = self
            .measured_gallery
            .as_ref()
            .and_then(|measured| measured.logical_first_row)
            .unwrap_or(latest.viewport.firstrow);
        Some(ExploreViewportUpdate {
            viewport: self.measured_viewport(columns, first_row, matching_count)?,
        })
    }

    pub(crate) fn gallery_row_extent(&self, columns: u32) -> f32 {
        self.measured_gallery
            .as_ref()
            .map_or(1.0, |value| value.logical_width / columns.max(1) as f32)
    }

    pub(crate) fn gallery_size(&self) -> Option<iced::Size> {
        self.measured_gallery
            .as_ref()
            .map(|value| iced::Size::new(value.logical_width, value.logical_height))
    }

    pub(crate) fn gallery_row_fraction(&self) -> f32 {
        self.measured_gallery
            .as_ref()
            .map_or(0.0, |value| value.row_fraction)
    }

    pub(crate) fn record_gallery_fraction(&mut self, fraction: f32) {
        if let Some(measured) = self.measured_gallery.as_mut() {
            measured.row_fraction = fraction;
        }
    }

    pub(crate) fn record_gallery_scroll(&mut self, first_row: u32) {
        if let Some(measured) = self.measured_gallery.as_mut() {
            measured.logical_first_row = Some(first_row);
        }
    }

    pub(crate) fn gallery_first_row(&self, fallback: u32) -> u32 {
        self.measured_gallery
            .as_ref()
            .and_then(|measured| measured.logical_first_row)
            .unwrap_or(fallback)
    }

    fn reconcile_gallery_hover(&self, snapshot: Option<&ExploreSnapshot>) {
        let mut focused = self.gallery_hover.focused.lock().expect("gallery local focus");
        if !snapshot.is_some_and(|snapshot| {
            GallerySource::current(Some(snapshot)).is_some()
                && (*focused).is_none_or(|index| index.is_none_or(|index| snapshot.order.visibleindices.contains(&index)))
        }) {
            *focused = None;
        }
    }

    pub(crate) fn gallery_input(
        &mut self,
        snapshot: Option<&ExploreSnapshot>,
        input: GalleryInput,
    ) -> Option<GalleryGestureOutcome> {
        if !Arc::ptr_eq(&input.owner, &self.gallery_hover) {
            return None;
        }
        let source = input
            .source
            .filter(|source| GallerySource::current(snapshot).as_ref() == Some(source));
        let selected = source.as_ref().and(input.selected);
        match input.kind {
            crate::presentation_surface::SurfaceGestureKind::Pointer => {
                if !input.pressed {
                    self.pressed_image = None;
                    return None;
                }
                if selected == self.pressed_image {
                    return None;
                }
                self.pressed_image = selected;
                selected.map(GalleryGestureOutcome::Selected)
            }
            crate::presentation_surface::SurfaceGestureKind::End
            | crate::presentation_surface::SurfaceGestureKind::Cancel => {
                self.pressed_image = None;
                None
            }
            crate::presentation_surface::SurfaceGestureKind::Viewport => {
                source.map(|_| GalleryGestureOutcome::Focused(selected))
            }
        }
    }

    #[cfg(test)]
    pub(crate) fn gallery_gesture(
        &mut self,
        snapshot: Option<&ExploreSnapshot>,
        displayed_snapshot: Option<&ExploreSnapshot>,
        gesture: crate::presentation_surface::SurfaceGesture,
    ) -> Option<GalleryGestureOutcome> {
        let source = GallerySource::current(snapshot);
        let hit = source
            .as_ref()
            .and_then(|source| source.hit(&ExploreImageMetadata::from(displayed_snapshot?), gesture.sample));
        self.gallery_input(
            snapshot,
            GalleryInput {
                owner: self.gallery_hover.clone(),
                source: hit.and(source),
                selected: hit.flatten(),
                kind: gesture.kind,
                pressed: gesture.sample.pressed,
            },
        )
    }
}

#[cfg(test)]
pub(crate) fn gallery_geometry(
    width: f32,
    height: f32,
    maximum_extent: VisualExtent,
    columns: u32,
    matching_count: u32,
    first_row: u32,
) -> Option<GalleryGeometry> {
    GalleryGeometry::new(
        width,
        height,
        maximum_extent,
        columns,
        matching_count,
        first_row,
    )
}

pub(crate) fn gallery_geometry_at_fraction(
    width: f32,
    height: f32,
    maximum_extent: VisualExtent,
    columns: u32,
    matching_count: u32,
    first_row: u32,
    fraction: f32,
) -> Option<GalleryGeometry> {
    GalleryGeometry::at_fraction(
        width,
        height,
        maximum_extent,
        columns,
        matching_count,
        first_row,
        fraction,
    )
}

pub(crate) fn logical_gallery_geometry(
    width: f32,
    height: f32,
    columns: u32,
    matching_count: u32,
    first_row: u32,
) -> LogicalGalleryGeometry {
    LogicalGalleryGeometry::new(width, height, columns, matching_count, first_row)
}

fn measured_dimension(value: f32) -> f32 {
    if value.is_finite() {
        value.max(1.0)
    } else {
        1.0
    }
}

pub(super) fn selected_at(
    snapshot: Option<&ExploreImageMetadata>,
    sample: crate::presentation_surface::SurfaceSample,
) -> Option<u32> {
    let snapshot = snapshot?;
    if snapshot.mode != crate::generated::ExploreMode::Gallery
        || snapshot.gallery.layout.columns == 0
        || snapshot.gallery.layout.cardextent == 0
        || snapshot.gallery.layout.rowcount == 0
    {
        return None;
    }
    let layout = &snapshot.gallery.layout;
    let column = sample.content_x as u32 / layout.cardextent;
    let row = sample.content_y as u32 / layout.cardextent;
    if column >= layout.columns || row >= layout.rowcount {
        return None;
    }
    let slot = row.checked_mul(layout.columns)?.checked_add(column)? as usize;
    // A displayed placeholder already identifies its image. Readiness governs
    // completed pixels and labels, independently of selection and hover focus.
    snapshot.order.visibleindices.get(slot).copied()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum GalleryGestureOutcome {
    Selected(u32),
    Focused(Option<u32>),
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view_model::test_support::explore_snapshot;

    fn capacity(width: u32, height: u32) -> VisualExtent {
        VisualExtent { width, height }
    }

    fn displayed_gallery_snapshot() -> ExploreSnapshot {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.mode = crate::generated::ExploreMode::Gallery;
        snapshot.viewport.extent = capacity(400, 200);
        snapshot.viewport.columns = 4;
        snapshot.viewport.rowcount = 2;
        snapshot.frame.extent = snapshot.viewport.extent.clone();
        snapshot.order.visibleindices = vec![10, 11, 12, 13, 20, 21, 22, 23];
        snapshot.gallery.slots = vec![true; 8];
        crate::view_model::test_support::gallery_layout(&mut snapshot);
        snapshot
    }

    fn measured_unfocused_gallery() -> (State, ExploreSnapshot) {
        let mut snapshot = displayed_gallery_snapshot();
        snapshot.order.matchingcount = 100;
        let mut state = State::default();
        state.rebase(Some(&snapshot), false);
        state.measure_gallery(400.0, 200.0, capacity(400, 300), 4);
        (state, snapshot)
    }

    fn gallery_sample(x: u32, y: u32) -> crate::presentation_surface::SurfaceSample {
        crate::presentation_surface::SurfaceSample {
            width: 400,
            height: 200,
            x,
            y,
            content_x: x as f32,
            content_y: y as f32,
            pressed: true,
        }
    }

    fn local_focus(state: &State, snapshot: &ExploreSnapshot) -> Option<GalleryInput> {
        state.gallery_hover.capture(
            GallerySource::current(Some(snapshot)).as_ref(),
            Some(&ExploreImageMetadata::from(snapshot)),
            crate::presentation_surface::SurfaceGesture {
                kind: crate::presentation_surface::SurfaceGestureKind::Viewport,
                sample: gallery_sample(150, 150),
            },
            state
                .measured_layout_request(Some(snapshot), 4, snapshot.order.matchingcount)
                .is_some(),
        )
    }

    fn accept_local_focus(state: &mut State, snapshot: &ExploreSnapshot, input: GalleryInput) {
        assert!(super::super::gallery::update(
            state, Some(snapshot), &mut crate::view::settings::SettingsModel::default(),
            super::super::gallery::Message::Surface(input),
        ).unwrap().is_none());
    }

    #[test]
    fn scroll_demand_preserves_partial_rows_and_selection_at_different_pointer_positions() {
        for (x, y, selected) in [(50, 50, 10), (350, 150, 23)] {
            let (mut state, snapshot) = measured_unfocused_gallery();
            let input = state.gallery_hover.capture(
                GallerySource::current(Some(&snapshot)).as_ref(),
                Some(&ExploreImageMetadata::from(&snapshot)),
                crate::presentation_surface::SurfaceGesture {
                    kind: crate::presentation_surface::SurfaceGestureKind::Viewport,
                    sample: gallery_sample(x, y),
                },
                true,
            ).unwrap();
            assert_eq!(input.selected, Some(selected));
            accept_local_focus(&mut state, &snapshot, input);
            for (first_row, fraction) in [(6, 0.5), (5, 0.0), (6, 0.5)] {
                state.record_gallery_fraction(fraction);
                let request = ExploreViewportUpdate {
                    viewport: state.measured_viewport(4, first_row, 100).unwrap(),
                };
                let result = super::super::gallery::update(
                    &mut state, Some(&snapshot), &mut crate::view::settings::SettingsModel::default(),
                    super::super::gallery::Message::Scrolled {
                        first_row, row_fraction: fraction, request: Some(request.clone()),
                    },
                ).unwrap();
                let Some(super::super::gallery::Outcome::ViewportChanged(changed)) = result else {
                    panic!("scroll must submit measured viewport demand");
                };
                assert_eq!(changed, request);
                assert_eq!(changed.viewport.firstrow, first_row);
                assert_eq!(changed.viewport.rowcount, if fraction == 0.0 { 2 } else { 3 });
                assert_eq!(state.gallery_first_row(0), first_row);
            }
        }
    }

    #[test]
    fn local_hover_retains_placeholder_identity_across_image_completion() {
        for ready in [false, true] {
            let (mut state, mut snapshot) = measured_unfocused_gallery();
            snapshot.gallery.slots.fill(ready);
            let input = local_focus(&state, &snapshot).unwrap();
            assert_eq!(input.selected, Some(21));
            accept_local_focus(&mut state, &snapshot, input);
            assert!(local_focus(&state, &snapshot).is_none());
            snapshot.revision += 1;
            snapshot.gallery.slots.fill(true);
            state.rebase(Some(&snapshot), false);
            assert!(local_focus(&state, &snapshot).is_none());
        }
    }

    #[test]
    fn row_return_and_failed_admission_preserve_local_selection() {
        let (mut state, mut snapshot) = measured_unfocused_gallery();
        let input = local_focus(&state, &snapshot).unwrap();
        accept_local_focus(&mut state, &snapshot, input);
        let mut away = State::committed_viewport(&snapshot);
        away.viewport.firstrow = 10;
        state.request_viewport(Some(&snapshot), away.clone());
        state.viewport_queued(away.clone());
        snapshot.viewportresult = Some(crate::generated::ExploreViewportResult {
            request: away.clone(),
            outcome: crate::generated::ExploreViewportOutcome::AtlasExtentExceeded,
        });
        state.rebase(Some(&snapshot), false);
        assert!(state.sent_viewport.is_none());
        assert!(local_focus(&state, &snapshot).is_none());
        snapshot.viewportresult = None;
        snapshot.viewport = away.viewport;
        snapshot.order.visibleindices = (40..48).collect();
        state.rebase(Some(&snapshot), false);
        snapshot.viewport.firstrow = 0;
        snapshot.order.visibleindices = vec![10, 11, 12, 13, 20, 21, 22, 23];
        state.rebase(Some(&snapshot), false);
        let input = local_focus(&state, &snapshot).unwrap();
        assert_eq!(input.selected, Some(21));
        accept_local_focus(&mut state, &snapshot, input);
    }

    #[test]
    fn captured_hits_keep_source_identity_and_bootstrap_invalidates_old_callbacks() {
        let (mut state, snapshot) = measured_unfocused_gallery();
        let input = local_focus(&state, &snapshot).unwrap();
        let old_callback = state.gallery_hover.clone();
        state.rebase(Some(&snapshot), true);
        assert_eq!(state.gallery_input(Some(&snapshot), input), None);
        assert!(!Arc::ptr_eq(&old_callback, &state.gallery_hover));
        let input = local_focus(&state, &snapshot).unwrap();
        let mut changed = snapshot.clone();
        changed.order.visibleindices.reverse();
        // A delayed message carries its hit, not coordinates to reinterpret
        // against whichever displayed directory happens to exist at reduction.
        assert_eq!(
            state.gallery_input(Some(&changed), input),
            Some(GalleryGestureOutcome::Focused(Some(21)))
        );
        state.clear_viewport_admission();
        let input = local_focus(&state, &snapshot).unwrap();
        changed.frame.revision += 1;
        assert_eq!(state.gallery_input(Some(&changed), input), None);
        let source = GallerySource::current(Some(&snapshot));
        let gesture = crate::presentation_surface::SurfaceGesture {
            kind: crate::presentation_surface::SurfaceGestureKind::Viewport,
            sample: gallery_sample(150, 150),
        };
        assert!(
            state
                .gallery_hover
                .capture(source.as_ref(), Some(&ExploreImageMetadata::from(&changed)), gesture, true)
                .is_none()
        );
        changed = snapshot.clone();
        changed.dataset.identity += 1;
        assert!(
            state
                .gallery_hover
                .capture(source.as_ref(), Some(&ExploreImageMetadata::from(&changed)), gesture, true)
                .is_none()
        );
        assert!(
            state
                .gallery_hover
                .capture(source.as_ref(), None, gesture, true)
                .is_none()
        );
    }

    fn submit_toggled_labels(state: &mut State, snapshot: &ExploreSnapshot) -> ExploreFilterUpdate {
        let mut request = ExploreFilterUpdate {
            filter: snapshot.filter.clone(),
            overlay: snapshot.overlay.clone(),
        };
        request.overlay.showlabels = !request.overlay.showlabels;
        state.record_submission(request.clone());
        request
    }

    struct ReturnedCursor {
        snapshot: ExploreSnapshot,
        committed: ExploreViewportUpdate,
        transported: ExploreViewportUpdate,
        state: State,
    }

    fn returned_cursor() -> ReturnedCursor {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.viewport.firstrow = 1;
        let committed = State::committed_viewport(&snapshot);
        let mut transported = committed.clone();
        transported.viewport.firstrow = 2;
        let mut state = State::default();
        state.request_viewport(Some(&snapshot), transported.clone());
        state.viewport_queued(transported.clone());
        state.request_viewport(Some(&snapshot), committed.clone());
        ReturnedCursor {
            snapshot,
            committed,
            transported,
            state,
        }
    }

    #[test]
    fn one_row_dataset_preserves_the_measured_open_viewport() {
        for width in [640.0, 894.0, 1200.0] {
            let mut state = State::default();
            assert!(state.measure_gallery(width, 720.0, capacity(2048, 2048), 3));
            let opening = state.measured_viewport(3, 0, 0).unwrap();
            assert_eq!(opening.rowcount, 1);
            assert_eq!(opening.firstrow, 0);
            assert_eq!(opening.extent.width, opening.extent.height * 3);
            for images in 1..=3 {
                assert_eq!(state.measured_viewport(3, 0, images).unwrap(), opening);
            }
        }
    }

    #[test]
    fn viewport_admission_coalesces_and_releases_after_commit() {
        let mut state = State::default();
        assert!(state.measure_gallery(640.0, 480.0, capacity(640, 480), 4));
        assert!(!state.measure_gallery(640.0, 480.0, capacity(640, 480), 4));
        let viewport = state.measured_viewport(4, 2, 100).unwrap();
        let request = ExploreViewportUpdate {
            viewport,
        };
        state.request_viewport(None, request.clone());
        assert_eq!(state.dispatchable_viewport(), Some(request.clone()));
        state.viewport_queued(request.clone());
        assert!(state.dispatchable_viewport().is_none());

        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.viewport = request.viewport;
        state.rebase(Some(&snapshot), false);
        assert!(state.dispatchable_viewport().is_none());
    }

    #[test]
    fn native_capacity_refusal_acknowledges_the_request_without_losing_newer_geometry() {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        let rejected = ExploreViewportUpdate {
            viewport: gallery_geometry_at_fraction(
                100.0,
                30_000.0,
                capacity(8192, 8192),
                1,
                VISIBLE_ITEM_CAPACITY + 1,
                0,
                0.5,
            )
            .unwrap()
            .viewport,
        };
        let mut state = State::default();
        state.request_viewport(Some(&snapshot), rejected.clone());
        state.viewport_queued(rejected.clone());
        snapshot.viewportresult = Some(crate::generated::ExploreViewportResult {
            request: rejected.clone(),
            outcome: crate::generated::ExploreViewportOutcome::VisibleCapacityExceeded,
        });
        state.rebase(Some(&snapshot), false);
        state.request_viewport(Some(&snapshot), rejected);
        assert!(state.sent_viewport.is_none());
        assert!(state.dispatchable_viewport().is_none());
        let restored = ExploreViewportUpdate {
            viewport: snapshot.viewport.clone(),
        };
        state.request_viewport(Some(&snapshot), restored.clone());
        assert_eq!(state.dispatchable_viewport(), Some(restored));
    }

    #[test]
    fn writable_wait_is_bounded_to_one_registration() {
        let mut state = State::default();
        assert!(state.arm_viewport_writable_wait());
        assert!(!state.arm_viewport_writable_wait());
        state.viewport_writable();
        assert!(state.arm_viewport_writable_wait());
    }

    #[test]
    fn saturated_transport_retains_only_the_newest_unsent_cursor() {
        let make = |firstrow| ExploreViewportUpdate {
            viewport: crate::generated::ExploreViewport {
                firstrow,
                ..crate::generated::default_request_exploreUpdateViewportviewport().unwrap()
            },
        };
        let mut state = State::default();
        state.request_viewport(None, make(1));
        assert!(state.arm_viewport_writable_wait());
        state.request_viewport(None, make(2));
        state.request_viewport(None, make(3));
        assert_eq!(state.dispatchable_viewport(), Some(make(3)));
        assert!(!state.arm_viewport_writable_wait());
        state.viewport_writable();
        assert_eq!(state.dispatchable_viewport(), Some(make(3)));
    }

    #[test]
    fn successful_enqueue_does_not_fence_the_latest_desired_viewport() {
        let make = |firstrow| ExploreViewportUpdate {
            viewport: crate::generated::ExploreViewport {
                firstrow,
                ..crate::generated::default_request_exploreUpdateViewportviewport().unwrap()
            },
        };
        let mut state = State::default();
        state.request_viewport(None, make(1));
        state.viewport_queued(make(1));
        state.request_viewport(None, make(2));
        state.request_viewport(None, make(3));
        assert_eq!(state.dispatchable_viewport(), Some(make(3)));
    }

    #[test]
    fn return_to_committed_cursor_supersedes_a_different_transport_accepted_cursor() {
        let returned = returned_cursor();
        assert_eq!(
            returned.state.dispatchable_viewport(),
            Some(returned.committed)
        );
    }

    #[test]
    fn partial_committed_frames_preserve_return_until_sent_cursor_arrives() {
        let ReturnedCursor {
            mut snapshot,
            committed,
            transported,
            mut state,
        } = returned_cursor();

        for revision in 1..=3 {
            snapshot.frame.revision = revision;
            snapshot.revision = revision;
            state.rebase(Some(&snapshot), false);
            assert_eq!(state.dispatchable_viewport(), Some(committed.clone()));
        }

        snapshot.viewport = transported.viewport;
        snapshot.frame.revision = 4;
        snapshot.revision = 4;
        state.rebase(Some(&snapshot), false);
        let final_request = state.dispatchable_viewport().unwrap();
        assert_eq!(final_request, committed);
        state.viewport_queued(final_request);
        assert!(state.dispatchable_viewport().is_none());
    }

    #[test]
    fn acknowledgement_preserves_newer_desired_and_measurement_merges_it() {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.order.matchingcount = 100;
        let mut state = State::default();
        assert!(state.measure_gallery(600.0, 420.0, capacity(600, 420), 4));
        let first = ExploreViewportUpdate {
            viewport: state.measured_viewport(4, 1, 100).unwrap(),
        };
        let latest = ExploreViewportUpdate {
            viewport: state.measured_viewport(4, 3, 100).unwrap(),
        };
        state.request_viewport(Some(&snapshot), first.clone());
        state.viewport_queued(first.clone());
        state.request_viewport(Some(&snapshot), latest.clone());
        snapshot.viewport = first.viewport;
        state.rebase(Some(&snapshot), false);
        assert_eq!(state.dispatchable_viewport(), Some(latest.clone()));
        assert!(state.measure_gallery(620.0, 430.0, capacity(620, 430), 4));
        let merged = state
            .measured_layout_request(Some(&snapshot), 4, 100)
            .unwrap();
        assert_eq!(merged.viewport.firstrow, latest.viewport.firstrow);
        assert_eq!(merged.viewport.extent.width % merged.viewport.columns, 0);
        assert!(merged.viewport.extent.width <= 620);
    }

    #[test]
    fn repeated_partial_frames_do_not_erase_a_newer_cursor() {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        let first = ExploreViewportUpdate {
            viewport: snapshot.viewport.clone(),
        };
        let mut latest = first.clone();
        latest.viewport.firstrow = 9;
        let mut state = State::default();
        state.request_viewport(Some(&snapshot), first.clone());
        state.viewport_queued(first.clone());
        state.request_viewport(Some(&snapshot), latest.clone());
        for revision in 1..=3 {
            snapshot.viewport = first.viewport.clone();
            snapshot.frame.revision = revision;
            snapshot.revision = revision;
            state.rebase(Some(&snapshot), false);
            assert_eq!(state.dispatchable_viewport(), Some(latest.clone()));
        }
    }

    #[test]
    fn fractional_windows_submit_every_intersecting_row_even_over_capacity() {
        let maximum = capacity(8192, 8192);
        let aligned =
            gallery_geometry_at_fraction(600.0, 450.0, maximum.clone(), 4, 1000, 8, 0.0).unwrap();
        let partial =
            gallery_geometry_at_fraction(600.0, 450.0, maximum.clone(), 4, 1000, 8, 0.25).unwrap();
        assert_eq!(aligned.viewport().rowcount, 3);
        assert_eq!(partial.viewport().rowcount, 4);
        assert_eq!(partial.viewport().extent, capacity(600, 600));
        let end = gallery_geometry_at_fraction(600.0, 450.0, maximum.clone(), 4, 1000, 249, 0.25)
            .unwrap();
        assert_eq!(end.viewport().rowcount, 1);
        for first_row in [0, 8, 249] {
            for fraction in [0.0, 0.25, 0.75] {
                let bounded = gallery_geometry_at_fraction(
                    600.0,
                    450.0,
                    capacity(600, 450),
                    4,
                    1000,
                    first_row,
                    fraction,
                )
                .unwrap();
                assert_eq!(bounded.card_extent, 112);
                assert!(bounded.viewport().extent.height <= 450);
            }
        }
        let empty = logical_gallery_geometry(600.0, 450.0, 4, 0, 0);
        assert_eq!(empty.virtual_height(), 0.0);
        let limit = VISIBLE_ITEM_CAPACITY as f32;
        let at_limit = gallery_geometry_at_fraction(
            100.0,
            100.0 * limit,
            maximum.clone(),
            1,
            VISIBLE_ITEM_CAPACITY + 1,
            0,
            0.0,
        )
        .unwrap();
        let over_limit = gallery_geometry_at_fraction(
            100.0,
            100.0 * limit,
            maximum,
            1,
            VISIBLE_ITEM_CAPACITY + 1,
            0,
            0.5,
        )
        .unwrap();
        assert_eq!(at_limit.viewport().rowcount, VISIBLE_ITEM_CAPACITY);
        assert_eq!(over_limit.viewport().rowcount, VISIBLE_ITEM_CAPACITY + 1);
        let narrow =
            gallery_geometry_at_fraction(1.0, 10_000.0, capacity(8192, 8192), 1, 100_000, 0, 0.5)
                .unwrap();
        assert_eq!(narrow.viewport().rowcount, 10_001);
        assert_eq!(narrow.viewport().extent, capacity(1, 10_001));
    }

    #[test]
    fn columns_and_fractional_geometry_remeasure_and_stay_bounded() {
        let mut state = State::default();
        assert!(state.measure_gallery(601.9, 601.9, capacity(601, 601), 4));
        assert!(state.measure_gallery(601.2, 601.2, capacity(601, 601), 4));
        assert!(state.measure_gallery(601.2, 601.2, capacity(601, 601), 5));
        assert_eq!(state.measured_viewport(5, 0, 1_000).unwrap().columns, 5);
        let geometry = gallery_geometry(601.9, 601.9, capacity(601, 601), 4, 1_000, 0).unwrap();
        assert_eq!(geometry.viewport().extent.width, 480);
        assert_eq!(geometry.viewport().rowcount, 4);
        assert_eq!(geometry.card_extent, 120);
        assert_eq!(
            logical_gallery_geometry(601.9, 601.9, 4, 1_000, 0).virtual_height(),
            37_618.75
        );
        let end = gallery_geometry(601.9, 601.9, capacity(601, 601), 4, 1_000, u32::MAX).unwrap();
        assert_eq!(end.viewport().firstrow, 249);
        assert_eq!(end.viewport().rowcount, 1);
        assert_eq!(end.card_extent, geometry.card_extent);
    }

    #[test]
    fn gallery_cells_match_the_native_extent_invariant() {
        for width in 1..=37 {
            for height in 1..=31 {
                for columns in 1..=8 {
                    for matching in 0..=65 {
                        let geometry = gallery_geometry(
                            width as f32,
                            height as f32,
                            capacity(width, height),
                            columns,
                            matching,
                            matching,
                        );
                        if let Some(geometry) = geometry {
                            let viewport = geometry.viewport();
                            let native_extent = (viewport.extent.width / viewport.columns)
                                .min(viewport.extent.height / viewport.rowcount)
                                .max(1);
                            assert_eq!(geometry.card_extent, native_extent);
                            assert!(viewport.rowcount * viewport.columns <= VISIBLE_ITEM_CAPACITY);
                            assert_eq!(
                                viewport.extent.width / columns,
                                viewport.extent.height / viewport.rowcount
                            );
                            if width >= columns && height >= viewport.rowcount {
                                assert!(viewport.extent.width <= width);
                                assert!(viewport.extent.height <= height);
                            } else {
                                assert_eq!(geometry.card_extent, 1);
                            }
                        } else {
                            let logical = logical_gallery_geometry(
                                width as f32,
                                height as f32,
                                columns,
                                matching,
                                matching,
                            );
                            assert!(
                                width < columns.min(VISIBLE_ITEM_CAPACITY)
                                    || height < logical.row_count()
                            );
                        }
                    }
                }
            }
        }
    }

    #[test]
    fn oversized_logical_gallery_is_an_exact_grid_within_maximum_extent() {
        let geometry =
            gallery_geometry(20_000.0, 12_000.0, capacity(1024, 768), 7, 10_000, 0).unwrap();
        let viewport = geometry.viewport();
        assert_eq!(viewport.extent.width % viewport.columns, 0);
        assert_eq!(viewport.extent.height % viewport.rowcount, 0);
        assert_eq!(
            viewport.extent.width / viewport.columns,
            viewport.extent.height / viewport.rowcount
        );
        assert!(viewport.extent.width <= 1024);
        assert!(viewport.extent.height <= 768);
        assert!(viewport.columns * viewport.rowcount <= VISIBLE_ITEM_CAPACITY);
    }

    #[test]
    fn logical_scroll_math_is_independent_of_native_atlas_resolution() {
        let logical = logical_gallery_geometry(600.0, 420.0, 4, 1_000, 0);
        let high = gallery_geometry(600.0, 420.0, capacity(600, 420), 4, 1_000, 0).unwrap();
        let low = gallery_geometry(600.0, 420.0, capacity(120, 90), 4, 1_000, 0).unwrap();
        assert_eq!(logical.virtual_height(), 37_500.0);
        assert_eq!(logical.first_row_for_offset(1_234.0), 8);
        assert_ne!(high.card_extent, low.card_extent);
    }

    #[test]
    fn unknown_native_extent_defers_submission_and_recovers_the_same_scroll_row() {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.order.matchingcount = 1_000;
        let mut state = State::default();
        assert!(state.measure_gallery(600.0, 420.0, capacity(600, 420), 4));
        let sent = ExploreViewportUpdate {
            viewport: state.measured_viewport(4, 3, 1_000).unwrap(),
        };
        state.request_viewport(Some(&snapshot), sent.clone());
        state.viewport_queued(sent);
        assert!(state.measure_gallery(600.0, 420.0, capacity(0, 0), 4));
        assert!(state.measured_viewport(4, 7, 1_000).is_none());
        state.record_gallery_scroll(7);
        assert!(
            state
                .measured_layout_request(Some(&snapshot), 4, 1_000)
                .is_none()
        );
        assert!(state.dispatchable_viewport().is_none());

        assert!(state.measure_gallery(600.0, 420.0, capacity(600, 420), 4));
        let recovered = state
            .measured_layout_request(Some(&snapshot), 4, 1_000)
            .unwrap();
        assert_eq!(recovered.viewport.firstrow, 7);
        assert_eq!(recovered.viewport.columns, 4);
    }

    #[test]
    fn column_rebase_preserves_latest_scroll() {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.order.matchingcount = 1_000;
        let mut state = State::default();
        state.measure_gallery(601.0, 601.0, capacity(601, 601), 4);
        let sent = ExploreViewportUpdate {
            viewport: state.measured_viewport(4, 1, 1_000).unwrap(),
        };
        state.request_viewport(Some(&snapshot), sent.clone());
        state.viewport_queued(sent);
        state.request_viewport(
            Some(&snapshot),
            ExploreViewportUpdate {
                viewport: state.measured_viewport(4, 17, 1_000).unwrap(),
            },
        );
        state.measure_gallery(601.0, 601.0, capacity(601, 601), 5);
        let rebased = state
            .measured_layout_request(Some(&snapshot), 5, 1_000)
            .unwrap();
        assert_eq!(
            (rebased.viewport.columns, rebased.viewport.firstrow),
            (5, 17)
        );
    }

    #[test]
    fn dataset_replacement_remeasures_equal_size_and_discards_previous_scroll() {
        let mut snapshot = explore_snapshot();
        snapshot.dataset.identity = 17;
        let mut state = State::default();
        state.rebase(Some(&snapshot), false);
        assert!(state.measure_gallery(400.0, 200.0, capacity(400, 200), 4));
        state.record_gallery_scroll(12);
        state.record_gallery_fraction(0.5);
        let request = ExploreViewportUpdate {
            viewport: state.measured_viewport(4, 12, 100).unwrap(),
        };
        state.request_viewport(Some(&snapshot), request.clone());
        state.viewport_queued(request);
        snapshot.dataset.identity = 18;
        state.rebase(Some(&snapshot), false);
        assert!(state.measured_viewport(4, 0, 100).is_none());
        assert!(state.sent_viewport.is_none());
        assert!(state.desired_viewport.is_none());
        assert!(state.measure_gallery(400.0, 200.0, capacity(400, 200), 4));
        assert_eq!(state.gallery_first_row(0), 0);
        assert_eq!(state.gallery_row_fraction(), 0.0);
        snapshot.frame.revision += 1;
        state.rebase(Some(&snapshot), false);
        assert!(!state.measure_gallery(400.0, 200.0, capacity(400, 200), 4));
    }

    #[test]
    fn measured_geometry_survives_transient_dataset_unavailability() {
        let mut snapshot = explore_snapshot();
        snapshot.dataset.identity = 17;
        let mut state = State::default();
        state.rebase(Some(&snapshot), false);
        assert!(state.measure_gallery(400.0, 200.0, capacity(400, 200), 4));
        state.rebase(None, false);
        assert!(state.measured_viewport(4, 0, 100).is_some());
        snapshot.dataset.identity = 0;
        state.rebase(Some(&snapshot), false);
        assert!(state.measured_viewport(4, 0, 100).is_some());
    }

    #[test]
    fn gallery_hit_boundaries_use_half_open_native_square_cells() {
        let mut snapshot = displayed_gallery_snapshot();
        for ready in [true, false] {
            snapshot.gallery.slots.fill(ready);
            for (x, y, expected) in [
                (0, 0, Some(10)),
                (99, 99, Some(10)),
                (100, 0, Some(11)),
                (0, 100, Some(20)),
                (399, 199, Some(23)),
                (400, 199, None),
                (399, 200, None),
                (u32::MAX, u32::MAX, None),
            ] {
                assert_eq!(selected_at(Some(&ExploreImageMetadata::from(&snapshot)), gallery_sample(x, y)), expected);
            }
        }
        assert_eq!(selected_at(None, gallery_sample(0, 0)), None);
        let mut empty = snapshot.clone();
        empty.order.visibleindices.clear();
        empty.gallery.slots.clear();
        assert_eq!(selected_at(Some(&ExploreImageMetadata::from(&empty)), gallery_sample(0, 0)), None);
        for invalid in 0..4 {
            let mut snapshot = snapshot.clone();
            match invalid {
                0 => snapshot.mode = crate::generated::ExploreMode::Detail,
                1 => snapshot.gallery.layout.columns = 0,
                2 => snapshot.gallery.layout.cardextent = 0,
                _ => snapshot.gallery.layout.rowcount = 0,
            }
            assert_eq!(selected_at(Some(&ExploreImageMetadata::from(&snapshot)), gallery_sample(0, 0)), None);
        }
    }

    #[test]
    fn gallery_pointer_resolves_native_visible_identity_and_deduplicates() {
        let mut snapshot = displayed_gallery_snapshot();
        snapshot.gallery.slots[5] = false;
        let mut gesture = crate::presentation_surface::SurfaceGesture {
            kind: crate::presentation_surface::SurfaceGestureKind::Pointer,
            sample: gallery_sample(150, 150),
        };
        let mut state = State::default();
        assert_eq!(
            state.gallery_gesture(Some(&snapshot), Some(&snapshot), gesture),
            Some(GalleryGestureOutcome::Selected(21))
        );
        assert_eq!(
            state.gallery_gesture(Some(&snapshot), Some(&snapshot), gesture),
            None
        );
        for release in [
            crate::presentation_surface::SurfaceGestureKind::Pointer,
            crate::presentation_surface::SurfaceGestureKind::End,
            crate::presentation_surface::SurfaceGestureKind::Cancel,
        ] {
            gesture.kind = release;
            gesture.sample.pressed = false;
            assert_eq!(
                state.gallery_gesture(Some(&snapshot), Some(&snapshot), gesture),
                None
            );
            gesture.kind = crate::presentation_surface::SurfaceGestureKind::Pointer;
            gesture.sample.pressed = true;
            assert_eq!(
                state.gallery_gesture(Some(&snapshot), Some(&snapshot), gesture),
                Some(GalleryGestureOutcome::Selected(21))
            );
        }
        snapshot.gallery.slots[5] = true;
        assert_eq!(
            state.gallery_gesture(Some(&snapshot), Some(&snapshot), gesture),
            None
        );
        snapshot.gallery.slots[5] = false;
        let mut unrelated = snapshot.clone();
        unrelated.frame.source.instance += 1;
        assert_eq!(
            state.gallery_gesture(Some(&snapshot), Some(&unrelated), gesture),
            None
        );
        assert_eq!(state.gallery_gesture(Some(&snapshot), None, gesture), None);
        gesture.kind = crate::presentation_surface::SurfaceGestureKind::Viewport;
        let mut replaced = snapshot.clone();
        replaced.dataset.identity += 1;
        assert_eq!(replaced.frame, snapshot.frame);
        assert_eq!(
            state.gallery_gesture(Some(&snapshot), Some(&replaced), gesture),
            None
        );
        assert_eq!(
            state.gallery_gesture(Some(&snapshot), Some(&snapshot), gesture),
            Some(GalleryGestureOutcome::Focused(Some(21)))
        );
        assert_eq!(
            state.gallery_gesture(Some(&snapshot), Some(&unrelated), gesture),
            None
        );
        assert_eq!(state.gallery_gesture(Some(&snapshot), None, gesture), None);
        assert_eq!(state.gallery_gesture(None, Some(&snapshot), gesture), None);
        gesture.sample.content_x = 400.0;
        assert_eq!(
            state.gallery_gesture(Some(&snapshot), Some(&snapshot), gesture),
            Some(GalleryGestureOutcome::Focused(None))
        );
    }

    #[test]
    fn circular_gallery_hits_use_displayed_identity_independently_of_readiness() {
        let mut snapshot = displayed_gallery_snapshot();
        snapshot.viewport.rowcount = 5;
        snapshot.viewport.firstrow = 7;
        snapshot.viewport.extent = VisualExtent {
            width: 400,
            height: 500,
        };
        snapshot.frame.extent = VisualExtent {
            width: 400,
            height: 800,
        };
        snapshot.order.visibleindices = (28..47).collect();
        snapshot.gallery.slots = vec![true; 19];
        crate::view_model::test_support::gallery_layout(&mut snapshot);
        snapshot.gallery.layout.roworigin = 7;
        let mut state = State::default();
        for ready in [true, false] {
            snapshot.gallery.slots.fill(ready);
            for (x, y, expected) in [
                (5, 5, Some(28)),
                (5, 105, Some(32)),
                (5, 205, Some(36)),
                (205, 405, Some(46)),
                (305, 405, None),
                (5, 505, None),
            ] {
                let sample = crate::presentation_surface::SurfaceSample {
                    height: 500,
                    ..gallery_sample(x, y)
                };
                assert_eq!(selected_at(Some(&ExploreImageMetadata::from(&snapshot)), sample), expected);
                for kind in [
                    crate::presentation_surface::SurfaceGestureKind::Pointer,
                    crate::presentation_surface::SurfaceGestureKind::Viewport,
                ] {
                    let outcome = match kind {
                        crate::presentation_surface::SurfaceGestureKind::Pointer => {
                            expected.map(GalleryGestureOutcome::Selected)
                        }
                        _ => Some(GalleryGestureOutcome::Focused(expected)),
                    };
                    assert_eq!(
                        state.gallery_gesture(
                            Some(&snapshot),
                            Some(&snapshot),
                            crate::presentation_surface::SurfaceGesture { kind, sample },
                        ),
                        outcome
                    );
                }
            }
        }
        // Sampling owns the inverse transform: at width 600 and y origin -37.5,
        // screen (150, 262.5) is logical content (100, 200), past the ring wrap.
        let gesture = crate::presentation_surface::SurfaceGesture {
            kind: crate::presentation_surface::SurfaceGestureKind::Viewport,
            sample: crate::presentation_surface::SurfaceSample {
                width: 600,
                height: 700,
                x: 150,
                y: 262,
                ..gallery_sample(100, 200)
            },
        };
        state.measure_gallery(600.0, 700.0, capacity(800, 800), 4);
        state.record_gallery_fraction(0.25);
        assert_eq!(
            state.gallery_gesture(Some(&snapshot), Some(&snapshot), gesture),
            Some(GalleryGestureOutcome::Focused(Some(37)))
        );
        // Newer desired geometry/order must not reinterpret the displayed cells.
        let mut current = snapshot.clone();
        current.viewport.columns = 5;
        current.gallery.layout.columns = 5;
        current.order.visibleindices.reverse();
        assert_eq!(
            state.gallery_gesture(Some(&current), Some(&snapshot), gesture),
            Some(GalleryGestureOutcome::Focused(Some(37)))
        );
        current.frame.revision += 1;
        assert_eq!(
            state.gallery_gesture(Some(&current), Some(&snapshot), gesture),
            None
        );
    }

    #[test]
    fn optimistic_filter_survives_admission_and_clears_after_native_settlement() {
        let mut state = State::default();
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        let request = submit_toggled_labels(&mut state, &snapshot);
        assert_eq!(
            state.presented_filter(Some(&snapshot)),
            Some(request.clone())
        );
        state.record_admission(snapshot.revision);
        state.rebase(Some(&snapshot), false);
        assert!(state.submitted_filter.is_some());
        let mut settled = snapshot;
        settled.revision += 1;
        settled.overlay = request.overlay;
        settled.busy = false;
        state.rebase(Some(&settled), false);
        assert!(state.submitted_filter.is_none());
        assert_eq!(
            state.presented_filter(Some(&settled)),
            Some(ExploreFilterUpdate {
                filter: settled.filter,
                overlay: settled.overlay,
            })
        );
    }

    #[test]
    fn synchronous_label_reply_releases_optimistic_preferences_at_its_revision() {
        let mut state = State::default();
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        let request = submit_toggled_labels(&mut state, &snapshot);
        snapshot.overlay = request.overlay;
        snapshot.revision += 1;
        state.record_admission(snapshot.revision);
        state.rebase(Some(&snapshot), false);
        assert!(state.submitted_filter.is_none());
    }

    #[test]
    fn failure_cancellation_and_bootstrap_abandon_filter_and_transport_admission() {
        let snapshot = explore_snapshot();
        let request = ExploreFilterUpdate {
            filter: snapshot.filter.clone(),
            overlay: snapshot.overlay.clone(),
        };
        let viewport = ExploreViewportUpdate {
            viewport: snapshot.viewport.clone(),
        };
        let mut state = State::default();
        assert!(state.measure_gallery(700.0, 400.0, capacity(700, 400), 4));
        state.record_submission(request.clone());
        state.abandon_submission();
        assert!(state.submitted_filter.is_none());
        state.record_submission(request);
        state.request_viewport(None, viewport.clone());
        state.viewport_queued(viewport);
        assert!(state.arm_viewport_writable_wait());
        state.rebase(Some(&snapshot), true);
        assert!(state.submitted_filter.is_none());
        assert!(state.sent_viewport.is_none());
        assert!(state.desired_viewport.is_none());
        assert!(!state.viewport_writable_wait);
        assert!(state.measured_viewport(4, 0, 20).is_some());
    }
}
