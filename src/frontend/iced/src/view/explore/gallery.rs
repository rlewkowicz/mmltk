use super::settings_edit;
use crate::fluent_theme::Element;
use crate::presentation_surface::Surface;
use crate::view::settings::{EditSchedule, SettingsModel};
use crate::view_model::ApplicationModel;
use iced::widget::scrollable::{Direction, Scrollbar};
use iced::widget::{
    button, checkbox, column, container, progress_bar, responsive, row, scrollable, space, stack,
    text,
};
use iced::{Center, Fill, Length, Padding, Size};

const TOOLBAR_HEIGHT: f32 = 112.0;
const STATUS_HEIGHT: f32 = 30.0;

#[derive(Debug, Clone)]
pub enum Message {
    Overlay(super::overlay::Message),
    ScrollRequested(f32),
    ColumnsChanged(i32),
    RerollRequested,
    AugmentationToggled(bool),
    AugmentationRerollRequested,
    Scrolled {
        first_row: u32,
        row_fraction: f32,
        request: Option<crate::generated::ExploreViewportUpdate>,
    },
    Measured {
        size: Size,
        maximum_extent: crate::generated::VisualExtent,
        columns: u32,
    },
    Surface(super::state::GalleryInput),
}

#[derive(Debug, Clone)]
pub(super) enum Outcome {
    OverlayUpdated(crate::generated::ExploreOverlay),
    ScrollRequested(f32),
    SettingsEdited(EditSchedule),
    RerollRequested,
    AugmentationUpdated(crate::generated::ExploreAugmentationUpdate),
    AugmentationRerollRequested,
    ImageSelected(u32),
    ViewportChanged(crate::generated::ExploreViewportUpdate),
}

pub(super) fn update(
    state: &mut super::state::State,
    snapshot: Option<&crate::generated::ExploreSnapshot>,
    settings: &mut SettingsModel,
    message: Message,
) -> Result<Option<Outcome>, String> {
    Ok(Some(match message {
        Message::ScrollRequested(offset) => Outcome::ScrollRequested(offset),
        Message::Overlay(message) => {
            Outcome::OverlayUpdated(super::overlay::update(state, snapshot, message)?)
        }
        Message::ColumnsChanged(columns) => {
            Outcome::SettingsEdited(settings_edit(settings, |draft| {
                crate::generated::edit_workflowsexploregridwidth(draft, columns)
            })?)
        }
        Message::RerollRequested => Outcome::RerollRequested,
        Message::AugmentationToggled(enabled) => {
            Outcome::AugmentationUpdated(crate::generated::ExploreAugmentationUpdate { enabled })
        }
        Message::AugmentationRerollRequested => Outcome::AugmentationRerollRequested,
        Message::Scrolled {
            first_row,
            row_fraction,
            request,
        } => {
            // The retained gallery scrollable remains laid out beneath detail
            // mode. Record its physical offset even while native viewport
            // mutation is unavailable so reopening cannot pair an old native
            // row with a different on-screen scroll position.
            state.record_gallery_scroll(first_row);
            state.record_gallery_fraction(row_fraction);
            if !snapshot.is_some_and(|value| {
                value.ready && value.mode == crate::generated::ExploreMode::Gallery
            }) {
                return Ok(None);
            }
            let Some(request) = request else {
                state.clear_viewport_admission();
                return Ok(None);
            };
            Outcome::ViewportChanged(request)
        }
        Message::Measured {
            size,
            maximum_extent,
            columns,
        } => {
            let Some(snapshot) =
                snapshot.filter(|value| value.mode == crate::generated::ExploreMode::Gallery)
            else {
                return Ok(None);
            };
            if !state.measure_gallery(size.width, size.height, maximum_extent, columns) {
                return Ok(None);
            }
            let Some(request) = state.measured_layout_request(
                Some(snapshot),
                columns,
                snapshot.order.matchingcount,
            ) else {
                state.clear_viewport_admission();
                return Ok(None);
            };
            Outcome::ViewportChanged(request)
        }
        Message::Surface(input) => {
            let snapshot = snapshot.filter(|value| {
                value.ready && value.mode == crate::generated::ExploreMode::Gallery
            });
            if snapshot.is_none() {
                return Ok(None);
            }
            match state.gallery_input(snapshot, input) {
                Some(super::state::GalleryGestureOutcome::Selected(index)) => {
                    Outcome::ImageSelected(index)
                }
                Some(super::state::GalleryGestureOutcome::Focused(index)) => {
                    let Some(snapshot) = snapshot else {
                        return Ok(None);
                    };
                    let columns = explore_columns(settings);
                    let Some(mut request) = state.measured_layout_request(
                        Some(snapshot),
                        columns,
                        snapshot.order.matchingcount,
                    ) else {
                        return Ok(None);
                    };
                    request.focusedcompiledindex = index;
                    Outcome::ViewportChanged(request)
                }
                None => return Ok(None),
            }
        }
    }))
}

