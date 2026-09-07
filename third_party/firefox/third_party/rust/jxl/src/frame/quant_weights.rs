// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use std::{borrow::Cow, f32::consts::SQRT_2, sync::OnceLock};

use crate::util::f16;

use crate::{
    BLOCK_DIM, BLOCK_SIZE,
    bit_reader::BitReader,
    error::{
        Error::{
            HfQuantFactorTooSmall, InvalidDistanceBand, InvalidQuantEncoding,
            InvalidQuantEncodingMode, InvalidQuantizationTableWeight, InvalidRawQuantTable,
        },
        Result,
    },
    frame::{
        LfGlobalState,
        modular::{ModularChannel, ModularStreamId, decode::decode_modular_subbitstream},
    },
    headers::{bit_depth::BitDepth, frame_header::FrameHeader},
    image::Rect,
};
use jxl_transforms::transform_map::*;

pub const INV_LF_QUANT: [f32; 3] = [4096.0, 512.0, 256.0];

pub const LF_QUANT: [f32; 3] = [
    1.0 / INV_LF_QUANT[0],
    1.0 / INV_LF_QUANT[1],
    1.0 / INV_LF_QUANT[2],
];

const ALMOST_ZERO: f32 = 1e-8;

const LOG2_NUM_QUANT_MODES: usize = 3;

#[derive(Debug)]
pub struct DctQuantWeightParams {
    params: [[f32; Self::MAX_DISTANCE_BANDS]; 3],
    num_bands: usize,
}
impl DctQuantWeightParams {
    const LOG2_MAX_DISTANCE_BANDS: usize = 4;
    const MAX_DISTANCE_BANDS: usize = 1 + (1 << Self::LOG2_MAX_DISTANCE_BANDS);

    #[inline(never)]
    pub fn from_array<const N: usize>(values: &[[f32; N]; 3]) -> Self {
        let mut result = Self {
            params: [[0.0; Self::MAX_DISTANCE_BANDS]; 3],
            num_bands: N,
        };
        for (params, values) in result.params.iter_mut().zip(values) {
            params[..values.len()].copy_from_slice(values);
        }
        result
    }

    #[inline(never)]
    pub fn decode(br: &mut BitReader) -> Result<Self> {
        let num_bands = br.read(Self::LOG2_MAX_DISTANCE_BANDS)? as usize + 1;
        let mut params = [[0.0; Self::MAX_DISTANCE_BANDS]; 3];
        for row in params.iter_mut() {
            for item in row[..num_bands].iter_mut() {
                *item = f32::from(f16::from_bits(br.read(16)? as u16));
            }
            if row[0] < ALMOST_ZERO {
                return Err(HfQuantFactorTooSmall(row[0]));
            }
            row[0] *= 64.0;
        }
        Ok(DctQuantWeightParams { params, num_bands })
    }
}

#[allow(clippy::large_enum_variant)]
#[derive(Debug)]
pub enum QuantEncoding {
    Library,
    Identity {
        xyb_weights: [[f32; 3]; 3],
    },
    Dct2 {
        xyb_weights: [[f32; 6]; 3],
    },
    Dct4 {
        params: DctQuantWeightParams,
        xyb_mul: [[f32; 2]; 3],
    },
    Dct4x8 {
        params: DctQuantWeightParams,
        xyb_mul: [f32; 3],
    },
    Afv {
        params4x8: DctQuantWeightParams,
        params4x4: DctQuantWeightParams,
        weights: [[f32; 9]; 3],
    },
    Dct {
        params: DctQuantWeightParams,
    },
    Raw {
        qtable: Vec<i32>,
        qtable_den: f32,
    },
}

impl QuantEncoding {
    #[allow(dead_code)]
    pub fn raw_from_qtable(qtable: Vec<i32>, shift: i32) -> Self {
        Self::Raw {
            qtable,
            qtable_den: (1 << shift) as f32 * (1.0 / (8.0 * 255.0)),
        }
    }

