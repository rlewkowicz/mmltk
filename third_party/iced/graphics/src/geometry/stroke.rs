pub use crate::geometry::Style;

use iced_core::Color;

#[derive(Debug, Clone, Copy)]
pub struct Stroke<'a> {
                pub style: Style,
        pub width: f32,
        pub line_cap: LineCap,
            pub line_join: LineJoin,
        pub line_dash: LineDash<'a>,
}

impl Stroke<'_> {
        pub fn with_color(self, color: Color) -> Self {
        Stroke {
            style: Style::Solid(color),
            ..self
        }
    }

        pub fn with_width(self, width: f32) -> Self {
        Stroke { width, ..self }
    }

        pub fn with_line_cap(self, line_cap: LineCap) -> Self {
        Stroke { line_cap, ..self }
    }

        pub fn with_line_join(self, line_join: LineJoin) -> Self {
        Stroke { line_join, ..self }
    }
}

impl Default for Stroke<'_> {
    fn default() -> Self {
        Stroke {
            style: Style::Solid(Color::BLACK),
            width: 1.0,
            line_cap: LineCap::default(),
            line_join: LineJoin::default(),
            line_dash: LineDash::default(),
        }
    }
}

#[derive(Debug, Clone, Copy, Default)]
pub enum LineCap {
        #[default]
    Butt,
            Square,
            Round,
}

#[derive(Debug, Clone, Copy, Default)]
pub enum LineJoin {
        #[default]
    Miter,
        Round,
        Bevel,
}

#[derive(Debug, Clone, Copy, Default)]
pub struct LineDash<'a> {
        pub segments: &'a [f32],

        pub offset: usize,
}
