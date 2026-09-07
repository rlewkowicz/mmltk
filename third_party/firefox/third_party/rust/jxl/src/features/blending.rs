// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

#![allow(clippy::needless_range_loop)]

use crate::headers::extra_channels::{ExtraChannel, ExtraChannelInfo};

use super::patches::{PatchBlendMode, PatchBlending};

#[inline]
fn maybe_clamp(v: f32, clamp: bool) -> f32 {
    if clamp { v.clamp(0.0, 1.0) } else { v }
}

pub fn perform_blending<T: AsRef<[f32]>, V: AsMut<[f32]>>(
    bg: &mut [V],
    fg: &[T],
    color_blending: &PatchBlending,
    ec_blending: &[PatchBlending],
    extra_channel_info: &[ExtraChannelInfo],
) {
    let has_alpha = extra_channel_info
        .iter()
        .any(|info| info.ec_type == ExtraChannel::Alpha);
    let num_ec = extra_channel_info.len();
    let xsize = bg[0].as_mut().len();

    let mut tmp = vec![vec![0.0f32; xsize]; 3 + num_ec];

    for i in 0..num_ec {
        let alpha = ec_blending[i].alpha_channel;
        let clamp = ec_blending[i].clamp;
        let alpha_associated = extra_channel_info[alpha].alpha_associated();

        match ec_blending[i].mode {
            PatchBlendMode::Add => {
                for x in 0..xsize {
                    tmp[3 + i][x] = bg[3 + i].as_mut()[x] + fg[3 + i].as_ref()[x];
                }
            }
            PatchBlendMode::BlendAbove => {
                if i == alpha {
                    for x in 0..xsize {
                        let fa = maybe_clamp(fg[3 + alpha].as_ref()[x], clamp);
                        tmp[3 + i][x] = 1.0 - (1.0 - fa) * (1.0 - bg[3 + i].as_mut()[x]);
                    }
                } else if alpha_associated {
                    for x in 0..xsize {
                        let fa = maybe_clamp(fg[3 + alpha].as_ref()[x], clamp);
                        tmp[3 + i][x] = fg[3 + i].as_ref()[x] + bg[3 + i].as_mut()[x] * (1.0 - fa);
                    }
                } else {
                    for x in 0..xsize {
                        let fa = maybe_clamp(fg[3 + alpha].as_ref()[x], clamp);
                        let new_a = 1.0 - (1.0 - fa) * (1.0 - bg[3 + alpha].as_mut()[x]);
                        let rnew_a = if new_a > 0.0 { 1.0 / new_a } else { 0.0 };
                        tmp[3 + i][x] = (fg[3 + i].as_ref()[x] * fa
                            + bg[3 + i].as_mut()[x] * bg[3 + alpha].as_mut()[x] * (1.0 - fa))
                            * rnew_a;
                    }
                }
            }
            PatchBlendMode::BlendBelow => {
                if i == alpha {
                    for x in 0..xsize {
                        let ba = maybe_clamp(bg[3 + alpha].as_mut()[x], clamp);
                        tmp[3 + i][x] = 1.0 - (1.0 - ba) * (1.0 - fg[3 + i].as_ref()[x]);
                    }
                } else if alpha_associated {
                    for x in 0..xsize {
                        let ba = maybe_clamp(bg[3 + alpha].as_mut()[x], clamp);
                        tmp[3 + i][x] = bg[3 + i].as_mut()[x] + fg[3 + i].as_ref()[x] * (1.0 - ba);
                    }
                } else {
                    for x in 0..xsize {
                        let ba = maybe_clamp(bg[3 + alpha].as_mut()[x], clamp);
                        let new_a = 1.0 - (1.0 - ba) * (1.0 - fg[3 + alpha].as_ref()[x]);
                        let rnew_a = if new_a > 0.0 { 1.0 / new_a } else { 0.0 };
                        tmp[3 + i][x] = (bg[3 + i].as_mut()[x] * ba
                            + fg[3 + i].as_ref()[x] * fg[3 + alpha].as_ref()[x] * (1.0 - ba))
                            * rnew_a;
                    }
                }
            }
            PatchBlendMode::AlphaWeightedAddAbove => {
                if i == alpha {
                    tmp[3 + i].copy_from_slice(bg[3 + i].as_mut());
                } else if clamp {
                    for x in 0..xsize {
                        tmp[3 + i][x] = bg[3 + i].as_mut()[x]
                            + fg[3 + i].as_ref()[x] * fg[3 + alpha].as_ref()[x].clamp(0.0, 1.0);
                    }
                } else {
                    for x in 0..xsize {
                        tmp[3 + i][x] = bg[3 + i].as_mut()[x]
                            + fg[3 + i].as_ref()[x] * fg[3 + alpha].as_ref()[x];
                    }
                }
            }
            PatchBlendMode::AlphaWeightedAddBelow => {
                if i == alpha {
                    tmp[3 + i].copy_from_slice(fg[3 + i].as_ref());
                } else if clamp {
                    for x in 0..xsize {
                        tmp[3 + i][x] = fg[3 + i].as_ref()[x]
                            + bg[3 + i].as_mut()[x] * bg[3 + alpha].as_mut()[x].clamp(0.0, 1.0);
                    }
                } else {
                    for x in 0..xsize {
                        tmp[3 + i][x] = fg[3 + i].as_ref()[x]
                            + bg[3 + i].as_mut()[x] * bg[3 + alpha].as_mut()[x];
                    }
                }
            }
            PatchBlendMode::Mul => {
                if clamp {
                    for x in 0..xsize {
                        tmp[3 + i][x] =
                            bg[3 + i].as_mut()[x] * fg[3 + i].as_ref()[x].clamp(0.0, 1.0);
                    }
                } else {
                    for x in 0..xsize {
                        tmp[3 + i][x] = bg[3 + i].as_mut()[x] * fg[3 + i].as_ref()[x];
                    }
                }
            }
            PatchBlendMode::Replace => {
                tmp[3 + i].copy_from_slice(fg[3 + i].as_ref());
            }
            PatchBlendMode::None => {
                tmp[3 + i].copy_from_slice(bg[3 + i].as_mut());
            }
        }
    }

    let alpha = color_blending.alpha_channel;
    let clamp = color_blending.clamp;

    match color_blending.mode {
        PatchBlendMode::Add => {
            for c in 0..3 {
                for x in 0..xsize {
                    tmp[c][x] = bg[c].as_mut()[x] + fg[c].as_ref()[x];
                }
            }
        }
        PatchBlendMode::AlphaWeightedAddAbove => {
            for c in 0..3 {
                if !has_alpha {
                    for x in 0..xsize {
                        tmp[c][x] = bg[c].as_mut()[x] + fg[c].as_ref()[x];
                    }
                } else if clamp {
                    for x in 0..xsize {
                        tmp[c][x] = bg[c].as_mut()[x]
                            + fg[c].as_ref()[x] * fg[3 + alpha].as_ref()[x].clamp(0.0, 1.0);
                    }
                } else {
                    for x in 0..xsize {
                        tmp[c][x] =
                            bg[c].as_mut()[x] + fg[c].as_ref()[x] * fg[3 + alpha].as_ref()[x];
                    }
                }
            }
        }
        PatchBlendMode::AlphaWeightedAddBelow => {
            for c in 0..3 {
                if !has_alpha {
                    for x in 0..xsize {
                        tmp[c][x] = bg[c].as_mut()[x] + fg[c].as_ref()[x];
                    }
                } else if clamp {
                    for x in 0..xsize {
                        tmp[c][x] = fg[c].as_ref()[x]
                            + bg[c].as_mut()[x] * bg[3 + alpha].as_mut()[x].clamp(0.0, 1.0);
                    }
                } else {
                    for x in 0..xsize {
                        tmp[c][x] =
                            fg[c].as_ref()[x] + bg[c].as_mut()[x] * bg[3 + alpha].as_mut()[x];
                    }
                }
            }
        }
        PatchBlendMode::BlendAbove => {
            if !has_alpha {
                for c in 0..3 {
                    tmp[c].copy_from_slice(fg[c].as_ref());
                }
            } else if extra_channel_info[alpha].alpha_associated() {
                for x in 0..xsize {
                    let fa = maybe_clamp(fg[3 + alpha].as_ref()[x], clamp);
                    for c in 0..3 {
                        tmp[c][x] = fg[c].as_ref()[x] + bg[c].as_mut()[x] * (1.0 - fa);
                    }
                    tmp[3 + alpha][x] = 1.0 - (1.0 - fa) * (1.0 - bg[3 + alpha].as_mut()[x]);
                }
            } else {
                for x in 0..xsize {
                    let fa = maybe_clamp(fg[3 + alpha].as_ref()[x], clamp);
                    let new_a = 1.0 - (1.0 - fa) * (1.0 - bg[3 + alpha].as_mut()[x]);
                    let rnew_a = if new_a > 0.0 { 1.0 / new_a } else { 0.0 };
                    for c in 0..3 {
                        tmp[c][x] = (fg[c].as_ref()[x] * fa
                            + bg[c].as_mut()[x] * bg[3 + alpha].as_mut()[x] * (1.0 - fa))
                            * rnew_a;
                    }
                    tmp[3 + alpha][x] = new_a;
                }
            }
        }
        PatchBlendMode::BlendBelow => {
            if !has_alpha {
                for c in 0..3 {
                    tmp[c].copy_from_slice(bg[c].as_mut());
                }
            } else if extra_channel_info[alpha].alpha_associated() {
                for x in 0..xsize {
                    let ba = maybe_clamp(bg[3 + alpha].as_mut()[x], clamp);
                    for c in 0..3 {
                        tmp[c][x] = bg[c].as_mut()[x] + fg[c].as_ref()[x] * (1.0 - ba);
                    }
                    tmp[3 + alpha][x] = 1.0 - (1.0 - ba) * (1.0 - fg[3 + alpha].as_ref()[x]);
                }
            } else {
                for x in 0..xsize {
                    let ba = maybe_clamp(bg[3 + alpha].as_mut()[x], clamp);
                    let new_a = 1.0 - (1.0 - ba) * (1.0 - fg[3 + alpha].as_ref()[x]);
                    let rnew_a = if new_a > 0.0 { 1.0 / new_a } else { 0.0 };
                    for c in 0..3 {
                        tmp[c][x] = (bg[c].as_mut()[x] * ba
                            + fg[c].as_ref()[x] * fg[3 + alpha].as_ref()[x] * (1.0 - ba))
                            * rnew_a;
                    }
                    tmp[3 + alpha][x] = new_a;
                }
            }
        }
        PatchBlendMode::Mul => {
            for c in 0..3 {
                for x in 0..xsize {
                    tmp[c][x] = bg[c].as_mut()[x] * maybe_clamp(fg[c].as_ref()[x], clamp);
                }
            }
        }
        PatchBlendMode::Replace => {
            for c in 0..3 {
                tmp[c].copy_from_slice(fg[c].as_ref());
            }
        }
        PatchBlendMode::None => {
            for c in 0..3 {
                tmp[c].copy_from_slice(bg[c].as_mut());
            }
        }
    }
    for i in 0..(3 + num_ec) {
        bg[i].as_mut().copy_from_slice(&tmp[i]);
    }
}