    pub fn decode(
        mut required_size_x: usize,
        mut required_size_y: usize,
        index: usize,
        header: &FrameHeader,
        lf_global: &LfGlobalState,
        br: &mut BitReader,
    ) -> Result<Self> {
        let required_size = required_size_x * required_size_y;
        required_size_x *= BLOCK_DIM;
        required_size_y *= BLOCK_DIM;
        let mode = br.read(LOG2_NUM_QUANT_MODES)? as u8;
        match mode {
            0 => Ok(Self::Library),
            1 => {
                if required_size != 1 {
                    return Err(InvalidQuantEncoding {
                        mode,
                        required_size,
                    });
                }
                let mut xyb_weights = [[0.0; 3]; 3];
                for row in xyb_weights.iter_mut() {
                    for item in row.iter_mut() {
                        *item = f32::from(f16::from_bits(br.read(16)? as u16));
                        if item.abs() < ALMOST_ZERO {
                            return Err(HfQuantFactorTooSmall(*item));
                        }
                        *item *= 64.0;
                    }
                }
                Ok(Self::Identity { xyb_weights })
            }
            2 => {
                if required_size != 1 {
                    return Err(InvalidQuantEncoding {
                        mode,
                        required_size,
                    });
                }
                let mut xyb_weights = [[0.0; 6]; 3];
                for row in xyb_weights.iter_mut() {
                    for item in row.iter_mut() {
                        *item = f32::from(f16::from_bits(br.read(16)? as u16));
                        if item.abs() < ALMOST_ZERO {
                            return Err(HfQuantFactorTooSmall(*item));
                        }
                        *item *= 64.0;
                    }
                }
                Ok(Self::Dct2 { xyb_weights })
            }
            3 => {
                if required_size != 1 {
                    return Err(InvalidQuantEncoding {
                        mode,
                        required_size,
                    });
                }
                let mut xyb_mul = [[0.0; 2]; 3];
                for row in xyb_mul.iter_mut() {
                    for item in row.iter_mut() {
                        *item = f32::from(f16::from_bits(br.read(16)? as u16));
                        if item.abs() < ALMOST_ZERO {
                            return Err(HfQuantFactorTooSmall(*item));
                        }
                    }
                }
                let params = DctQuantWeightParams::decode(br)?;
                Ok(Self::Dct4 { params, xyb_mul })
            }
            4 => {
                if required_size != 1 {
                    return Err(InvalidQuantEncoding {
                        mode,
                        required_size,
                    });
                }
                let mut xyb_mul = [0.0; 3];
                for item in xyb_mul.iter_mut() {
                    *item = f32::from(f16::from_bits(br.read(16)? as u16));
                    if item.abs() < ALMOST_ZERO {
                        return Err(HfQuantFactorTooSmall(*item));
                    }
                }
                let params = DctQuantWeightParams::decode(br)?;
                Ok(Self::Dct4x8 { params, xyb_mul })
            }
            5 => {
                if required_size != 1 {
                    return Err(InvalidQuantEncoding {
                        mode,
                        required_size,
                    });
                }
                let mut weights = [[0.0; 9]; 3];
                for row in weights.iter_mut() {
                    for item in row.iter_mut() {
                        *item = f32::from(f16::from_bits(br.read(16)? as u16));
                    }
                    for item in row[0..6].iter_mut() {
                        *item *= 64.0;
                    }
                }
                let params4x8 = DctQuantWeightParams::decode(br)?;
                let params4x4 = DctQuantWeightParams::decode(br)?;
                Ok(Self::Afv {
                    params4x8,
                    params4x4,
                    weights,
                })
            }
            6 => {
                let params = DctQuantWeightParams::decode(br)?;
                Ok(Self::Dct { params })
            }
            7 => {
                let qtable_den = f32::from(f16::from_bits(br.read(16)? as u16));
                if qtable_den < ALMOST_ZERO {
                    return Err(InvalidRawQuantTable);
                }
                let bit_depth = BitDepth::integer_samples(8);
                let mut image = [
                    ModularChannel::new((required_size_x, required_size_y), bit_depth)?,
                    ModularChannel::new((required_size_x, required_size_y), bit_depth)?,
                    ModularChannel::new((required_size_x, required_size_y), bit_depth)?,
                ];
                let stream_id = ModularStreamId::QuantTable(index).get_id(header);
                decode_modular_subbitstream(
                    image.iter_mut().collect(),
                    stream_id,
                    None,
                    &lf_global.tree,
                    br,
                    None,
                )?;
                let mut qtable = Vec::with_capacity(required_size_x * required_size_y * 3);
                for channel in image.iter_mut() {
                    for entry in channel
                        .data
                        .get_rect(Rect {
                            size: (required_size_x, required_size_y),
                            origin: (0, 0),
                        })
                        .iter()
                    {
                        qtable.push(entry);
                        if entry <= 0 {
                            return Err(InvalidRawQuantTable);
                        }
                    }
                }
                Ok(Self::Raw { qtable, qtable_den })
            }
            _ => Err(InvalidQuantEncoding {
                mode,
                required_size,
            }),
        }
    }
}

#[derive(Clone, Copy, Debug)]
enum QuantTable {
    Dct,
    Identity,
    Dct2x2,
    Dct4x4,
    Dct16x16,
    Dct32x32,
    Dct8x16,
    Dct8x32,
    Dct16x32,
    Dct4x8,
    Afv0,
    Dct64x64,
    Dct32x64,
    Dct128x128,
    Dct64x128,
    Dct256x256,
    Dct128x256,
}

