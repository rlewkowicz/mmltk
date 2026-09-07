// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

#![allow(clippy::needless_range_loop)]
#![allow(clippy::too_many_arguments)]

use std::any::Any;

use crate::{
    headers::CustomTransformData,
    render::{Channels, ChannelsMut, RenderPipelineInOutStage},
};
use jxl_simd::{F32SimdVec, simd_function};

pub struct Upsample<const N: usize, const SHIFT: u8> {
    flat_kernels: Vec<[f32; 25]>,
    channel: usize,
}

impl<const N: usize, const SHIFT: u8> Upsample<N, SHIFT> {
    pub fn new(ups_factors: &CustomTransformData, channel: usize) -> Self {
        const { assert!(SHIFT >= 1 && SHIFT <= 3) }
        const { assert!(1 << SHIFT == N) }

        let weights: &[f32] = match N {
            2 => &ups_factors.weights2,
            4 => &ups_factors.weights4,
            8 => &ups_factors.weights8,
            _ => unreachable!(),
        };

        let mut kernel = [[[[0.0; 5]; 5]; N]; N];
        let n = N / 2;
        for i in 0..5 * n {
            for j in 0..5 * n {
                let y = i.min(j);
                let x = i.max(j);
                let y = y as isize;
                let x = x as isize;
                let n = n as isize;
                let index = (5 * n * y - y * (y - 1) / 2 + x - y) as usize;
                kernel[j / 5][i / 5][j % 5][i % 5] = weights[index];
                kernel[(2 * n as usize - 1) - j / 5][i / 5][4 - (j % 5)][i % 5] = weights[index];
                kernel[j / 5][(2 * n as usize - 1) - i / 5][j % 5][4 - (i % 5)] = weights[index];
                kernel[(2 * n as usize - 1) - j / 5][(2 * n as usize - 1) - i / 5][4 - (j % 5)]
                    [4 - (i % 5)] = weights[index];
            }
        }

        let mut flat_kernels = Vec::with_capacity(N * N);
        for di in 0..N {
            for dj in 0..N {
                let mut k = [0.0f32; 25];
                for i in 0..5 {
                    for j in 0..5 {
                        k[i * 5 + j] = kernel[di][dj][i][j];
                    }
                }
                flat_kernels.push(k);
            }
        }

        Self {
            flat_kernels,
            channel,
        }
    }
}

impl<const N: usize, const SHIFT: u8> std::fmt::Display for Upsample<N, SHIFT> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{N}x{N} upsampling of channel {}", self.channel)
    }
}

/// State for upsampling stage containing reusable temp buffers
struct UpsampleState {
    col_min: Vec<f32>,
    col_max: Vec<f32>,
    mins: Vec<f32>,
    maxs: Vec<f32>,
}

impl UpsampleState {
    fn new() -> Self {
        Self {
            col_min: Vec::new(),
            col_max: Vec::new(),
            mins: Vec::new(),
            maxs: Vec::new(),
        }
    }

    fn ensure_capacity(&mut self, xsize: usize) {
        let needed = xsize + 24;
        if self.col_min.len() < needed {
            self.col_min.resize(needed, 0.0);
            self.col_max.resize(needed, 0.0);
            self.mins.resize(needed, 0.0);
            self.maxs.resize(needed, 0.0);
        }
    }
}