// Retained component focus suppresses same-cell motion before it becomes an
// application message. Selection and native viewport admission keep their owners.
fn local_gestures(
    state: &super::state::State,
    snapshot: Option<&crate::generated::ExploreSnapshot>,
    columns: u32,
) -> std::sync::Arc<
    dyn Fn(crate::presentation_surface::SurfaceGesture) -> Option<Message> + Send + Sync,
> {
    let hover = state.gallery_hover.clone();
    let current = super::state::GallerySource::current(snapshot);
    let focus_ready = snapshot.is_some_and(|snapshot| {
        state
            .measured_layout_request(Some(snapshot), columns, snapshot.order.matchingcount)
            .is_some()
    });
    std::sync::Arc::new(move |gesture| {
        let shown = crate::presentation_surface::gallery::displayed();
        hover
            .capture(
                current.as_ref(),
                shown.as_ref().map(|(_, snapshot)| snapshot.as_ref()),
                gesture,
                focus_ready,
            )
            .map(Message::Surface)
    })
}

pub(super) fn view<'a>(
    state: &'a super::state::State,
    model: &'a ApplicationModel,
    settings: &'a SettingsModel,
    surface: Option<Surface>,
    width: f32,
) -> Element<'a, Message> {
    let snapshot = model.explore.snapshot.as_ref();
    let presentation_title = model.explore.presentation_title();
    let columns = explore_columns(settings);
    let settings_available = settings.draft.is_some() && model.settings_edit_available();
    let mutation_available = !settings.has_local_edits() && model.explore_mutation_available();
    let augmentation_update_available = augmentation_toggle_available(model, settings);
    let shuffled = snapshot
        .is_some_and(|value| value.filter.order == crate::generated::ExploreOrder::Shuffled);
    let reshuffle: Element<'a, Message> = if shuffled {
        container(
            button("Reshuffle")
                .on_press_maybe(mutation_available.then_some(Message::RerollRequested))
                .style(crate::fluent_theme::button_secondary),
        )
        .id(super::RESHUFFLE_ID)
        .into()
    } else {
        space::horizontal().width(Length::Shrink).into()
    };
    let navigation = row![
        text(snapshot.map_or_else(
            || "No dataset open".to_owned(),
            |value| format!(
                "{} of {} samples",
                value.order.matchingcount, value.dataset.imagecount
            )
        )),
        space::horizontal(),
        text("Columns"),
        column_control(columns, settings_available),
        space::horizontal(),
        text(snapshot.map_or("Order unavailable", |value| {
            if value.order.shuffleseed == 0 {
                "Sequential order"
            } else {
                "Shuffled order"
            }
        })),
        reshuffle,
        container(button("Earlier").on_press_maybe(snapshot.and_then(|value| {
            mutation_available.then(|| page_message(state, value, columns, PageDirection::Earlier))
        })))
        .id("explore.gallery.earlier"),
        container(button("Later").on_press_maybe(snapshot.and_then(|value| {
            mutation_available.then(|| page_message(state, value, columns, PageDirection::Later))
        })))
        .id(super::GALLERY_LATER_ID),
    ]
    .spacing(7)
    .align_y(Center);
    let augmentation_enabled = snapshot.is_some_and(|value| value.augmentation.enabled);
    let augmentation = row![
        container(
            checkbox(augmentation_enabled)
                .label("Augmentation preview")
                .on_toggle_maybe(
                    augmentation_update_available.then_some(Message::AugmentationToggled),
                )
        )
        .id(super::AUGMENTATION_TOGGLE_ID),
        container(text(snapshot.map_or_else(
            || "Seed unavailable".to_owned(),
            |value| format!("Seed {}", value.augmentation.seed),
        )))
        .id(super::AUGMENTATION_SEED_ID),
        container(
            button("Reroll Augmentation")
                .on_press_maybe(
                    (mutation_available && augmentation_enabled)
                        .then_some(Message::AugmentationRerollRequested),
                )
                .style(crate::fluent_theme::button_secondary),
        )
        .id(super::AUGMENTATION_REROLL_ID),
    ]
    .spacing(7)
    .align_y(Center);
    let overlay_controls = state.presented_filter(snapshot).map_or_else(
        || text("Overlays unavailable").into(),
        |request| {
            super::overlay::view(&request.overlay, mutation_available, false).map(Message::Overlay)
        },
    );
    let progress: Element<'a, Message> = model.explore.gallery_progress().map_or_else(
        || text("").size(12).into(),
        |(ready, total)| {
            row![
                text(format!("{ready}/{total} ready")).size(12),
                container(progress_bar(0.0..=total.max(1) as f32, ready as f32))
                    .width(48)
                    .height(5),
            ]
            .spacing(5)
            .align_y(Center)
            .into()
        },
    );
    let toolbar = container(
        column![
            navigation,
            augmentation,
            row![overlay_controls, space::horizontal(), progress].align_y(Center)
        ]
        .spacing(4),
    )
    .padding(Padding::from([6, 10]))
    .width(Fill)
    .height(Length::Fixed(TOOLBAR_HEIGHT))
    .style(crate::fluent_theme::container_header);

    let gallery = responsive(move |size| {
        gallery_viewport(state, snapshot, presentation_title, surface, size, columns)
    })
    .width(Fill)
    .height(Fill);
    let status = container(text(gallery_status(&model.explore)).size(12))
        .id(super::STATUS_CARD_ID)
        .padding(Padding::from([6, 10]))
        .width(Fill)
        .height(Length::Fixed(STATUS_HEIGHT))
        .style(crate::fluent_theme::container_header);
    container(column![toolbar, gallery, status].spacing(0))
        .width(Length::Fixed(width))
        .height(Fill)
        .style(crate::fluent_theme::container_card)
        .into()
}