impl QuantTable {
    pub const CARDINALITY: usize = Self::VALUES.len();
    pub const VALUES: [QuantTable; 17] = [
        QuantTable::Dct,
        QuantTable::Identity,
        QuantTable::Dct2x2,
        QuantTable::Dct4x4,
        QuantTable::Dct16x16,
        QuantTable::Dct32x32,
        QuantTable::Dct8x16,
        QuantTable::Dct8x32,
        QuantTable::Dct16x32,
        QuantTable::Dct4x8,
        QuantTable::Afv0,
        QuantTable::Dct64x64,
        QuantTable::Dct32x64,
        QuantTable::Dct128x128,
        QuantTable::Dct64x128,
        QuantTable::Dct256x256,
        QuantTable::Dct128x256,
    ];
    fn for_strategy(strategy: HfTransformType) -> QuantTable {
        match strategy {
            HfTransformType::DCT => QuantTable::Dct,
            HfTransformType::IDENTITY => QuantTable::Identity,
            HfTransformType::DCT2X2 => QuantTable::Dct2x2,
            HfTransformType::DCT4X4 => QuantTable::Dct4x4,
            HfTransformType::DCT16X16 => QuantTable::Dct16x16,
            HfTransformType::DCT32X32 => QuantTable::Dct32x32,
            HfTransformType::DCT16X8 | HfTransformType::DCT8X16 => QuantTable::Dct8x16,
            HfTransformType::DCT32X8 | HfTransformType::DCT8X32 => QuantTable::Dct8x32,
            HfTransformType::DCT32X16 | HfTransformType::DCT16X32 => QuantTable::Dct16x32,
            HfTransformType::DCT4X8 | HfTransformType::DCT8X4 => QuantTable::Dct4x8,
            HfTransformType::AFV0
            | HfTransformType::AFV1
            | HfTransformType::AFV2
            | HfTransformType::AFV3 => QuantTable::Afv0,
            HfTransformType::DCT64X64 => QuantTable::Dct64x64,
            HfTransformType::DCT64X32 | HfTransformType::DCT32X64 => QuantTable::Dct32x64,
            HfTransformType::DCT128X128 => QuantTable::Dct128x128,
            HfTransformType::DCT128X64 | HfTransformType::DCT64X128 => QuantTable::Dct64x128,
            HfTransformType::DCT256X256 => QuantTable::Dct256x256,
            HfTransformType::DCT256X128 | HfTransformType::DCT128X256 => QuantTable::Dct128x256,
        }
    }
}

pub struct DequantMatrices {
    /// 17 separate tables, one per QuantTable type.
    /// Uses Cow to allow zero-copy borrowing from static cache for library tables.
    tables: [Cow<'static, [f32]>; QuantTable::CARDINALITY],
}

/// Cached computed library tables per QuantTable type.
/// Each entry contains the computed f32 weights for all 3 channels.
/// Computed lazily on first access.
static LIBRARY_TABLES: [OnceLock<Box<[f32]>>; QuantTable::CARDINALITY] = [
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
    OnceLock::new(),
];

#[allow(clippy::excessive_precision)]
impl DequantMatrices {
    fn dct() -> QuantEncoding {
        QuantEncoding::Dct {
            params: DctQuantWeightParams::from_array(&[
                [3150.0, 0.0, -0.4, -0.4, -0.4, -2.0],
                [560.0, 0.0, -0.3, -0.3, -0.3, -0.3],
                [512.0, -2.0, -1.0, 0.0, -1.0, -2.0],
            ]),
        }
    }
    fn id() -> QuantEncoding {
        QuantEncoding::Identity {
            xyb_weights: [
                [280.0, 3160.0, 3160.0],
                [60.0, 864.0, 864.0],
                [18.0, 200.0, 200.0],
            ],
        }
    }
    fn dct2x2() -> QuantEncoding {
        QuantEncoding::Dct2 {
            xyb_weights: [
                [3840.0, 2560.0, 1280.0, 640.0, 480.0, 300.0],
                [960.0, 640.0, 320.0, 180.0, 140.0, 120.0],
                [640.0, 320.0, 128.0, 64.0, 32.0, 16.0],
            ],
        }
    }
    fn dct4x4() -> QuantEncoding {
        QuantEncoding::Dct4 {
            params: DctQuantWeightParams::from_array(&[
                [2200.0, 0.0, 0.0, 0.0],
                [392.0, 0.0, 0.0, 0.0],
                [112.0, -0.25, -0.25, -0.5],
            ]),
            xyb_mul: [[1.0, 1.0], [1.0, 1.0], [1.0, 1.0]],
        }
    }
    fn dct16x16() -> QuantEncoding {
        QuantEncoding::Dct {
            params: DctQuantWeightParams::from_array(&[
                [
                    8996.8725711814115328,
                    -1.3000777393353804,
                    -0.49424529824571225,
                    -0.439093774457103443,
                    -0.6350101832695744,
                    -0.90177264050827612,
                    -1.6162099239887414,
                ],
                [
                    3191.48366296844234752,
                    -0.67424582104194355,
                    -0.80745813428471001,
                    -0.44925837484843441,
                    -0.35865440981033403,
                    -0.31322389111877305,
                    -0.37615025315725483,
                ],
                [
                    1157.50408145487200256,
                    -2.0531423165804414,
                    -1.4,
                    -0.50687130033378396,
                    -0.42708730624733904,
                    -1.4856834539296244,
                    -4.9209142884401604,
                ],
            ]),
        }
    }
    fn dct32x32() -> QuantEncoding {
        QuantEncoding::Dct {
            params: DctQuantWeightParams::from_array(&[
                [
                    15718.40830982518931456,
                    -1.025,
                    -0.98,
                    -0.9012,
                    -0.4,
                    -0.48819395464,
                    -0.421064,
                    -0.27,
                ],
                [
                    7305.7636810695983104,
                    -0.8041958212306401,
                    -0.7633036457487539,
                    -0.55660379990111464,
                    -0.49785304658857626,
                    -0.43699592683512467,
                    -0.40180866526242109,
                    -0.27321683125358037,
                ],
                [
                    3803.53173721215041536,
                    -3.060733579805728,
                    -2.0413270132490346,
                    -2.0235650159727417,
                    -0.5495389509954993,
                    -0.4,
                    -0.4,
                    -0.3,
                ],
            ]),
        }
    }