#[inline(always)]
fn compute_minmax<D: jxl_simd::SimdDescriptor>(
    d: D,
    input: &[&[f32]],
    xsize: usize,
    col_min: &mut [f32],
    col_max: &mut [f32],
    mins: &mut [f32],
    maxs: &mut [f32],
) {
    let r0 = input[0];
    let r1 = input[1];
    let r2 = input[2];
    let r3 = input[3];
    let r4 = input[4];

    let col_fill_len = xsize + 4;
    let num_vecs = col_fill_len.div_ceil(D::F32Vec::LEN);
    for i in 0..num_vecs {
        let offset = i * D::F32Vec::LEN;
        let v0 = D::F32Vec::load(d, &r0[offset..]);
        let v1 = D::F32Vec::load(d, &r1[offset..]);
        let v2 = D::F32Vec::load(d, &r2[offset..]);
        let v3 = D::F32Vec::load(d, &r3[offset..]);
        let v4 = D::F32Vec::load(d, &r4[offset..]);

        let col_min_v = v0.min(v1).min(v2).min(v3).min(v4);
        let col_max_v = v0.max(v1).max(v2).max(v3).max(v4);

        col_min_v.store(&mut col_min[offset..]);
        col_max_v.store(&mut col_max[offset..]);
    }

    let num_output_vecs = xsize.div_ceil(D::F32Vec::LEN);
    for i in 0..num_output_vecs {
        let offset = i * D::F32Vec::LEN;
        let m0 = D::F32Vec::load(d, &col_min[offset..]);
        let m1 = D::F32Vec::load(d, &col_min[offset + 1..]);
        let m2 = D::F32Vec::load(d, &col_min[offset + 2..]);
        let m3 = D::F32Vec::load(d, &col_min[offset + 3..]);
        let m4 = D::F32Vec::load(d, &col_min[offset + 4..]);
        let min_v = m0.min(m1).min(m2).min(m3).min(m4);
        min_v.store(&mut mins[offset..]);

        let m0 = D::F32Vec::load(d, &col_max[offset..]);
        let m1 = D::F32Vec::load(d, &col_max[offset + 1..]);
        let m2 = D::F32Vec::load(d, &col_max[offset + 2..]);
        let m3 = D::F32Vec::load(d, &col_max[offset + 3..]);
        let m4 = D::F32Vec::load(d, &col_max[offset + 4..]);
        let max_v = m0.max(m1).max(m2).max(m3).max(m4);
        max_v.store(&mut maxs[offset..]);
    }
}

macro_rules! kernel_conv {
    ($d:expr, $kv:expr, $r0:expr, $r1:expr, $r2:expr, $r3:expr, $r4:expr, $x:expr) => {{
        let mut acc0 = <D::F32Vec>::load($d, &$r0[$x..]) * $kv[0];
        let mut acc1 = <D::F32Vec>::load($d, &$r0[$x + 1..]) * $kv[1];
        let mut acc2 = <D::F32Vec>::load($d, &$r0[$x + 2..]) * $kv[2];
        acc0 = <D::F32Vec>::load($d, &$r0[$x + 3..]).mul_add($kv[3], acc0);
        acc1 = <D::F32Vec>::load($d, &$r0[$x + 4..]).mul_add($kv[4], acc1);
        acc2 = <D::F32Vec>::load($d, &$r1[$x..]).mul_add($kv[5], acc2);
        acc0 = <D::F32Vec>::load($d, &$r1[$x + 1..]).mul_add($kv[6], acc0);
        acc1 = <D::F32Vec>::load($d, &$r1[$x + 2..]).mul_add($kv[7], acc1);
        acc2 = <D::F32Vec>::load($d, &$r1[$x + 3..]).mul_add($kv[8], acc2);
        acc0 = <D::F32Vec>::load($d, &$r1[$x + 4..]).mul_add($kv[9], acc0);
        acc1 = <D::F32Vec>::load($d, &$r2[$x..]).mul_add($kv[10], acc1);
        acc2 = <D::F32Vec>::load($d, &$r2[$x + 1..]).mul_add($kv[11], acc2);
        acc0 = <D::F32Vec>::load($d, &$r2[$x + 2..]).mul_add($kv[12], acc0);
        acc1 = <D::F32Vec>::load($d, &$r2[$x + 3..]).mul_add($kv[13], acc1);
        acc2 = <D::F32Vec>::load($d, &$r2[$x + 4..]).mul_add($kv[14], acc2);
        acc0 = <D::F32Vec>::load($d, &$r3[$x..]).mul_add($kv[15], acc0);
        acc1 = <D::F32Vec>::load($d, &$r3[$x + 1..]).mul_add($kv[16], acc1);
        acc2 = <D::F32Vec>::load($d, &$r3[$x + 2..]).mul_add($kv[17], acc2);
        acc0 = <D::F32Vec>::load($d, &$r3[$x + 3..]).mul_add($kv[18], acc0);
        acc1 = <D::F32Vec>::load($d, &$r3[$x + 4..]).mul_add($kv[19], acc1);
        acc2 = <D::F32Vec>::load($d, &$r4[$x..]).mul_add($kv[20], acc2);
        acc0 = <D::F32Vec>::load($d, &$r4[$x + 1..]).mul_add($kv[21], acc0);
        acc1 = <D::F32Vec>::load($d, &$r4[$x + 2..]).mul_add($kv[22], acc1);
        acc2 = <D::F32Vec>::load($d, &$r4[$x + 3..]).mul_add($kv[23], acc2);
        acc0 = <D::F32Vec>::load($d, &$r4[$x + 4..]).mul_add($kv[24], acc0);

        acc0 + acc1 + acc2
    }};
}