fn augmentation_toggle_available(model: &ApplicationModel, settings: &SettingsModel) -> bool {
    !settings.has_local_edits() && model.explore_augmentation_update_available()
}

#[derive(Clone, Copy)]
enum PageDirection {
    Earlier,
    Later,
}

fn page_message(
    state: &super::state::State,
    snapshot: &crate::generated::ExploreSnapshot,
    columns: u32,
    direction: PageDirection,
) -> Message {
    let current = state.gallery_first_row(snapshot.viewport.firstrow);
    let first_row = match direction {
        PageDirection::Earlier => current.saturating_sub(1),
        PageDirection::Later => current.saturating_add(1),
    };
    Message::ScrollRequested(state.gallery_row_extent(columns) * first_row as f32)
}

fn gallery_viewport<'a>(
    state: &'a super::state::State,
    snapshot: Option<&'a crate::generated::ExploreSnapshot>,
    presentation_title: &'static str,
    surface: Option<Surface>,
    size: Size,
    columns: u32,
) -> Element<'a, Message> {
    let width = size.width.max(1.0);
    let height = size.height.max(1.0);
    let surface = snapshot
        .is_some_and(|value| {
            value.ready
                && value.order.matchingcount != 0
                && value.mode == crate::generated::ExploreMode::Gallery
        })
        .then_some(surface)
        .flatten();
    let matching = snapshot.map_or(0, |value| value.order.matchingcount);
    let first_row = snapshot.map_or(0, |value| value.viewport.firstrow);
    let maximum_extent = snapshot.map_or(
        crate::generated::VisualExtent {
            width: 0,
            height: 0,
        },
        |value| value.maximumatlasextent.clone(),
    );
    let logical_geometry =
        super::state::logical_gallery_geometry(width, height, columns, matching, first_row);
    crate::presentation_surface::gallery::observe(snapshot, false);
    let displayed = surface.and_then(|_| crate::presentation_surface::gallery::displayed());
    let presented = displayed.as_ref().map(|(_, snapshot)| snapshot.as_ref());
    let presented_grid = presented.map_or((columns, logical_geometry.row_count()), |snapshot| {
        (snapshot.viewport.columns, snapshot.viewport.rowcount)
    });
    let virtual_height = logical_geometry.virtual_height();
    let surface: Element<'a, Message> = surface.map_or_else(
        || {
            container(
                column![
                    space::vertical(),
                    text(presentation_title).size(22),
                    text(snapshot.map_or("", |value| value.failure.as_str()))
                        .size(12)
                        .style(crate::fluent_theme::text_secondary),
                    space::vertical(),
                ]
                .align_x(Center),
            )
            .id(super::GALLERY_EMPTY_ID)
            .center(Fill)
            .width(Fill)
            .height(Fill)
            .style(crate::fluent_theme::container_workspace)
            .into()
        },
        |surface| {
            crate::presentation_surface::labels::view(
                crate::presentation_surface::Program {
                    local: Some(local_gestures(state, snapshot, columns)),
                    surface,
                    publish: None,
                    placement: crate::presentation_surface::Placement::GalleryGrid {
                        first_row,
                        columns: presented_grid.0,
                        rows: presented_grid.1,
                        row_capacity: presented_grid.1,
                        row_origin: 0,
                    },
                    control_id: super::GALLERY_WORKSPACE_ID,
                },
                displayed.as_ref().map_or(
                    crate::presentation_surface::labels::Source::Hidden,
                    |(_, snapshot)| {
                        crate::presentation_surface::labels::Source::Gallery(snapshot.clone())
                    },
                ),
            )
        },
    );
    // The scrollable owns the transform of images, labels, and hit testing.
    // Only the native window is materialized; spacers retain the full row-based extent.
    let row_side = width / presented_grid.0.max(1) as f32;
    let top = first_row as f32 * row_side;
    let atlas_height = if matching == 0 {
        height
    } else {
        presented_grid.1 as f32 * row_side
    };
    let content = column![
        space::vertical().height(Length::Fixed(top)),
        container(surface)
            .width(Fill)
            .height(Length::Fixed(atlas_height)),
        space::vertical().height(Length::Fixed(
            (virtual_height - top - atlas_height).max(0.0)
        )),
    ]
    .spacing(0);
    let scroll_capacity = maximum_extent.clone();
    let scroll_layer = scrollable(content)
        .id(super::GALLERY_WORKSPACE_ID)
        .direction(Direction::Vertical(
            Scrollbar::new().width(5).scroller_width(5),
        ))
        .style(crate::fluent_theme::scrollable_default)
        .on_scroll(move |viewport| {
            let offset = viewport.absolute_offset().y;
            let first_row = logical_geometry.first_row_for_offset(offset);
            let row_fraction = (offset / logical_geometry.row_extent()).fract();
            let request = super::state::gallery_geometry_at_fraction(
                width,
                height,
                scroll_capacity.clone(),
                columns,
                matching,
                first_row,
                row_fraction,
            )
            .map(|geometry| crate::generated::ExploreViewportUpdate {
                viewport: geometry.viewport().clone(),
                focusedcompiledindex: snapshot.and_then(|value| value.focusedimage),
            });
            Message::Scrolled {
                first_row,
                row_fraction,
                request,
            }
        })
        .width(Fill)
        .height(Fill);
    let requested = super::state::gallery_geometry_at_fraction(
        width,
        height,
        maximum_extent.clone(),
        columns,
        matching,
        state.gallery_first_row(first_row),
        state.gallery_row_fraction(),
    );
    let outcome = snapshot
        .and_then(|snapshot| snapshot.viewportresult.as_ref())
        .filter(|result| {
            requested
                .as_ref()
                .is_some_and(|geometry| geometry.viewport() == &result.request.viewport)
        })
        .map_or(crate::generated::ExploreViewportOutcome::Ready, |result| {
            result.outcome
        });
    let capacity_copy = match outcome {
        crate::generated::ExploreViewportOutcome::Ready => None,
        crate::generated::ExploreViewportOutcome::VisibleCapacityExceeded => Some(format!(
            "This viewport exceeds the {}-tile capacity. Reduce columns or viewport height.",
            crate::generated::EXPLORE_VISIBLE_ITEM_CAPACITY
        )),
        crate::generated::ExploreViewportOutcome::AtlasExtentExceeded => Some(
            "This viewport exceeds the native atlas dimensions. Reduce columns or viewport height."
                .to_owned(),
        ),
    };
    let capacity_notice: Element<'a, Message> = capacity_copy.map_or_else(
        || space::horizontal().width(0).into(),
        |copy| {
            container(text(copy))
                .id(super::GALLERY_CAPACITY_ID)
                .center(Fill)
                .width(Fill)
                .height(Fill)
                .style(crate::fluent_theme::container_workspace)
                .into()
        },
    );
    let show_capacity = maximum_extent.clone();
    iced::widget::sensor(stack![scroll_layer, capacity_notice])
        .key((
            snapshot.map_or(0, |snapshot| snapshot.dataset.identity),
            columns,
            maximum_extent.width,
            maximum_extent.height,
        ))
        .on_show(move |size| Message::Measured {
            size,
            maximum_extent: show_capacity.clone(),
            columns,
        })
        .on_resize(move |size| Message::Measured {
            size,
            maximum_extent: maximum_extent.clone(),
            columns,
        })
        .into()
}

