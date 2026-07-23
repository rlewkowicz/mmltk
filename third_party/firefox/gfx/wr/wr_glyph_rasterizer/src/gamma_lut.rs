/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*!
Gamma correction lookup tables.

This is a port of Skia gamma LUT logic into Rust, used by WebRender.
*/
#![allow(dead_code)]

use api::ColorU;
use std::cmp::max;

/// Color space responsible for converting between lumas and luminances.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum LuminanceColorSpace {
    /// Linear space - no conversion involved.
    Linear,
    /// Simple gamma space - uses the `luminance ^ gamma` function.
    Gamma(f32),
    /// Srgb space.
    Srgb,
}

impl LuminanceColorSpace {
    pub fn new(gamma: f32) -> LuminanceColorSpace {
        if gamma == 1.0 {
            LuminanceColorSpace::Linear
        } else if gamma == 0.0 {
            LuminanceColorSpace::Srgb
        } else {
            LuminanceColorSpace::Gamma(gamma)
        }
    }

    pub fn to_luma(&self, luminance: f32) -> f32 {
        match *self {
            LuminanceColorSpace::Linear => luminance,
            LuminanceColorSpace::Gamma(gamma) => luminance.powf(gamma),
            LuminanceColorSpace::Srgb => {
                if luminance <= 0.04045 {
                    luminance / 12.92
                } else {
                    ((luminance + 0.055) / 1.055).powf(2.4)
                }
            }
        }
    }

    pub fn from_luma(&self, luma: f32) -> f32 {
        match *self {
            LuminanceColorSpace::Linear => luma,
            LuminanceColorSpace::Gamma(gamma) => luma.powf(1. / gamma),
            LuminanceColorSpace::Srgb => {
                if luma <= 0.0031308 {
                    luma * 12.92
                } else {
                    1.055 * luma.powf(1./2.4) - 0.055
                }
            }
        }
    }
}

fn round_to_u8(x : f32) -> u8 {
    let v = (x + 0.5).floor() as i32;
    assert!(0 <= v && v < 0x100);
    v as u8
}

fn scale255(n: u8, mut base: u8) -> u8 {
    base <<= 8 - n;
    let mut lum = base;
    let mut i = n;

    while i < 8 {
        lum |= base >> i;
        i += n;
    }

    lum
}

fn compute_luminance(r: u8, g: u8, b: u8) -> u8 {
    let val: u32 = r as u32 * 54 + g as u32 * 183 + b as u32 * 19;
    assert!(val < 0x10000);
    (val >> 8) as u8
}

const LUM_BITS: u8 = 3;
const LUM_MASK: u8 = ((1 << LUM_BITS) - 1) << (8 - LUM_BITS);

pub trait ColorLut {
    fn quantize(&self) -> ColorU;
    fn quantized_floor(&self) -> ColorU;
    fn quantized_ceil(&self) -> ColorU;
    fn luminance(&self) -> u8;
    fn luminance_color(&self) -> ColorU;
}

impl ColorLut for ColorU {
    fn quantize(&self) -> ColorU {
        ColorU::new(
            scale255(LUM_BITS, self.r >> (8 - LUM_BITS)),
            scale255(LUM_BITS, self.g >> (8 - LUM_BITS)),
            scale255(LUM_BITS, self.b >> (8 - LUM_BITS)),
            255,
        )
    }

    fn quantized_floor(&self) -> ColorU {
        ColorU::new(
            self.r & LUM_MASK,
            self.g & LUM_MASK,
            self.b & LUM_MASK,
            255,
        )
    }

    fn quantized_ceil(&self) -> ColorU {
        ColorU::new(
            self.r | !LUM_MASK,
            self.g | !LUM_MASK,
            self.b | !LUM_MASK,
            255,
        )
    }

    fn luminance(&self) -> u8 {
        compute_luminance(self.r, self.g, self.b)
    }

    fn luminance_color(&self) -> ColorU {
        let lum = self.luminance();
        ColorU::new(lum, lum, lum, self.a)
    }
}

fn apply_contrast(srca: f32, contrast: f32) -> f32 {
    srca + ((1.0 - srca) * contrast * srca)
}

