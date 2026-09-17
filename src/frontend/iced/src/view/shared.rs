use crate::fluent_theme::Element;
use iced::widget::{column, container, text};
use iced::{Fill, Length};

pub fn card<'a, Message: 'a>(
    title: &'a str,
    subtitle: &'a str,
    content: impl Into<Element<'a, Message>>,
) -> Element<'a, Message> {
    container(
        column![
            text(title).size(18),
            text(subtitle)
                .size(12)
                .style(crate::fluent_theme::text_secondary),
            content.into(),
        ]
        .spacing(crate::fluent_theme::FIELD_SPACING),
    )
    .padding(crate::fluent_theme::CARD_PADDING)
    .width(Fill)
    .style(crate::fluent_theme::container_card)
    .into()
}

pub fn identified<'a, Message: 'a>(
    id: &'static str,
    content: impl Into<Element<'a, Message>>,
) -> Element<'a, Message> {
    container(content).id(id).width(Fill).into()
}

pub fn modal<'a, Message: 'a>(
    id: &'static str,
    width: f32,
    content: impl Into<Element<'a, Message>>,
) -> Element<'a, Message> {
    container(
        container(content)
            .id(id)
            .padding(crate::fluent_theme::MODAL_PADDING)
            .width(Length::Fixed(width))
            .style(crate::fluent_theme::container_modal),
    )
    .center(Fill)
    .width(Fill)
    .height(Fill)
    .into()
}