fn explore_columns(settings: &SettingsModel) -> u32 {
    settings
        .draft
        .as_ref()
        .map_or(4, |draft| draft.workflows.explore.gridwidth)
        .clamp(1, 99) as u32
}

fn column_control(columns: u32, available: bool) -> Element<'static, Message> {
    let constraint = crate::generated::constraint_workflowsexploregridwidth();
    let minimum = constraint.minimum.unwrap_or(1.0) as i32;
    let maximum = constraint.maximum.unwrap_or(99.0) as i32;
    if available {
        let value = columns as i32;
        iced_aw::number_input(&value, minimum..=maximum, Message::ColumnsChanged)
            .ignore_scroll(true)
            .ignore_buttons(true)
            .width(Length::Fixed(42.0))
            .into()
    } else {
        text(columns).into()
    }
}

fn gallery_status(model: &crate::view_model::ExploreModel) -> String {
    let status = model.snapshot.as_ref().map_or_else(
        || "Waiting for the current typed Explore snapshot".to_owned(),
        |value| {
            format!(
                "{} · rows {}–{} · {} workers · revision {}",
                model.presentation_title(),
                value.viewport.firstrow,
                value
                    .viewport
                    .firstrow
                    .saturating_add(value.viewport.rowcount),
                value.nproc,
                value.revision
            )
        },
    );
    match model.gallery_progress() {
        Some((ready, total)) => format!("{status} · {ready}/{total} tiles ready"),
        None => status,
    }
}