    fn dct8x16() -> QuantEncoding {
        QuantEncoding::Dct {
            params: DctQuantWeightParams::from_array(&[
                [7240.7734393502, -0.7, -0.7, -0.2, -0.2, -0.2, -0.5],
                [1448.15468787004, -0.5, -0.5, -0.5, -0.2, -0.2, -0.2],
                [506.854140754517, -1.4, -0.2, -0.5, -0.5, -1.5, -3.6],
            ]),
        }
    }

    fn dct8x32() -> QuantEncoding {
        QuantEncoding::Dct {
            params: DctQuantWeightParams::from_array(&[
                [
                    16283.2494710648897,
                    -1.7812845336559429,
                    -1.6309059012653515,
                    -1.0382179034313539,
                    -0.85,
                    -0.7,
                    -0.9,
                    -1.2360638576849587,
                ],
                [
                    5089.15750884921511936,
                    -0.320049391452786891,
                    -0.35362849922161446,
                    -0.30340000000000003,
                    -0.61,
                    -0.5,
                    -0.5,
                    -0.6,
                ],
                [
                    3397.77603275308720128,
                    -0.321327362693153371,
                    -0.34507619223117997,
                    -0.70340000000000003,
                    -0.9,
                    -1.0,
                    -1.0,
                    -1.1754605576265209,
                ],
            ]),
        }
    }

    fn dct16x32() -> QuantEncoding {
        QuantEncoding::Dct {
            params: DctQuantWeightParams::from_array(&[
                [
                    13844.97076442300573,
                    -0.97113799999999995,
                    -0.658,
                    -0.42026,
                    -0.22712,
                    -0.2206,
                    -0.226,
                    -0.6,
                ],
                [
                    4798.964084220744293,
                    -0.61125308982767057,
                    -0.83770786552491361,
                    -0.79014862079498627,
                    -0.2692727459704829,
                    -0.38272769465388551,
                    -0.22924222653091453,
                    -0.20719098826199578,
                ],
                [
                    1807.236946760964614,
                    -1.2,
                    -1.2,
                    -0.7,
                    -0.7,
                    -0.7,
                    -0.4,
                    -0.5,
                ],
            ]),
        }
    }

    fn dct4x8() -> QuantEncoding {
        QuantEncoding::Dct4x8 {
            params: DctQuantWeightParams::from_array(&[
                [
                    2198.050556016380522,
                    -0.96269623020744692,
                    -0.76194253026666783,
                    -0.6551140670773547,
                ],
                [
                    764.3655248643528689,
                    -0.92630200888366945,
                    -0.9675229603596517,
                    -0.27845290869168118,
                ],
                [
                    527.107573587542228,
                    -1.4594385811273854,
                    -1.450082094097871593,
                    -1.5843722511996204,
                ],
            ]),
            xyb_mul: [1.0, 1.0, 1.0],
        }
    }
    fn afv0() -> QuantEncoding {
        let QuantEncoding::Dct4x8 {
            params: params4x8, ..
        } = Self::dct4x8()
        else {
            unreachable!();
        };
        let QuantEncoding::Dct4 {
            params: params4x4, ..
        } = Self::dct4x4()
        else {
            unreachable!()
        };
        QuantEncoding::Afv {
            params4x8,
            params4x4,
            weights: [
                [
                    3072.0, 3072.0, 
                    256.0, 256.0, 256.0, 
                    414.0, 0.0, 0.0, 0.0, 
                ],
                [
                    1024.0, 1024.0, 
                    50.0, 50.0, 50.0, 
                    58.0, 0.0, 0.0, 0.0, 
                ],
                [
                    384.0, 384.0, 
                    12.0, 12.0, 12.0, 
                    22.0, -0.25, -0.25, -0.25, 
                ],
            ],
        }
    }

