use iced_core::{Point, Radians, Vector};

#[derive(Debug, Clone, Copy)]
pub struct Arc {
        pub center: Point,
        pub radius: f32,
        pub start_angle: Radians,
        pub end_angle: Radians,
}

#[derive(Debug, Clone, Copy)]
pub struct Elliptical {
        pub center: Point,
        pub radii: Vector,
        pub rotation: Radians,
        pub start_angle: Radians,
        pub end_angle: Radians,
}

impl From<Arc> for Elliptical {
    fn from(arc: Arc) -> Elliptical {
        Elliptical {
            center: arc.center,
            radii: Vector::new(arc.radius, arc.radius),
            rotation: Radians(0.0),
            start_angle: arc.start_angle,
            end_angle: arc.end_angle,
        }
    }
}
