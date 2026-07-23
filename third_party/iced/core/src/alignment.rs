#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Alignment {
        Start,

        Center,

        End,
}

impl From<Horizontal> for Alignment {
    fn from(horizontal: Horizontal) -> Self {
        match horizontal {
            Horizontal::Left => Self::Start,
            Horizontal::Center => Self::Center,
            Horizontal::Right => Self::End,
        }
    }
}

impl From<Vertical> for Alignment {
    fn from(vertical: Vertical) -> Self {
        match vertical {
            Vertical::Top => Self::Start,
            Vertical::Center => Self::Center,
            Vertical::Bottom => Self::End,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Horizontal {
        Left,

        Center,

        Right,
}

impl From<Alignment> for Horizontal {
    fn from(alignment: Alignment) -> Self {
        match alignment {
            Alignment::Start => Self::Left,
            Alignment::Center => Self::Center,
            Alignment::End => Self::Right,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Vertical {
        Top,

        Center,

        Bottom,
}

impl From<Alignment> for Vertical {
    fn from(alignment: Alignment) -> Self {
        match alignment {
            Alignment::Start => Self::Top,
            Alignment::Center => Self::Center,
            Alignment::End => Self::Bottom,
        }
    }
}
