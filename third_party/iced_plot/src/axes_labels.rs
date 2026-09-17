use crate::{Element, Theme};
use iced::alignment::{Horizontal, Vertical};
use iced::widget::{canvas, column, container, row, text};
use iced::{Color, Length};
use std::cell::RefCell;
use iced::advanced::text::Renderer as _;

pub(crate) const Y_TICK_WIDTH: f32 = 54.0;
const Y_LABEL_WIDTH: f32 = 22.0;

/// Axis text occupies logical layout space outside the shader's data rectangle.
pub(crate) fn stack_with_labels<'a, M: 'a>(
    widget: impl Into<Element<'a, M>>,
    y_ticks: Element<'a, M>,
    x_ticks: Element<'a, M>,
    tick_size: f32,
    x_label: &'a str,
    y_label: &'a str,
    axis_label_size: f32,
    style: &'a crate::style::StyleFn,
) -> Element<'a, M> {
    let plot = row![
        canvas(RotatedLabel { label: y_label, size: axis_label_size, style })
            .width(Y_LABEL_WIDTH).height(Length::Fill),
        container(y_ticks).width(Y_TICK_WIDTH).height(Length::Fill),
        widget.into(),
    ].height(Length::Fill);
    column![plot, row![iced::widget::space().width(Y_LABEL_WIDTH + Y_TICK_WIDTH), container(x_ticks).width(Length::Fill).height(tick_size + 4.0)], container(text(x_label).size(axis_label_size).style(move |theme| iced::widget::text::Style { color: Some(style(theme).axis_label_color) }))
        .align_x(Horizontal::Center).width(Length::Fill)].height(Length::Fill).into()
}
struct RotatedLabel<'a> { label: &'a str, size: f32, style: &'a crate::style::StyleFn }
#[derive(Default)]
struct LabelState {
    geometry: canvas::Cache,
    key: RefCell<Option<(String, f32, Color, iced::Font)>>,
}
impl<M> canvas::Program<M, Theme> for RotatedLabel<'_> {
    type State = LabelState;
    fn draw(&self, state: &LabelState, renderer: &iced::Renderer, theme: &Theme, bounds: iced::Rectangle, _cursor: iced::mouse::Cursor) -> Vec<canvas::Geometry> {
        let font = renderer.default_font();
        let current_color = (self.style)(theme).axis_label_color;
        let mut key = state.key.borrow_mut();
        if key.as_ref().is_none_or(|(label, size, color, cached_font)| label != self.label || *size != self.size || *color != current_color || *cached_font != font) {
            state.geometry.clear();
            *key = Some((self.label.to_owned(), self.size, current_color, font));
        }
        vec![state.geometry.draw(renderer, bounds.size(), |frame| {
            frame.translate(iced::Vector::new(bounds.width * 0.5, bounds.height * 0.5));
            frame.rotate(-std::f32::consts::FRAC_PI_2);
            frame.fill_text(canvas::Text {
                content: self.label.to_owned(), position: iced::Point::ORIGIN,
                color: current_color, font, size: self.size.into(),
                align_x: iced::advanced::text::Alignment::Center, align_y: Vertical::Center,
                ..Default::default()
            });
        })]
    }
}