#[cfg(test)]
mod tests {
    use super::super::state;
    use super::*;
    use crate::view_model::test_support::explore_snapshot;

    #[test]
    fn native_viewport_tracks_real_scroll_and_stays_within_capacity() {
        let capacity = crate::generated::VisualExtent {
            width: 600,
            height: 420,
        };
        let geometry =
            state::gallery_geometry(600.0, 420.0, capacity.clone(), 4, 1_000, 200).unwrap();
        let viewport = geometry.viewport();
        assert_eq!(viewport.firstrow, 200);
        assert!(
            viewport.rowcount * viewport.columns <= crate::generated::EXPLORE_VISIBLE_ITEM_CAPACITY
        );
        let end = state::gallery_geometry(600.0, 420.0, capacity, 4, 1_000, u32::MAX).unwrap();
        assert_eq!(end.viewport().firstrow, 249);
    }

    #[test]
    fn unknown_native_extent_keeps_logical_scroll_and_recovers_exact_grid() {
        let logical = state::logical_gallery_geometry(600.0, 420.0, 4, 1_000, 0);
        let first_row = logical.first_row_for_offset(1_200.0);
        assert!(
            state::gallery_geometry(
                600.0,
                420.0,
                crate::generated::VisualExtent {
                    width: 0,
                    height: 0,
                },
                4,
                1_000,
                first_row,
            )
            .is_none()
        );
        let recovered = state::gallery_geometry(
            600.0,
            420.0,
            crate::generated::VisualExtent {
                width: 600,
                height: 420,
            },
            4,
            1_000,
            first_row,
        )
        .unwrap();
        assert_eq!(recovered.viewport().firstrow, first_row);
        assert_eq!(recovered.viewport().extent.width % 4, 0);
    }