simd_function!(
    upsample_2x_simd_dispatch,
    d: D,
    fn upsample_2x_simd(
        input: &[&[f32]],
        xsize: usize,
        flat_kernels: &[[f32; 25]],
        col_min: &mut [f32],
        col_max: &mut [f32],
        mins: &mut [f32],
        maxs: &mut [f32],
        output: &mut [&mut [f32]],
    ) {
        compute_minmax(d, input, xsize, col_min, col_max, mins, maxs);

        let r0 = input[0];
        let r1 = input[1];
        let r2 = input[2];
        let r3 = input[3];
        let r4 = input[4];

        let mut kernel_vecs = [[D::F32Vec::splat(d, 0.0); 25]; 4];
        for idx in 0..4 {
            let k = &flat_kernels[idx];
            for i in 0..25 {
                kernel_vecs[idx][i] = D::F32Vec::splat(d, k[i]);
            }
        }

        let mins_iter = mins.chunks_exact(D::F32Vec::LEN);
        let maxs_iter = maxs.chunks_exact(D::F32Vec::LEN);

        for ((mins_chunk, maxs_chunk), x) in mins_iter
            .zip(maxs_iter)
            .zip((0..xsize).step_by(D::F32Vec::LEN))
            .take(xsize.div_ceil(D::F32Vec::LEN))
        {
            let minval = D::F32Vec::load(d, mins_chunk);
            let maxval = D::F32Vec::load(d, maxs_chunk);
            let out_x = x * 2;

            let r0_0 = kernel_conv!(d, kernel_vecs[0], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
            let r0_1 = kernel_conv!(d, kernel_vecs[1], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
            D::F32Vec::store_interleaved_2(r0_0, r0_1, &mut output[0][out_x..]);

            let r1_0 = kernel_conv!(d, kernel_vecs[2], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
            let r1_1 = kernel_conv!(d, kernel_vecs[3], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
            D::F32Vec::store_interleaved_2(r1_0, r1_1, &mut output[1][out_x..]);
        }
    }
);

simd_function!(
    upsample_4x_simd_dispatch,
    d: D,
    fn upsample_4x_simd(
        input: &[&[f32]],
        xsize: usize,
        flat_kernels: &[[f32; 25]],
        col_min: &mut [f32],
        col_max: &mut [f32],
        mins: &mut [f32],
        maxs: &mut [f32],
        output: &mut [&mut [f32]],
    ) {
        compute_minmax(d, input, xsize, col_min, col_max, mins, maxs);

        let r0 = input[0];
        let r1 = input[1];
        let r2 = input[2];
        let r3 = input[3];
        let r4 = input[4];

        let mut kernel_vecs = [[D::F32Vec::splat(d, 0.0); 25]; 16];
        for idx in 0..16 {
            let k = &flat_kernels[idx];
            for i in 0..25 {
                kernel_vecs[idx][i] = D::F32Vec::splat(d, k[i]);
            }
        }

        let mins_iter = mins.chunks_exact(D::F32Vec::LEN);
        let maxs_iter = maxs.chunks_exact(D::F32Vec::LEN);

        for ((mins_chunk, maxs_chunk), x) in mins_iter
            .zip(maxs_iter)
            .zip((0..xsize).step_by(D::F32Vec::LEN))
            .take(xsize.div_ceil(D::F32Vec::LEN))
        {
            let minval = D::F32Vec::load(d, mins_chunk);
            let maxval = D::F32Vec::load(d, maxs_chunk);
            let out_x = x * 4;

            for oy in 0..4 {
                let base = oy * 4;
                let v0 = kernel_conv!(d, kernel_vecs[base], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
                let v1 = kernel_conv!(d, kernel_vecs[base + 1], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
                let v2 = kernel_conv!(d, kernel_vecs[base + 2], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
                let v3 = kernel_conv!(d, kernel_vecs[base + 3], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
                D::F32Vec::store_interleaved_4(v0, v1, v2, v3, &mut output[oy][out_x..]);
            }
        }
    }
);

simd_function!(
    upsample_8x_simd_dispatch,
    d: D,
    fn upsample_8x_simd(
        input: &[&[f32]],
        xsize: usize,
        flat_kernels: &[[f32; 25]],
        col_min: &mut [f32],
        col_max: &mut [f32],
        mins: &mut [f32],
        maxs: &mut [f32],
        output: &mut [&mut [f32]],
    ) {
        compute_minmax(d, input, xsize, col_min, col_max, mins, maxs);

        let r0 = input[0];
        let r1 = input[1];
        let r2 = input[2];
        let r3 = input[3];
        let r4 = input[4];

        let mut kernel_vecs = [[D::F32Vec::splat(d, 0.0); 25]; 64];
        for idx in 0..64 {
            let k = &flat_kernels[idx];
            for i in 0..25 {
                kernel_vecs[idx][i] = D::F32Vec::splat(d, k[i]);
            }
        }

        let mins_iter = mins.chunks_exact(D::F32Vec::LEN);
        let maxs_iter = maxs.chunks_exact(D::F32Vec::LEN);

        for ((mins_chunk, maxs_chunk), x) in mins_iter
            .zip(maxs_iter)
            .zip((0..xsize).step_by(D::F32Vec::LEN))
            .take(xsize.div_ceil(D::F32Vec::LEN))
        {
            let minval = D::F32Vec::load(d, mins_chunk);
            let maxval = D::F32Vec::load(d, maxs_chunk);
            let out_x = x * 8;

            for oy in 0..8 {
                let base = oy * 8;
                let v0 = kernel_conv!(d, kernel_vecs[base], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
                let v1 = kernel_conv!(d, kernel_vecs[base + 1], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
                let v2 = kernel_conv!(d, kernel_vecs[base + 2], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
                let v3 = kernel_conv!(d, kernel_vecs[base + 3], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
                let v4 = kernel_conv!(d, kernel_vecs[base + 4], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
                let v5 = kernel_conv!(d, kernel_vecs[base + 5], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
                let v6 = kernel_conv!(d, kernel_vecs[base + 6], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
                let v7 = kernel_conv!(d, kernel_vecs[base + 7], r0, r1, r2, r3, r4, x).max(minval).min(maxval);
                D::F32Vec::store_interleaved_8(v0, v1, v2, v3, v4, v5, v6, v7, &mut output[oy][out_x..]);
            }
        }
    }
);

impl<const N: usize, const SHIFT: u8> RenderPipelineInOutStage for Upsample<N, SHIFT> {
    type InputT = f32;
    type OutputT = f32;
    const SHIFT: (u8, u8) = (SHIFT, SHIFT);
    const BORDER: (u8, u8) = (2, 2);

    fn uses_channel(&self, c: usize) -> bool {
        c == self.channel
    }

    fn init_local_state(&self, _thread_index: usize) -> crate::error::Result<Option<Box<dyn Any>>> {
        Ok(Some(Box::new(UpsampleState::new()) as Box<dyn Any>))
    }

    /// Processes a chunk of a row, applying NxN upsampling using a 5x5 kernel.
    /// Each input value expands into a NxN region in the output, based on neighboring inputs.
    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        input_rows: &Channels<f32>,
        output_rows: &mut ChannelsMut<f32>,
        state: Option<&mut dyn std::any::Any>,
    ) {
        let input = &input_rows[0];
        let state: &mut UpsampleState = state.unwrap().downcast_mut().unwrap();
        state.ensure_capacity(xsize);

        match N {
            2 => {
                upsample_2x_simd_dispatch(
                    input,
                    xsize,
                    self.flat_kernels.as_slice(),
                    &mut state.col_min,
                    &mut state.col_max,
                    &mut state.mins,
                    &mut state.maxs,
                    &mut output_rows[0],
                );
            }
            4 => {
                upsample_4x_simd_dispatch(
                    input,
                    xsize,
                    self.flat_kernels.as_slice(),
                    &mut state.col_min,
                    &mut state.col_max,
                    &mut state.mins,
                    &mut state.maxs,
                    &mut output_rows[0],
                );
            }
            8 => {
                upsample_8x_simd_dispatch(
                    input,
                    xsize,
                    self.flat_kernels.as_slice(),
                    &mut state.col_min,
                    &mut state.col_max,
                    &mut state.mins,
                    &mut state.maxs,
                    &mut output_rows[0],
                );
            }
            _ => unreachable!(),
        }
    }
}

pub type Upsample2x = Upsample<2, 1>;
pub type Upsample4x = Upsample<4, 2>;
pub type Upsample8x = Upsample<8, 3>;
