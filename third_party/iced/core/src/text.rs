pub mod editor;
pub mod highlighter;
pub mod paragraph;

pub use editor::Editor;
pub use highlighter::Highlighter;
pub use paragraph::Paragraph;

use crate::alignment;
use crate::{Background, Border, Color, Padding, Pixels, Point, Rectangle, Size};

use std::borrow::Cow;
use std::hash::{Hash, Hasher};

#[derive(Debug, Clone, Copy)]
pub struct Text<Content = String, Font = crate::Font> {
        pub content: Content,

        pub bounds: Size,

        pub size: Pixels,

        pub line_height: LineHeight,

        pub font: Font,

        pub align_x: Alignment,

        pub align_y: alignment::Vertical,

        pub shaping: Shaping,

        pub wrapping: Wrapping,

        pub ellipsis: Ellipsis,

                                    pub hint_factor: Option<f32>,
}

impl<Content, Font> Text<Content, Font>
where
    Font: Copy,
{
            pub fn with_content<T>(&self, content: T) -> Text<T, Font> {
        Text {
            content,
            bounds: self.bounds,
            size: self.size,
            line_height: self.line_height,
            font: self.font,
            align_x: self.align_x,
            align_y: self.align_y,
            shaping: self.shaping,
            wrapping: self.wrapping,
            ellipsis: self.ellipsis,
            hint_factor: self.hint_factor,
        }
    }
}