    #[test]
    fn unknown_native_extent_pager_retains_row_without_reusing_stale_viewport() {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.order.matchingcount = 1_000;
        snapshot.viewport.firstrow = 4;
        snapshot.focusedimage = Some(16);
        let mut state = state::State::default();
        assert!(state.measure_gallery(
            600.0,
            420.0,
            crate::generated::VisualExtent {
                width: 0,
                height: 0,
            },
            4,
        ));

        let Message::ScrollRequested(offset) =
            page_message(&state, &snapshot, 4, PageDirection::Later)
        else {
            panic!("pager must use the deferred scroll path");
        };
        assert_eq!(offset, 750.0);
        state.record_gallery_scroll(5);

        assert!(state.measure_gallery(
            600.0,
            420.0,
            crate::generated::VisualExtent {
                width: 600,
                height: 420,
            },
            4,
        ));
        let recovered = state
            .measured_layout_request(Some(&snapshot), 4, 1_000)
            .unwrap();
        assert_eq!(recovered.viewport.firstrow, 5);
        assert_eq!(recovered.viewport.extent.width % 4, 0);
    }

    #[test]
    fn retained_gallery_records_physical_scroll_while_detail_is_active() {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.mode = crate::generated::ExploreMode::Detail;
        snapshot.order.matchingcount = 127;
        snapshot.viewport.firstrow = 33;
        let mut state = state::State::default();
        assert!(state.measure_gallery(
            900.0,
            900.0,
            crate::generated::VisualExtent {
                width: 1920,
                height: 1080,
            },
            3,
        ));
        state.record_gallery_scroll(33);

        assert!(
            update(
                &mut state,
                Some(&snapshot),
                &mut SettingsModel::default(),
                Message::Scrolled {
                    first_row: 0,
                    row_fraction: 0.0,
                    request: None,
                },
            )
            .unwrap()
            .is_none()
        );

        let reopened = state
            .measured_layout_request(Some(&snapshot), 3, snapshot.order.matchingcount)
            .unwrap();
        assert_eq!(reopened.viewport.firstrow, 0);
    }

