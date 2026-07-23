// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use std::any::Any;

use crate::api::JxlCmsTransformer;
use crate::error::Result;
use crate::render::RenderPipelineInPlaceStage;

use crate::render::simd_utils::{
    deinterleave_2_dispatch, deinterleave_3_dispatch, deinterleave_4_dispatch,
    interleave_2_dispatch, interleave_3_dispatch, interleave_4_dispatch,
};
use crate::util::AtomicRefCell;

/// Thread-local state for CMS transform.
struct CmsLocalState {
    transformer_idx: usize,
    /// Buffer for interleaved input pixels (always used).
    input_buffer: Vec<f32>,
    /// Buffer for interleaved output pixels (only used when in_channels != out_channels).
    output_buffer: Vec<f32>,
}

/// Applies CMS color transform between color profiles.
///
/// The stage receives channels re-indexed from 0 in `process_row_chunk`:
/// - row[0], row[1], row[2] are color channels (always present)
/// - row[3] is the black (K) channel if `black_channel` is `Some`
///
/// Output is written to row[0..out_channels].
pub struct CmsStage {
    transformers: Vec<AtomicRefCell<Box<dyn JxlCmsTransformer + Send>>>,
    /// Number of input channels (3 for RGB, 4 for CMYK).
    in_channels: usize,
    /// Number of output channels (typically 3 for RGB output).
    out_channels: usize,
    /// Pipeline index of the black (K) channel, if present.
    /// Used by `uses_channel` to request the K channel from the pipeline.
    black_channel: Option<usize>,
    input_buffer_size: usize,
    output_buffer_size: usize,
}

impl CmsStage {
    /// Creates a new CMS stage.
    ///
    /// # Arguments
    /// * `transformers` - CMS transformer instances (one per thread recommended)
    /// * `in_channels` - Number of input channels (3 for RGB, 4 for CMYK)
    /// * `out_channels` - Number of output channels (must be <= in_channels)
    /// * `black_channel` - Pipeline index of K channel if present (for CMYK)
    /// * `max_pixels` - Maximum pixels per row chunk
    ///
    /// When input and output channel counts match, uses in-place transform.
    /// When they differ, uses separate input/output buffers.
    ///
    /// # Example
    /// ```ignore
    /// // RGB -> RGB (in-place)
    /// CmsStage::new(transformers, 3, 3, None, max_pixels);
    ///
    /// // CMYK -> RGB where K is at pipeline channel 5
    /// CmsStage::new(transformers, 4, 3, Some(5), max_pixels);
    /// ```
    pub fn new(
        transformers: Vec<Box<dyn JxlCmsTransformer + Send>>,
        in_channels: usize,
        out_channels: usize,
        black_channel: Option<usize>,
        max_pixels: usize,
    ) -> Self {
        assert!(
            (1..=4).contains(&in_channels),
            "CMS stage only supports 1-4 input channels, got {in_channels}"
        );
        assert!(
            (1..=4).contains(&out_channels),
            "CMS stage only supports 1-4 output channels, got {out_channels}"
        );
        assert!(
            out_channels <= in_channels,
            "out_channels ({out_channels}) must be <= in_channels ({in_channels})"
        );
        assert!(
            black_channel.is_some() == (in_channels == 4),
            "black_channel must be Some iff in_channels == 4"
        );
        let padded_pixels = max_pixels.next_multiple_of(16);
        Self {
            transformers: transformers.into_iter().map(AtomicRefCell::new).collect(),
            in_channels,
            out_channels,
            black_channel,
            input_buffer_size: padded_pixels
                .checked_mul(in_channels)
                .expect("CMS input buffer size overflow"),
            output_buffer_size: padded_pixels
                .checked_mul(out_channels)
                .expect("CMS output buffer size overflow"),
        }
    }
}

impl std::fmt::Display for CmsStage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if let Some(k) = self.black_channel {
            write!(
                f,
                "CMS transform: {} channels (K at {}) -> {} channels",
                self.in_channels, k, self.out_channels
            )
        } else {
            write!(
                f,
                "CMS transform: {} channels -> {} channels",
                self.in_channels, self.out_channels
            )
        }
    }
}

impl RenderPipelineInPlaceStage for CmsStage {
    type Type = f32;

