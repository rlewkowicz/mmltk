use crate::fluent_theme::Element;
use crate::view_model::ApplicationModel;
use iced::widget::{button, column, container, text};

#[derive(Debug, Clone, Copy)]
pub enum Message {
    StopRequested,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    StopRequested,
}

pub const STABLE_ID: &str = "dialog.file.active";
pub const STOP_STABLE_ID: &str = "dialog.file.stop";

pub const fn update(message: Message) -> Outcome {
    match message {
        Message::StopRequested => Outcome::StopRequested,
    }
}

pub fn view(model: &ApplicationModel) -> Option<Element<'_, Message>> {
    let snapshot = model
        .file_dialog
        .as_ref()
        .filter(|snapshot| snapshot.active)?;
    let context = model.dialog_context()?;
    (snapshot.target == context.target).then(|| {
        crate::view::shared::modal(
            STABLE_ID,
            420.0,
            column![
                text(context.title).size(24),
                text("The native file chooser is open."),
                container(
                    button("Cancel file selection")
                        .on_press_maybe(
                            model
                                .dialog_stop_available()
                                .then_some(Message::StopRequested)
                        )
                        .style(crate::fluent_theme::button_secondary),
                )
                .id(STOP_STABLE_ID),
            ]
            .spacing(14),
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn local_message_reduces_to_a_compact_domain_outcome() {
        assert_eq!(update(Message::StopRequested), Outcome::StopRequested);
        assert_eq!(STABLE_ID, "dialog.file.active");
        assert_eq!(STOP_STABLE_ID, "dialog.file.stop");
    }
}