    #[test]
    fn focus_requires_an_exact_grid_and_a_displayed_image() {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.mode = crate::generated::ExploreMode::Gallery;
        snapshot.order.matchingcount = 8;
        snapshot.order.visibleindices = (0..8).collect();
        snapshot.focusedimage = Some(0);
        snapshot.viewport.columns = 4;
        snapshot.viewport.rowcount = 2;
        snapshot.viewport.extent = crate::generated::VisualExtent {
            width: 400,
            height: 200,
        };
        snapshot.frame.extent = snapshot.viewport.extent.clone();
        snapshot.gallery.slots = vec![true; snapshot.order.visibleindices.len()];
        crate::view_model::test_support::gallery_layout(&mut snapshot);
        let mut state = state::State::default();
        assert!(state.measure_gallery(
            400.0,
            200.0,
            crate::generated::VisualExtent {
                width: 0,
                height: 0,
            },
            4,
        ));
        let gesture = crate::presentation_surface::SurfaceGesture {
            kind: crate::presentation_surface::SurfaceGestureKind::Viewport,
            sample: crate::presentation_surface::SurfaceSample {
                width: 400,
                height: 200,
                x: 50,
                y: 50,
                content_x: 50.0,
                content_y: 50.0,
                pressed: false,
            },
        };

        assert!(local_gestures(&state, Some(&snapshot), 4)(gesture).is_none());

        assert!(state.measure_gallery(
            400.0,
            200.0,
            crate::generated::VisualExtent {
                width: 400,
                height: 200,
            },
            4,
        ));
        assert!(
            local_gestures(&state, Some(&snapshot), 4)(gesture).is_none(),
            "viewport gestures without displayed pixels must preserve focus"
        );
    }

    #[test]
    fn augmentation_toggle_and_both_rerolls_encode_distinct_endpoints() {
        let mut state = state::State::default();
        let mut settings = SettingsModel::default();
        assert!(matches!(
            update(
                &mut state,
                None,
                &mut settings,
                Message::AugmentationToggled(true),
            )
            .unwrap(),
            Some(Outcome::AugmentationUpdated(
                crate::generated::ExploreAugmentationUpdate { enabled: true }
            ))
        ));

        let toggle = crate::generated::encode_explore_UpdateAugmentation(
            1,
            crate::generated::ExploreAugmentationUpdate { enabled: true },
        );
        let augmentation = crate::generated::encode_explore_RerollAugmentation(2);
        let order = crate::generated::encode_explore_Reroll(3);
        assert_eq!(
            toggle.endpoint,
            crate::generated::ApplicationIntentEndpoint::ExploreUpdateAugmentation
        );
        assert_eq!(
            augmentation.endpoint,
            crate::generated::ApplicationIntentEndpoint::ExploreRerollAugmentation
        );
        assert_eq!(
            order.endpoint,
            crate::generated::ApplicationIntentEndpoint::ExploreReroll
        );
        assert_ne!(augmentation.record.endpoint_id, order.record.endpoint_id);
        assert_eq!(toggle.record.fields.len(), 1);
    }

    #[test]
    fn no_dataset_toggle_accepts_latest_input_while_its_intent_is_pending() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let settings = crate::view::settings::installed_settings_model();
        let snapshot = model.explore.snapshot.as_ref().unwrap();
        assert!(!snapshot.ready);
        assert!(augmentation_toggle_available(&model, &settings));
        assert!(!model.explore_mutation_available());

        let pending = model
            .begin_intent(crate::generated::ApplicationIntentEndpoint::ExploreUpdateAugmentation)
            .unwrap();
        assert!(augmentation_toggle_available(&model, &settings));
        model.abandon_intent(pending);
        assert!(augmentation_toggle_available(&model, &settings));
    }
}
