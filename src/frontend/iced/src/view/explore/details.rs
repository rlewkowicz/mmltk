use super::dataset::{class_checklist, class_selection, toggle_selection};
use super::filter_edit;
use crate::fluent_theme::Element;
use crate::view_model::{ApplicationModel, ExploreModel};
use iced::Center;
use iced::widget::{column, container, row, rule, space, text};

#[derive(Debug, Clone)]
pub enum Message {
    ClassToggled(u32),
    AllClasses,
    NoClasses,
}

#[derive(Debug, Clone)]
pub(super) enum Outcome {
    FilterEdited(crate::generated::ExploreFilterUpdate),
}

pub(super) fn update(
    state: &super::state::State,
    model: &ExploreModel,
    message: Message,
) -> Result<Outcome, String> {
    let class_count = model
        .snapshot
        .as_ref()
        .map_or(0, |value| value.dataset.classnames.len());
    let request = filter_edit(state, model, |request| match message {
        Message::AllClasses => {
            request.overlay.classselection =
                class_selection(crate::generated::ExploreClassSelectionMode::All);
        }
        Message::NoClasses => {
            request.overlay.classselection =
                class_selection(crate::generated::ExploreClassSelectionMode::None);
        }
        Message::ClassToggled(index) => {
            toggle_selection(&mut request.overlay.classselection, class_count, index);
        }
    })?;
    Ok(Outcome::FilterEdited(request))
}

pub(super) fn view<'a>(
    state: &'a super::state::State,
    model: &'a ApplicationModel,
    settings: &'a crate::view::settings::SettingsModel,
) -> Element<'a, Message> {
    let snapshot = model.explore.snapshot.as_ref();
    let filter = state.presented_filter(snapshot);
    let available = !settings.has_local_edits() && model.explore_mutation_available();
    let status = if snapshot.is_some_and(|value| value.cancellationrequested) {
        "Stopping Explore"
    } else {
        model.explore.presentation_title()
    };
    let overlay_classes = class_checklist(
        snapshot,
        filter.as_ref().map(|filter| &filter.overlay.classselection),
        available,
        Message::ClassToggled,
    );
    let failure: Element<'a, Message> = snapshot
        .filter(|value| !value.failure.is_empty())
        .map_or_else(
            || space::vertical().height(0).into(),
            |value| {
                text(value.failure.as_str())
                    .style(crate::fluent_theme::text_secondary)
                    .into()
            },
        );
    super::sidebar(
        super::DETAILS_SCROLL_ID,
        "Dataset details",
        column![
            text("Dataset metadata").size(20),
            text(status).style(crate::fluent_theme::text_secondary),
            failure,
            fact(
                "Images",
                snapshot.map_or_else(
                    || "—".to_owned(),
                    |value| value.dataset.imagecount.to_string()
                )
            ),
            fact(
                "Compiled size",
                snapshot.map_or_else(
                    || "—".to_owned(),
                    |value| format!(
                        "{} × {}",
                        value.dataset.imagewidth, value.dataset.imageheight
                    )
                )
            ),
            fact(
                "Classes",
                snapshot.map_or_else(
                    || "—".to_owned(),
                    |value| value.dataset.classnames.len().to_string()
                )
            ),
            fact(
                "Matches",
                snapshot.map_or_else(
                    || "—".to_owned(),
                    |value| value.order.matchingcount.to_string()
                )
            ),
            rule::horizontal(1),
            text("Selected sample").size(18),
            text(snapshot.and_then(|value| value.selectedimage).map_or_else(
                || "None".to_owned(),
                |index| format!("Compiled image #{index}")
            )),
            rule::horizontal(1),
            text("Overlay").size(18),
            row![
                text("Visible classes"),
                space::horizontal(),
                iced::widget::button("All")
                    .on_press_maybe(available.then_some(Message::AllClasses)),
                container(
                    iced::widget::button("None")
                        .on_press_maybe(available.then_some(Message::NoClasses)),
                )
                .id(super::OVERLAY_NONE_ID),
            ],
            container(overlay_classes).id(super::OVERLAY_CLASSES_ID),
            rule::horizontal(1),
            text("Native render").size(18),
            fact(
                "Viewport",
                snapshot.map_or_else(
                    || "—".to_owned(),
                    |value| format!(
                        "{} columns · {} rows",
                        value.viewport.columns, value.viewport.rowcount
                    )
                )
            ),
            fact(
                "Shuffle seed",
                snapshot.map_or_else(
                    || "—".to_owned(),
                    |value| value.order.shuffleseed.to_string()
                )
            ),
            fact(
                "Revision",
                snapshot.map_or_else(|| "—".to_owned(), |value| value.revision.to_string())
            ),
        ]
        .spacing(10),
    )
}

fn fact<'a>(label: &'static str, value: String) -> Element<'a, Message> {
    row![text(label), space::horizontal(), text(value)]
        .align_y(Center)
        .into()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view_model::test_support::explore_snapshot;

    #[test]
    fn overlay_class_toggle_is_one_generated_filter_update() {
        let mut model = ExploreModel::default();
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.dataset.classnames = vec![
            crate::generated::ClassName { value: "a".into() },
            crate::generated::ClassName { value: "b".into() },
        ];
        model.snapshot = Some(snapshot);
        let state = super::super::state::State::default();
        let Outcome::FilterEdited(request) =
            update(&state, &model, Message::ClassToggled(1)).unwrap();
        assert_eq!(
            request.overlay.classselection.mode,
            crate::generated::ExploreClassSelectionMode::Subset
        );
        assert_eq!(request.overlay.classselection.classes, vec![0]);
    }
}
