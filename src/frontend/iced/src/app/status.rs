use super::*;
use crate::view::status::{Control, Message as StatusMessage, Opening};
impl App {
    pub(super) fn modal_active(&self) -> bool {
        self.settings.is_open() || self.settings.reset_confirmation() || self.model.dialog_context().is_some()
    }
    pub(super) fn on_status(&mut self, message: StatusMessage) -> Task<Message> {
        let modal_active = self.modal_active();
        let notices = if let Some(integration) = self.integration.as_mut() { integration.status_notices(&mut self.model.notices) } else { &mut self.model.notices };
        let mut focus = None;
        match message {
            StatusMessage::Activate(Control::Settings, _) => {
                if !modal_active { self.status.close(); self.settings.open(); }
            }
            StatusMessage::Activate(Control::Trigger, opening) => {
                self.status.activate(opening, !notices.is_empty());
                if opening != Opening::Mouse { focus = Some(Control::Trigger); }
            }
            StatusMessage::Activate(Control::Detail(id), _) => { focus = Some(Control::Detail(id)); }
            StatusMessage::SelectDetail(id, action) => self.status.select_detail(id, action),
            StatusMessage::Activate(Control::Copy(id), _) => {
                if let Some((token, payload)) = notices.begin_copy(id) {
                    self.status.focus = Some(Control::Copy(id));
                    return Task::batch([
                        iced::widget::operation::focus(Control::Copy(id).id()),
                        iced::clipboard::write(payload).map(move |result| Message::Status(StatusMessage::CopyResolved { token, result })),
                    ]);
                }
            }
            StatusMessage::Activate(Control::Dismiss(id), _) => {
                let controls = self.status.controls(notices);
                let index = controls.iter().position(|control| *control == Control::Copy(id)).unwrap_or(0);
                let focused = matches!(self.status.focus, Some(Control::Copy(owned) | Control::Detail(owned) | Control::Dismiss(owned)) if owned == id);
                notices.dismiss(id);
                if focused {
                    focus = controls.iter().skip(index + 3).copied().next().or_else(|| index.checked_sub(3).and_then(|index| controls.get(index).copied())).or(Some(Control::Trigger));
                }
                if notices.is_empty() { self.status.close(); focus = Some(Control::Trigger); }
            }
            StatusMessage::Activate(Control::Close, _) | StatusMessage::Close => {
                self.status.close(); focus = Some(Control::Trigger);
            }
            StatusMessage::Outside => {
                self.status.close();
                if self.status.focus.is_some() { focus = Some(Control::Trigger); }
            }
            StatusMessage::Hover(inside) => {
                let was_open = self.status.open;
                self.status.hover(inside, !notices.is_empty());
                if was_open && !self.status.open && self.status.focus.is_some_and(|control| !matches!(control, Control::Trigger | Control::Settings)) { focus = Some(Control::Trigger); }
            }
            StatusMessage::Focused(control, focused) => {
                if focused { self.status.focus = Some(control); }
                else if self.status.focus == Some(control) {
                    return crate::view::status::check_focus(self.status.controls(notices), self.interaction_revision);
                }
            }
            StatusMessage::FocusChecked { revision, focused } => {
                if revision != self.interaction_revision { return Task::none(); }
                self.status.focus = focused;
                if focused.is_none() && self.status.open { self.status.close(); }
            }
            StatusMessage::Traverse(reverse) => {
                if self.status.open && self.status.focus.is_some() { focus = Some(self.status.traverse(reverse, notices)); }
                else { return if reverse { iced::widget::operation::focus_previous() } else { iced::widget::operation::focus_next() }; }
            }
            StatusMessage::CopyResolved { token, result } => {
                if !notices.finish_copy(token, result.is_ok()) { return Task::none(); }
                if let Some(integration) = self.integration.as_mut() { return integration.status_copied(token, result.is_ok()); }
            },
            StatusMessage::Environment(environment) => self.status.environment = environment,
        }
        if let Some(control) = focus { self.status.focus = Some(control); iced::widget::operation::focus(control.id()) } else { Task::none() }
    }
}
