use crate::Point;

use super::Button;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Event {
        CursorEntered,

        CursorLeft,

        CursorMoved {
                position: Point,
    },

        ButtonPressed(Button),

        ButtonReleased(Button),

        WheelScrolled {
                delta: ScrollDelta,
    },
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum ScrollDelta {
        Lines {
                x: f32,

                y: f32,
    },
        Pixels {
                x: f32,
                y: f32,
    },
}
