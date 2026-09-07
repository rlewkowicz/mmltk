/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use api::{ColorF, GradientStop};

mod linear;
mod radial;
mod conic;

pub use linear::*;
pub use radial::*;
pub use conic::*;

pub use api::key_types::GradientStopKey;

fn stops_and_min_alpha(stop_keys: &[GradientStopKey]) -> (Vec<GradientStop>, f32) {
    let mut min_alpha: f32 = 1.0;
    let stops = stop_keys.iter().map(|stop_key| {
        let color: ColorF = stop_key.color.into();
        min_alpha = min_alpha.min(color.a);

        GradientStop {
            offset: stop_key.offset,
            color,
        }
    }).collect();

    (stops, min_alpha)
}

pub use api::prim_geometry::apply_gradient_local_clip;
