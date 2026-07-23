// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use crate::render::RenderPipelineInPlaceStage;

/// Render spot color
pub struct SpotColorStage {
    /// Spot color channel index
    spot_c: usize,
    /// Spot color in linear RGBA
    spot_color: [f32; 4],
}

impl std::fmt::Display for SpotColorStage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "spot color stage for channel {}", self.spot_c)
    }
}

impl SpotColorStage {
    #[allow(unused, reason = "remove once we actually use this")]
    pub fn new(spot_c_offset: usize, spot_color: [f32; 4]) -> Self {
        Self {
            spot_c: 3 + spot_c_offset,
            spot_color,
        }
    }
}

impl RenderPipelineInPlaceStage for SpotColorStage {
    type Type = f32;

    fn uses_channel(&self, c: usize) -> bool {
        c < 3 || c == self.spot_c
    }

    fn process_row_chunk(
        &self,
        _position: (usize, usize),
        xsize: usize,
        row: &mut [&mut [f32]],
        _state: Option<&mut dyn std::any::Any>,
    ) {
        let [row_r, row_g, row_b, row_s] = row else {
            panic!(
                "incorrect number of channels; expected 4, found {}",
                row.len()
            );
        };

        let scale = self.spot_color[3];
        assert!(
            xsize <= row_r.len()
                && xsize <= row_g.len()
                && xsize <= row_b.len()
                && xsize <= row_s.len()
        );
        for idx in 0..xsize {
            let mix = scale * row_s[idx];
            row_r[idx] = mix * self.spot_color[0] + (1.0 - mix) * row_r[idx];
            row_g[idx] = mix * self.spot_color[1] + (1.0 - mix) * row_g[idx];
            row_b[idx] = mix * self.spot_color[2] + (1.0 - mix) * row_b[idx];
        }
    }
}
