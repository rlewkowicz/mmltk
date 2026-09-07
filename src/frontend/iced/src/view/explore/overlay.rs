use crate::fluent_theme::Element;
use iced::Center;
use iced::widget::{checkbox, container, row};

#[derive(Debug, Clone)]
pub enum Message {
    LabelsToggled(bool),
    MasksToggled(bool),
    BoxesToggled(bool),
}

pub(super) fn update(
    state: &mut super::state::State,
    snapshot: Option<&crate::generated::ExploreSnapshot>,
    message: Message,
) -> Result<crate::generated::ExploreOverlay, String> {
    let mut request = state
        .presented_filter(snapshot)
        .ok_or_else(|| "Explore overlays are not installed".to_owned())?;
    match message {
        Message::LabelsToggled(value) => request.overlay.showlabels = value,
        Message::MasksToggled(value) => request.overlay.showmasks = value,
        Message::BoxesToggled(value) => request.overlay.showboxes = value,
    }
    state.record_submission(request.clone());
    Ok(request.overlay)
}

pub(super) fn view(
    overlay: &crate::generated::ExploreOverlay,
    available: bool,
    detail: bool,
) -> Element<'static, Message> {
    let ids = if detail {
        [
            super::DETAIL_LABELS_ID,
            super::DETAIL_MASKS_ID,
            super::DETAIL_BOXES_ID,
        ]
    } else {
        [
            super::GALLERY_LABELS_ID,
            super::GALLERY_MASKS_ID,
            super::GALLERY_BOXES_ID,
        ]
    };
    row![
        container(
            checkbox(overlay.showlabels)
                .label("Labels")
                .on_toggle_maybe(available.then_some(Message::LabelsToggled))
        )
        .id(ids[0]),
        container(
            checkbox(overlay.showmasks)
                .label("Masks")
                .on_toggle_maybe(available.then_some(Message::MasksToggled))
        )
        .id(ids[1]),
        container(
            checkbox(overlay.showboxes)
                .label("Boxes")
                .on_toggle_maybe(available.then_some(Message::BoxesToggled))
        )
        .id(ids[2]),
    ]
    .spacing(7)
    .align_y(Center)
    .into()
}