impl<Content, Font> Text<Content, Font>
where
    Content: AsRef<str>,
    Font: Copy,
{
        pub fn as_ref(&self) -> Text<&str, Font> {
        self.with_content(self.content.as_ref())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum Alignment {
                    #[default]
    Default,
        Left,
        Center,
        Right,
        Justified,
}

impl From<alignment::Horizontal> for Alignment {
    fn from(alignment: alignment::Horizontal) -> Self {
        match alignment {
            alignment::Horizontal::Left => Self::Left,
            alignment::Horizontal::Center => Self::Center,
            alignment::Horizontal::Right => Self::Right,
        }
    }
}

impl From<crate::Alignment> for Alignment {
    fn from(alignment: crate::Alignment) -> Self {
        match alignment {
            crate::Alignment::Start => Self::Left,
            crate::Alignment::Center => Self::Center,
            crate::Alignment::End => Self::Right,
        }
    }
}

impl From<Alignment> for alignment::Horizontal {
    fn from(alignment: Alignment) -> Self {
        match alignment {
            Alignment::Default | Alignment::Left | Alignment::Justified => {
                alignment::Horizontal::Left
            }
            Alignment::Center => alignment::Horizontal::Center,
            Alignment::Right => alignment::Horizontal::Right,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Shaping {
                                    Auto,
                                            Basic,
                                        Advanced,
}

impl Default for Shaping {
    fn default() -> Self {
        if cfg!(feature = "advanced-shaping") {
            Self::Advanced
        } else if cfg!(feature = "basic-shaping") {
            Self::Basic
        } else {
            Self::Auto
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum Wrapping {
        None,
                #[default]
    Word,
        Glyph,
        WordOrGlyph,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum Ellipsis {
                #[default]
    None,
        Start,
        Middle,
        End,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum LineHeight {
        Relative(f32),

        Absolute(Pixels),
}

impl LineHeight {
        pub fn to_absolute(self, text_size: Pixels) -> Pixels {
        match self {
            Self::Relative(factor) => Pixels(factor * text_size.0),
            Self::Absolute(pixels) => pixels,
        }
    }
}

impl Default for LineHeight {
    fn default() -> Self {
        Self::Relative(1.3)
    }
}

impl From<f32> for LineHeight {
    fn from(factor: f32) -> Self {
        Self::Relative(factor)
    }
}

impl From<Pixels> for LineHeight {
    fn from(pixels: Pixels) -> Self {
        Self::Absolute(pixels)
    }
}

impl Hash for LineHeight {
    fn hash<H: Hasher>(&self, state: &mut H) {
        match self {
            Self::Relative(factor) => {
                state.write_u8(0);
                factor.to_bits().hash(state);
            }
            Self::Absolute(pixels) => {
                state.write_u8(1);
                f32::from(*pixels).to_bits().hash(state);
            }
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Hit {
        CharOffset(usize),
}

impl Hit {
        pub fn cursor(self) -> usize {
        match self {
            Self::CharOffset(i) => i,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Difference {
                None,

                    Bounds,

                        Shape,
}

pub trait Renderer: crate::Renderer {
        type Font: Copy + PartialEq;

        type Paragraph: Paragraph<Font = Self::Font> + 'static;

        type Editor: Editor<Font = Self::Font> + 'static;

        const ICON_FONT: Self::Font;

                const CHECKMARK_ICON: char;

                const ARROW_DOWN_ICON: char;

                const SCROLL_UP_ICON: char;

                const SCROLL_DOWN_ICON: char;

                const SCROLL_LEFT_ICON: char;

                const SCROLL_RIGHT_ICON: char;

                const ICED_LOGO: char;

        fn default_font(&self) -> Self::Font;

        fn default_size(&self) -> Pixels;

            fn fill_paragraph(
        &mut self,
        text: &Self::Paragraph,
        position: Point,
        color: Color,
        clip_bounds: Rectangle,
    );

            fn fill_editor(
        &mut self,
        editor: &Self::Editor,
        position: Point,
        color: Color,
        clip_bounds: Rectangle,
    );

            fn fill_text(
        &mut self,
        text: Text<String, Self::Font>,
        position: Point,
        color: Color,
        clip_bounds: Rectangle,
    );
}

#[derive(Debug, Clone)]
pub struct Span<'a, Link = (), Font = crate::Font> {
        pub text: Fragment<'a>,
        pub size: Option<Pixels>,
        pub line_height: Option<LineHeight>,
        pub font: Option<Font>,
        pub color: Option<Color>,
        pub link: Option<Link>,
        pub highlight: Option<Highlight>,
                pub padding: Padding,
        pub underline: bool,
        pub strikethrough: bool,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Highlight {
        pub background: Background,
        pub border: Border,
}

impl<'a, Link, Font> Span<'a, Link, Font> {
        pub fn new(fragment: impl IntoFragment<'a>) -> Self {
        Self {
            text: fragment.into_fragment(),
            ..Self::default()
        }
    }

        pub fn size(mut self, size: impl Into<Pixels>) -> Self {
        self.size = Some(size.into());
        self
    }

        pub fn line_height(mut self, line_height: impl Into<LineHeight>) -> Self {
        self.line_height = Some(line_height.into());
        self
    }

        pub fn font(mut self, font: impl Into<Font>) -> Self {
        self.font = Some(font.into());
        self
    }

        pub fn font_maybe(mut self, font: Option<impl Into<Font>>) -> Self {
        self.font = font.map(Into::into);
        self
    }

        pub fn color(mut self, color: impl Into<Color>) -> Self {
        self.color = Some(color.into());
        self
    }

        pub fn color_maybe(mut self, color: Option<impl Into<Color>>) -> Self {
        self.color = color.map(Into::into);
        self
    }

        pub fn link(mut self, link: impl Into<Link>) -> Self {
        self.link = Some(link.into());
        self
    }

        pub fn link_maybe(mut self, link: Option<impl Into<Link>>) -> Self {
        self.link = link.map(Into::into);
        self
    }

        pub fn background(self, background: impl Into<Background>) -> Self {
        self.background_maybe(Some(background))
    }

        pub fn background_maybe(mut self, background: Option<impl Into<Background>>) -> Self {
        let Some(background) = background else {
            return self;
        };

        match &mut self.highlight {
            Some(highlight) => {
                highlight.background = background.into();
            }
            None => {
                self.highlight = Some(Highlight {
                    background: background.into(),
                    border: Border::default(),
                });
            }
        }

        self
    }

        pub fn border(self, border: impl Into<Border>) -> Self {
        self.border_maybe(Some(border))
    }

        pub fn border_maybe(mut self, border: Option<impl Into<Border>>) -> Self {
        let Some(border) = border else {
            return self;
        };

        match &mut self.highlight {
            Some(highlight) => {
                highlight.border = border.into();
            }
            None => {
                self.highlight = Some(Highlight {
                    border: border.into(),
                    background: Background::Color(Color::TRANSPARENT),
                });
            }
        }

        self
    }

                                pub fn padding(mut self, padding: impl Into<Padding>) -> Self {
        self.padding = padding.into();
        self
    }

        pub fn underline(mut self, underline: bool) -> Self {
        self.underline = underline;
        self
    }

        pub fn strikethrough(mut self, strikethrough: bool) -> Self {
        self.strikethrough = strikethrough;
        self
    }

        pub fn to_static(self) -> Span<'static, Link, Font> {
        Span {
            text: Cow::Owned(self.text.into_owned()),
            size: self.size,
            line_height: self.line_height,
            font: self.font,
            color: self.color,
            link: self.link,
            highlight: self.highlight,
            padding: self.padding,
            underline: self.underline,
            strikethrough: self.strikethrough,
        }
    }
}

impl<Link, Font> Default for Span<'_, Link, Font> {
    fn default() -> Self {
        Self {
            text: Cow::default(),
            size: None,
            line_height: None,
            font: None,
            color: None,
            link: None,
            highlight: None,
            padding: Padding::default(),
            underline: false,
            strikethrough: false,
        }
    }
}

impl<'a, Link, Font> From<&'a str> for Span<'a, Link, Font> {
    fn from(value: &'a str) -> Self {
        Span::new(value)
    }
}

impl<Link, Font: PartialEq> PartialEq for Span<'_, Link, Font> {
    fn eq(&self, other: &Self) -> bool {
        self.text == other.text
            && self.size == other.size
            && self.line_height == other.line_height
            && self.font == other.font
            && self.color == other.color
    }
}

pub type Fragment<'a> = Cow<'a, str>;

pub trait IntoFragment<'a> {
        fn into_fragment(self) -> Fragment<'a>;
}

impl<'a> IntoFragment<'a> for Fragment<'a> {
    fn into_fragment(self) -> Fragment<'a> {
        self
    }
}

impl<'a> IntoFragment<'a> for &'a Fragment<'_> {
    fn into_fragment(self) -> Fragment<'a> {
        Fragment::Borrowed(self)
    }
}

impl<'a> IntoFragment<'a> for &'a str {
    fn into_fragment(self) -> Fragment<'a> {
        Fragment::Borrowed(self)
    }
}

impl<'a> IntoFragment<'a> for &'a String {
    fn into_fragment(self) -> Fragment<'a> {
        Fragment::Borrowed(self.as_str())
    }
}

impl<'a> IntoFragment<'a> for String {
    fn into_fragment(self) -> Fragment<'a> {
        Fragment::Owned(self)
    }
}

macro_rules! into_fragment {
    ($type:ty) => {
        impl<'a> IntoFragment<'a> for $type {
            fn into_fragment(self) -> Fragment<'a> {
                Fragment::Owned(self.to_string())
            }
        }

        impl<'a> IntoFragment<'a> for &$type {
            fn into_fragment(self) -> Fragment<'a> {
                Fragment::Owned(self.to_string())
            }
        }
    };
}

into_fragment!(char);
into_fragment!(bool);

into_fragment!(u8);
into_fragment!(u16);
into_fragment!(u32);
into_fragment!(u64);
into_fragment!(u128);
into_fragment!(usize);

into_fragment!(i8);
into_fragment!(i16);
into_fragment!(i32);
into_fragment!(i64);
into_fragment!(i128);
into_fragment!(isize);

into_fragment!(f32);
into_fragment!(f64);
