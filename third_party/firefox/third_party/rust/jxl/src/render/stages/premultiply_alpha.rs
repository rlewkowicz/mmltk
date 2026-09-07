// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use crate::render::RenderPipelineInPlaceStage;
use jxl_simd::{F32SimdVec, simd_function};

/// Premultiply color channels by alpha.
/// This multiplies RGB values by the alpha channel value.
pub struct PremultiplyAlphaStage {
    /// First color channel index (typically 0 for R)
    first_color_channel: usize,
    /// Number of color channels (typically 3 for RGB)
    num_color_channels: usize,
    /// Alpha channel index
    alpha_channel: usize,
}

impl std::fmt::Display for PremultiplyAlphaStage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "premultiply alpha stage for color channels {}-{} with alpha channel {}",
            self.first_color_channel,
            self.first_color_channel + self.num_color_channels - 1,
            self.alpha_channel
        )
    }
}

impl PremultiplyAlphaStage {
    pub fn new(
        first_color_channel: usize,
        num_color_channels: usize,
        alpha_channel: usize,
    ) -> Self {
        Self {
            first_color_channel,
            num_color_channels,
            alpha_channel,
        }
    }
}

simd_function!(
    premultiply_rows_simd_dispatch,
    d: D,
    fn premultiply_rows_simd(color_rows: &mut [&mut [f32]], alpha_row: &[f32], xsize: usize) {
        for color_row in color_rows.iter_mut() {
            let iter_color = color_row.chunks_exact_mut(D::F32Vec::LEN);
            let iter_alpha = alpha_row.chunks_exact(D::F32Vec::LEN);
            for (color_chunk, alpha_chunk) in iter_color.zip(iter_alpha).take(xsize.div_ceil(D::F32Vec::LEN)) {
                let color_vec = D::F32Vec::load(d, color_chunk);
                let alpha_vec = D::F32Vec::load(d, alpha_chunk);
                let result = color_vec * alpha_vec;
                result.store(color_chunk);
            }
        }
    }
);

impl RenderPipelineInPlaceStage for PremultiplyAlphaStage {
    type Type = f32;

    fn uses_channel(&self, c: usize) -> bool {
        (self.first_color_channel..self.first_color_channel + self.num_color_channels).contains(&c)
            || c == self.alpha_channel
    }

    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        row: &mut [&mut [f32]],
        _state: Option<&mut dyn std::any::Any>,
    ) {
        let num_channels = row.len();
        if num_channels < 2 {
            return;
        }

        let (color_rows, alpha_row) = row.split_at_mut(num_channels - 1);
        let alpha_row = &alpha_row[0][..];

        premultiply_rows_simd_dispatch(color_rows, alpha_row, xsize);
    }
}
