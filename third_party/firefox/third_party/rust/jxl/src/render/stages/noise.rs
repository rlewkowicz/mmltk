// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

#![allow(clippy::needless_range_loop)]

use std::{any::Any, sync::Arc};

use crate::{
    features::noise::Noise,
    frame::color_correlation_map::ColorCorrelationParams,
    render::{Channels, ChannelsMut, RenderPipelineInOutStage, RenderPipelineInPlaceStage},
    util::AtomicRefCell,
};
use jxl_simd::{F32SimdVec, simd_function};

pub struct ConvolveNoiseStage {
    channel: usize,
}

impl ConvolveNoiseStage {
    pub fn new(channel: usize) -> ConvolveNoiseStage {
        ConvolveNoiseStage { channel }
    }
}

impl std::fmt::Display for ConvolveNoiseStage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "convolve noise for channel {}", self.channel,)
    }
}

simd_function!(
    convolve_noise_simd_dispatch,
    d: D,
    fn convolve_noise_simd(input: &[&[f32]], output: &mut [f32], xsize: usize) {
        let c016 = D::F32Vec::splat(d, 0.16);
        let cn384 = D::F32Vec::splat(d, -3.84);

        let iter0 = input[0].windows(D::F32Vec::LEN + 4).step_by(D::F32Vec::LEN);
        let iter1 = input[1].windows(D::F32Vec::LEN + 4).step_by(D::F32Vec::LEN);
        let iter2 = input[2].windows(D::F32Vec::LEN + 4).step_by(D::F32Vec::LEN);
        let iter3 = input[3].windows(D::F32Vec::LEN + 4).step_by(D::F32Vec::LEN);
        let iter4 = input[4].windows(D::F32Vec::LEN + 4).step_by(D::F32Vec::LEN);
        let out_iter = output.chunks_exact_mut(D::F32Vec::LEN);

        for ((((w0, w1), w2), w3), (w4, out)) in iter0
            .zip(iter1)
            .zip(iter2)
            .zip(iter3)
            .zip(iter4.zip(out_iter))
            .take(xsize.div_ceil(D::F32Vec::LEN))
        {
            let p00 = D::F32Vec::load(d, &w2[2..]);

            let mut others = D::F32Vec::splat(d, 0.0);

            for i in 0..5 {
                others += D::F32Vec::load(d, &w0[i..]);
                others += D::F32Vec::load(d, &w1[i..]);
                others += D::F32Vec::load(d, &w3[i..]);
                others += D::F32Vec::load(d, &w4[i..]);
            }

            others += D::F32Vec::load(d, &w2[0..]);
            others += D::F32Vec::load(d, &w2[1..]);
            others += D::F32Vec::load(d, &w2[3..]);
            others += D::F32Vec::load(d, &w2[4..]);

            let result = others.mul_add(c016, p00 * cn384);
            result.store(out);
        }
    }
);

impl RenderPipelineInOutStage for ConvolveNoiseStage {
    type InputT = f32;
    type OutputT = f32;
    const SHIFT: (u8, u8) = (0, 0);
    const BORDER: (u8, u8) = (2, 2);

    fn uses_channel(&self, c: usize) -> bool {
        c == self.channel
    }

    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        input_rows: &Channels<f32>,
        output_rows: &mut ChannelsMut<f32>,
        _state: Option<&mut dyn std::any::Any>,
    ) {
        let input = &input_rows[0];
        convolve_noise_simd_dispatch(input, output_rows[0][0], xsize);
    }
}

pub struct AddNoiseStage {
    noise: Arc<AtomicRefCell<Noise>>,
    first_channel: usize,
    color_correlation: Arc<AtomicRefCell<ColorCorrelationParams>>,
}

impl AddNoiseStage {
    #[allow(dead_code)]
    pub fn new(
        noise: Arc<AtomicRefCell<Noise>>,
        color_correlation: Arc<AtomicRefCell<ColorCorrelationParams>>,
        first_channel: usize,
    ) -> AddNoiseStage {
        assert!(first_channel > 2);
        AddNoiseStage {
            noise,
            first_channel,
            color_correlation,
        }
    }
}

impl std::fmt::Display for AddNoiseStage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "add noise for channels [{},{},{}]",
            self.first_channel,
            self.first_channel + 1,
            self.first_channel + 2
        )
    }
}

impl RenderPipelineInPlaceStage for AddNoiseStage {
    type Type = f32;

    fn uses_channel(&self, c: usize) -> bool {
        c < 3 || (c >= self.first_channel && c < self.first_channel + 3)
    }

    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        row: &mut [&mut [f32]],
        _state: Option<&mut dyn Any>,
    ) {
        let noise = self.noise.borrow();
        if noise.lut == [0.0; 8] {
            return;
        }
        let color_correlation = self.color_correlation.borrow();
        let norm_const = 0.22;
        let ytox = color_correlation.y_to_x_lf();
        let ytob = color_correlation.y_to_b_lf();
        for x in 0..xsize {
            let row_rnd_r = row[3][x];
            let row_rnd_g = row[4][x];
            let row_rnd_c = row[5][x];
            let vx = row[0][x];
            let vy = row[1][x];
            let in_g = vy - vx;
            let in_r = vy + vx;
            let noise_strength_g = noise.strength(in_g * 0.5);
            let noise_strength_r = noise.strength(in_r * 0.5);
            let addit_rnd_noise_red = row_rnd_r * norm_const;
            let addit_rnd_noise_green = row_rnd_g * norm_const;
            let addit_rnd_noise_correlated = row_rnd_c * norm_const;
            let k_rg_corr = 0.9921875;
            let k_rgn_corr = 0.0078125;
            let red_noise = noise_strength_r
                * (k_rgn_corr * addit_rnd_noise_red + k_rg_corr * addit_rnd_noise_correlated);
            let green_noise = noise_strength_g
                * (k_rgn_corr * addit_rnd_noise_green + k_rg_corr * addit_rnd_noise_correlated);
            let rg_noise = red_noise + green_noise;
            row[0][x] += ytox * rg_noise + red_noise - green_noise;
            row[1][x] += rg_noise;
            row[2][x] += ytob * rg_noise;
        }
    }
}
