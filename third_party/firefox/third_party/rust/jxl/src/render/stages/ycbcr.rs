// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use crate::render::RenderPipelineInPlaceStage;
use jxl_simd::{F32SimdVec, simd_function};

/// Convert YCbCr to RGB
pub struct YcbcrToRgbStage {
    first_channel: usize,
}

impl YcbcrToRgbStage {
    pub fn new(first_channel: usize) -> Self {
        Self { first_channel }
    }
}

impl std::fmt::Display for YcbcrToRgbStage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let channel = self.first_channel;
        write!(
            f,
            "YCbCr to RGB for channel [{},{},{}]",
            channel,
            channel + 1,
            channel + 2
        )
    }
}

simd_function!(
    ycbcr_to_rgb_simd_dispatch,
    d: D,
    fn ycbcr_to_rgb_simd(
        row_cb: &mut [f32],
        row_y: &mut [f32],
        row_cr: &mut [f32],
        xsize: usize,
    ) {
        let c128 = D::F32Vec::splat(d, 128.0 / 255.0);
        let cr_to_r = D::F32Vec::splat(d, 1.402);
        let cr_to_g = D::F32Vec::splat(d, -0.299 * 1.402 / 0.587);
        let cb_to_g = D::F32Vec::splat(d, -0.114 * 1.772 / 0.587);
        let cb_to_b = D::F32Vec::splat(d, 1.772);

        let iter_cb = row_cb.chunks_exact_mut(D::F32Vec::LEN);
        let iter_y = row_y.chunks_exact_mut(D::F32Vec::LEN);
        let iter_cr = row_cr.chunks_exact_mut(D::F32Vec::LEN);
        for ((cb_chunk, y_chunk), cr_chunk) in iter_cb
            .zip(iter_y)
            .zip(iter_cr)
            .take(xsize.div_ceil(D::F32Vec::LEN))
        {
            let y_vec = D::F32Vec::load(d, y_chunk) + c128;
            let cb_vec = D::F32Vec::load(d, cb_chunk);
            let cr_vec = D::F32Vec::load(d, cr_chunk);

            let r_vec = cr_vec.mul_add(cr_to_r, y_vec);

            let g_vec = cr_vec.mul_add(cr_to_g, cb_vec.mul_add(cb_to_g, y_vec));

            let b_vec = cb_vec.mul_add(cb_to_b, y_vec);

            r_vec.store(cb_chunk);
            g_vec.store(y_chunk);
            b_vec.store(cr_chunk);
        }
    }
);

impl RenderPipelineInPlaceStage for YcbcrToRgbStage {
    type Type = f32;

    fn uses_channel(&self, c: usize) -> bool {
        (self.first_channel..self.first_channel + 3).contains(&c)
    }

    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        row: &mut [&mut [f32]],
        _state: Option<&mut dyn std::any::Any>,
    ) {
        let [row_cb, row_y, row_cr] = row else {
            panic!(
                "incorrect number of channels; expected 3, found {}",
                row.len()
            );
        };

        assert!(xsize <= row_cb.len() && xsize <= row_y.len() && xsize <= row_cr.len());

        ycbcr_to_rgb_simd_dispatch(row_cb, row_y, row_cr, xsize);
    }
}
