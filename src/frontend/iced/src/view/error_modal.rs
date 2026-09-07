use crate::fluent_theme::Element;
use crate::view_model::UiError;
use iced::Fill;
use iced::widget::{button, column, container, row, space, text};

pub const COPY_ID: &str = "error.copy";
pub const DISMISS_ID: &str = "error.dismiss";

#[derive(Debug, Clone, PartialEq)]
pub enum Message {
    Copy,
    CopyResolved {
        source: UiError,
        result: Result<(), iced::clipboard::Error>,
    },
    Dismiss,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Outcome {
    CopyRequested { source: UiError, payload: String },
    CopySucceeded,
    CopyFailed,
    Dismissed,
    Ignored,
}

pub fn update(error: Option<&UiError>, message: Message) -> Outcome {
    match message {
        Message::Copy => error.map_or(Outcome::Ignored, |error| Outcome::CopyRequested {
            source: error.clone(),
            payload: clipboard_payload(error),
        }),
        Message::CopyResolved { source, result } => {
            if error != Some(&source) {
                Outcome::Ignored
            } else if result.is_ok() {
                Outcome::CopySucceeded
            } else {
                Outcome::CopyFailed
            }
        }
        Message::Dismiss => Outcome::Dismissed,
    }
}

fn clipboard_payload(error: &UiError) -> String {
    format!("{}\n\n{}", error.title, error.detail)
}

pub fn view(error: &UiError) -> Element<'_, Message> {
    crate::view::shared::modal(
        "error.modal",
        560.0,
        column![
            text(error.title).size(25),
            container(text(&error.detail))
                .padding(14)
                .width(Fill)
                .style(crate::fluent_theme::container_error),
            row![
                container(
                    button("Copy")
                        .on_press(Message::Copy)
                        .style(crate::fluent_theme::button_secondary)
                )
                .id(COPY_ID),
                space::horizontal(),
                container(
                    button("Dismiss")
                        .on_press(Message::Dismiss)
                        .style(crate::fluent_theme::button_primary)
                )
                .id(DISMISS_ID),
            ],
        ]
        .spacing(16),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dismissal_is_not_misrepresented_as_retry() {
        assert_eq!(update(None, Message::Dismiss), Outcome::Dismissed);
    }

    #[test]
    fn copy_uses_the_exact_visible_title_blank_line_and_detail() {
        let error = UiError::protocol("decoded detail");
        assert_eq!(
            update(Some(&error), Message::Copy),
            Outcome::CopyRequested {
                source: error,
                payload: "Protocol error\n\ndecoded detail".to_owned(),
            }
        );
    }

    #[test]
    fn clipboard_completion_is_actionable_only_for_the_visible_source_error() {
        let source = UiError::protocol("decoded detail");
        assert_eq!(
            update(
                Some(&source),
                Message::CopyResolved {
                    source: source.clone(),
                    result: Ok(()),
                }
            ),
            Outcome::CopySucceeded
        );
        assert_eq!(
            update(
                Some(&source),
                Message::CopyResolved {
                    source: source.clone(),
                    result: Err(iced::clipboard::Error::ClipboardUnavailable),
                }
            ),
            Outcome::CopyFailed
        );
        assert_eq!(
            update(
                None,
                Message::CopyResolved {
                    source: source.clone(),
                    result: Err(iced::clipboard::Error::ClipboardUnavailable),
                }
            ),
            Outcome::Ignored
        );
        let replacement = UiError::busy("another error");
        assert_eq!(
            update(
                Some(&replacement),
                Message::CopyResolved {
                    source,
                    result: Err(iced::clipboard::Error::ClipboardUnavailable),
                }
            ),
            Outcome::Ignored
        );
    }

    #[test]
    fn modal_controls_publish_stable_component_owned_identities() {
        assert_eq!(COPY_ID, "error.copy");
        assert_eq!(DISMISS_ID, "error.dismiss");
    }
}
