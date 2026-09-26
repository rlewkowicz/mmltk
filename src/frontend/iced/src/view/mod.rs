pub mod annotation;
pub mod aspect_ratio;
pub mod diagnostics;
pub mod explore;
pub mod export;
pub mod file_dialog;
pub mod image_viewer;
pub mod live;
pub mod metrics;
pub mod navigation;
pub mod predict;
pub mod router;
pub mod settings;
pub mod shared;
pub mod status;
pub mod train;
pub mod validate;
pub mod workflow;
pub mod workspace;

use crate::fluent_theme::{Element, Theme};
use crate::message::Message;
use crate::view_model::ApplicationModel;
use iced::widget::scrollable::{Direction, Scrollbar};
use iced::widget::{container, opaque, responsive, scrollable, space, stack};
use iced::{Color, Fill, Length, Padding};

pub const HORIZONTAL_SCROLL_ID: &str = "application.horizontal.scroll";
pub const PAGE_SCROLL_ID: &str = "application.page.scroll";
pub const PAGE_MIN_WIDTH: f32 = 1020.0;
pub const PAGE_MAX_WIDTH: f32 = 1500.0;
pub const NAVIGATION_HEIGHT: f32 = 52.0;
const HEADER_PADDING: f32 = 10.0;
const HEADER_GROUP_SPACING: f32 = 8.0;
const SETTINGS_SPACING: f32 = 16.0;
const SCROLLBAR_WIDTH: f32 = 5.0;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CanvasLayout {
    pub canvas_width: f32,
    pub page_width: f32,
    pub page_offset: f32,
    pub horizontal_overflow: bool,
}

pub fn canvas_layout(viewport_width: f32) -> CanvasLayout {
    let canvas_width = viewport_width.max(PAGE_MIN_WIDTH);
    let page_width = canvas_width.min(PAGE_MAX_WIDTH);
    CanvasLayout {
        canvas_width,
        page_width,
        page_offset: ((canvas_width - page_width) * 0.5).max(0.0),
        horizontal_overflow: viewport_width < PAGE_MIN_WIDTH,
    }
}

fn compact_scrollbar() -> Scrollbar {
    Scrollbar::new()
        .width(SCROLLBAR_WIDTH)
        .scroller_width(SCROLLBAR_WIDTH)
}

fn opaque_fill<'a>(content: Element<'a, Message>) -> Element<'a, Message> {
    opaque(container(content).width(Fill).height(Fill))
}

