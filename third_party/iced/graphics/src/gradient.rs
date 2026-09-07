use crate::color;
use crate::core::gradient::ColorStop;
use crate::core::{self, Color, Point, Rectangle};

use bytemuck::{Pod, Zeroable};
use half::f16;
use std::cmp::Ordering;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Gradient {
            Linear(Linear),
}

impl From<Linear> for Gradient {
    fn from(gradient: Linear) -> Self {
        Self::Linear(gradient)
    }
}

impl Gradient {
        pub fn pack(&self) -> Packed {
        match self {
            Gradient::Linear(linear) => linear.pack(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Linear {
        pub start: Point,

        pub end: Point,

        pub stops: [Option<ColorStop>; 8],
}

impl Linear {
        pub fn new(start: Point, end: Point) -> Self {
        Self {
            start,
            end,
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
            log::warn!("Gradient: ColorStop must be within 0.0..=1.0 range.");
        };

        self
    }

                pub fn add_stops(mut self, stops: impl IntoIterator<Item = ColorStop>) -> Self {
        for stop in stops {
            self = self.add_stop(stop.offset, stop.color);
        }

        self
    }

        pub fn pack(&self) -> Packed {
        let mut colors = [[0u32; 2]; 8];
        let mut offsets = [f16::from(0u8); 8];

        for (index, stop) in self.stops.iter().enumerate() {
            let [r, g, b, a] = color::pack(stop.map_or(Color::default(), |s| s.color)).components();

            colors[index] = [
                pack_f16s([f16::from_f32(r), f16::from_f32(g)]),
                pack_f16s([f16::from_f32(b), f16::from_f32(a)]),
            ];

            offsets[index] = stop.map_or(f16::from_f32(2.0), |s| f16::from_f32(s.offset));
        }

        let offsets = [
            pack_f16s([offsets[0], offsets[1]]),
            pack_f16s([offsets[2], offsets[3]]),
            pack_f16s([offsets[4], offsets[5]]),
            pack_f16s([offsets[6], offsets[7]]),
        ];

        let direction = [self.start.x, self.start.y, self.end.x, self.end.y];

        Packed {
            colors,
            offsets,
            direction,
        }
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Zeroable, Pod)]
#[repr(C)]
pub struct Packed {
    colors: [[u32; 2]; 8],
    offsets: [u32; 4],
    direction: [f32; 4],
}

pub fn pack(gradient: &core::Gradient, bounds: Rectangle) -> Packed {
    match gradient {
        core::Gradient::Linear(linear) => {
            let mut colors = [[0u32; 2]; 8];
            let mut offsets = [f16::from(0u8); 8];

            for (index, stop) in linear.stops.iter().enumerate() {
                let [r, g, b, a] =
                    color::pack(stop.map_or(Color::default(), |s| s.color)).components();

                colors[index] = [
                    pack_f16s([f16::from_f32(r), f16::from_f32(g)]),
                    pack_f16s([f16::from_f32(b), f16::from_f32(a)]),
                ];

                offsets[index] = stop.map_or(f16::from_f32(2.0), |s| f16::from_f32(s.offset));
            }

            let offsets = [
                pack_f16s([offsets[0], offsets[1]]),
                pack_f16s([offsets[2], offsets[3]]),
                pack_f16s([offsets[4], offsets[5]]),
                pack_f16s([offsets[6], offsets[7]]),
            ];

            let (start, end) = bounds.chord(linear.angle);

            let direction = [start.x, start.y, end.x, end.y];

            Packed {
                colors,
                offsets,
                direction,
            }
        }
    }
}

fn pack_f16s(f: [f16; 2]) -> u32 {
    let one = (f[0].to_bits() as u32) << 16;
    let two = f[1].to_bits() as u32;

    one | two
}