pub fn build_gamma_correcting_lut(table: &mut [u8; 256], src: u8, contrast: f32,
                                  src_space: LuminanceColorSpace,
                                  dst_convert: LuminanceColorSpace) {
    let src = src as f32 / 255.0;
    let lin_src = src_space.to_luma(src);
    let dst = 1.0 - src;
    let lin_dst = dst_convert.to_luma(dst);

    let adjusted_contrast = contrast * lin_dst;

    if (src - dst).abs() < (1.0 / 256.0) {
        let mut ii : f32 = 0.0;
        for v in table.iter_mut() {
            let raw_srca = ii / 255.0;
            let srca = apply_contrast(raw_srca, adjusted_contrast);

            *v = round_to_u8(255.0 * srca);
            ii += 1.0;
        }
    } else {
        let mut ii : f32 = 0.0;
        for v in table.iter_mut() {
            let raw_srca = ii / 255.0;
            let srca = apply_contrast(raw_srca, adjusted_contrast);
            assert!(srca <= 1.0);
            let dsta = 1.0 - srca;

            let lin_out = lin_src * srca + dsta * lin_dst;
            assert!(lin_out <= 1.0);
            let out = dst_convert.from_luma(lin_out);

            let result = (out - dst) / (src - dst);

            *v = round_to_u8(255.0 * result);
            debug!("Setting {:?} to {:?}", ii as u8, *v);

            ii += 1.0;
        }
    }
}

pub struct GammaLut {
    tables: [[u8; 256]; 1 << LUM_BITS],
}

impl GammaLut {
    fn generate_tables(&mut self, contrast: f32, paint_gamma: f32, device_gamma: f32) {
        let paint_color_space = LuminanceColorSpace::new(paint_gamma);
        let device_color_space = LuminanceColorSpace::new(device_gamma);

        for (i, entry) in self.tables.iter_mut().enumerate() {
            let luminance = scale255(LUM_BITS, i as u8);
            build_gamma_correcting_lut(entry,
                                       luminance,
                                       contrast,
                                       paint_color_space,
                                       device_color_space);
        }
    }

    pub fn table_count(&self) -> usize {
        self.tables.len()
    }

    pub fn get_table(&self, color: u8) -> &[u8; 256] {
        &self.tables[(color >> (8 - LUM_BITS)) as usize]
    }

    pub fn new(contrast: f32, paint_gamma: f32, device_gamma: f32) -> GammaLut {

        let mut table = GammaLut {
            tables: [[0; 256]; 1 << LUM_BITS],
        };

        table.generate_tables(contrast, paint_gamma, device_gamma);

        table
    }

    pub fn preblend(&self, pixels: &mut [u8], color: ColorU) {
        let table_r = self.get_table(color.r);
        let table_g = self.get_table(color.g);
        let table_b = self.get_table(color.b);

        for pixel in pixels.chunks_mut(4) {
            let (b, g, r) = (table_b[pixel[0] as usize], table_g[pixel[1] as usize], table_r[pixel[2] as usize]);
            pixel[0] = b;
            pixel[1] = g;
            pixel[2] = r;
            pixel[3] = max(max(b, g), r);
        }
    }

    pub fn preblend_scaled(&self, pixels: &mut [u8], color: ColorU, percent: u8) {
        if percent >= 100 {
            self.preblend(pixels, color);
            return;
        }

        let table_r = self.get_table(color.r);
        let table_g = self.get_table(color.g);
        let table_b = self.get_table(color.b);
        let scale = (percent as i32 * 256) / 100;

        for pixel in pixels.chunks_mut(4) {
            let (mut b, g, mut r) = (
                table_b[pixel[0] as usize] as i32,
                table_g[pixel[1] as usize] as i32,
                table_r[pixel[2] as usize] as i32,
            );
            b = g + (((b - g) * scale) >> 8);
            r = g + (((r - g) * scale) >> 8);
            pixel[0] = b as u8;
            pixel[1] = g as u8;
            pixel[2] = r as u8;
            pixel[3] = max(max(b, g), r) as u8;
        }
    }

pub fn preblend_grayscale(&self, pixels: &mut [u8], color: ColorU) {
        let table_g = self.get_table(color.g);

        for pixel in pixels.chunks_mut(4) {
            let luminance = compute_luminance(pixel[2], pixel[1], pixel[0]);
            let alpha = table_g[luminance as usize];
            pixel[0] = alpha;
            pixel[1] = alpha;
            pixel[2] = alpha;
            pixel[3] = alpha;
        }
    }

} 
