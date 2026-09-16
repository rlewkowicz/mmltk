use super::{FrameReady, Placement, Surface};
use crate::generated::ExploreImageMetadata;
use std::sync::Arc;

thread_local! {
    static DARK: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

pub(super) fn observe_theme(theme: &crate::fluent_theme::Theme) {
    if crate::integration_control::reporting_enabled() {
        DARK.with(|value| value.set(theme.is_dark()));
    }
}

pub(super) fn dark() -> bool {
    crate::integration_control::reporting_enabled() && DARK.with(|value| value.get())
}

pub(super) fn valid_layout(snapshot: &ExploreImageMetadata) -> bool {
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

pub(super) fn matching(frame: Option<FrameReady>) -> Option<Arc<ExploreImageMetadata>> {
    super::metadata::pending(frame?).and_then(|metadata| metadata.content.gallery().cloned())
}

pub(crate) fn displayed() -> Option<(Surface, Arc<ExploreImageMetadata>)> {
    match super::explore_display(None)? {
        super::ExploreDisplay::Gallery(surface, gallery) => Some((surface, gallery)),
        super::ExploreDisplay::Detail(..) => None,
    }
}

pub(crate) fn awaiting_display(frame: FrameReady) -> bool {
    matching(Some(frame)).is_some()
        && displayed().is_none_or(|(surface, _)| surface.frame != Some(frame))
}

pub(super) fn placement(snapshot: &ExploreImageMetadata) -> Placement {
    let layout = &snapshot.gallery.layout;
    Placement::GalleryGrid {
        first_row: layout.firstrow,
        columns: layout.columns,
        rows: layout.rowcount,
        row_capacity: layout.rowcapacity,
        row_origin: layout.roworigin,
    }
}

pub(super) fn row_offset(layout: Placement, snapshot: &ExploreImageMetadata, width: f32) -> f32 {
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

#[cfg(test)]
mod tests {
    use super::super::metadata;
    use super::*;
    use crate::generated::ExploreMode;

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

    fn gallery_snapshot() -> crate::generated::ExploreSnapshot {
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
    fn gallery_metadata_is_immutable_across_dataset_and_label_changes() {
        super::super::reset_test_releases();
        let mut snapshot = gallery_snapshot();
        snapshot.dataset.identity = 11;
        let frame = frame_ready();
        metadata::install_explore(frame, &snapshot);
        let original = matching(Some(frame)).unwrap();
        snapshot.dataset.identity = 12;
        snapshot.revision += 1;
        snapshot.overlay.showlabels = !snapshot.overlay.showlabels;
        assert_eq!(matching(Some(frame)).unwrap(), original);
        let next = FrameReady {
            slot: 1,
            presentation_revision: frame.presentation_revision + 1,
            ..frame
        };
        metadata::install_explore(next, &snapshot);
        assert_eq!(matching(Some(next)).unwrap().dataset.identity, 12);
        assert_eq!(matching(Some(frame)).unwrap().dataset.identity, 11);
    }

    #[test]
    fn gallery_copy_completion_and_encoder_custody_are_independent_of_application_observations() {
        super::super::reset_test_releases();
        let mut snapshot = gallery_snapshot();
        snapshot.ready = true;
        let frame = frame_ready();
        metadata::install_explore(frame, &snapshot);
        assert!(super::super::accept_publication(frame));
        let read = super::super::SampleRead::acquire(frame).unwrap();
        let mut pending = metadata::pending(frame).unwrap();
        pending.read = Some(read.clone());
        let surface = pending.surface;
        let mut image = super::super::ImagePublication {
            surface,
            completed: None,
            retained_read: None,
            pending_sample: Some(pending),
            content: metadata::Content::default(),
            placement: placement(&ExploreImageMetadata::from(&snapshot)),
        };
        let mut model = crate::view_model::test_support::bootstrapped();
        model.presentation = None;
        model.explore.snapshot = None;
        assert!(image.submitted_draw(surface).is_none());
        super::super::authorize_draw(Some(frame));
        assert!(image.submitted_draw(surface).is_some());
        assert!(!image.promote(frame, &model));
        image.complete(frame);
        assert!(image.promote(frame, &model));
        assert_eq!(image.retained(), Some(surface));
        snapshot.frame.revision += 1;
        snapshot.frame.extent.height = 500;
        snapshot.viewport.rowcount = 5;
        snapshot.viewport.firstrow = 1;
        snapshot.viewport.extent = snapshot.frame.extent.clone();
        crate::view_model::test_support::gallery_layout(&mut snapshot);
        let next = FrameReady {
            slot: 1,
            content_sequence: snapshot.frame.revision,
            content_height: 500,
            presentation_revision: frame.presentation_revision + 1,
            ..frame
        };
        assert!(matching(Some(next)).is_none());
        metadata::install_explore(next, &snapshot);
        assert_eq!(matching(Some(next)).unwrap().viewport.rowcount, 5);
        assert_eq!(image.content.gallery().unwrap().viewport.rowcount, 4);
        super::super::retire_publication(frame);
        drop(image);
        assert!(super::super::test_releases().is_empty());
        drop(read);
        assert_eq!(super::super::test_releases(), vec![frame]);
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
        assert_eq!(
            row_offset(newer, &ExploreImageMetadata::from(&retained), 600.0),
            -150.0
        );
        assert_eq!(
            row_offset(
                placement(&ExploreImageMetadata::from(&retained)),
                &ExploreImageMetadata::from(&retained),
                600.0
            ),
            0.0
        );
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
        assert_eq!(
            row_offset(earlier, &ExploreImageMetadata::from(&retained), 600.0),
            1200.0
        );
    }

    #[test]
    fn detail_return_keeps_independent_retained_gallery_read_custody() {
        use super::super::{
            SampleRead, accept_publication, reset_test_releases, retire_publication, test_releases,
        };
        reset_test_releases();
        let mut snapshot = gallery_snapshot();
        let original = frame_ready();
        metadata::install_explore(original, &snapshot);
        let original_facts = matching(Some(original)).unwrap();
        assert!(accept_publication(original));
        let encoded_draw = SampleRead::acquire(original).unwrap();
        retire_publication(original);
        assert!(test_releases().is_empty());
        snapshot.revision += 20;
        let returned = FrameReady {
            slot: 1,
            presentation_revision: original.presentation_revision + 2,
            ..original
        };
        metadata::install_explore(returned, &snapshot);
        let returned_facts = matching(Some(returned)).unwrap();
        assert_eq!(returned_facts.frame, original_facts.frame);
        assert_eq!(returned_facts.gallery, original_facts.gallery);
        assert!(accept_publication(returned));
        let returned_read = SampleRead::acquire(returned).unwrap();
        drop(encoded_draw);
        assert_eq!(test_releases(), vec![original]);
        retire_publication(returned);
        assert_eq!(test_releases(), vec![original]);
        drop(returned_read);
        assert_eq!(test_releases(), vec![original, returned]);
    }

    #[test]
    fn circular_layout_stays_with_the_exact_displayed_product() {
        super::super::reset_test_releases();
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
        assert!(physical.matches_content(&snapshot.frame));
        assert!(valid_layout(&ExploreImageMetadata::from(&snapshot)));
        metadata::install_explore(physical, &snapshot);
        let retained = matching(Some(physical)).unwrap();
        assert_eq!(retained.gallery.layout.roworigin, 7);
        let mut replacement = snapshot.clone();
        replacement.revision += 1;
        replacement.gallery.layout.roworigin = 0;
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
        metadata::install_explore(newer, &replacement);
        assert_eq!(matching(Some(newer)).unwrap().gallery.layout.rowcount, 6);
        assert_eq!(retained.gallery.layout.rowcount, 5);
        assert_eq!(retained.gallery.layout.roworigin, 7);
    }
}