    fn dct64x64() -> QuantEncoding {
        QuantEncoding::Dct {
            params: DctQuantWeightParams::from_array(&[
                [
                    0.9 * 26629.073922049845,
                    -1.025,
                    -0.78,
                    -0.65012,
                    -0.19041574084286472,
                    -0.20819395464,
                    -0.421064,
                    -0.32733845535848671,
                ],
                [
                    0.9 * 9311.3238710010046,
                    -0.3041958212306401,
                    -0.3633036457487539,
                    -0.35660379990111464,
                    -0.3443074455424403,
                    -0.33699592683512467,
                    -0.30180866526242109,
                    -0.27321683125358037,
                ],
                [
                    0.9 * 4992.2486445538634,
                    -1.2,
                    -1.2,
                    -0.8,
                    -0.7,
                    -0.7,
                    -0.4,
                    -0.5,
                ],
            ]),
        }
    }

    fn dct32x64() -> QuantEncoding {
        QuantEncoding::Dct {
            params: DctQuantWeightParams::from_array(&[
                [
                    0.65 * 23629.073922049845,
                    -1.025,
                    -0.78,
                    -0.65012,
                    -0.19041574084286472,
                    -0.20819395464,
                    -0.421064,
                    -0.32733845535848671,
                ],
                [
                    0.65 * 8611.3238710010046,
                    -0.3041958212306401,
                    -0.3633036457487539,
                    -0.35660379990111464,
                    -0.3443074455424403,
                    -0.33699592683512467,
                    -0.30180866526242109,
                    -0.27321683125358037,
                ],
                [
                    0.65 * 4492.2486445538634,
                    -1.2,
                    -1.2,
                    -0.8,
                    -0.7,
                    -0.7,
                    -0.4,
                    -0.5,
                ],
            ]),
        }
    }
    fn dct128x128() -> QuantEncoding {
        QuantEncoding::Dct {
            params: DctQuantWeightParams::from_array(&[
                [
                    1.8 * 26629.073922049845,
                    -1.025,
                    -0.78,
                    -0.65012,
                    -0.19041574084286472,
                    -0.20819395464,
                    -0.421064,
                    -0.32733845535848671,
                ],
                [
                    1.8 * 9311.3238710010046,
                    -0.3041958212306401,
                    -0.3633036457487539,
                    -0.35660379990111464,
                    -0.3443074455424403,
                    -0.33699592683512467,
                    -0.30180866526242109,
                    -0.27321683125358037,
                ],
                [
                    1.8 * 4992.2486445538634,
                    -1.2,
                    -1.2,
                    -0.8,
                    -0.7,
                    -0.7,
                    -0.4,
                    -0.5,
                ],
            ]),
        }
    }

    fn dct64x128() -> QuantEncoding {
        QuantEncoding::Dct {
            params: DctQuantWeightParams::from_array(&[
                [
                    1.3 * 23629.073922049845,
                    -1.025,
                    -0.78,
                    -0.65012,
                    -0.19041574084286472,
                    -0.20819395464,
                    -0.421064,
                    -0.32733845535848671,
                ],
                [
                    1.3 * 8611.3238710010046,
                    -0.3041958212306401,
                    -0.3633036457487539,
                    -0.35660379990111464,
                    -0.3443074455424403,
                    -0.33699592683512467,
                    -0.30180866526242109,
                    -0.27321683125358037,
                ],
                [
                    1.3 * 4492.2486445538634,
                    -1.2,
                    -1.2,
                    -0.8,
                    -0.7,
                    -0.7,
                    -0.4,
                    -0.5,
                ],
            ]),
        }
    }
    fn dct256x256() -> QuantEncoding {
        QuantEncoding::Dct {
            params: DctQuantWeightParams::from_array(&[
                [
                    3.6 * 26629.073922049845,
                    -1.025,
                    -0.78,
                    -0.65012,
                    -0.19041574084286472,
                    -0.20819395464,
                    -0.421064,
                    -0.32733845535848671,
                ],
                [
                    3.6 * 9311.3238710010046,
                    -0.3041958212306401,
                    -0.3633036457487539,
                    -0.35660379990111464,
                    -0.3443074455424403,
                    -0.33699592683512467,
                    -0.30180866526242109,
                    -0.27321683125358037,
                ],
                [
                    3.6 * 4992.2486445538634,
                    -1.2,
                    -1.2,
                    -0.8,
                    -0.7,
                    -0.7,
                    -0.4,
                    -0.5,
                ],
            ]),
        }
    }

