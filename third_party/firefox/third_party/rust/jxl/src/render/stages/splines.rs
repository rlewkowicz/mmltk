// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use std::{any::Any, sync::Arc};

use crate::{
    features::spline::Splines, frame::color_correlation_map::ColorCorrelationParams,
    render::RenderPipelineInPlaceStage, util::AtomicRefCell,
};

pub struct SplinesStage {
    splines: Arc<AtomicRefCell<Splines>>,
    image_size: (usize, usize),
    color_correlation_params: Arc<AtomicRefCell<ColorCorrelationParams>>,
    high_precision: bool,
}

impl SplinesStage {
    pub fn new(
        splines: Arc<AtomicRefCell<Splines>>,
        image_size: (usize, usize),
        color_correlation_params: Arc<AtomicRefCell<ColorCorrelationParams>>,
        high_precision: bool,
    ) -> Self {
        SplinesStage {
            splines,
            image_size,
            color_correlation_params,
            high_precision,
        }
    }
}

impl std::fmt::Display for SplinesStage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "splines")
    }
}

impl RenderPipelineInPlaceStage for SplinesStage {
    type Type = f32;

    fn uses_channel(&self, c: usize) -> bool {
        c < 3
    }

    fn process_row_chunk(
        &self,
        position: (usize, usize),
        xsize: usize,
        row: &mut [&mut [f32]],
        _state: Option<&mut dyn Any>,
    ) {
        let mut splines = self.splines.borrow_mut();
        if splines.splines.is_empty() {
            return;
        }
        if !splines.is_initialized() {
            let color_correlation_params = self.color_correlation_params.borrow();
            splines
                .initialize_draw_cache(
                    self.image_size.0 as u64,
                    self.image_size.1 as u64,
                    &color_correlation_params,
                    self.high_precision,
                )
                .unwrap();
        }
        splines.draw_segments(row, position, xsize);
    }
}
