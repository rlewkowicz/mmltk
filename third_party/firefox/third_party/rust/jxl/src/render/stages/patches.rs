// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use std::{any::Any, sync::Arc};

use crate::{
    features::patches::PatchesDictionary,
    frame::ReferenceFrame,
    headers::extra_channels::ExtraChannelInfo,
    render::RenderPipelineInPlaceStage,
    util::{AtomicRefCell, NewWithCapacity as _},
};

pub struct PatchesStage {
    patches: Arc<AtomicRefCell<PatchesDictionary>>,
    extra_channels: Vec<ExtraChannelInfo>,
    decoder_state: Arc<[Option<ReferenceFrame>; 4]>,
}

impl PatchesStage {
    pub fn new(
        patches: Arc<AtomicRefCell<PatchesDictionary>>,
        extra_channels: Vec<ExtraChannelInfo>,
        decoder_state: Arc<[Option<ReferenceFrame>; 4]>,
    ) -> Self {
        Self {
            patches,
            extra_channels,
            decoder_state,
        }
    }
}

impl std::fmt::Display for PatchesStage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "patches")
    }
}

impl RenderPipelineInPlaceStage for PatchesStage {
    type Type = f32;

    fn uses_channel(&self, c: usize) -> bool {
        c < 3 + self.extra_channels.len()
    }

    fn process_row_chunk(
        &self,
        position: (usize, usize),
        xsize: usize,
        row: &mut [&mut [f32]],
        state: Option<&mut dyn Any>,
    ) {
        let patches = self.patches.borrow();
        if patches.positions.is_empty() {
            return;
        }
        let state: &mut Vec<usize> = state.unwrap().downcast_mut().unwrap();
        if state.capacity() < patches.positions.len() {
            state.reserve(patches.positions.len() - state.len());
        }
        patches.add_one_row(
            row,
            position,
            xsize,
            &self.extra_channels,
            &self.decoder_state[..],
            state,
        );
    }

    fn init_local_state(&self, _thread_index: usize) -> crate::error::Result<Option<Box<dyn Any>>> {
        let patches = self.patches.borrow();
        let len = patches.positions.len();
        let patches_for_row_result = Vec::<usize>::new_with_capacity(len)?;
        Ok(Some(Box::new(patches_for_row_result) as Box<dyn Any>))
    }
}