    fn dct128x256() -> QuantEncoding {
        QuantEncoding::Dct {
            params: DctQuantWeightParams::from_array(&[
                [
                    2.6 * 23629.073922049845,
                    -1.025,
                    -0.78,
                    -0.65012,
                    -0.19041574084286472,
                    -0.20819395464,
                    -0.421064,
                    -0.32733845535848671,
                ],
                [
                    2.6 * 8611.3238710010046,
                    -0.3041958212306401,
                    -0.3633036457487539,
                    -0.35660379990111464,
                    -0.3443074455424403,
                    -0.33699592683512467,
                    -0.30180866526242109,
                    -0.27321683125358037,
                ],
                [
                    2.6 * 4492.2486445538634,
                    -1.2,
                    -1.2,
                    -0.8,
                    -0.7,
                    -0.7,
                    -0.4,
                    -0.5,
                ],
            ]),
        }
    }

    /// Get library quantization encoding for a table type index.
    fn get_library_encoding(idx: usize) -> QuantEncoding {
        match idx {
            0 => Self::dct(),
            1 => Self::id(),
            2 => Self::dct2x2(),
            3 => Self::dct4x4(),
            4 => Self::dct16x16(),
            5 => Self::dct32x32(),
            6 => Self::dct8x16(),
            7 => Self::dct8x32(),
            8 => Self::dct16x32(),
            9 => Self::dct4x8(),
            10 => Self::afv0(),
            11 => Self::dct64x64(),
            12 => Self::dct32x64(),
            13 => Self::dct128x128(),
            14 => Self::dct64x128(),
            15 => Self::dct256x256(),
            16 => Self::dct128x256(),
            _ => unreachable!(),
        }
    }

    /// Get cached computed library table for a QuantTable type index.
    /// Computes the table lazily on first access.
    fn get_library_table(idx: usize) -> &'static [f32] {
        LIBRARY_TABLES[idx].get_or_init(|| {
            let encoding = Self::get_library_encoding(idx);
            Self::compute_table(&encoding, idx).expect("library table computation should not fail")
        })
    }