pub fn view<'a>(
    model: &'a ApplicationModel,
    surface: Option<crate::presentation_surface::Surface>,
    diagnostics_component: &'a diagnostics::Component,
    router: &'a router::Router,
    settings_component: &'a settings::Component,
    status_component: &'a status::Component,
) -> Element<'a, Message> {
    let base = responsive(move |size| {
        let layout = canvas_layout(size.width);
        let canvas_width = layout.canvas_width;
        let page_width = layout.page_width;
        let page_offset = layout.page_offset;
        let body: Element<'_, Message> = responsive(move |body_size| -> Element<'_, Message> {
            let page = router
                .view(
                    model,
                    settings_component,
                    surface,
                    page_width,
                    (body_size.height - 10.0).max(1.0),
                )
                .map(Message::Workspace);
            let page = container(page).padding(Padding {
                top: 10.0,
                left: page_offset,
                ..Padding::ZERO
            });
            if router.active() == crate::generated::FeatureId::Explore {
                page.width(Fill).height(Fill).into()
            } else {
                // Iced stores wheel offsets absolutely and unsnaps scrollbar
                // drags immediately. Keep this scroll tree/identity mounted:
                // intrinsic form reflow preserves the viewport, with ordinary
                // content-boundary clamping and no automatic reveal operation.
                scrollable(page)
                    .id(PAGE_SCROLL_ID)
                    .direction(Direction::Vertical(compact_scrollbar()))
                    .style(crate::fluent_theme::scrollable_default)
                    .width(Fill)
                    .height(Fill)
                    .into()
            }
        })
        .into();
        let typography = settings_component.state().typography(model.typography());
        let connection_width = (size.width
            - status::STATUS_WIDTH
            - status::SETTINGS_WIDTH
            - 2.0 * HEADER_PADDING
            - HEADER_GROUP_SPACING
            - SETTINGS_SPACING)
            .max(0.0);
        let connection = container(
            shared::status_text(model.connection.label())
                .size(typography.secondary)
                .compact()
                .style(crate::fluent_theme::text_secondary),
        )
        .width(Length::Shrink.max(connection_width))
        .id("navigation.connection");
        let navigation = responsive(move |available| {
            iced::widget::row![
                scrollable(router.navigation(typography).map(Message::Workspace))
                    .direction(Direction::Horizontal(compact_scrollbar()))
                    .width(Length::Shrink.max((available.width - status::STATUS_WIDTH).max(0.0))),
                container(
                    status_component
                        .trigger(&model.notices)
                        .map(Message::Status)
                )
                .center_x(Fill),
            ]
            .height(Fill)
            .align_y(iced::Center)
        });
        let header = container(
            iced::widget::row![
                navigation,
                iced::widget::row![connection, status_component.settings().map(Message::Status)]
                    .spacing(if connection_width > 0.0 {
                        SETTINGS_SPACING
                    } else {
                        0.0
                    })
                    .align_y(iced::Center),
            ]
            .spacing(HEADER_GROUP_SPACING)
            .align_y(iced::Center),
        )
        .padding([0.0, HEADER_PADDING])
        .height(NAVIGATION_HEIGHT)
        .width(Fill)
        .style(crate::fluent_theme::container_header_shadow);
        let body = scrollable(container(body).width(canvas_width).height(Fill))
            .id(HORIZONTAL_SCROLL_ID)
            .direction(Direction::Horizontal(compact_scrollbar().spacing(0)))
            .style(crate::fluent_theme::scrollable_default)
            .width(Fill)
            .height(Fill);
        let shell = iced::widget::column![header, body].spacing(0).height(Fill);
        let shell = if settings_component.state().diagnostics_visible() {
            shell.push(diagnostics::view(model, diagnostics_component).map(Message::Diagnostics))
        } else {
            shell
        };
        container(shell)
            .width(Fill)
            .height(Fill)
            .style(crate::fluent_theme::container_shell)
    });

    let settings_overlay = settings_component
        .is_open()
        .then(|| opaque_fill(settings_component.view(model).map(Message::Settings)));
    let reset_overlay = settings_component.reset_confirmation().then(|| {
        opaque_fill(
            settings::reset_confirmation(
                !settings_component.has_local_edits() && model.settings_reset_available(),
            )
            .map(Message::Settings),
        )
    });
    let file_dialog_overlay =
        file_dialog::view(model).map(|dialog| opaque_fill(dialog.map(Message::FileDialog)));

    let overlays = [file_dialog_overlay, settings_overlay, reset_overlay];
    let mut layers: Vec<Element<'a, Message>> = Vec::with_capacity(5);
    layers.push(base.into());
    layers.extend(overlays.into_iter().map(|overlay| {
        overlay.map_or_else(
            || space::horizontal().width(0).height(0).into(),
            |overlay| {
                iced::widget::column![
                    space::vertical().height(NAVIGATION_HEIGHT),
                    container(overlay)
                        .width(Fill)
                        .height(Fill)
                        .style(modal_backdrop)
                ]
                .height(Fill)
                .into()
            },
        )
    }));
    status_component.wrap(
        stack(layers).width(Fill).height(Fill).into(),
        &model.notices,
    )
}

fn modal_backdrop(_theme: &Theme) -> iced::widget::container::Style {
    iced::widget::container::Style {
        background: Some(Color::from_rgba(0.0, 0.0, 0.0, 0.76).into()),
        ..Default::default()
    }
}

#[cfg(test)]
mod tests {
    use crate::view_model::ApplicationModel;

