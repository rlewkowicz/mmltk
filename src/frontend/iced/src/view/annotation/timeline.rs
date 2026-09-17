use crate::fluent_theme::Element;
use iced::Fill;
use iced::widget::{button, column, container, text};

#[derive(Debug, Clone)]
pub enum Message {
    Action(crate::generated::AnnotationSetupAction),
}

#[derive(Debug, Clone)]
pub(super) enum Outcome {
    EditRequested(crate::generated::AnnotationEditRequest),
}

#[derive(Default)]
pub(super) struct Component;

impl Component {
    pub(super) fn update(&mut self, message: Message) -> Outcome {
        match message {
            Message::Action(action) => {
                Outcome::EditRequested(crate::generated::AnnotationEditRequest {
                    edit: crate::generated::AnnotationEdit::AnnotationSetupEdit(
                        crate::generated::AnnotationSetupEdit { action },
                    ),
                })
            }
        }
    }

    pub(super) fn view<'a>(
        &'a self,
        model: &'a crate::view_model::AnnotationModel,
        available: bool,
    ) -> Element<'a, Message> {
        let state = model.snapshot.as_ref().map(|snapshot| &snapshot.ui);
        let controls = crate::generated::ANNOTATION_SETUP_ACTION_VALUES
            .iter()
            .copied()
            .fold(column![].spacing(4), |controls, action| {
                controls.push(
                    container(
                        button(text(format!("{action:?}"))).on_press_maybe(
                            state
                                .is_some_and(|state| available && action_available(state))
                                .then_some(Message::Action(action)),
                        ),
                    )
                    .id(action_id(action)),
                )
            });
        container(crate::view::shared::card(
            "Timeline",
            "Imported image. Video and live navigation require a frame provider.",
            column![
                controls,
                text(state.map_or_else(
                    || "No annotation frame".to_owned(),
                    |state| format!(
                        "Frame {} · {} × {} · {}",
                        state.scene.frameindex,
                        state.scene.framewidth,
                        state.scene.frameheight,
                        if state.scene.frameready {
                            "ready"
                        } else {
                            "pending"
                        }
                    )
                ))
                .size(12),
            ]
            .spacing(6)
            .width(Fill),
        ))
        .id(super::TIMELINE_ID)
        .width(Fill)
        .into()
    }
}

fn action_id(action: crate::generated::AnnotationSetupAction) -> String {
    format!("annotation.timeline.{action:?}").to_ascii_lowercase()
}

fn action_available(state: &crate::generated::AnnotationUiState) -> bool {
    state.sourcenavigationavailable
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_generated_timeline_action_maps_to_one_typed_edit() {
        let mut component = Component;
        for action in crate::generated::ANNOTATION_SETUP_ACTION_VALUES {
            let Outcome::EditRequested(request) = component.update(Message::Action(*action));
            let crate::generated::AnnotationEdit::AnnotationSetupEdit(edit) = request.edit else {
                panic!("typed timeline edit")
            };
            assert_eq!(edit.action, *action);
        }
    }
}
