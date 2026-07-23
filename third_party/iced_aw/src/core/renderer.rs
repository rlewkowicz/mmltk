use iced_core::{Layout, Point, Rectangle};

#[derive(Debug)]
pub struct DrawEnvironment<'a, Defaults, Style, Focus> {
        pub defaults: &'a Defaults,
        pub layout: Layout<'a>,
        pub cursor_position: Point,
        pub style_sheet: &'a Style,
        pub viewport: Option<&'a Rectangle>,
        pub focus: Focus,
}
