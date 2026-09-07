// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use std::sync::Arc;

use crate::{
    frame::quantizer::LfQuantFactors,
    headers::bit_depth::BitDepth,
    render::{Channels, ChannelsMut, RenderPipelineInOutStage, StageSpecialCase},
    util::AtomicRefCell,
};
use jxl_simd::{F32SimdVec, I32SimdVec, SimdMask, simd_function};

pub struct ConvertModularXYBToF32Stage {
    first_channel: usize,
    lf_quant: Arc<AtomicRefCell<LfQuantFactors>>,
}

impl ConvertModularXYBToF32Stage {
    pub fn new(
        first_channel: usize,
        lf_quant: Arc<AtomicRefCell<LfQuantFactors>>,
    ) -> ConvertModularXYBToF32Stage {
        ConvertModularXYBToF32Stage {
            first_channel,
            lf_quant,
        }
    }
}

impl std::fmt::Display for ConvertModularXYBToF32Stage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "convert modular xyb data to F32 in channels {}..{}",
            self.first_channel,
            self.first_channel + 2,
        )
    }
}

impl RenderPipelineInOutStage for ConvertModularXYBToF32Stage {
    type InputT = i32;
    type OutputT = f32;
    const SHIFT: (u8, u8) = (0, 0);
    const BORDER: (u8, u8) = (0, 0);

    fn uses_channel(&self, c: usize) -> bool {
        (self.first_channel..self.first_channel + 3).contains(&c)
    }

    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        input_rows: &Channels<i32>,
        output_rows: &mut ChannelsMut<f32>,
        _state: Option<&mut dyn std::any::Any>,
    ) {
        let lf_quant = self.lf_quant.borrow();
        let [scale_x, scale_y, scale_b] = lf_quant.quant_factors;
        assert_eq!(
            input_rows.len(),
            3,
            "incorrect number of channels; expected 3, found {}",
            input_rows.len()
        );
        let (input_y, input_x, input_b) = (&input_rows[0], &input_rows[1], &input_rows[2]);
        let (output_x, output_y, output_b) = output_rows.split_first_3_mut();
        for i in 0..xsize {
            output_x[0][i] = input_x[0][i] as f32 * scale_x;
            output_y[0][i] = input_y[0][i] as f32 * scale_y;
            output_b[0][i] = (input_b[0][i] + input_y[0][i]) as f32 * scale_b;
        }
    }
}

pub struct ConvertModularToF32Stage {
    channel: usize,
    bit_depth: BitDepth,
}

impl ConvertModularToF32Stage {
    pub fn new(channel: usize, bit_depth: BitDepth) -> ConvertModularToF32Stage {
        ConvertModularToF32Stage { channel, bit_depth }
    }
}

impl std::fmt::Display for ConvertModularToF32Stage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "convert modular data to F32 in channel {} with bit depth {:?}",
            self.channel, self.bit_depth
        )
    }
}

simd_function!(
    int_to_float_32bit_simd_dispatch,
    d: D,
    fn int_to_float_32bit_simd(input: &[i32], output: &mut [f32], xsize: usize) {
        let simd_width = D::I32Vec::LEN;

        for (in_chunk, out_chunk) in input
            .chunks_exact(simd_width)
            .zip(output.chunks_exact_mut(simd_width))
            .take(xsize.div_ceil(simd_width))
        {
            let val = D::I32Vec::load(d, in_chunk);
            val.bitcast_to_f32().store(out_chunk);
        }
    }
);

simd_function!(
    int_to_float_16bit_simd_dispatch,
    d: D,
    fn int_to_float_16bit_simd(input: &[i32], output: &mut [f32], xsize: usize) {
        let simd_width = D::F32Vec::LEN;

        const { assert!(D::F32Vec::LEN <= 16) }
        let mut u16_buf = [0u16; 16];

        for (in_chunk, out_chunk) in input
            .chunks_exact(simd_width)
            .zip(output.chunks_exact_mut(simd_width))
            .take(xsize.div_ceil(simd_width))
        {
            let i32_vec = D::I32Vec::load(d, in_chunk);
            i32_vec.store_u16(&mut u16_buf[..simd_width]);
            let result = D::F32Vec::load_f16_bits(d, &u16_buf[..simd_width]);
            result.store(out_chunk);
        }
    }
);

