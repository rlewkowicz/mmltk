use crate::text::highlighter::{self, Highlighter};
use crate::text::{LineHeight, Wrapping};
use crate::{Pixels, Point, Rectangle, Size};

use std::borrow::Cow;
use std::sync::Arc;

pub trait Editor: Sized + Default {
        type Font: Copy + PartialEq + Default;

        fn with_text(text: &str) -> Self;

        fn is_empty(&self) -> bool;

        fn cursor(&self) -> Cursor;

        fn selection(&self) -> Selection;

        fn copy(&self) -> Option<String>;

        fn line(&self, index: usize) -> Option<Line<'_>>;

        fn line_count(&self) -> usize;

        fn perform(&mut self, action: Action);

        fn move_to(&mut self, cursor: Cursor);

        fn bounds(&self) -> Size;

            fn min_bounds(&self) -> Size;

        fn hint_factor(&self) -> Option<f32>;

        fn update(
        &mut self,
        new_bounds: Size,
        new_font: Self::Font,
        new_size: Pixels,
        new_line_height: LineHeight,
        new_wrapping: Wrapping,
        new_hint_factor: Option<f32>,
        new_highlighter: &mut impl Highlighter,
    );

        fn highlight<H: Highlighter>(
        &mut self,
        font: Self::Font,
        highlighter: &mut H,
        format_highlight: impl Fn(&H::Highlight) -> highlighter::Format<Self::Font>,
    );
}

#[derive(Debug, Clone, PartialEq)]
pub enum Action {
        Move(Motion),
        Select(Motion),
        SelectWord,
        SelectLine,
        SelectAll,
        Edit(Edit),
        Click(Point),
        Drag(Point),
        Scroll {
                lines: i32,
    },
}

impl Action {
        pub fn is_edit(&self) -> bool {
        matches!(self, Self::Edit(_))
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Edit {
        Insert(char),
        Paste(Arc<String>),
        Enter,
        Indent,
        Unindent,
        Backspace,
        Delete,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Motion {
        Left,
        Right,
        Up,
        Down,
        WordLeft,
        WordRight,
        Home,
        End,
        PageUp,
        PageDown,
        DocumentStart,
        DocumentEnd,
}

impl Motion {
        pub fn widen(self) -> Self {
        match self {
            Self::Left => Self::WordLeft,
            Self::Right => Self::WordRight,
            Self::Home => Self::DocumentStart,
            Self::End => Self::DocumentEnd,
            _ => self,
        }
    }

        pub fn direction(&self) -> Direction {
        match self {
            Self::Left
            | Self::Up
            | Self::WordLeft
            | Self::Home
            | Self::PageUp
            | Self::DocumentStart => Direction::Left,
            Self::Right
            | Self::Down
            | Self::WordRight
            | Self::End
            | Self::PageDown
            | Self::DocumentEnd => Direction::Right,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Direction {
        Left,
        Right,
}

#[derive(Debug, Clone)]
pub enum Selection {
        Caret(Point),

        Range(Vec<Rectangle>),
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Cursor {
        pub position: Position,

        pub selection: Option<Position>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Position {
        pub line: usize,
        pub column: usize,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct Line<'a> {
        pub text: Cow<'a, str>,
        pub ending: LineEnding,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum LineEnding {
        #[default]
    Lf,
        CrLf,
        Cr,
        LfCr,
        None,
}

impl LineEnding {
        pub fn as_str(self) -> &'static str {
        match self {
            Self::Lf => "\n",
            Self::CrLf => "\r\n",
            Self::Cr => "\r",
            Self::LfCr => "\n\r",
            Self::None => "",
        }
    }
}
