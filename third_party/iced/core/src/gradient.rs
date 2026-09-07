use crate::{Color, Radians};

use std::cmp::Ordering;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Gradient {
        Linear(Linear),
}

impl Gradient {
        pub fn scale_alpha(self, factor: f32) -> Self {
        match self {
            Gradient::Linear(linear) => Gradient::Linear(linear.scale_alpha(factor)),
        }
    }
}

impl From<Linear> for Gradient {
    fn from(gradient: Linear) -> Self {
        Self::Linear(gradient)
    }
}

#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct ColorStop {
        pub offset: f32,

                pub color: Color,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Linear {
        pub angle: Radians,
        pub stops: [Option<ColorStop>; 8],
}

impl Linear {
        pub fn new(angle: impl Into<Radians>) -> Self {
        Self {
            angle: angle.into(),
            stops: [None; 8],
        }
    }

                        pub fn add_stop(mut self, offset: f32, color: Color) -> Self {
        if offset.is_finite() && (0.0..=1.0).contains(&offset) {
            let (Ok(index) | Err(index)) = self.stops.binary_search_by(|stop| match stop {
                None => Ordering::Greater,
                Some(stop) => stop.offset.partial_cmp(&offset).unwrap(),
            });

            if index < 8 {
                self.stops[index] = Some(ColorStop { offset, color });
            }
        } else {
            log::warn!("Gradient color stop must be within 0.0..=1.0 range.");
        };

        self
    }

                pub fn add_stops(mut self, stops: impl IntoIterator<Item = ColorStop>) -> Self {
        for stop in stops {
            self = self.add_stop(stop.offset, stop.color);
        }

        self
    }

            pub fn scale_alpha(mut self, factor: f32) -> Self {
        for stop in self.stops.iter_mut().flatten() {
            stop.color.a *= factor;
        }

        self
    }
}