fn int_to_float(input: &[i32], output: &mut [f32], bit_depth: &BitDepth, xsize: usize) {
    assert_eq!(input.len(), output.len());
    let bits = bit_depth.bits_per_sample();
    let exp_bits = bit_depth.exponent_bits_per_sample();

    if bits == 32 && exp_bits == 8 {
        int_to_float_32bit_simd_dispatch(input, output, xsize);
        return;
    }

    if bits == 16 && exp_bits == 5 {
        int_to_float_16bit_simd_dispatch(input, output, xsize);
        return;
    }

    int_to_float_generic(input, output, bits, exp_bits);
}

fn int_to_float_generic(input: &[i32], output: &mut [f32], bits: u32, exp_bits: u32) {
    let exp_bias = (1 << (exp_bits - 1)) - 1;
    let sign_shift = bits - 1;
    let mant_bits = bits - exp_bits - 1;
    let mant_shift = 23 - mant_bits;
    for (&in_val, out_val) in input.iter().zip(output) {
        let mut f = in_val as u32;
        let signbit = (f >> sign_shift) != 0;
        f &= (1 << sign_shift) - 1;
        if f == 0 {
            *out_val = if signbit { -0.0 } else { 0.0 };
            continue;
        }
        let mut exp = (f >> mant_bits) as i32;
        let mut mantissa = f & ((1 << mant_bits) - 1);
        if exp == (1 << exp_bits) - 1 {
            f = if signbit { 0x80000000 } else { 0 };
            f |= 0b11111111 << 23;
            f |= mantissa << mant_shift;
            *out_val = f32::from_bits(f);
            continue;
        }
        mantissa <<= mant_shift;
        if exp == 0 && exp_bits < 8 {
            while (mantissa & 0x800000) == 0 {
                mantissa <<= 1;
                exp -= 1;
            }
            exp += 1;
            mantissa &= 0x7fffff;
        }
        exp -= exp_bias;
        exp += 127;
        assert!(exp >= 0);
        f = if signbit { 0x80000000 } else { 0 };
        f |= (exp as u32) << 23;
        f |= mantissa;
        *out_val = f32::from_bits(f);
    }
}

simd_function!(
    modular_to_float_32bit_simd_dispatch,
    d: D,
    fn modular_to_float_32bit_simd(input: &[i32], output: &mut [f32], scale: f32, xsize: usize) {
        let simd_width = D::I32Vec::LEN;

        let scale = D::F32Vec::splat(d, scale);

        for (in_chunk, out_chunk) in input
            .chunks_exact(simd_width)
            .zip(output.chunks_exact_mut(simd_width))
            .take(xsize.div_ceil(simd_width))
        {
            let val = D::I32Vec::load(d, in_chunk);
            (val.as_f32() * scale).store(out_chunk);
        }
    }
);

impl RenderPipelineInOutStage for ConvertModularToF32Stage {
    type InputT = i32;
    type OutputT = f32;
    const SHIFT: (u8, u8) = (0, 0);
    const BORDER: (u8, u8) = (0, 0);

    fn uses_channel(&self, c: usize) -> bool {
        c == self.channel
    }

    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        input_rows: &Channels<i32>,
        output_rows: &mut ChannelsMut<f32>,
        _state: Option<&mut dyn std::any::Any>,
    ) {
        let input = &input_rows[0];
        if self.bit_depth.floating_point_sample() {
            int_to_float(input[0], output_rows[0][0], &self.bit_depth, xsize);
        } else {
            let scale = 1.0 / ((1u64 << self.bit_depth.bits_per_sample()) - 1) as f32;
            modular_to_float_32bit_simd_dispatch(input[0], output_rows[0][0], scale, xsize);
        }
    }

    fn is_special_case(&self) -> Option<StageSpecialCase> {
        if self.bit_depth.floating_point_sample() {
            None
        } else {
            Some(StageSpecialCase::ModularToF32 {
                channel: self.channel,
                bit_depth: self.bit_depth.bits_per_sample() as u8,
            })
        }
    }
}

/// Stage that converts f32 values in [0, 1] range to u8 values.
pub struct ConvertF32ToU8Stage {
    channel: usize,
    bit_depth: u8,
}

