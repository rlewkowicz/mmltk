use crate::core::{Point, Radians, Rectangle, Size, Vector};
use crate::geometry::{self, Fill, Image, Path, Stroke, Svg, Text};

pub struct Frame<Renderer>
where
    Renderer: geometry::Renderer,
{
    raw: Renderer::Frame,
}

impl<Renderer> Frame<Renderer>
where
    Renderer: geometry::Renderer,
{
                    pub fn new(renderer: &Renderer, size: Size) -> Self {
        Self::with_bounds(renderer, Rectangle::with_size(size))
    }

        pub fn with_bounds(renderer: &Renderer, bounds: Rectangle) -> Self {
        Self {
            raw: renderer.new_frame(bounds),
        }
    }

        pub fn width(&self) -> f32 {
        self.raw.width()
    }

        pub fn height(&self) -> f32 {
        self.raw.height()
    }

        pub fn size(&self) -> Size {
        self.raw.size()
    }

        pub fn center(&self) -> Point {
        self.raw.center()
    }

            pub fn fill(&mut self, path: &Path, fill: impl Into<Fill>) {
        self.raw.fill(path, fill);
    }

            pub fn fill_rectangle(&mut self, top_left: Point, size: Size, fill: impl Into<Fill>) {
        self.raw.fill_rectangle(top_left, size, fill);
    }

            pub fn stroke<'a>(&mut self, path: &Path, stroke: impl Into<Stroke<'a>>) {
        self.raw.stroke(path, stroke);
    }

            pub fn stroke_rectangle<'a>(
        &mut self,
        top_left: Point,
        size: Size,
        stroke: impl Into<Stroke<'a>>,
    ) {
        self.raw.stroke_rectangle(top_left, size, stroke);
    }

                            pub fn fill_text(&mut self, text: impl Into<Text>) {
        self.raw.fill_text(text);
    }

        #[cfg(feature = "image")]
    pub fn draw_image(&mut self, bounds: Rectangle, image: impl Into<Image>) {
        self.raw.draw_image(bounds, image);
    }

        #[cfg(feature = "svg")]
    pub fn draw_svg(&mut self, bounds: Rectangle, svg: impl Into<Svg>) {
        self.raw.draw_svg(bounds, svg);
    }

                        #[inline]
    pub fn with_save<R>(&mut self, f: impl FnOnce(&mut Self) -> R) -> R {
        self.push_transform();

        let result = f(self);

        self.pop_transform();

        result
    }

        pub fn push_transform(&mut self) {
        self.raw.push_transform();
    }

        pub fn pop_transform(&mut self) {
        self.raw.pop_transform();
    }

                            #[inline]
    pub fn with_clip<R>(&mut self, region: Rectangle, f: impl FnOnce(&mut Self) -> R) -> R {
        let mut frame = self.draft(region);

        let result = f(&mut frame);
        self.paste(frame);

        result
    }

                        fn draft(&mut self, clip_bounds: Rectangle) -> Self {
        Self {
            raw: self.raw.draft(clip_bounds),
        }
    }

        fn paste(&mut self, frame: Self) {
        self.raw.paste(frame.raw);
    }

        pub fn translate(&mut self, translation: Vector) {
        self.raw.translate(translation);
    }

        pub fn rotate(&mut self, angle: impl Into<Radians>) {
        self.raw.rotate(angle);
    }

        pub fn scale(&mut self, scale: f32) {
        self.raw.scale(scale);
    }

        pub fn scale_nonuniform(&mut self, scale: impl Into<Vector>) {
        self.raw.scale_nonuniform(scale);
    }

        pub fn into_geometry(self) -> Renderer::Geometry {
        self.raw.into_geometry()
    }
}

#[allow(missing_docs)]
pub trait Backend: Sized {
    type Geometry;

    fn width(&self) -> f32;
    fn height(&self) -> f32;
    fn size(&self) -> Size;
    fn center(&self) -> Point;

    fn push_transform(&mut self);
    fn pop_transform(&mut self);

    fn translate(&mut self, translation: Vector);
    fn rotate(&mut self, angle: impl Into<Radians>);
    fn scale(&mut self, scale: f32);
    fn scale_nonuniform(&mut self, scale: impl Into<Vector>);

    fn draft(&mut self, clip_bounds: Rectangle) -> Self;
    fn paste(&mut self, frame: Self);

    fn stroke<'a>(&mut self, path: &Path, stroke: impl Into<Stroke<'a>>);
    fn stroke_rectangle<'a>(&mut self, top_left: Point, size: Size, stroke: impl Into<Stroke<'a>>);
    fn stroke_text<'a>(&mut self, text: impl Into<Text>, stroke: impl Into<Stroke<'a>>);

    fn fill(&mut self, path: &Path, fill: impl Into<Fill>);
    fn fill_text(&mut self, text: impl Into<Text>);
    fn fill_rectangle(&mut self, top_left: Point, size: Size, fill: impl Into<Fill>);

    fn draw_image(&mut self, bounds: Rectangle, image: impl Into<Image>);
    fn draw_svg(&mut self, bounds: Rectangle, svg: impl Into<Svg>);

    fn into_geometry(self) -> Self::Geometry;
}

#[cfg(debug_assertions)]
impl Backend for () {
    type Geometry = ();

    fn width(&self) -> f32 {
        0.0
    }

    fn height(&self) -> f32 {
        0.0
    }

    fn size(&self) -> Size {
        Size::ZERO
    }

    fn center(&self) -> Point {
        Point::ORIGIN
    }

    fn push_transform(&mut self) {}
    fn pop_transform(&mut self) {}

    fn translate(&mut self, _translation: Vector) {}
    fn rotate(&mut self, _angle: impl Into<Radians>) {}
    fn scale(&mut self, _scale: f32) {}
    fn scale_nonuniform(&mut self, _scale: impl Into<Vector>) {}

    fn draft(&mut self, _clip_bounds: Rectangle) -> Self {}
    fn paste(&mut self, _frame: Self) {}

    fn stroke<'a>(&mut self, _path: &Path, _stroke: impl Into<Stroke<'a>>) {}
    fn stroke_rectangle<'a>(
        &mut self,
        _top_left: Point,
        _size: Size,
        _stroke: impl Into<Stroke<'a>>,
    ) {
    }
    fn stroke_text<'a>(&mut self, _text: impl Into<Text>, _stroke: impl Into<Stroke<'a>>) {}

    fn fill(&mut self, _path: &Path, _fill: impl Into<Fill>) {}
    fn fill_text(&mut self, _text: impl Into<Text>) {}
    fn fill_rectangle(&mut self, _top_left: Point, _size: Size, _fill: impl Into<Fill>) {}

    fn draw_image(&mut self, _bounds: Rectangle, _image: impl Into<Image>) {}
    fn draw_svg(&mut self, _bounds: Rectangle, _svg: impl Into<Svg>) {}

    fn into_geometry(self) -> Self::Geometry {}
}
