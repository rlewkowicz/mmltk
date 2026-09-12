use super::{FrameReady, Placement, RENDERER, Surface};
use crate::generated::{ExploreMode, ExploreSnapshot, VisualFrame};
use std::cell::RefCell;
use std::sync::Arc;

thread_local! {
    // Selection coalesces with native presentation work. The renderer separately
    // owns the facts for its completed pixels; no revision history is retained.
    static SELECTED: RefCell<Option<Arc<ExploreSnapshot>>> = const { RefCell::new(None) };
    static CONFIRMED: RefCell<Option<(FrameReady, VisualFrame)>> = const { RefCell::new(None) };
    static DARK: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

pub(crate) fn observe(snapshot: Option<&ExploreSnapshot>, dark: bool) {
    DARK.with(|value| value.set(dark));
    SELECTED.with(|selected| {
        let mut selected = selected.borrow_mut();
        if let Some(snapshot) = snapshot
            && selected.as_ref().is_some_and(|value| {
                value.frame == snapshot.frame
                    && value.dataset.identity == snapshot.dataset.identity
                    && value.viewport == snapshot.viewport
                    && value.gallery.layout == snapshot.gallery.layout
                    && value.order.visibleindices == snapshot.order.visibleindices
                    && value.augmentation == snapshot.augmentation
                    && value.revision != snapshot.revision
            })
        {
            *selected = Some(Arc::new(snapshot.clone()));
        }
    });
    if let Some(snapshot) = snapshot {
        RENDERER.with(|renderer| {
            let mut renderer = renderer.borrow_mut();
            if let Some(imported) = renderer.as_mut().and_then(|owner| owner.imported.as_mut())
                && imported.image.gallery.as_ref().is_some_and(|shown| {
                    shown.frame == snapshot.frame
                        && shown.dataset.identity == snapshot.dataset.identity
                        && shown.viewport == snapshot.viewport
                        && shown.gallery.layout == snapshot.gallery.layout
                        && shown.order.visibleindices == snapshot.order.visibleindices
                        && shown.augmentation == snapshot.augmentation
                        && shown.revision != snapshot.revision
                })
            {
                imported.image.gallery = Some(Arc::new(snapshot.clone()));
            }
        });
    }
}

pub(super) fn dark() -> bool {
    DARK.with(|value| value.get())
}

pub(crate) fn select(snapshot: Option<&ExploreSnapshot>, frame: &VisualFrame) {
    SELECTED.with(|selected| {
        let mut selected = selected.borrow_mut();
        let snapshot =
            snapshot.filter(|value| value.mode == ExploreMode::Gallery && value.frame == *frame);
        if selected.as_ref().is_some_and(|value| {
            value.frame == *frame
                && snapshot
                    .is_some_and(|current| current.dataset.identity == value.dataset.identity)
        }) {
            if let Some(snapshot) = snapshot
                && selected.as_ref().is_some_and(|value| {
                    snapshot.revision > value.revision
                        && snapshot.gallery.layout == value.gallery.layout
                })
            {
                *selected = Some(Arc::new(snapshot.clone()));
            }
            return;
        }
        CONFIRMED.with(|value| *value.borrow_mut() = None);
        *selected = snapshot.map(|value| Arc::new(value.clone()));
        if let Some(snapshot) = selected.as_ref() {
            super::trace_gallery_source(snapshot);
        }
    });
}

fn matches(snapshot: &ExploreSnapshot, frame: FrameReady) -> bool {
    snapshot.frame.source.kind == crate::generated::PresentationSourceKind::Explore
        && frame.matches_content(&snapshot.frame)
        && valid_layout(snapshot)
}

fn valid_layout(snapshot: &ExploreSnapshot) -> bool {
    let layout = &snapshot.gallery.layout;
    layout.columns != 0
        && layout.cardextent != 0
        && layout.rowcount != 0
        && layout.rowcount <= layout.rowcapacity
        && layout.roworigin < layout.rowcapacity
        && layout.columns.checked_mul(layout.cardextent) == Some(snapshot.frame.extent.width)
        && layout.rowcapacity.checked_mul(layout.cardextent) == Some(snapshot.frame.extent.height)
        && layout.firstrow == snapshot.viewport.firstrow
        && layout.rowcount == snapshot.viewport.rowcount
        && layout.columns == snapshot.viewport.columns
        && snapshot.gallery.slots.len() == snapshot.order.visibleindices.len()
}

pub(super) fn matching(frame: Option<FrameReady>) -> Option<Arc<ExploreSnapshot>> {
    let frame = frame?;
    SELECTED.with(|selected| {
        selected
            .borrow()
            .as_ref()
            .filter(|value| {
                matches(value, frame)
                    && CONFIRMED.with(|confirmed| {
                        confirmed
                            .borrow()
                            .as_ref()
                            .is_some_and(|(publication, source)| {
                                super::same_publication(*publication, frame)
                                    && *source == value.frame
                            })
                    })
            })
            .cloned()
    })
}

pub(crate) fn displayed() -> Option<(Surface, Arc<ExploreSnapshot>)> {
    RENDERER.with(|renderer| {
        let renderer = renderer.borrow();
        let renderer = renderer.as_ref()?;
        if let Some(submitted) = [&renderer.pending, &renderer.imported]
            .into_iter()
            .flatten()
            .find_map(|imported| {
                let pending = imported.image.pending_sample.as_ref()?;
                let pending = imported.image.submitted_draw(pending.surface)?;
                Some((pending.surface, pending.gallery.clone()?))
            })
        {
            return Some(submitted);
        }
        let imported = renderer.imported.as_ref()?;
        let surface = imported.image.retained()?;
        let frame = surface.frame?;
        let snapshot = imported.image.gallery.as_ref()?;
        matches(snapshot, frame).then(|| (surface, snapshot.clone()))
    })
}

pub(crate) fn awaiting_display(frame: FrameReady) -> bool {
    matching(Some(frame)).is_some()
        && displayed().is_none_or(|(surface, _)| surface.frame != Some(frame))
}

pub(crate) fn confirm(
    frame: FrameReady,
    completed: &VisualFrame,
    current: Option<&ExploreSnapshot>,
) {
    SELECTED.with(|selected| {
        let mut selected = selected.borrow_mut();
        if current.is_some_and(|current| {
            selected
                .as_ref()
                .is_some_and(|snapshot| snapshot.dataset.identity != current.dataset.identity)
        }) {
            return;
        }
        if selected
            .as_ref()
            .is_some_and(|snapshot| snapshot.frame.source == completed.source)
            && let Some(current) = current.filter(|snapshot| {
                snapshot.mode == ExploreMode::Gallery
                    && snapshot.frame == *completed
                    && matches(snapshot, frame)
            })
        {
            if selected.as_ref().is_some_and(|snapshot| {
                snapshot.frame == current.frame && snapshot.gallery.layout != current.gallery.layout
            }) {
                return;
            }
            if selected
                .as_ref()
                .is_some_and(|snapshot| snapshot.frame != current.frame)
            {
                super::trace_gallery_source(current);
            }
            *selected = Some(Arc::new(current.clone()));
        }
        if selected
            .as_ref()
            .is_some_and(|snapshot| snapshot.frame == *completed && matches(snapshot, frame))
        {
            CONFIRMED.with(|confirmed| *confirmed.borrow_mut() = Some((frame, completed.clone())));
        }
    });
}

pub(super) fn current_source(snapshot: &ExploreSnapshot) -> bool {
    SELECTED.with(|selected| {
        selected.borrow().as_ref().is_some_and(|value| {
            value.frame.source == snapshot.frame.source
                && value.dataset.identity == snapshot.dataset.identity
        })
    })
}

pub(super) fn placement(snapshot: &ExploreSnapshot) -> Placement {
    let layout = &snapshot.gallery.layout;
    Placement::GalleryGrid {
        first_row: layout.firstrow,
        columns: layout.columns,
        rows: layout.rowcount,
        row_capacity: layout.rowcapacity,
        row_origin: layout.roworigin,
    }
}

pub(super) fn row_offset(layout: Placement, snapshot: &ExploreSnapshot, width: f32) -> f32 {
    match layout {
        Placement::GalleryGrid {
            columns, first_row, ..
        } => {
            width
                * (snapshot.gallery.layout.firstrow as f32
                    / snapshot.gallery.layout.columns.max(1) as f32
                    - first_row as f32 / columns.max(1) as f32)
        }
        Placement::Contain => 0.0,
    }
}

pub(super) fn clear() {
    SELECTED.with(|selected| *selected.borrow_mut() = None);
    CONFIRMED.with(|confirmed| *confirmed.borrow_mut() = None);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame_ready() -> FrameReady {
        FrameReady {
            source_high: 0,
            source_low: 0,
            direct_sampling: false,
            high: 1,
            low: 2,
            layer: 0,
            slot: 0,
            content_session: 1,
            content_sequence: 41,
            presentation_revision: 51,
            content_width: 400,
            content_height: 400,
        }
    }

    fn gallery_snapshot() -> ExploreSnapshot {
        let mut snapshot = crate::view_model::test_support::explore_snapshot();
        snapshot.mode = ExploreMode::Gallery;
        snapshot.frame = crate::view_model::test_support::visual_frame(
            crate::generated::PresentationSourceKind::Explore,
            41,
        );
        snapshot.frame.extent = crate::generated::VisualExtent {
            width: 400,
            height: 400,
        };
        snapshot.viewport = crate::generated::ExploreViewport {
            columns: 4,
            rowcount: 4,
            firstrow: 0,
            extent: snapshot.frame.extent.clone(),
        };
        crate::view_model::test_support::gallery_layout(&mut snapshot);
        snapshot
    }

    #[test]
    fn completed_sample_from_another_dataset_cannot_be_promoted() {
        let mut snapshot = gallery_snapshot();
        snapshot.dataset.identity = 11;
        let native = frame_ready();
        select(Some(&snapshot), &snapshot.frame);
        confirm(native, &snapshot.frame, Some(&snapshot));
        assert!(matching(Some(native)).is_some());
        assert!(current_source(&snapshot));
        let mut replacement = snapshot.clone();
        replacement.dataset.identity = 12;
        replacement.revision += 1;
        assert_eq!(replacement.frame, snapshot.frame);
        observe(Some(&replacement), false);
        assert!(current_source(&snapshot));
        assert!(!current_source(&replacement));
        confirm(native, &replacement.frame, Some(&replacement));
        assert!(current_source(&snapshot));
        select(Some(&replacement), &replacement.frame);
        assert!(!current_source(&snapshot));
        assert!(current_source(&replacement));
        assert!(matching(Some(native)).is_none());
        observe(Some(&snapshot), false);
        confirm(native, &snapshot.frame, Some(&snapshot));
        assert!(current_source(&replacement));
        assert!(matching(Some(native)).is_none());
        confirm(native, &replacement.frame, Some(&replacement));
        assert_eq!(matching(Some(native)).unwrap().dataset.identity, 12);
        select(Some(&replacement), &replacement.frame);
        assert!(matching(Some(native)).is_some());
        replacement.revision += 1;
        observe(Some(&replacement), false);
        assert_eq!(
            matching(Some(native)).unwrap().revision,
            replacement.revision
        );
        clear();
    }

    #[test]
    fn selected_gallery_facts_require_the_exact_revision_and_extent() {
        super::super::reset_test_releases();
        let mut snapshot = crate::view_model::test_support::explore_snapshot();
        snapshot.ready = true;
        snapshot.revision = 1;
        snapshot.dataset.identity = 11;
        snapshot.mode = ExploreMode::Gallery;
        snapshot.frame.source = crate::generated::PresentationSourceIdentity {
            kind: crate::generated::PresentationSourceKind::Explore,
            instance: 1,
        };
        snapshot.viewport.columns = 4;
        snapshot.viewport.rowcount = 4;
        snapshot.frame.extent.width = 400;
        snapshot.frame.extent.height = 400;
        snapshot.viewport.extent = snapshot.frame.extent.clone();
        snapshot.frame.revision = 41;
        crate::view_model::test_support::gallery_layout(&mut snapshot);
        let frame = frame_ready();
        select(Some(&snapshot), &snapshot.frame);
        confirm(frame, &snapshot.frame, Some(&snapshot));
        let retained = matching(Some(frame)).unwrap();
        let surface = crate::view_model::test_support::physical_surface(frame);
        assert!(super::super::accept_publication(frame));
        let read = super::super::SampleRead::acquire(frame).unwrap();
        let mut image = super::super::ImagePublication {
            surface,
            completed: None,
            retained_read: None,
            pending_sample: Some(super::super::PendingImage {
                read: Some(read),
                surface,
                gallery: Some(retained.clone()),
                detail: None,
                annotation: None,
                placement: placement(&snapshot),
                complete: false,
                view_ready: false,
            }),
            gallery: None,
            detail: None,
            annotation: None,
            placement: placement(&snapshot),
        };
        let mut model = crate::view_model::test_support::bootstrapped();
        model.set_foreground_feature(crate::generated::FeatureId::Explore);
        model.explore.snapshot = Some(snapshot.clone());
        let control = model.presentation.as_mut().unwrap();
        control.completed = snapshot.frame.clone();
        control.completedsourcerevision = snapshot.revision;
        control.presentationrevision = frame.presentation_revision;
        snapshot.revision += 1;
        snapshot.overlay.showlabels = !snapshot.overlay.showlabels;
        model.explore.snapshot = Some(snapshot.clone());
        select(Some(&snapshot), &snapshot.frame);
        assert_eq!(matching(Some(frame)).unwrap().revision, snapshot.revision);
        image.reconcile_pending(frame, &model);
        assert!(image.submitted_draw(surface).is_none());
        super::super::authorize_draw(Some(frame));
        assert!(image.submitted_draw(surface).is_some());
        let pending = image.pending_sample.as_ref().unwrap();
        assert_eq!(
            pending.gallery.as_ref().unwrap().revision,
            snapshot.revision
        );
        assert_eq!(
            pending.gallery.as_ref().unwrap().overlay.showlabels,
            snapshot.overlay.showlabels
        );
        assert!(!pending.complete);
        assert!(image.retained().is_none());
        assert!(!image.promote(frame, &model));
        let mut other_dataset = snapshot.clone();
        other_dataset.dataset.identity += 1;
        select(Some(&other_dataset), &other_dataset.frame);
        confirm(frame, &other_dataset.frame, Some(&other_dataset));
        image.reconcile_pending(frame, &model);
        assert_eq!(
            image
                .pending_sample
                .as_ref()
                .unwrap()
                .gallery
                .as_ref()
                .unwrap()
                .dataset
                .identity,
            snapshot.dataset.identity
        );
        assert!(image.submitted_draw(surface).is_none());
        select(Some(&snapshot), &snapshot.frame);
        confirm(frame, &snapshot.frame, Some(&snapshot));
        super::super::complete_sample(frame);
        image.complete(frame);
        assert!(image.promote(frame, &model));
        assert_eq!(image.retained(), Some(surface));
        snapshot.viewport.rowcount = 5;
        snapshot.viewport.firstrow = 1;
        snapshot.frame.extent.height = 500;
        snapshot.viewport.extent = snapshot.frame.extent.clone();
        snapshot.frame.revision = 42;
        assert!(matching(Some(frame)).is_some());
        assert_eq!(retained.viewport.rowcount, 4);
        assert_eq!(retained.viewport.firstrow, 0);
        assert!(
            matching(Some(FrameReady {
                content_sequence: 42,
                ..frame
            }))
            .is_none()
        );
        let next_frame = FrameReady {
            content_sequence: 42,
            content_height: 500,
            ..frame
        };
        crate::view_model::test_support::gallery_layout(&mut snapshot);
        confirm(next_frame, &snapshot.frame, Some(&snapshot));
        let next = matching(Some(FrameReady {
            content_sequence: 42,
            content_height: 500,
            ..frame
        }))
        .unwrap();
        assert_eq!(next.viewport.rowcount, 5);
        assert_eq!(next.viewport.firstrow, 1);
        assert!(super::super::test_releases().is_empty());
        super::super::retire_publication(frame);
        assert!(super::super::test_releases().is_empty());
        drop(image);
        assert_eq!(super::super::test_releases(), vec![frame]);
        clear();
    }

    #[test]
    fn retained_rows_keep_their_document_origin_through_grid_replacement() {
        let mut retained = crate::view_model::test_support::explore_snapshot();
        retained.viewport.columns = 4;
        retained.viewport.rowcount = 4;
        retained.viewport.firstrow = 1;
        crate::view_model::test_support::gallery_layout(&mut retained);
        let newer = Placement::GalleryGrid {
            columns: 4,
            rows: 5,
            row_capacity: 5,
            row_origin: 0,
            first_row: 2,
        };
        assert_eq!(row_offset(newer, &retained, 600.0), -150.0);
        assert_eq!(row_offset(placement(&retained), &retained, 600.0), 0.0);
        retained.viewport.rowcount = 5;
        retained.viewport.firstrow = 10;
        crate::view_model::test_support::gallery_layout(&mut retained);
        let earlier = Placement::GalleryGrid {
            columns: 4,
            rows: 4,
            row_capacity: 4,
            row_origin: 0,
            first_row: 2,
        };
        assert_eq!(row_offset(earlier, &retained, 600.0), 1200.0);
    }

    #[test]
    fn native_publication_requires_the_completed_reflected_source_identity() {
        super::super::reset_test_releases();
        let snapshot = gallery_snapshot();
        let native = frame_ready();
        select(Some(&snapshot), &snapshot.frame);
        assert!(matching(Some(native)).is_none());
        let mut different_source = snapshot.frame.clone();
        different_source.source.instance += 1;
        confirm(native, &different_source, Some(&snapshot));
        assert!(matching(Some(native)).is_none());
        confirm(
            FrameReady {
                content_session: 3,
                ..native
            },
            &snapshot.frame,
            Some(&snapshot),
        );
        assert!(matching(Some(native)).is_none());
        confirm(native, &snapshot.frame, Some(&snapshot));
        assert!(matching(Some(native)).is_some());
        // An available native borrow is still not a completed display.
        assert!(super::super::accept_publication(native));
        assert!(displayed().is_none());
        super::super::retire_publication(native);
        let mut replacement = snapshot.clone();
        replacement.frame.source.instance += 1;
        select(Some(&replacement), &replacement.frame);
        confirm(native, &snapshot.frame, Some(&snapshot));
        assert!(matching(Some(native)).is_none());

        clear();
    }

    #[test]
    fn detail_return_reauthorizes_the_retained_gallery_with_independent_read_custody() {
        use super::super::{
            SampleRead, accept_publication, reset_test_releases, retire_publication, test_releases,
        };

        reset_test_releases();
        let mut snapshot = gallery_snapshot();
        snapshot.revision = 10;
        let original = frame_ready();
        select(Some(&snapshot), &snapshot.frame);
        confirm(original, &snapshot.frame, Some(&snapshot));
        let original_facts = matching(Some(original)).unwrap();
        assert!(accept_publication(original));
        let encoded_draw = SampleRead::acquire(original).unwrap();

        let mut detail = snapshot.clone();
        detail.mode = ExploreMode::Detail;
        detail.selectedimage = Some(0);
        detail.revision = 20;
        detail.frame.revision += 1;
        select(Some(&detail), &detail.frame);
        assert!(matching(Some(original)).is_none());
        retire_publication(original);
        assert!(test_releases().is_empty());

        snapshot.revision = 30;
        let returned = FrameReady {
            slot: 1,
            presentation_revision: original.presentation_revision + 2,
            ..original
        };
        select(Some(&snapshot), &snapshot.frame);
        assert!(matching(Some(returned)).is_none());
        // A late detail completion cannot authorize a gallery with older pixels.
        confirm(returned, &detail.frame, Some(&detail));
        assert!(matching(Some(returned)).is_none());
        confirm(returned, &snapshot.frame, Some(&snapshot));
        let returned_facts = matching(Some(returned)).unwrap();
        assert_eq!(returned_facts.revision, 30);
        assert_eq!(returned_facts.frame, original_facts.frame);
        assert_eq!(returned_facts.gallery, original_facts.gallery);
        assert_eq!(returned_facts.order, original_facts.order);
        assert!(matching(Some(original)).is_none());
        assert!(accept_publication(returned));
        let returned_read = SampleRead::acquire(returned).unwrap();
        // Return selection does not revoke an already encoded old draw.
        drop(encoded_draw);
        assert_eq!(test_releases(), vec![original]);
        retire_publication(returned);
        assert_eq!(test_releases(), vec![original]);
        drop(returned_read);
        assert_eq!(test_releases(), vec![original, returned]);
        clear();
    }

    #[test]
    fn circular_layout_stays_with_the_exact_displayed_product() {
        let mut snapshot = gallery_snapshot();
        snapshot.viewport.rowcount = 5;
        snapshot.viewport.extent.height = 500;
        snapshot.frame.extent.height = 800;
        crate::view_model::test_support::gallery_layout(&mut snapshot);
        snapshot.gallery.layout.roworigin = 7;
        let physical = FrameReady {
            content_height: 800,
            ..frame_ready()
        };
        assert!(matches(&snapshot, physical));
        select(Some(&snapshot), &snapshot.frame);
        confirm(physical, &snapshot.frame, Some(&snapshot));
        let retained = matching(Some(physical)).unwrap();
        assert_eq!(retained.gallery.layout.roworigin, 7);
        let mut replacement = snapshot.clone();
        replacement.revision += 1;
        replacement.gallery.layout.roworigin = 0;
        observe(Some(&replacement), false);
        select(Some(&replacement), &replacement.frame);
        confirm(physical, &replacement.frame, Some(&replacement));
        assert_eq!(
            matching(Some(physical)).unwrap().gallery.layout.roworigin,
            7
        );
        replacement.frame.revision += 1;
        replacement.viewport.rowcount = 6;
        replacement.viewport.extent.height = 600;
        replacement.gallery.layout.rowcount = 6;
        let newer = FrameReady {
            content_sequence: replacement.frame.revision,
            ..physical
        };
        select(Some(&replacement), &replacement.frame);
        confirm(newer, &replacement.frame, Some(&replacement));
        assert_eq!(matching(Some(newer)).unwrap().gallery.layout.rowcount, 6);
        assert_eq!(retained.gallery.layout.rowcount, 5);
        assert_eq!(retained.gallery.layout.roworigin, 7);
        clear();
    }
}