impl ConvertF32ToU8Stage {
    pub fn new(channel: usize, bit_depth: u8) -> ConvertF32ToU8Stage {
        ConvertF32ToU8Stage { channel, bit_depth }
    }
}

impl std::fmt::Display for ConvertF32ToU8Stage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "convert F32 to U8 in channel {} with bit depth {}",
            self.channel, self.bit_depth
        )
    }
}

simd_function!(
    f32_to_u8_simd_dispatch,
    d: D,
    fn f32_to_u8_simd(input: &[f32], output: &mut [u8], max: f32, xsize: usize) {
        let simd_width = D::F32Vec::LEN;
        let zero = D::F32Vec::splat(d, 0.0);
        let one = D::F32Vec::splat(d, 1.0);
        let scale = D::F32Vec::splat(d, max);

        for (input_chunk, output_chunk) in input
            .chunks_exact(simd_width)
            .zip(output.chunks_exact_mut(simd_width))
            .take(xsize.div_ceil(simd_width))
        {
            let val = D::F32Vec::load(d, input_chunk);
            let clamped = val.max(zero).min(one);
            let scaled = clamped * scale;
            scaled.round_store_u8(output_chunk);
        }
    }
);

impl RenderPipelineInOutStage for ConvertF32ToU8Stage {
    type InputT = f32;
    type OutputT = u8;
    const SHIFT: (u8, u8) = (0, 0);
    const BORDER: (u8, u8) = (0, 0);

    fn uses_channel(&self, c: usize) -> bool {
        c == self.channel
    }

    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        input_rows: &Channels<f32>,
        output_rows: &mut ChannelsMut<u8>,
        _state: Option<&mut dyn std::any::Any>,
    ) {
        let input = input_rows[0][0];
        let output = &mut output_rows[0][0];
        let max = ((1u32 << self.bit_depth) - 1) as f32;
        f32_to_u8_simd_dispatch(input, output, max, xsize);
    }

    fn is_special_case(&self) -> Option<StageSpecialCase> {
        Some(StageSpecialCase::F32ToU8 {
            channel: self.channel,
            bit_depth: self.bit_depth,
        })
    }
}

/// Stage that converts i32 values to u8 values, applying a multiplier.
pub struct ConvertI32ToU8Stage {
    channel: usize,
    multiplier: i32,
    max: i32,
}

impl ConvertI32ToU8Stage {
    pub fn new(channel: usize, multiplier: i32, max: i32) -> ConvertI32ToU8Stage {
        ConvertI32ToU8Stage {
            channel,
            multiplier,
            max,
        }
    }
}

impl std::fmt::Display for ConvertI32ToU8Stage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "convert I32 to U8 in channel {} with multiplier {}",
            self.channel, self.multiplier
        )
    }
}

simd_function!(
    i32_to_u8_simd_dispatch,
    d: D,
    fn i32_to_u8_simd(input: &[i32], output: &mut [u8], scale: i32, max: i32, xsize: usize) {
        let simd_width = D::F32Vec::LEN;
        let scale = D::I32Vec::splat(d, scale);
        let max = D::I32Vec::splat(d, max);
        let zero = D::I32Vec::splat(d, 0);

        for (input_chunk, output_chunk) in input
            .chunks_exact(simd_width)
            .zip(output.chunks_exact_mut(simd_width))
            .take(xsize.div_ceil(simd_width))
        {
            let val = D::I32Vec::load(d, input_chunk);
            let scaled = val * scale;
            let zeroclip = scaled.lt_zero().if_then_else_i32(zero, scaled);
            let clip = scaled.gt(max).if_then_else_i32(max, zeroclip);
            clip.store_u8(output_chunk);
        }
    }
);

impl RenderPipelineInOutStage for ConvertI32ToU8Stage {
    type InputT = i32;
    type OutputT = u8;
    const SHIFT: (u8, u8) = (0, 0);
    const BORDER: (u8, u8) = (0, 0);

    fn uses_channel(&self, c: usize) -> bool {
        c == self.channel
    }

    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        input_rows: &Channels<i32>,
        output_rows: &mut ChannelsMut<u8>,
        _state: Option<&mut dyn std::any::Any>,
    ) {
        let input = input_rows[0][0];
        let output = &mut output_rows[0][0];
        i32_to_u8_simd_dispatch(input, output, self.multiplier, self.max, xsize);
    }
}

