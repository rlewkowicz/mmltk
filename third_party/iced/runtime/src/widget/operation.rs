use crate::core::widget::Id;
use crate::core::widget::operation;
use crate::task;
use crate::{Action, Task};

pub use crate::core::widget::operation::scrollable::{AbsoluteOffset, RelativeOffset};

pub fn snap_to<T>(id: impl Into<Id>, offset: impl Into<RelativeOffset<Option<f32>>>) -> Task<T> {
    task::effect(Action::widget(operation::scrollable::snap_to(
        id.into(),
        offset.into(),
    )))
}

pub fn snap_to_end<T>(id: impl Into<Id>) -> Task<T> {
    task::effect(Action::widget(operation::scrollable::snap_to(
        id.into(),
        RelativeOffset::END.into(),
    )))
}

pub fn scroll_to<T>(id: impl Into<Id>, offset: impl Into<AbsoluteOffset<Option<f32>>>) -> Task<T> {
    task::effect(Action::widget(operation::scrollable::scroll_to(
        id.into(),
        offset.into(),
    )))
}

pub fn scroll_by<T>(id: impl Into<Id>, offset: AbsoluteOffset) -> Task<T> {
    task::effect(Action::widget(operation::scrollable::scroll_by(
        id.into(),
        offset,
    )))
}

pub fn focus_previous<T>() -> Task<T> {
    task::effect(Action::widget(operation::focusable::focus_previous()))
}

pub fn focus_next<T>() -> Task<T> {
    task::effect(Action::widget(operation::focusable::focus_next()))
}

pub fn is_focused(id: impl Into<Id>) -> Task<bool> {
    task::widget(operation::focusable::is_focused(id.into()))
}

pub fn focus<T>(id: impl Into<Id>) -> Task<T> {
    task::effect(Action::widget(operation::focusable::focus(id.into())))
}

pub fn move_cursor_to_end<T>(id: impl Into<Id>) -> Task<T> {
    task::effect(Action::widget(operation::text_input::move_cursor_to_end(
        id.into(),
    )))
}

pub fn move_cursor_to_front<T>(id: impl Into<Id>) -> Task<T> {
    task::effect(Action::widget(operation::text_input::move_cursor_to_front(
        id.into(),
    )))
}

pub fn move_cursor_to<T>(id: impl Into<Id>, position: usize) -> Task<T> {
    task::effect(Action::widget(operation::text_input::move_cursor_to(
        id.into(),
        position,
    )))
}

pub fn select_all<T>(id: impl Into<Id>) -> Task<T> {
    task::effect(Action::widget(operation::text_input::select_all(id.into())))
}

pub fn select_range<T>(id: impl Into<Id>, start: usize, end: usize) -> Task<T> {
    task::effect(Action::widget(operation::text_input::select_range(
        id.into(),
        start,
        end,
    )))
}
