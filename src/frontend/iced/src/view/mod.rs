pub mod annotation;
pub mod aspect_ratio;
pub mod diagnostics;
pub mod error_modal;
pub mod explore;
pub mod export;
pub mod file_dialog;
pub mod live;
pub mod navigation;
pub mod predict;
pub mod router;
pub mod settings;
pub mod shared;
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

pub const PAGE_SCROLL_ID: &str = "application.page.scroll";
pub const PAGE_MIN_WIDTH: f32 = 1020.0;
pub const PAGE_MAX_WIDTH: f32 = 1500.0;
pub const NAVIGATION_HEIGHT: f32 = 52.0;
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

fn page_canvas_layout(viewport_width: f32, feature: crate::generated::FeatureId) -> CanvasLayout {
    if feature != crate::generated::FeatureId::Annotate {
        return canvas_layout(viewport_width);
    }
    let canvas_width = viewport_width.max(1.0);
    let page_width = canvas_width.min(PAGE_MAX_WIDTH);
    CanvasLayout {
        canvas_width,
        page_width,
        page_offset: (canvas_width - page_width) * 0.5,
        horizontal_overflow: false,
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
) -> Element<'a, Message> {
    let base = responsive(move |size| {
        let layout = page_canvas_layout(size.width, router.active());
        let canvas_width = layout.canvas_width;
        let page_width = layout.page_width;
        let page_offset = layout.page_offset;
        let page = router
            .view(model, settings_component, surface, page_width)
            .map(Message::Workspace);
        let page = container(page).padding(Padding {
            top: 10.0,
            left: page_offset,
            ..Padding::ZERO
        });
        let body: Element<'_, Message> = if router.active() == crate::generated::FeatureId::Explore
        {
            page.width(Fill).height(Fill).into()
        } else {
            scrollable(page)
                .id(PAGE_SCROLL_ID)
                .direction(Direction::Vertical(compact_scrollbar()))
                .style(crate::fluent_theme::scrollable_default)
                .width(Fill)
                .height(Fill)
                .into()
        };
        let header = scrollable(
            container(
                container(
                    router
                        .navigation(
                            model.connection.label(),
                            settings_component.state().typography(model.typography()),
                        )
                        .map(Message::Workspace),
                )
                .width(Length::Fixed(page_width.max(PAGE_MIN_WIDTH)))
                .center_y(Fill),
            )
            .padding(Padding {
                left: page_offset,
                ..Padding::ZERO
            })
            .width(Length::Fixed(canvas_width.max(PAGE_MIN_WIDTH)))
            .height(Length::Fixed(NAVIGATION_HEIGHT))
            .style(crate::fluent_theme::container_header_shadow),
        )
        .direction(Direction::Horizontal(compact_scrollbar()))
        .width(Fill)
        .height(Length::Fixed(NAVIGATION_HEIGHT));
        let shell = iced::widget::column![header, body,].spacing(0).height(Fill);
        let shell = if settings_component.state().diagnostics_visible() {
            shell.push(diagnostics::view(model, diagnostics_component).map(Message::Diagnostics))
        } else {
            shell
        };
        let shell: Element<'_, Message> = container(shell)
            .width(Length::Fixed(canvas_width))
            .height(Fill)
            .style(crate::fluent_theme::container_shell)
            .into();
        scrollable(shell)
            .direction(Direction::Horizontal(compact_scrollbar()))
            .style(crate::fluent_theme::scrollable_default)
            .width(Fill)
            .height(Fill)
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
    let error_overlay = model
        .error
        .as_ref()
        .map(|error| opaque_fill(error_modal::view(error).map(Message::Error)));
    let file_dialog_overlay =
        file_dialog::view(model).map(|dialog| opaque_fill(dialog.map(Message::FileDialog)));

    let overlays = [
        file_dialog_overlay,
        settings_overlay,
        reset_overlay,
        error_overlay,
    ];
    let mut layers: Vec<Element<'a, Message>> = Vec::with_capacity(5);
    layers.push(base.into());
    layers.extend(overlays.into_iter().map(|overlay| {
        overlay.map_or_else(
            || space::horizontal().width(0).height(0).into(),
            |overlay| {
                container(overlay)
                    .padding(Padding::ZERO)
                    .width(Fill)
                    .height(Fill)
                    .style(modal_backdrop)
                    .into()
            },
        )
    }));
    stack(layers).width(Fill).height(Fill).into()
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
            drop(router.view(&model, &settings, None, 700.0));
            drop(router.view(&model, &settings, None, 1200.0));
        }

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
    #[test]
    fn annotation_canvas_uses_the_available_compact_width() {
        let layout = super::page_canvas_layout(700.0, crate::generated::FeatureId::Annotate);
        assert_eq!(layout.page_width, 700.0);
        assert!(!layout.horizontal_overflow);
        assert_eq!(
            super::page_canvas_layout(700.0, crate::generated::FeatureId::Train),
            super::canvas_layout(700.0)
        );
    }
}
