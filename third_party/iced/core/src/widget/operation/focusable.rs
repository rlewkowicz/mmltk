use crate::Rectangle;
use crate::widget::Id;
use crate::widget::operation::{self, Operation, Outcome};

pub trait Focusable {
        fn is_focused(&self) -> bool;

        fn focus(&mut self);

        fn unfocus(&mut self);
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Count {
        pub focused: Option<usize>,

        pub total: usize,
}

pub fn focus<T>(target: Id) -> impl Operation<T> {
    struct Focus {
        target: Id,
    }

    impl<T> Operation<T> for Focus {
        fn focusable(&mut self, id: Option<&Id>, _bounds: Rectangle, state: &mut dyn Focusable) {
            match id {
                Some(id) if id == &self.target => {
                    state.focus();
                }
                _ => {
                    state.unfocus();
                }
            }
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<T>)) {
            operate(self);
        }
    }

    Focus { target }
}

pub fn unfocus<T>() -> impl Operation<T> {
    struct Unfocus;

    impl<T> Operation<T> for Unfocus {
        fn focusable(&mut self, _id: Option<&Id>, _bounds: Rectangle, state: &mut dyn Focusable) {
            state.unfocus();
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<T>)) {
            operate(self);
        }
    }

    Unfocus
}

pub fn count() -> impl Operation<Count> {
    struct CountFocusable {
        count: Count,
    }

    impl Operation<Count> for CountFocusable {
        fn focusable(&mut self, _id: Option<&Id>, _bounds: Rectangle, state: &mut dyn Focusable) {
            if state.is_focused() {
                self.count.focused = Some(self.count.total);
            }

            self.count.total += 1;
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<Count>)) {
            operate(self);
        }

        fn finish(&self) -> Outcome<Count> {
            Outcome::Some(self.count)
        }
    }

    CountFocusable {
        count: Count::default(),
    }
}

pub fn focus_previous<T>() -> impl Operation<T>
where
    T: Send + 'static,
{
    struct FocusPrevious {
        count: Count,
        current: usize,
    }

    impl<T> Operation<T> for FocusPrevious {
        fn focusable(&mut self, _id: Option<&Id>, _bounds: Rectangle, state: &mut dyn Focusable) {
            if self.count.total == 0 {
                return;
            }

            match self.count.focused {
                None if self.current == self.count.total - 1 => state.focus(),
                Some(0) if self.current == 0 => state.unfocus(),
                Some(0) => {}
                Some(focused) if focused == self.current => state.unfocus(),
                Some(focused) if focused - 1 == self.current => state.focus(),
                _ => {}
            }

            self.current += 1;
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<T>)) {
            operate(self);
        }
    }

    operation::then(count(), |count| FocusPrevious { count, current: 0 })
}

pub fn focus_next<T>() -> impl Operation<T>
where
    T: Send + 'static,
{
    struct FocusNext {
        count: Count,
        current: usize,
    }

    impl<T> Operation<T> for FocusNext {
        fn focusable(&mut self, _id: Option<&Id>, _bounds: Rectangle, state: &mut dyn Focusable) {
            match self.count.focused {
                None if self.current == 0 => state.focus(),
                Some(focused) if focused == self.current => state.unfocus(),
                Some(focused) if focused + 1 == self.current => state.focus(),
                _ => {}
            }

            self.current += 1;
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<T>)) {
            operate(self);
        }
    }

    operation::then(count(), |count| FocusNext { count, current: 0 })
}

pub fn find_focused() -> impl Operation<Id> {
    struct FindFocused {
        focused: Option<Id>,
    }

    impl Operation<Id> for FindFocused {
        fn focusable(&mut self, id: Option<&Id>, _bounds: Rectangle, state: &mut dyn Focusable) {
            if state.is_focused() && id.is_some() {
                self.focused = id.cloned();
            }
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<Id>)) {
            operate(self);
        }

        fn finish(&self) -> Outcome<Id> {
            if let Some(id) = &self.focused {
                Outcome::Some(id.clone())
            } else {
                Outcome::None
            }
        }
    }

    FindFocused { focused: None }
}

pub fn is_focused(target: Id) -> impl Operation<bool> {
    struct IsFocused {
        target: Id,
        is_focused: Option<bool>,
    }

    impl Operation<bool> for IsFocused {
        fn focusable(&mut self, id: Option<&Id>, _bounds: Rectangle, state: &mut dyn Focusable) {
            if id.is_some_and(|id| *id == self.target) {
                self.is_focused = Some(state.is_focused());
            }
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<bool>)) {
            if self.is_focused.is_some() {
                return;
            }

            operate(self);
        }

        fn finish(&self) -> Outcome<bool> {
            self.is_focused.map_or(Outcome::None, Outcome::Some)
        }
    }

    IsFocused {
        target,
        is_focused: None,
    }
}
