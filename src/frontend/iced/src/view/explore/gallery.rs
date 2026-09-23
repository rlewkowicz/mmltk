use iced::advanced::{layout, Layout, mouse, overlay, renderer, Shell};
use iced::advanced::widget::{Operation, Tree};
use iced::{Event, Rectangle, Vector};
use crate::fluent_theme::Theme;
type Renderer = iced::Renderer;
use crate::view::shared::status_text;
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
    LayoutMeasured,
    ViewportChanged(crate::generated::ExploreViewportUpdate),
}

pub(super) fn update(
    state: &mut super::state::State,
    snapshot: Option<&crate::generated::ExploreSnapshot>,
    settings: &mut SettingsModel,
    message: Message,
) -> Result<Option<Outcome>, String> {
    Ok(Some(match message {
        Message::ScrollRequested(offset) => {
            crate::integration_control::report_gallery_scroll("requested", || {
                [
                    f64::from(offset),
                    f64::from(state.gallery_first_row(0)),
                    f64::from(state.gallery_row_fraction()),
                    f64::from(snapshot.map_or(0, |value| value.viewport.firstrow)),
                ]
            });
            Outcome::ScrollRequested(offset)
        }
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
            // The retained sensor also measures beneath Detail. Returning to
            // Gallery reconciles this geometry through the ordinary native event.
            // A first measurement also wakes an Open awaiting usable geometry.
            let changed = state.measure_gallery(size.width, size.height, maximum_extent, columns);
            if !changed {
                return Ok(None);
            }
            let Some(snapshot) =
                snapshot.filter(|value| value.mode == crate::generated::ExploreMode::Gallery)
            else {
                return Ok(Some(Outcome::LayoutMeasured));
            };
            let Some(request) = state.measured_layout_request(
                Some(snapshot),
                columns,
                snapshot.order.matchingcount,
            ) else {
                state.clear_viewport_admission();
                return Ok(Some(Outcome::LayoutMeasured));
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
                Some(super::state::GalleryGestureOutcome::Focused(_)) => return Ok(None),
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
                shown
                    .as_ref()
                    .map(|(_, snapshot)| snapshot.metadata.as_ref()),
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
    paired: Option<(
        Surface,
        std::sync::Arc<crate::presentation_surface::GalleryContent>,
    )>,
    width: f32,
    input: crate::workspace_input::Binding,
) -> Element<'a, Message> {
    let snapshot = model.explore.snapshot.as_ref();
    let presentation_title = model
        .explore
        .gallery_title(paired.is_some(), model.gallery_presentation());
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
    let navigation = toolbar_row(vec![
        (status_text(snapshot.map_or_else(
            || "No dataset open".to_owned(),
            |value| format!(
                "{} of {} samples",
                value.order.matchingcount, value.dataset.imagecount
            )
        )).compact()).into(),
        (space::horizontal()).into(),
        (text("Columns")).into(),
        (column_control(columns, settings_available)).into(),
        (space::horizontal()).into(),
        (status_text(snapshot.map_or("Order unavailable", |value| {
            if value.order.shuffleseed == 0 {
                "Sequential order"
            } else {
                "Shuffled order"
            }
        })).compact()).into(),
        (reshuffle).into(),
        (container(button("Earlier").on_press_maybe(snapshot.and_then(|value| {
            mutation_available.then(|| page_message(state, value, columns, PageDirection::Earlier))
        })))
        .id("explore.gallery.earlier")).into(),
        (container(button("Later").on_press_maybe(snapshot.and_then(|value| {
            mutation_available.then(|| page_message(state, value, columns, PageDirection::Later))
        })))
        .id(super::GALLERY_LATER_ID)).into(),
    ], &[0, 5], 7);
    let augmentation_enabled = snapshot.is_some_and(|value| value.augmentation.enabled);
    let augmentation = toolbar_row(vec![
        (container(
            checkbox(augmentation_enabled)
                .label("Augmentation preview")
                .on_toggle_maybe(
                    augmentation_update_available.then_some(Message::AugmentationToggled),
                )
        )
        .id(super::AUGMENTATION_TOGGLE_ID)).into(),
        (container(status_text(snapshot.map_or_else(
            || "Seed unavailable".to_owned(),
            |value| format!("Seed {}", value.augmentation.seed),
        )).compact())
        .id(super::AUGMENTATION_SEED_ID)).into(),
        (container(
            button("Reroll Augmentation")
                .on_press_maybe(
                    (mutation_available && augmentation_enabled)
                        .then_some(Message::AugmentationRerollRequested),
                )
                .style(crate::fluent_theme::button_secondary),
        )
        .id(super::AUGMENTATION_REROLL_ID)).into(),
    ], &[1], 7);
    let overlay_controls = state.presented_filter(snapshot).map_or_else(
        || status_text("Overlays unavailable").into(),
        |request| {
            super::overlay::view(request.overlay, mutation_available, false).map(Message::Overlay)
        },
    );
    let progress: Element<'a, Message> = model.explore.gallery_progress().map_or_else(
        || text("").size(12).into(),
        |(ready, total)| {
            toolbar_row(vec![
                status_text(format!("{ready}/{total} ready")).size(12).compact().into(),
                container(progress_bar(0.0..=total.max(1) as f32, ready as f32))
                    .width(48)
                    .height(5).into(),
            ], &[0], 5)
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

    let empty_source = model
        .foreground_visual()
        .filter(|source| {
            matches!(
                source,
                crate::generated::PresentationSourceKind::Explore
                    | crate::generated::PresentationSourceKind::Upscale
            )
        })
        .unwrap_or(crate::generated::PresentationSourceKind::Explore);
    let input = input.for_source(empty_source, 0, None);
    let gallery = responsive(move |size| {
        gallery_viewport(
            state,
            snapshot,
            presentation_title,
            paired.clone(),
            size,
            columns,
            input.clone(),
            crate::workspace_fps::enabled(settings),
        )
    })
    .width(Fill)
    .height(Fill);
    let status = container(status_text(gallery_status(&model.explore, Some(presentation_title))).size(12))
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
    displayed: Option<(
        Surface,
        std::sync::Arc<crate::presentation_surface::GalleryContent>,
    )>,
    size: Size,
    columns: u32,
    input: crate::workspace_input::Binding,
    show_fps: bool,
) -> Element<'a, Message> {
    let width = size.width.max(1.0);
    let height = size.height.max(1.0);
    let presented = displayed
        .as_ref()
        .map(|(_, snapshot)| snapshot.metadata.as_ref());
    // Missing pixels do not erase the logical document or the retained scroll row.
    let matching = presented.map_or_else(
        || snapshot.map_or(0, |value| value.order.matchingcount),
        |value| value.order.matchingcount,
    );
    let first_row = presented.map_or_else(
        || state.gallery_first_row(snapshot.map_or(0, |value| value.viewport.firstrow)),
        |value| value.viewport.firstrow,
    );
    let display_columns = presented.map_or(columns, |value| value.viewport.columns);
    let maximum_extent = snapshot.map_or(
        crate::generated::VisualExtent {
            width: 0,
            height: 0,
        },
        |value| value.maximumatlasextent.clone(),
    );
    let logical_geometry =
        super::state::logical_gallery_geometry(width, height, display_columns, matching, first_row);
    let presented_grid = presented.map_or((columns, logical_geometry.row_count()), |snapshot| {
        (snapshot.viewport.columns, snapshot.viewport.rowcount)
    });
    let virtual_height = logical_geometry.virtual_height();
    let surface: Element<'a, Message> = displayed.as_ref().map_or_else(
        || {
            let input_layer = crate::presentation_surface::labels::view(
                crate::presentation_surface::Program {
                    show_fps,
                    input: Some(input.clone()),
                    local: None,
                    publish: None,
                    surface: Surface::empty(),
                    placement: crate::presentation_surface::Placement::Contain,
                    control_id: super::GALLERY_WORKSPACE_ID,
                },
                crate::presentation_surface::labels::Source::Hidden,
            );
            stack![
                input_layer,
                container(
                    column![
                        space::vertical(),
                        text(presentation_title).size(22),
                        status_text(snapshot.map_or("", |value| value.failure.as_str()))
                            .size(12)
                            .style(crate::fluent_theme::text_secondary),
                        space::vertical()
                    ]
                    .align_x(Center)
                )
                .id(super::GALLERY_EMPTY_ID)
                .center(Fill)
                .width(Fill)
                .height(Fill)
                .style(crate::fluent_theme::container_workspace)
            ]
            .into()
        },
        |(surface, metadata)| {
            let image = crate::presentation_surface::labels::view(
                crate::presentation_surface::Program {
                    show_fps,
                    input: Some(input.for_source(
                        crate::generated::PresentationSourceKind::Explore,
                        0,
                        None,
                    )),
                    local: Some(local_gestures(state, snapshot, columns)),
                    surface: *surface,
                    publish: None,
                    placement: crate::presentation_surface::Placement::GalleryGrid {
                        first_row,
                        columns: presented_grid.0,
                        rows: presented_grid.1,
                        row_capacity: presented.map_or(presented_grid.1, |snapshot| {
                            snapshot.gallery.layout.rowcapacity
                        }),
                        row_origin: presented
                            .map_or(0, |snapshot| snapshot.gallery.layout.roworigin),
                    },
                    control_id: super::GALLERY_WORKSPACE_ID,
                },
                crate::presentation_surface::labels::Source::Gallery(
                    metadata.clone(),
                    state
                        .presented_filter(snapshot)
                        .map_or(metadata.metadata.overlay.showlabels, |request| {
                            request.overlay.showlabels
                        }),
                ),
            );
            if metadata.metadata.order.matchingcount == 0 {
                stack![
                    image,
                    container(text("No samples match the filters").size(22))
                        .id(super::GALLERY_EMPTY_ID)
                        .center(Fill)
                        .width(Fill)
                        .height(Fill)
                        .style(crate::fluent_theme::container_workspace)
                ]
                .into()
            } else {
                image
            }
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
            crate::integration_control::report_gallery_scroll("observed", || {
                [
                    f64::from(offset),
                    f64::from(logical_geometry.row_extent()),
                    f64::from(first_row),
                    f64::from(matching),
                ]
            });
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
            container(status_text(copy).align_x(Center))
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

/// Reserve fixed controls before fitting compact captions; only the existing
/// spacer cells receive slack. At ordinary widths this is the original row
/// geometry, while narrow rows cannot give the progress bar's pixels to text.
fn toolbar_row<'a>(children: Vec<Element<'a, Message>>, captions: &'static [usize], spacing: f32) -> Element<'a, Message> {
    iced::Element::new(ToolbarRow { children, captions, spacing })
}

struct ToolbarRow<'a> {
    children: Vec<Element<'a, Message>>,
    captions: &'static [usize],
    spacing: f32,
}

impl iced::advanced::Widget<Message, crate::fluent_theme::Theme, iced::Renderer> for ToolbarRow<'_> {
    fn size(&self) -> Size<Length> {
        Size::new(if self.children.iter().any(|child| child.as_widget().size().width.is_fill()) { Fill } else { Length::Shrink }, Length::Shrink)
    }
    fn diff(&mut self, tree: &mut iced::advanced::widget::Tree) { tree.diff_children(&mut self.children); }
    fn layout(&mut self, tree: &mut iced::advanced::widget::Tree, renderer: &iced::Renderer, limits: &iced::advanced::layout::Limits) -> iced::advanced::layout::Node {
        let spacing = self.spacing * self.children.len().saturating_sub(1) as f32;
        let mut fixed = spacing;
        let mut natural = 0.0;
        let mut spacers = 0;
        let mut nodes = Vec::with_capacity(self.children.len());
        let natural_limits = layout::Limits::new(Size::ZERO, Size::new(f32::INFINITY, limits.max().height));
        for (index, child) in self.children.iter_mut().enumerate() {
            let node = if child.as_widget().size().width.is_fill() {
                spacers += 1;
                layout::Node::new(Size::ZERO)
            } else {
                child.as_widget_mut().layout(&mut tree.children[index], renderer, &natural_limits)
            };
            if self.captions.contains(&index) { natural += node.size().width; }
            else { fixed += node.size().width; }
            nodes.push(node);
        }
        let available = (limits.max().width - fixed).max(0.0);
        if natural > available {
            // A proportional budget preserves every caption rather than letting
            // the first long string consume a later label's entire allocation.
            for &index in self.captions {
                let width = available * (nodes[index].size().width / natural);
                nodes[index] = self.children[index].as_widget_mut().layout(&mut tree.children[index], renderer,
                    &layout::Limits::new(Size::ZERO, Size::new(width, limits.max().height)));
            }
        }
        let used = spacing + nodes.iter().map(|node| node.size().width).sum::<f32>();
        let slack = if spacers > 0 && limits.max().width.is_finite() { (limits.max().width - used).max(0.0) / spacers as f32 } else { 0.0 };
        let height = nodes.iter().map(|node| node.size().height).fold(0.0, f32::max);
        let mut x = 0.0;
        for (index, node) in nodes.iter_mut().enumerate() {
            if self.children[index].as_widget().size().width.is_fill() { *node = layout::Node::new(Size::new(slack, height)); }
            let size = node.size();
            node.move_to_mut(iced::Point::new(x, (height - size.height) * 0.5));
            x += size.width + self.spacing;
        }
        let width = (x - self.spacing).max(0.0);
        layout::Node::with_children(limits.resolve(self.size().width, Length::Shrink, Size::new(width, height)), nodes)
    }
    fn operate(
        &mut self,
        tree: &mut Tree,
        layout: Layout<'_>,
        renderer: &Renderer,
        operation: &mut dyn Operation,
    ) {
        operation.container(None, layout.bounds());
        operation.traverse(&mut |operation| {
            self.children
                .iter_mut()
                .zip(&mut tree.children)
                .zip(layout.children())
                .for_each(|((child, state), layout)| {
                    child
                        .as_widget_mut()
                        .operate(state, layout, renderer, operation);
                });
        });
    }

    fn update(
        &mut self,
        tree: &mut Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        renderer: &Renderer,
        shell: &mut Shell<'_, Message>,
        viewport: &Rectangle,
    ) {
        for ((child, tree), layout) in self
            .children
            .iter_mut()
            .zip(&mut tree.children)
            .zip(layout.children())
        {
            child
                .as_widget_mut()
                .update(tree, event, layout, cursor, renderer, shell, viewport);
        }
    }

    fn mouse_interaction(
        &self,
        tree: &Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
        renderer: &Renderer,
    ) -> mouse::Interaction {
        self.children
            .iter()
            .zip(&tree.children)
            .zip(layout.children())
            .map(|((child, tree), layout)| {
                child
                    .as_widget()
                    .mouse_interaction(tree, layout, cursor, viewport, renderer)
            })
            .max()
            .unwrap_or_default()
    }

    fn draw(
        &self,
        tree: &Tree,
        renderer: &mut Renderer,
        theme: &Theme,
        style: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        if let Some(clipped_viewport) = layout.bounds().intersection(viewport) {
            let _ = clipped_viewport;

            for ((child, tree), layout) in self
                .children
                .iter()
                .zip(&tree.children)
                .zip(layout.children())
                .filter(|(_, layout)| layout.bounds().intersects(viewport))
            {
                child
                    .as_widget()
                    .draw(tree, renderer, theme, style, layout, cursor, viewport);
            }
        }
    }

    fn overlay<'b>(
        &'b mut self,
        tree: &'b mut Tree,
        layout: Layout<'b>,
        renderer: &Renderer,
        viewport: &Rectangle,
        translation: Vector,
    ) -> Option<overlay::Element<'b, Message, Theme, Renderer>> {
        overlay::from_children(
            &mut self.children,
            tree,
            layout,
            renderer,
            viewport,
            translation,
        )
    }
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

fn gallery_status(model: &crate::view_model::ExploreModel, title: Option<&str>) -> String {
    let status = model.snapshot.as_ref().map_or_else(
        || "Waiting for the current typed Explore snapshot".to_owned(),
        |value| {
            format!(
                "{} · rows {}–{} · {} workers · revision {}",
                title.unwrap_or_else(|| model.presentation_title()),
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
    fn compact_toolbar_preserves_wide_positions_and_reserves_narrow_controls() {
        use iced::advanced::renderer::Headless;
        let renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(Default::default(), Some("wgpu"))).unwrap();
        let children = |compact: bool| -> Vec<Element<'static, Message>> {
            let caption = |value| -> Element<'static, Message> {
                if compact { status_text(value).size(12).compact().into() } else { text(value).size(12).into() }
            };
            vec![caption("123456 of 234567 samples"), space::horizontal().into(),
                text("Columns").into(), space::horizontal().width(42).into(), space::horizontal().into(),
                caption("Sequential order"), button("Earlier").into(), button("Later").into()]
        };
        let measure = |mut element: Element<'_, Message>, width| {
            let mut tree = Tree::new(&element);
            tree.diff(&mut element);
            element.as_widget_mut().layout(&mut tree, &renderer, &layout::Limits::new(Size::ZERO, Size::new(width, 100.0)))
        };
        let ordinary = measure(iced::widget::Row::from_vec(children(false)).spacing(7).align_y(Center).into(), 900.0);
        let wide = measure(toolbar_row(children(true), &[0, 5], 7.0), 900.0);
        for (before, after) in ordinary.children().iter().zip(wide.children()) {
            assert!((before.bounds().x - after.bounds().x).abs() < 0.1);
            assert!((before.size().width - after.size().width).abs() < 0.1);
        }
        let narrow = measure(toolbar_row(children(true), &[0, 5], 7.0), 330.0);
        for index in [2, 3, 6, 7] {
            assert_eq!(narrow.children()[index].size().width, wide.children()[index].size().width);
        }
        assert!(narrow.children()[7].bounds().x + narrow.children()[7].size().width <= 330.1);
        for width in [400.0, 90.0] {
            let progress = measure(toolbar_row(vec![
                status_text("123456/234567 ready").size(12).compact().into(),
                container(progress_bar(0.0..=1.0, 0.5)).width(48).height(5).into(),
            ], &[0], 5.0), width);
            let caption = &progress.children()[0];
            let bar = &progress.children()[1];
            assert_eq!(bar.size().width, 48.0);
            assert!((bar.bounds().x - caption.size().width - 5.0).abs() < 0.1);
            assert!(progress.size().width <= width + 0.1);
        }
    }

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
    fn measurements_retain_capacity_without_native_state_and_deduplicate_when_available() {
        let mut state = state::State::default();
        let mut settings = SettingsModel::default();
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.mode = crate::generated::ExploreMode::Gallery;
        snapshot.order.matchingcount = 127;
        let measurement = |height| Message::Measured {
            size: Size::new(601.5, 420.25),
            maximum_extent: crate::generated::VisualExtent {
                width: 1024,
                height,
            },
            columns: 4,
        };
        assert!(matches!(
            update(&mut state, None, &mut settings, measurement(0)).unwrap(),
            Some(Outcome::LayoutMeasured)
        ));
        assert_eq!(state.gallery_size(), Some(Size::new(601.5, 420.25)));
        assert!(
            state
                .measured_layout_request(Some(&snapshot), 4, 127)
                .is_none()
        );
        assert!(matches!(
            update(
                &mut state,
                Some(&snapshot),
                &mut settings,
                measurement(1024)
            )
            .unwrap(),
            Some(Outcome::ViewportChanged(_))
        ));
        assert!(
            update(
                &mut state,
                Some(&snapshot),
                &mut settings,
                measurement(1024)
            )
            .unwrap()
            .is_none()
        );
    }

    #[test]
    fn focus_requires_an_exact_grid_and_a_displayed_image() {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.mode = crate::generated::ExploreMode::Gallery;
        snapshot.order.matchingcount = 8;
        snapshot.order.visibleindices = (0..8).collect();
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
