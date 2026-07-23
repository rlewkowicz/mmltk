// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use crate::render::{Channels, ChannelsMut, RenderPipelineInOutStage};
pub struct NearestNeighbourUpsample {
    channel: usize,
}

impl NearestNeighbourUpsample {
    pub fn new(channel: usize) -> NearestNeighbourUpsample {
        NearestNeighbourUpsample { channel }
    }
}

impl std::fmt::Display for NearestNeighbourUpsample {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "2x2 nearest neighbour upsample of channel {}",
            self.channel
        )
    }
}

impl RenderPipelineInOutStage for NearestNeighbourUpsample {
    type InputT = f32;
    type OutputT = f32;
    const SHIFT: (u8, u8) = (1, 1);
    const BORDER: (u8, u8) = (0, 0);

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
        for i in 0..xsize {
            output[0][i * 2] = input[0][i];
            output[0][i * 2 + 1] = input[0][i];
            output[1][i * 2] = input[0][i];
            output[1][i * 2 + 1] = input[0][i];
        }
    }
}
