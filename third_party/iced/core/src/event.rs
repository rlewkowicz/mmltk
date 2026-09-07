use crate::clipboard;
use crate::input_method;
use crate::keyboard;
use crate::mouse;
use crate::touch;
use crate::window;

#[derive(Debug, Clone, PartialEq)]
pub enum Event {
        Keyboard(keyboard::Event),

        Mouse(mouse::Event),

        Window(window::Event),

        Touch(touch::Event),

        InputMethod(input_method::Event),

        Clipboard(clipboard::Event),

        Waken,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
        Ignored,

        Captured,
}

impl Status {
                                                    pub fn merge(self, b: Self) -> Self {
        match self {
            Status::Ignored => b,
            Status::Captured => Status::Captured,
        }
    }
}
