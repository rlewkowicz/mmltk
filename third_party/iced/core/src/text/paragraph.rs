use crate::alignment;
use crate::text::{
    Alignment, Difference, Ellipsis, Hit, LineHeight, Shaping, Span, Text, Wrapping,
};
use crate::{Pixels, Point, Rectangle, Size};

pub trait Paragraph: Sized + Default {
        type Font: Copy + PartialEq;

        fn with_text(text: Text<&str, Self::Font>) -> Self;

        fn with_spans<Link>(text: Text<&[Span<'_, Link, Self::Font>], Self::Font>) -> Self;

        fn resize(&mut self, new_bounds: Size);

            fn compare(&self, text: Text<(), Self::Font>) -> Difference;

        fn size(&self) -> Pixels;

        fn hint_factor(&self) -> Option<f32>;

        fn font(&self) -> Self::Font;

        fn line_height(&self) -> LineHeight;

        fn align_x(&self) -> Alignment;

        fn align_y(&self) -> alignment::Vertical;

        fn wrapping(&self) -> Wrapping;

        fn ellipsis(&self) -> Ellipsis;

        fn shaping(&self) -> Shaping;

        fn bounds(&self) -> Size;

            fn min_bounds(&self) -> Size;

            fn hit_test(&self, point: Point) -> Option<Hit>;

        fn hit_test_strict(&self, point: Point) -> Option<Hit>;

                fn hit_span(&self, point: Point) -> Option<usize>;

            fn span_bounds(&self, index: usize) -> Vec<Rectangle>;

        fn selection_bounds(
        &self,
        start_line: usize,
        start_index: usize,
        end_line: usize,
        end_index: usize,
        bounds: &mut Vec<Rectangle>,
    );

        fn grapheme_position(&self, line: usize, index: usize) -> Option<Point>;

        fn min_width(&self) -> f32 {
        self.min_bounds().width
    }

        fn min_height(&self) -> f32 {
        self.min_bounds().height
    }
}

#[derive(Debug, Clone, Default)]
pub struct Plain<P: Paragraph> {
    raw: P,
    content: String,
}

impl<P: Paragraph> Plain<P> {
        pub fn new(text: Text<String, P::Font>) -> Self {
        Self {
            raw: P::with_text(text.as_ref()),
            content: text.content,
        }
    }

                pub fn update(&mut self, text: Text<&str, P::Font>) -> bool {
        if self.content != text.content {
            text.content.clone_into(&mut self.content);
            self.raw = P::with_text(text);
            return true;
        }

        match self.raw.compare(text.with_content(())) {
            Difference::None => false,
            Difference::Bounds => {
                self.raw.resize(text.bounds);
                true
            }
            Difference::Shape => {
                self.raw = P::with_text(text);
                true
            }
        }
    }

        pub fn align_x(&self) -> Alignment {
        self.raw.align_x()
    }

        pub fn align_y(&self) -> alignment::Vertical {
        self.raw.align_y()
    }

            pub fn min_bounds(&self) -> Size {
        self.raw.min_bounds()
    }

            pub fn min_width(&self) -> f32 {
        self.raw.min_width()
    }

            pub fn min_height(&self) -> f32 {
        self.raw.min_height()
    }

        pub fn raw(&self) -> &P {
        &self.raw
    }

        pub fn content(&self) -> &str {
        &self.content
    }

        pub fn as_text(&self) -> Text<&str, P::Font> {
        Text {
            content: &self.content,
            bounds: self.raw.bounds(),
            size: self.raw.size(),
            line_height: self.raw.line_height(),
            font: self.raw.font(),
            align_x: self.raw.align_x(),
            align_y: self.raw.align_y(),
            shaping: self.raw.shaping(),
            wrapping: self.raw.wrapping(),
            ellipsis: self.raw.ellipsis(),
            hint_factor: self.raw.hint_factor(),
        }
    }
}
