use crate::Point;

#[derive(Debug, Clone, Copy, PartialEq)]
#[allow(missing_docs)]
pub enum Event {
        FingerPressed { id: Finger, position: Point },

        FingerMoved { id: Finger, position: Point },

        FingerLifted { id: Finger, position: Point },

        FingerLost { id: Finger, position: Point },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Finger(pub u64);
