mod controls;
mod diagnostics;
mod navigation;
mod settings;
mod workspace;

use crate::app::App;
use crate::message::Message;
use iced::widget::scrollable::{Direction, Scrollbar, Scroller, Status, Style};
use iced::widget::{
    column, container, mouse_area, opaque, responsive, row, scrollable, space, stack,
};
use iced::{Background, Color, Element, Fill, Length, Padding, Shadow, Theme, Vector};

const PAGE_MIN_WIDTH: f32 = 1020.0;
const PAGE_MAX_WIDTH: f32 = 1500.0;
const NAVIGATION_HEIGHT: f32 = 52.0;
const SIDEBAR_PORTION: u16 = 19;
const WORKSPACE_PORTION: u16 = 62;
const PROPERTY_LABEL_PORTION: u16 = 3;
const PROPERTY_INPUT_PORTION: u16 = 2;
const SCROLLBAR_WIDTH: f32 = 5.0;
const SCROLLER_DARKEN_FACTOR: f32 = 0.3;
const HEADER_SHADOW_ALPHA: f32 = 0.16;
const HEADER_SHADOW_SIZE: f32 = 2.0;

fn compact_scrollbar() -> Scrollbar {
    Scrollbar::new()
        .width(SCROLLBAR_WIDTH)
        .scroller_width(SCROLLBAR_WIDTH)
}

fn darken_scroller(scroller: &mut Scroller) {
    if let Background::Color(color) = scroller.background {
        scroller.background = color.mix(Color::BLACK, SCROLLER_DARKEN_FACTOR).into();
    }
}

fn compact_scrollbar_style(theme: &Theme, status: Status) -> Style {
    let mut style = iced::widget::scrollable::default(theme, status);
    darken_scroller(&mut style.vertical_rail.scroller);
    darken_scroller(&mut style.horizontal_rail.scroller);
    style
}

fn header_bar_style(theme: &Theme) -> iced::widget::container::Style {
    let mut style = iced::widget::container::secondary(theme);
    style.shadow = Shadow {
        color: Color::BLACK.scale_alpha(HEADER_SHADOW_ALPHA),
        offset: Vector::new(0.0, HEADER_SHADOW_SIZE),
        blur_radius: HEADER_SHADOW_SIZE,
    };
    style
}

pub fn view(app: &App) -> Element<'_, Message> {
    let base = responsive(move |viewport| {
        let canvas_width = viewport.width.max(PAGE_MIN_WIDTH);
        let page_width = page_width(canvas_width);
        let page_offset = page_offset(canvas_width, page_width, app.ui_scale());
        let body = container(
            row![
                controls::view(app),
                workspace::view(app),
                diagnostics::view(app)
            ]
            .spacing(0)
            .width(Fill)
            .height(Length::Fit),
        )
        .width(Fill)
        .height(Length::Fit)
        .style(iced::widget::container::secondary);

        let document = scrollable(
            container(
                column![space::vertical().height(NAVIGATION_HEIGHT + 10.0), body]
                    .height(Length::Fit)
                    .width(page_width),
            )
            .padding(Padding {
                left: page_offset,
                ..Padding::ZERO
            })
            .width(canvas_width)
            .height(Length::Fit.min(viewport.height))
            .style(iced::widget::container::secondary),
        )
        .direction(Direction::Vertical(compact_scrollbar()))
        .style(compact_scrollbar_style)
        .width(canvas_width)
        .height(Fill);

        let navigation = opaque(
            container(container(navigation::view(app)).width(page_width))
                .padding(Padding {
                    left: page_offset,
                    ..Padding::ZERO
                })
                .width(canvas_width)
                .height(NAVIGATION_HEIGHT)
                .style(header_bar_style),
        );

        scrollable(
            stack![document, navigation]
                .width(canvas_width)
                .height(Fill),
        )
        .direction(Direction::Horizontal(compact_scrollbar()))
        .style(compact_scrollbar_style)
        .width(Fill)
        .height(Fill)
    })
    .width(Fill)
    .height(Fill);

    let mut content: Element<'_, Message> = base.into();
    if app.settings_open {
        content = stack![
            content,
            opaque(mouse_area(settings::view(app)).on_press(Message::ToggleSettings))
        ]
        .into();
    }
    if app.custom_model_confirmation.is_some() {
        content = stack![
            content,
            opaque(
                mouse_area(settings::custom_model_confirmation(app))
                    .on_press(Message::CancelCustomWeights)
            )
        ]
        .into();
    }
    if app.settings_reset_confirmation {
        content = stack![
            content,
            opaque(
                mouse_area(settings::reset_confirmation()).on_press(Message::CancelSettingsReset)
            )
        ]
        .into();
    }
    if app.active_error.is_some() {
        content = stack![content, opaque(settings::error_modal(app))].into();
    }
    content
}

fn page_width(viewport_width: f32) -> f32 {
    viewport_width.clamp(PAGE_MIN_WIDTH, PAGE_MAX_WIDTH)
}

fn page_offset(viewport_width: f32, page_width: f32, ui_scale: f32) -> f32 {
    let offset = (viewport_width - page_width) * 0.5;
    let scale_factor = browser_scale_factor() * ui_scale.clamp(0.85, 1.75);

    (offset * scale_factor).round() / scale_factor
}

#[cfg(target_arch = "wasm32")]
fn browser_scale_factor() -> f32 {
    web_sys::window()
        .map(|window| window.device_pixel_ratio() as f32)
        .filter(|scale_factor| scale_factor.is_finite() && *scale_factor > 0.0)
        .unwrap_or(1.0)
}

#[cfg(not(target_arch = "wasm32"))]
fn browser_scale_factor() -> f32 {
    1.0
}
