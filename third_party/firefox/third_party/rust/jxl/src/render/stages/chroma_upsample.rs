// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use crate::render::{Channels, ChannelsMut, RenderPipelineInOutStage};
use jxl_simd::{F32SimdVec, simd_function};

pub struct HorizontalChromaUpsample {
    channel: usize,
}

impl HorizontalChromaUpsample {
    pub fn new(channel: usize) -> HorizontalChromaUpsample {
        HorizontalChromaUpsample { channel }
    }
}

impl std::fmt::Display for HorizontalChromaUpsample {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "chroma upsample of channel {}, horizontally",
            self.channel
        )
    }
}

simd_function!(
    hchroma_upsample_simd_dispatch,
    d: D,
    fn hchroma_upsample_simd(input: &[f32], output: &mut [f32], xsize: usize) {
        let c025 = D::F32Vec::splat(d, 0.25);
        let c075 = D::F32Vec::splat(d, 0.75);

        let input_iter = input.windows(D::F32Vec::LEN + 2).step_by(D::F32Vec::LEN);
        let output_iter = output.chunks_exact_mut(2 * D::F32Vec::LEN);

        for (in_win, out_chunk) in input_iter.zip(output_iter).take(xsize.div_ceil(D::F32Vec::LEN))
        {
            let prev_vec = D::F32Vec::load(d, &in_win[0..]);
            let cur_vec = D::F32Vec::load(d, &in_win[1..]);
            let next_vec = D::F32Vec::load(d, &in_win[2..]);

            let left = prev_vec.mul_add(c025, cur_vec * c075);

            let right = next_vec.mul_add(c025, cur_vec * c075);

            D::F32Vec::store_interleaved_2(left, right, out_chunk);
        }
    }
);

impl RenderPipelineInOutStage for HorizontalChromaUpsample {
    type InputT = f32;
    type OutputT = f32;
    const SHIFT: (u8, u8) = (1, 0);
    const BORDER: (u8, u8) = (1, 0);

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
        let output = &mut output_rows[0];
        hchroma_upsample_simd_dispatch(input[0], output[0], xsize);
    }
}

pub struct VerticalChromaUpsample {
    channel: usize,
}

impl VerticalChromaUpsample {
    pub fn new(channel: usize) -> VerticalChromaUpsample {
        VerticalChromaUpsample { channel }
    }
}

impl std::fmt::Display for VerticalChromaUpsample {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "chroma upsample of channel {}, vertically", self.channel)
    }
}

simd_function!(
    vchroma_upsample_simd_dispatch,
    d: D,
    fn vchroma_upsample_simd(
        input_prev: &[f32],
        input_cur: &[f32],
        input_next: &[f32],
        output_up: &mut [f32],
        output_down: &mut [f32],
        xsize: usize,
    ) {
        let c025 = D::F32Vec::splat(d, 0.25);
        let c075 = D::F32Vec::splat(d, 0.75);

        let prev_iter = input_prev.chunks_exact(D::F32Vec::LEN);
        let cur_iter = input_cur.chunks_exact(D::F32Vec::LEN);
        let next_iter = input_next.chunks_exact(D::F32Vec::LEN);
        let up_iter = output_up.chunks_exact_mut(D::F32Vec::LEN);
        let down_iter = output_down.chunks_exact_mut(D::F32Vec::LEN);

        for ((((prev_chunk, cur_chunk), next_chunk), up_chunk), down_chunk) in prev_iter
            .zip(cur_iter)
            .zip(next_iter)
            .zip(up_iter)
            .zip(down_iter)
            .take(xsize.div_ceil(D::F32Vec::LEN))
        {
            let prev_vec = D::F32Vec::load(d, prev_chunk);
            let cur_vec = D::F32Vec::load(d, cur_chunk);
            let next_vec = D::F32Vec::load(d, next_chunk);

            let up = prev_vec.mul_add(c025, cur_vec * c075);

            let down = next_vec.mul_add(c025, cur_vec * c075);

            up.store(up_chunk);
            down.store(down_chunk);
        }
    }
);

impl RenderPipelineInOutStage for VerticalChromaUpsample {
    type InputT = f32;
    type OutputT = f32;
    const SHIFT: (u8, u8) = (0, 1);
    const BORDER: (u8, u8) = (0, 1);

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
        let output = &mut output_rows[0];
        let (output_up, output_down) = output.split_at_mut(1);
        vchroma_upsample_simd_dispatch(
            input[0],
            input[1],
            input[2],
            output_up[0],
            output_down[0],
            xsize,
        );
    }
}