    fn uses_channel(&self, c: usize) -> bool {
        c < self.in_channels.min(3) || self.black_channel == Some(c)
    }

    fn init_local_state(&self, thread_index: usize) -> Result<Option<Box<dyn Any>>> {
        if self.transformers.is_empty() {
            return Ok(None);
        }
        let idx = thread_index % self.transformers.len();

        let output_buffer = if self.in_channels != self.out_channels {
            vec![0.0f32; self.output_buffer_size]
        } else {
            Vec::new()
        };

        Ok(Some(Box::new(CmsLocalState {
            transformer_idx: idx,
            input_buffer: vec![0.0f32; self.input_buffer_size],
            output_buffer,
        })))
    }

    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        row: &mut [&mut [f32]],
        state: Option<&mut dyn Any>,
    ) {
        let Some(state) = state else {
            return;
        };
        let state: &mut CmsLocalState = state.downcast_mut().unwrap();
        let same_channels = self.in_channels == self.out_channels;

        debug_assert!(
            xsize * self.in_channels <= state.input_buffer.len(),
            "xsize {} exceeds buffer capacity",
            xsize
        );

        if self.in_channels == 1 && self.out_channels == 1 {
            let mut transformer = self.transformers[state.transformer_idx].borrow_mut();
            transformer
                .do_transform_inplace(&mut row[0][..xsize])
                .expect("CMS transform failed");
            return;
        }

        let xsize_padded = xsize.next_multiple_of(16);

        match self.in_channels {
            2 => {
                interleave_2_dispatch(
                    &row[0][..xsize_padded],
                    &row[1][..xsize_padded],
                    &mut state.input_buffer[..xsize_padded * 2],
                );
            }
            3 => {
                interleave_3_dispatch(
                    &row[0][..xsize_padded],
                    &row[1][..xsize_padded],
                    &row[2][..xsize_padded],
                    &mut state.input_buffer[..xsize_padded * 3],
                );
            }
            4 => {
                interleave_4_dispatch(
                    &row[0][..xsize_padded],
                    &row[1][..xsize_padded],
                    &row[2][..xsize_padded],
                    &row[3][..xsize_padded],
                    &mut state.input_buffer[..xsize_padded * 4],
                );
            }
            _ => unreachable!("CMS stage only supports 2-4 input channels here"),
        }

        let mut transformer = self.transformers[state.transformer_idx].borrow_mut();
        if same_channels {
            transformer
                .do_transform_inplace(&mut state.input_buffer[..xsize * self.in_channels])
                .expect("CMS transform failed");
        } else {
            transformer
                .do_transform(
                    &state.input_buffer[..xsize * self.in_channels],
                    &mut state.output_buffer[..xsize * self.out_channels],
                )
                .expect("CMS transform failed");
        }

        let output_buf = if same_channels {
            &state.input_buffer
        } else {
            &state.output_buffer
        };

        match self.out_channels {
            1 => {
                row[0][..xsize].copy_from_slice(&output_buf[..xsize]);
            }
            2 => {
                let (r0, r1) = row.split_at_mut(1);
                deinterleave_2_dispatch(
                    &output_buf[..xsize_padded * 2],
                    &mut r0[0][..xsize_padded],
                    &mut r1[0][..xsize_padded],
                );
            }
            3 => {
                let (r0, rest) = row.split_at_mut(1);
                let (r1, r2) = rest.split_at_mut(1);
                deinterleave_3_dispatch(
                    &output_buf[..xsize_padded * 3],
                    &mut r0[0][..xsize_padded],
                    &mut r1[0][..xsize_padded],
                    &mut r2[0][..xsize_padded],
                );
            }
            4 => {
                let (r0, rest) = row.split_at_mut(1);
                let (r1, rest) = rest.split_at_mut(1);
                let (r2, r3) = rest.split_at_mut(1);
                deinterleave_4_dispatch(
                    &output_buf[..xsize_padded * 4],
                    &mut r0[0][..xsize_padded],
                    &mut r1[0][..xsize_padded],
                    &mut r2[0][..xsize_padded],
                    &mut r3[0][..xsize_padded],
                );
            }
            _ => unreachable!("CMS stage only supports 1-4 output channels"),
        }
    }
}
