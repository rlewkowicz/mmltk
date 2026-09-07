use crate::geometry::path::{Arc, Path, arc};

use crate::core::border;
use crate::core::{Point, Radians, Size};

use lyon_path::builder::{self, SvgPathBuilder};
use lyon_path::geom;
use lyon_path::math;

pub struct Builder {
    raw: builder::WithSvg<lyon_path::path::BuilderImpl>,
}

impl Builder {
        pub fn new() -> Builder {
        Builder {
            raw: lyon_path::Path::builder().with_svg(),
        }
    }

        #[inline]
    pub fn move_to(&mut self, point: Point) {
        let _ = self.raw.move_to(math::Point::new(point.x, point.y));
    }

            #[inline]
    pub fn line_to(&mut self, point: Point) {
        let _ = self.raw.line_to(math::Point::new(point.x, point.y));
    }

            #[inline]
    pub fn arc(&mut self, arc: Arc) {
        self.ellipse(arc.into());
    }

                                                        pub fn arc_to(&mut self, a: Point, b: Point, radius: f32) {
        let start = self.raw.current_position();
        let mid = math::Point::new(a.x, a.y);
        let end = math::Point::new(b.x, b.y);

        if start == mid || mid == end || radius == 0.0 {
            let _ = self.raw.line_to(mid);
            return;
        }

        let double_area =
            start.x * (mid.y - end.y) + mid.x * (end.y - start.y) + end.x * (start.y - mid.y);

        if double_area == 0.0 {
            let _ = self.raw.line_to(mid);
            return;
        }

        let to_start = (start - mid).normalize();
        let to_end = (end - mid).normalize();

        let inner_angle = to_start.dot(to_end).acos();

        let origin_angle = inner_angle / 2.0;

        let origin_adjacent = radius / origin_angle.tan();

        let arc_start = mid + to_start * origin_adjacent;
        let arc_end = mid + to_end * origin_adjacent;

        let sweep = to_start.cross(to_end) < 0.0;

        let _ = self.raw.line_to(arc_start);

        self.raw.arc_to(
            math::Vector::new(radius, radius),
            math::Angle::radians(0.0),
            lyon_path::ArcFlags {
                large_arc: false,
                sweep,
            },
            arc_end,
        );
    }

        pub fn ellipse(&mut self, arc: arc::Elliptical) {
        let arc = geom::Arc {
            center: math::Point::new(arc.center.x, arc.center.y),
            radii: math::Vector::new(arc.radii.x, arc.radii.y),
            x_rotation: math::Angle::radians(arc.rotation.0),
            start_angle: math::Angle::radians(arc.start_angle.0),
            sweep_angle: math::Angle::radians((arc.end_angle - arc.start_angle).0),
        };

        let _ = self.raw.move_to(arc.sample(0.0));

        arc.cast::<f64>().for_each_quadratic_bezier(&mut |curve| {
            let curve = curve.cast::<f32>();
            let _ = self.raw.quadratic_bezier_to(curve.ctrl, curve.to);
        });
    }

            #[inline]
    pub fn bezier_curve_to(&mut self, control_a: Point, control_b: Point, to: Point) {
        let _ = self.raw.cubic_bezier_to(
            math::Point::new(control_a.x, control_a.y),
            math::Point::new(control_b.x, control_b.y),
            math::Point::new(to.x, to.y),
        );
    }

            #[inline]
    pub fn quadratic_curve_to(&mut self, control: Point, to: Point) {
        let _ = self.raw.quadratic_bezier_to(
            math::Point::new(control.x, control.y),
            math::Point::new(to.x, to.y),
        );
    }

            #[inline]
    pub fn rectangle(&mut self, top_left: Point, size: Size) {
        self.move_to(top_left);
        self.line_to(Point::new(top_left.x + size.width, top_left.y));
        self.line_to(Point::new(
            top_left.x + size.width,
            top_left.y + size.height,
        ));
        self.line_to(Point::new(top_left.x, top_left.y + size.height));
        self.close();
    }

            #[inline]
    pub fn rounded_rectangle(&mut self, top_left: Point, size: Size, radius: border::Radius) {
        let min_size = (size.height / 2.0).min(size.width / 2.0);
        let [
            top_left_corner,
            top_right_corner,
            bottom_right_corner,
            bottom_left_corner,
        ] = radius.into();

        self.move_to(Point::new(
            top_left.x + min_size.min(top_left_corner),
            top_left.y,
        ));
        self.line_to(Point::new(
            top_left.x + size.width - min_size.min(top_right_corner),
            top_left.y,
        ));
        self.arc_to(
            Point::new(top_left.x + size.width, top_left.y),
            Point::new(
                top_left.x + size.width,
                top_left.y + min_size.min(top_right_corner),
            ),
            min_size.min(top_right_corner),
        );
        self.line_to(Point::new(
            top_left.x + size.width,
            top_left.y + size.height - min_size.min(bottom_right_corner),
        ));
        self.arc_to(
            Point::new(top_left.x + size.width, top_left.y + size.height),
            Point::new(
                top_left.x + size.width - min_size.min(bottom_right_corner),
                top_left.y + size.height,
            ),
            min_size.min(bottom_right_corner),
        );
        self.line_to(Point::new(
            top_left.x + min_size.min(bottom_left_corner),
            top_left.y + size.height,
        ));
        self.arc_to(
            Point::new(top_left.x, top_left.y + size.height),
            Point::new(
                top_left.x,
                top_left.y + size.height - min_size.min(bottom_left_corner),
            ),
            min_size.min(bottom_left_corner),
        );
        self.line_to(Point::new(
            top_left.x,
            top_left.y + min_size.min(top_left_corner),
        ));
        self.arc_to(
            Point::new(top_left.x, top_left.y),
            Point::new(top_left.x + min_size.min(top_left_corner), top_left.y),
            min_size.min(top_left_corner),
        );
        self.close();
    }

            #[inline]
    pub fn circle(&mut self, center: Point, radius: f32) {
        self.arc(Arc {
            center,
            radius,
            start_angle: Radians(0.0),
            end_angle: Radians(2.0 * std::f32::consts::PI),
        });
    }

            #[inline]
    pub fn close(&mut self) {
        self.raw.close();
    }

        #[inline]
    pub fn build(self) -> Path {
        Path {
            raw: self.raw.build(),
        }
    }
}

impl Default for Builder {
    fn default() -> Self {
        Self::new()
    }
}