    /// Compute a single quant table from an encoding.
    /// Returns the computed weights as a boxed slice for all 3 channels.
    fn compute_table(encoding: &QuantEncoding, table_idx: usize) -> Result<Box<[f32]>> {
        let wrows = 8 * Self::REQUIRED_SIZE_X[table_idx];
        let wcols = 8 * Self::REQUIRED_SIZE_Y[table_idx];
        let num = wrows * wcols;
        let mut weights = vec![0f32; 3 * num];
        match encoding {
            QuantEncoding::Library => {
                return Err(InvalidQuantEncodingMode);
            }
            QuantEncoding::Identity { xyb_weights } => {
                for c in 0..3 {
                    for i in 0..64 {
                        weights[64 * c + i] = xyb_weights[c][0];
                    }
                    weights[64 * c + 1] = xyb_weights[c][1];
                    weights[64 * c + 8] = xyb_weights[c][1];
                    weights[64 * c + 9] = xyb_weights[c][2];
                }
            }
            QuantEncoding::Dct2 { xyb_weights } => {
                for (c, xyb_weight) in xyb_weights.iter().enumerate() {
                    let start = c * 64;
                    weights[start] = 0xBAD as f32;
                    weights[start + 1] = xyb_weight[0];
                    weights[start + 8] = xyb_weight[0];
                    weights[start + 9] = xyb_weight[1];
                    for y in 0..2 {
                        for x in 0..2 {
                            weights[start + y * 8 + x + 2] = xyb_weight[2];
                            weights[start + (y + 2) * 8 + x] = xyb_weight[2];
                        }
                    }
                    for y in 0..2 {
                        for x in 0..2 {
                            weights[start + (y + 2) * 8 + x + 2] = xyb_weight[3];
                        }
                    }
                    for y in 0..4 {
                        for x in 0..4 {
                            weights[start + y * 8 + x + 4] = xyb_weight[4];
                            weights[start + (y + 4) * 8 + x] = xyb_weight[4];
                        }
                    }
                    for y in 0..4 {
                        for x in 0..4 {
                            weights[start + (y + 4) * 8 + x + 4] = xyb_weight[5];
                        }
                    }
                }
            }
            QuantEncoding::Dct4 { params, xyb_mul } => {
                let mut weights4x4 = [0f32; 3 * 4 * 4];
                get_quant_weights(4, 4, params, &mut weights4x4)?;
                for c in 0..3 {
                    for y in 0..BLOCK_DIM {
                        for x in 0..BLOCK_DIM {
                            weights[c * num + y * BLOCK_DIM + x] =
                                weights4x4[c * 16 + (y / 2) * 4 + (x / 2)];
                        }
                    }
                    weights[c * num + 1] /= xyb_mul[c][0];
                    weights[c * num + BLOCK_DIM] /= xyb_mul[c][0];
                    weights[c * num + BLOCK_DIM + 1] /= xyb_mul[c][1];
                }
            }
            QuantEncoding::Dct4x8 { params, xyb_mul } => {
                let mut weights4x8 = [0f32; 3 * 4 * 8];
                get_quant_weights(4, 8, params, &mut weights4x8)?;
                for c in 0..3 {
                    for y in 0..BLOCK_DIM {
                        for x in 0..BLOCK_DIM {
                            weights[c * num + y * BLOCK_DIM + x] =
                                weights4x8[c * 32 + (y / 2) * 8 + x];
                        }
                    }
                    weights[c * num + BLOCK_DIM] /= xyb_mul[c];
                }
            }
            QuantEncoding::Dct { params } => {
                get_quant_weights(wrows, wcols, params, &mut weights)?;
            }
            QuantEncoding::Raw { qtable, qtable_den } => {
                if qtable.len() != 3 * num {
                    return Err(InvalidRawQuantTable);
                }
                for i in 0..3 * num {
                    weights[i] = 1f32 / (qtable_den * qtable[i] as f32);
                }
            }
            QuantEncoding::Afv {
                params4x8,
                params4x4,
                weights: afv_weights,
            } => {
                const FREQS: [f32; 16] = [
                    0xBAD as f32,
                    0xBAD as f32,
                    0.8517778890324296,
                    5.37778436506804,
                    0xBAD as f32,
                    0xBAD as f32,
                    4.734747904497923,
                    5.449245381693219,
                    1.6598270267479331,
                    4f32,
                    7.275749096817861,
                    10.423227632456525,
                    2.662932286148962,
                    7.630657783650829,
                    8.962388608184032,
                    12.97166202570235,
                ];
                let mut weights4x8 = [0f32; 3 * 4 * 8];
                get_quant_weights(4, 8, params4x8, &mut weights4x8)?;
                let mut weights4x4 = [0f32; 3 * 4 * 4];
                get_quant_weights(4, 4, params4x4, &mut weights4x4)?;
                const LO: f32 = 0.8517778890324296;
                const HI: f32 = 12.97166202570235f32 - LO + 1e-6f32;
                for c in 0..3 {
                    let mut bands = [0f32; 4];
                    bands[0] = afv_weights[c][5];
                    if bands[0] < ALMOST_ZERO {
                        return Err(InvalidDistanceBand(0, bands[0]));
                    }
                    for i in 1..4 {
                        bands[i] = bands[i - 1] * mult(afv_weights[c][i + 5]);
                        if bands[i] < ALMOST_ZERO {
                            return Err(InvalidDistanceBand(i, bands[i]));
                        }
                    }

                    {
                        let start = c * 64;
                        weights[start] = 1f32;
                        let mut set = |x, y, val| {
                            weights[start + y * 8 + x] = val;
                        };
                        set(0, 1, afv_weights[c][0]);
                        set(1, 0, afv_weights[c][1]);
                        set(0, 2, afv_weights[c][2]);
                        set(2, 0, afv_weights[c][3]);
                        set(2, 2, afv_weights[c][4]);

                        for y in 0..4 {
                            for x in 0..4 {
                                if x < 2 && y < 2 {
                                    continue;
                                }
                                let val = interpolate(FREQS[y * 4 + x] - LO, HI, &bands);
                                set(2 * x, 2 * y, val);
                            }
                        }
                    }

                    for y in 0..BLOCK_DIM / 2 {
                        for x in 0..BLOCK_DIM {
                            if x == 0 && y == 0 {
                                continue;
                            }
                            weights[c * num + (2 * y + 1) * BLOCK_DIM + x] =
                                weights4x8[c * 32 + y * 8 + x];
                        }
                    }

                    for y in 0..BLOCK_DIM / 2 {
                        for x in 0..BLOCK_DIM / 2 {
                            if x == 0 && y == 0 {
                                continue;
                            }
                            weights[c * num + (2 * y) * BLOCK_DIM + 2 * x + 1] =
                                weights4x4[c * 16 + y * 4 + x];
                        }
                    }
                }
            }
        }
        for weight in &mut weights {
            if !(ALMOST_ZERO..=1.0 / ALMOST_ZERO).contains(weight) {
                return Err(InvalidQuantizationTableWeight(*weight));
            }
            *weight = 1f32 / *weight;
        }
        Ok(weights.into_boxed_slice())
    }

