// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

//! SIMD utilities for interleaving and deinterleaving channel data.
//!
//! These functions assume that input buffers are padded to at least the SIMD
//! vector length (up to 16 elements), as is standard in the render pipeline.

use jxl_simd::{F32SimdVec, simd_function};

simd_function!(
    interleave_2_dispatch,
    d: D,
    /// Interleave 2 planar channels into packed format.
    /// Buffers must be padded to SIMD vector length.
    pub fn interleave_2(a: &[f32], b: &[f32], out: &mut [f32]) {
        let len = D::F32Vec::LEN;
        for ((chunk_a, chunk_b), chunk_out) in a
            .chunks_exact(len)
            .zip(b.chunks_exact(len))
            .zip(out.chunks_exact_mut(len * 2))
        {
            let va = D::F32Vec::load(d, chunk_a);
            let vb = D::F32Vec::load(d, chunk_b);
            D::F32Vec::store_interleaved_2(va, vb, chunk_out);
        }
    }
);

simd_function!(
    deinterleave_2_dispatch,
    d: D,
    /// Deinterleave packed format into 2 planar channels.
    /// Buffers must be padded to SIMD vector length.
    pub fn deinterleave_2(input: &[f32], a: &mut [f32], b: &mut [f32]) {
        let len = D::F32Vec::LEN;
        for ((chunk_a, chunk_b), chunk_in) in a
            .chunks_exact_mut(len)
            .zip(b.chunks_exact_mut(len))
            .zip(input.chunks_exact(len * 2))
        {
            let (va, vb) = D::F32Vec::load_deinterleaved_2(d, chunk_in);
            va.store(chunk_a);
            vb.store(chunk_b);
        }
    }
);

simd_function!(
    interleave_3_dispatch,
    d: D,
    /// Interleave 3 planar channels into packed RGB format.
    /// Buffers must be padded to SIMD vector length.
    pub fn interleave_3(a: &[f32], b: &[f32], c: &[f32], out: &mut [f32]) {
        let len = D::F32Vec::LEN;
        for (((chunk_a, chunk_b), chunk_c), chunk_out) in a
            .chunks_exact(len)
            .zip(b.chunks_exact(len))
            .zip(c.chunks_exact(len))
            .zip(out.chunks_exact_mut(len * 3))
        {
            let va = D::F32Vec::load(d, chunk_a);
            let vb = D::F32Vec::load(d, chunk_b);
            let vc = D::F32Vec::load(d, chunk_c);
            D::F32Vec::store_interleaved_3(va, vb, vc, chunk_out);
        }
    }
);

simd_function!(
    deinterleave_3_dispatch,
    d: D,
    /// Deinterleave packed RGB format into 3 planar channels.
    /// Buffers must be padded to SIMD vector length.
    pub fn deinterleave_3(input: &[f32], a: &mut [f32], b: &mut [f32], c: &mut [f32]) {
        let len = D::F32Vec::LEN;
        for (((chunk_a, chunk_b), chunk_c), chunk_in) in a
            .chunks_exact_mut(len)
            .zip(b.chunks_exact_mut(len))
            .zip(c.chunks_exact_mut(len))
            .zip(input.chunks_exact(len * 3))
        {
            let (va, vb, vc) = D::F32Vec::load_deinterleaved_3(d, chunk_in);
            va.store(chunk_a);
            vb.store(chunk_b);
            vc.store(chunk_c);
        }
    }
);

simd_function!(
    interleave_4_dispatch,
    d: D,
    /// Interleave 4 planar channels into packed RGBA format.
    /// Buffers must be padded to SIMD vector length.
    pub fn interleave_4(a: &[f32], b: &[f32], c: &[f32], e: &[f32], out: &mut [f32]) {
        let len = D::F32Vec::LEN;
        for ((((chunk_a, chunk_b), chunk_c), chunk_d), chunk_out) in a
            .chunks_exact(len)
            .zip(b.chunks_exact(len))
            .zip(c.chunks_exact(len))
            .zip(e.chunks_exact(len))
            .zip(out.chunks_exact_mut(len * 4))
        {
            let va = D::F32Vec::load(d, chunk_a);
            let vb = D::F32Vec::load(d, chunk_b);
            let vc = D::F32Vec::load(d, chunk_c);
            let vd = D::F32Vec::load(d, chunk_d);
            D::F32Vec::store_interleaved_4(va, vb, vc, vd, chunk_out);
        }
    }
);

simd_function!(
    deinterleave_4_dispatch,
    d: D,
    /// Deinterleave packed RGBA format into 4 planar channels.
    /// Buffers must be padded to SIMD vector length.
    pub fn deinterleave_4(
        input: &[f32],
        a: &mut [f32],
        b: &mut [f32],
        c: &mut [f32],
        e: &mut [f32],
    ) {
        let len = D::F32Vec::LEN;
        for ((((chunk_a, chunk_b), chunk_c), chunk_d), chunk_in) in a
            .chunks_exact_mut(len)
            .zip(b.chunks_exact_mut(len))
            .zip(c.chunks_exact_mut(len))
            .zip(e.chunks_exact_mut(len))
            .zip(input.chunks_exact(len * 4))
        {
            let (va, vb, vc, vd) = D::F32Vec::load_deinterleaved_4(d, chunk_in);
            va.store(chunk_a);
            vb.store(chunk_b);
            vc.store(chunk_c);
            vd.store(chunk_d);
        }
    }
);
