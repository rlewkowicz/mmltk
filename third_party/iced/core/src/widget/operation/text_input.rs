use crate::Rectangle;
use crate::widget::Id;
use crate::widget::operation::Operation;

pub trait TextInput {
                fn text(&self) -> &str;

        fn move_cursor_to_front(&mut self);

        fn move_cursor_to_end(&mut self);

        fn move_cursor_to(&mut self, position: usize);

        fn select_all(&mut self);
        fn select_range(&mut self, start: usize, end: usize);

        fn selected_text(&self) -> Option<String>;

        fn paste_text(&mut self, content: &str) -> String;

        fn cut_text(&mut self) -> Option<TextCut>;
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TextCut {
        pub selection: String,
        pub value: String,
}

pub fn selected_text(target: Id) -> impl Operation<Option<String>> {
    struct SelectedText {
        target: Id,
        selection: Option<String>,
    }

    impl Operation<Option<String>> for SelectedText {
        fn text_input(&mut self, id: Option<&Id>, _bounds: Rectangle, state: &mut dyn TextInput) {
            if id.is_some_and(|id| id == &self.target) {
                self.selection = state.selected_text();
            }
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<Option<String>>)) {
            operate(self);
        }

        fn finish(&self) -> super::Outcome<Option<String>> {
            super::Outcome::Some(self.selection.clone())
        }
    }

    SelectedText {
        target,
        selection: None,
    }
}

pub fn paste_text(target: Id, content: String) -> impl Operation<Option<String>> {
    struct PasteText {
        target: Id,
        content: String,
        value: Option<String>,
    }

    impl Operation<Option<String>> for PasteText {
        fn text_input(&mut self, id: Option<&Id>, _bounds: Rectangle, state: &mut dyn TextInput) {
            if id.is_some_and(|id| id == &self.target) {
                self.value = Some(state.paste_text(&self.content));
            }
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<Option<String>>)) {
            operate(self);
        }

        fn finish(&self) -> super::Outcome<Option<String>> {
            super::Outcome::Some(self.value.clone())
        }
    }

    PasteText {
        target,
        content,
        value: None,
    }
}

pub fn cut_text(target: Id) -> impl Operation<Option<TextCut>> {
    struct CutText {
        target: Id,
        cut: Option<TextCut>,
    }

    impl Operation<Option<TextCut>> for CutText {
        fn text_input(&mut self, id: Option<&Id>, _bounds: Rectangle, state: &mut dyn TextInput) {
            if id.is_some_and(|id| id == &self.target) {
                self.cut = state.cut_text();
            }
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<Option<TextCut>>)) {
            operate(self);
        }

        fn finish(&self) -> super::Outcome<Option<TextCut>> {
            super::Outcome::Some(self.cut.clone())
        }
    }

    CutText { target, cut: None }
}

pub fn move_cursor_to_front<T>(target: Id) -> impl Operation<T> {
    struct MoveCursor {
        target: Id,
    }

    impl<T> Operation<T> for MoveCursor {
        fn text_input(&mut self, id: Option<&Id>, _bounds: Rectangle, state: &mut dyn TextInput) {
            match id {
                Some(id) if id == &self.target => {
                    state.move_cursor_to_front();
                }
                _ => {}
            }
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<T>)) {
            operate(self);
        }
    }

    MoveCursor { target }
}

pub fn move_cursor_to_end<T>(target: Id) -> impl Operation<T> {
    struct MoveCursor {
        target: Id,
    }

    impl<T> Operation<T> for MoveCursor {
        fn text_input(&mut self, id: Option<&Id>, _bounds: Rectangle, state: &mut dyn TextInput) {
            match id {
                Some(id) if id == &self.target => {
                    state.move_cursor_to_end();
                }
                _ => {}
            }
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<T>)) {
            operate(self);
        }
    }

    MoveCursor { target }
}

pub fn move_cursor_to<T>(target: Id, position: usize) -> impl Operation<T> {
    struct MoveCursor {
        target: Id,
        position: usize,
    }

    impl<T> Operation<T> for MoveCursor {
        fn text_input(&mut self, id: Option<&Id>, _bounds: Rectangle, state: &mut dyn TextInput) {
            match id {
                Some(id) if id == &self.target => {
                    state.move_cursor_to(self.position);
                }
                _ => {}
            }
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<T>)) {
            operate(self);
        }
    }

    MoveCursor { target, position }
}

pub fn select_all<T>(target: Id) -> impl Operation<T> {
    struct MoveCursor {
        target: Id,
    }

    impl<T> Operation<T> for MoveCursor {
        fn text_input(&mut self, id: Option<&Id>, _bounds: Rectangle, state: &mut dyn TextInput) {
            match id {
                Some(id) if id == &self.target => {
                    state.select_all();
                }
                _ => {}
            }
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<T>)) {
            operate(self);
        }
    }

    MoveCursor { target }
}

pub fn select_range<T>(target: Id, start: usize, end: usize) -> impl Operation<T> {
    struct SelectRange {
        target: Id,
        start: usize,
        end: usize,
    }

    impl<T> Operation<T> for SelectRange {
        fn text_input(&mut self, id: Option<&Id>, _bounds: Rectangle, state: &mut dyn TextInput) {
            match id {
                Some(id) if id == &self.target => {
                    state.select_range(self.start, self.end);
                }
                _ => {}
            }
        }

        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<T>)) {
            operate(self);
        }
    }

    SelectRange { target, start, end }
}