    #[test]
    fn header_layout_preserves_button_height_and_status_connection_order() {
        use iced::advanced::{Layout, layout, renderer::Headless, widget};
        use iced::{Rectangle, Size};
        use std::collections::HashMap;

        #[derive(Default)]
        struct Bounds(HashMap<widget::Id, Rectangle>);
        impl widget::Operation for Bounds {
            fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn widget::Operation)) {
                operate(self);
            }
            fn container(&mut self, id: Option<&widget::Id>, bounds: Rectangle) {
                if let Some(id) = id {
                    self.0.insert(id.clone(), bounds);
                }
            }
        }

        let renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
            Default::default(),
            Some("wgpu"),
        ))
        .expect("header layout requires the container renderer");
        let model = crate::view_model::test_support::bootstrapped();
        let router = super::router::Router::default();
        let settings = super::settings::Component::default();
        let status = super::status::Component::default();
        let diagnostics = super::diagnostics::Component::default();
        for width in [320.0, 360.0, 700.0, 1020.0, 1479.0, 2200.0] {
            let mut view = super::view(&model, None, &diagnostics, &router, &settings, &status);
            let mut tree = widget::Tree::new(&view);
            tree.diff(&mut view);
            let node = view.as_widget_mut().layout(
                &mut tree,
                &renderer,
                &layout::Limits::new(Size::ZERO, Size::new(width, 720.0)),
            );
            let mut bounds = Bounds::default();
            view.as_widget_mut()
                .operate(&mut tree, Layout::new(&node), &renderer, &mut bounds);
            let control = |id| bounds.0[&widget::Id::from(id)];
            let train = control("navigation.train");
            assert!(train.height >= 28.0, "width {width}: {train:?}");
            let status = control(super::status::TRIGGER_ID);
            let settings = control("navigation.settings");
            assert!(status.x >= 0.0 && status.x + status.width <= settings.x);
            assert!(settings.x + settings.width <= width);
            assert_eq!(status.height, super::status::CONTROL_HEIGHT);
            assert_eq!(settings.height, status.height);
            if width == 320.0 {
                assert_eq!(control("navigation.connection").width, 0.0);
                continue;
            }
            let connection = control("navigation.connection");
            assert!(status.x + status.width <= connection.x);
            assert!(connection.x + connection.width < settings.x);
            if width >= super::PAGE_MIN_WIDTH {
                let explore = control("navigation.explore");
                let midpoint = (explore.x + explore.width + connection.x) * 0.5;
                assert!(
                    (status.center_x() - midpoint).abs() <= 10.0,
                    "width {width}: Status {status:?}, Explore {explore:?}, Connected {connection:?}"
                );
            }
        }
    }

    #[test]
    fn focused_routes_construct_at_narrow_and_wide_shell_widths() {
        let snapshots = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .map(|snapshot| snapshot.value)
            .collect();
        let mut model = ApplicationModel::default();
        model
            .install_bootstrap(crate::generated::SCHEMA_FINGERPRINT, snapshots)
            .unwrap();
        let mut router = super::router::Router::default();
        let settings = super::settings::Component::default();
        for feature in crate::generated::FEATURE_ID_VALUES {
            router.select(*feature);
            drop(router.view(&model, &settings, None, 700.0, 720.0));
            drop(router.view(&model, &settings, None, 1200.0, 720.0));
        }

        assert_eq!(super::HORIZONTAL_SCROLL_ID, "application.horizontal.scroll");
        assert_eq!(super::PAGE_SCROLL_ID, "application.page.scroll");
        assert_eq!(super::workspace::STABLE_ID, "workflow.visual.workspace");
    }

    #[test]
    fn canvas_keeps_the_reference_minimum_maximum_and_horizontal_overflow() {
        assert_eq!(
            super::canvas_layout(700.0),
            super::CanvasLayout {
                canvas_width: 1020.0,
                page_width: 1020.0,
                page_offset: 0.0,
                horizontal_overflow: true,
            }
        );
        assert_eq!(super::canvas_layout(1020.0).page_width, 1020.0);
        assert_eq!(super::canvas_layout(1500.0).page_width, 1500.0);
        assert_eq!(super::canvas_layout(1800.0).page_width, 1500.0);
        assert_eq!(super::canvas_layout(1800.0).page_offset, 150.0);
        assert!(!super::canvas_layout(1500.0).horizontal_overflow);
    }
}