    pub fn matrix(&self, quant_kind: HfTransformType, c: usize) -> &[f32] {
        let qt_idx = QuantTable::for_strategy(quant_kind) as usize;
        let table = &self.tables[qt_idx];
        let num = Self::REQUIRED_SIZE_X[qt_idx] * Self::REQUIRED_SIZE_Y[qt_idx] * BLOCK_SIZE;
        &table[c * num..]
    }

    pub fn decode(
        header: &FrameHeader,
        lf_global: &LfGlobalState,
        br: &mut BitReader,
    ) -> Result<Self> {
        let all_default = br.read(1)? == 1;

        let tables: [Cow<'static, [f32]>; QuantTable::CARDINALITY] = if all_default {
            std::array::from_fn(|idx| Cow::Borrowed(Self::get_library_table(idx)))
        } else {
            let mut tables_vec: Vec<Cow<'static, [f32]>> =
                Vec::with_capacity(QuantTable::CARDINALITY);
            for (i, (&required_size_x, required_size_y)) in Self::REQUIRED_SIZE_X
                .iter()
                .zip(Self::REQUIRED_SIZE_Y)
                .enumerate()
            {
                let encoding = QuantEncoding::decode(
                    required_size_x,
                    required_size_y,
                    i,
                    header,
                    lf_global,
                    br,
                )?;
                let table = match encoding {
                    QuantEncoding::Library => Cow::Borrowed(Self::get_library_table(i)),
                    _ => Cow::Owned(Self::compute_table(&encoding, i)?.into_vec()),
                };
                tables_vec.push(table);
            }
            tables_vec.try_into().unwrap()
        };

        Ok(Self { tables })
    }

    pub const REQUIRED_SIZE_X: [usize; QuantTable::CARDINALITY] =
        [1, 1, 1, 1, 2, 4, 1, 1, 2, 1, 1, 8, 4, 16, 8, 32, 16];

    pub const REQUIRED_SIZE_Y: [usize; QuantTable::CARDINALITY] =
        [1, 1, 1, 1, 2, 4, 2, 4, 4, 1, 1, 8, 8, 16, 16, 32, 32];

}

fn get_quant_weights(
    rows: usize,
    cols: usize,
    distance_bands: &DctQuantWeightParams,
    out: &mut [f32],
) -> Result<()> {
    for c in 0..3 {
        let mut bands = [0f32; DctQuantWeightParams::MAX_DISTANCE_BANDS];
        bands[0] = distance_bands.params[c][0];
        if bands[0] < ALMOST_ZERO {
            return Err(InvalidDistanceBand(0, bands[0]));
        }
        for i in 1..distance_bands.num_bands {
            bands[i] = bands[i - 1] * mult(distance_bands.params[c][i]);
            if bands[i] < ALMOST_ZERO {
                return Err(InvalidDistanceBand(i, bands[i]));
            }
        }
        let scale = (distance_bands.num_bands - 1) as f32 / (SQRT_2 + 1e-6);
        let rcpcol = scale / (cols - 1) as f32;
        let rcprow = scale / (rows - 1) as f32;
        for y in 0..rows {
            let dy = y as f32 * rcprow;
            let dy2 = dy * dy;
            for x in 0..cols {
                let dx = x as f32 * rcpcol;
                let scaled_distance = (dx * dx + dy2).sqrt();
                let weight = if distance_bands.num_bands == 1 {
                    bands[0]
                } else {
                    interpolate_vec(scaled_distance, &bands)
                };
                out[c * cols * rows + y * cols + x] = weight;
            }
        }
    }
    Ok(())
}

fn interpolate_vec(scaled_pos: f32, array: &[f32]) -> f32 {
    let idxf32 = scaled_pos.floor();
    let frac = scaled_pos - idxf32;
    let idx = idxf32 as usize;
    let a = array[idx];
    let b = array[1..][idx];
    (b / a).powf(frac) * a
}

fn interpolate(pos: f32, max: f32, array: &[f32]) -> f32 {
    let scaled_pos = pos * (array.len() - 1) as f32 / max;
    let idx = scaled_pos as usize;
    let a = array[idx];
    let b = array[idx + 1];
    a * (b / a).powf(scaled_pos - idx as f32)
}

fn mult(v: f32) -> f32 {
    if v > 0f32 {
        1f32 + v
    } else {
        1f32 / (1f32 - v)
    }
}
