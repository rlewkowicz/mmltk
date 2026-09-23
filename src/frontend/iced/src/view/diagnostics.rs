use crate::fluent_theme::Element;
use crate::view_model::ApplicationModel;
use iced::Fill;
use iced::widget::{column, container, row, text};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Message {
    Toggled,
}

#[derive(Debug, Clone, Default)]
pub struct Component {
    pub expanded: bool,
}

impl Component {
    pub fn update(&mut self, message: Message) {
        match message {
            Message::Toggled => self.expanded = !self.expanded,
        }
    }
}

pub fn view<'a>(model: &'a ApplicationModel, component: &Component) -> Element<'a, Message> {
    let content = {
        let presentation = model.presentation.as_ref();
        let typography = model.typography();
        let content = column![
            row![
                text("Transport").size(typography.secondary),
                text(model.connection.label()).size(typography.monospace),
            ]
            .spacing(10),
            row![
                text("Pending intents").size(typography.secondary),
                text(model.pending_count().to_string()).size(typography.monospace),
            ]
            .spacing(10),
            row![
                text("Presentation").size(typography.secondary),
                text(
                    presentation
                        .map(|snapshot| format!(
                            "{:?} · selection revision {}",
                            snapshot.selected.kind, snapshot.revision
                        ))
                        .unwrap_or_else(|| "not installed".into())
                )
                .size(typography.monospace),
            ]
            .spacing(10),
        ]
        .spacing(6);
        content
    };
    // The shell anchors this card at the bottom: keep the trigger on that edge.
    let content = column![
        crate::view::shared::disclosure("diagnostics.content", component.expanded,
            container(content).padding(iced::Padding::ZERO.bottom(6))),
        iced::widget::button(if component.expanded { "Hide diagnostics" } else { "Show diagnostics" })
            .on_press(Message::Toggled),
    ];
    container(content)
        .id("diagnostics.summary")
        .padding(12)
        .width(Fill)
        .style(crate::fluent_theme::container_card)
        .into()
}
