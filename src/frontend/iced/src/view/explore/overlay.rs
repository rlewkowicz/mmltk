use crate::fluent_theme::Element;

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
    crate::view::workflow::overlay_controls::view(overlay.showlabels, overlay.showmasks, overlay.showboxes,
        available, available, ids).map(|message| match message {
            crate::view::workflow::overlay_controls::Message::Labels(value) => Message::LabelsToggled(value),
            crate::view::workflow::overlay_controls::Message::Masks(value) => Message::MasksToggled(value),
            crate::view::workflow::overlay_controls::Message::Boxes(value) => Message::BoxesToggled(value),
        })
}
