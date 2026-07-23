#[derive(Debug, Hash, PartialEq, Eq, Clone, Copy)]
pub enum Button {
        Left,

        Right,

        Middle,

        Back,

        Forward,

        Other(u16),
}