/// Stage that converts f32 values in [0, 1] range to u16 values.
pub struct ConvertF32ToU16Stage {
    channel: usize,
    bit_depth: u8,
}

impl ConvertF32ToU16Stage {
    pub fn new(channel: usize, bit_depth: u8) -> ConvertF32ToU16Stage {
        ConvertF32ToU16Stage { channel, bit_depth }
    }
}

impl std::fmt::Display for ConvertF32ToU16Stage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "convert F32 to U16 in channel {} with bit depth {}",
            self.channel, self.bit_depth
        )
    }
}

simd_function!(
    f32_to_u16_simd_dispatch,
    d: D,
    fn f32_to_u16_simd(input: &[f32], output: &mut [u16], max: f32, xsize: usize) {
        let simd_width = D::F32Vec::LEN;
        let zero = D::F32Vec::splat(d, 0.0);
        let one = D::F32Vec::splat(d, 1.0);
        let scale = D::F32Vec::splat(d, max);

        for (input_chunk, output_chunk) in input
            .chunks_exact(simd_width)
            .zip(output.chunks_exact_mut(simd_width))
            .take(xsize.div_ceil(simd_width))
        {
            let val = D::F32Vec::load(d, input_chunk);
            let clamped = val.max(zero).min(one);
            let scaled = clamped * scale;
            scaled.round_store_u16(output_chunk);
        }
    }
);

impl RenderPipelineInOutStage for ConvertF32ToU16Stage {
    type InputT = f32;
    type OutputT = u16;
    const SHIFT: (u8, u8) = (0, 0);
    const BORDER: (u8, u8) = (0, 0);

    fn uses_channel(&self, c: usize) -> bool {
        c == self.channel
    }

    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        input_rows: &Channels<f32>,
        output_rows: &mut ChannelsMut<u16>,
        _state: Option<&mut dyn std::any::Any>,
    ) {
        let input = input_rows[0][0];
        let output = &mut output_rows[0][0];
        let max = ((1u32 << self.bit_depth) - 1) as f32;
        f32_to_u16_simd_dispatch(input, output, max, xsize);
    }
}

/// Stage that converts f32 values to f16 (half-precision float) values.
pub struct ConvertF32ToF16Stage {
    channel: usize,
    clamp_range: Option<(f32, f32)>,
}

impl ConvertF32ToF16Stage {
    pub fn new(channel: usize) -> ConvertF32ToF16Stage {
        ConvertF32ToF16Stage {
            channel,
            clamp_range: None,
        }
    }

    pub fn new_with_clamp_range(
        channel: usize,
        clamp_range: Option<(f32, f32)>,
    ) -> ConvertF32ToF16Stage {
        ConvertF32ToF16Stage {
            channel,
            clamp_range,
        }
    }

    pub fn new_with_unit_clamp(channel: usize, clamp_unit_range: bool) -> ConvertF32ToF16Stage {
        ConvertF32ToF16Stage {
            channel,
            clamp_range: if clamp_unit_range {
                Some((0.0, 1.0))
            } else {
                None
            },
        }
    }
}

impl std::fmt::Display for ConvertF32ToF16Stage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "convert F32 to F16 in channel {}", self.channel)
    }
}

impl RenderPipelineInOutStage for ConvertF32ToF16Stage {
    type InputT = f32;
    type OutputT = crate::util::f16;
    const SHIFT: (u8, u8) = (0, 0);
    const BORDER: (u8, u8) = (0, 0);

    fn uses_channel(&self, c: usize) -> bool {
        c == self.channel
    }

    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        input_rows: &Channels<f32>,
        output_rows: &mut ChannelsMut<crate::util::f16>,
        _state: Option<&mut dyn std::any::Any>,
    ) {
        let input = &input_rows[0];
        if let Some((min_value, max_value)) = self.clamp_range {
            for i in 0..xsize {
                output_rows[0][0][i] =
                    crate::util::f16::from_f32(input[0][i].clamp(min_value, max_value));
            }
        } else {
            for i in 0..xsize {
                output_rows[0][0][i] = crate::util::f16::from_f32(input[0][i]);
            }
        }
    }
}
