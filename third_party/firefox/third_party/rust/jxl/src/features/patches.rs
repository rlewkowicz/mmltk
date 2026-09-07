// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use num_derive::FromPrimitive;
use num_traits::FromPrimitive;

use crate::{
    bit_reader::BitReader,
    entropy_coding::decode::Histograms,
    entropy_coding::decode::SymbolReader,
    error::{Error, Result},
    features::blending::perform_blending,
    frame::{DecoderState, ReferenceFrame},
    headers::extra_channels::ExtraChannelInfo,
    util::{NewWithCapacity, slice, tracing_wrappers::*},
};

#[derive(Debug, PartialEq, Eq, Clone, Copy)]
#[repr(usize)]
pub enum PatchContext {
    NumRefPatch = 0,
    ReferenceFrame = 1,
    PatchSize = 2,
    PatchReferencePosition = 3,
    PatchPosition = 4,
    PatchBlendMode = 5,
    PatchOffset = 6,
    PatchCount = 7,
    PatchAlphaChannel = 8,
    PatchClamp = 9,
}

impl PatchContext {
    const NUM: usize = 10;
}

/// Blend modes
#[derive(Debug, PartialEq, Eq, Clone, Copy, FromPrimitive)]
#[repr(u8)]
pub enum PatchBlendMode {
    None = 0,
    Replace = 1,
    Add = 2,
    Mul = 3,
    BlendAbove = 4,
    BlendBelow = 5,
    AlphaWeightedAddAbove = 6,
    AlphaWeightedAddBelow = 7,
}

impl PatchBlendMode {
    pub const NUM_BLEND_MODES: u8 = 8;



    pub fn uses_alpha(self) -> bool {
        matches!(
            self,
            Self::BlendAbove
                | Self::BlendBelow
                | Self::AlphaWeightedAddAbove
                | Self::AlphaWeightedAddBelow
        )
    }

    pub fn uses_clamp(self) -> bool {
        self.uses_alpha() || self == Self::Mul
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PatchBlending {
    pub mode: PatchBlendMode,
    pub alpha_channel: usize,
    pub clamp: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PatchReferencePosition {
    reference: usize,
    x0: usize,
    y0: usize,
    xsize: usize,
    ysize: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PatchPosition {
    x: usize,
    y: usize,
    ref_pos_idx: usize,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct PatchTreeNode {
    left_child: isize,
    right_child: isize,
    y_center: usize,
    start: usize,
    num: usize,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PatchesDictionary {
    pub positions: Vec<PatchPosition>,
    pub ref_positions: Vec<PatchReferencePosition>,
    blendings: Vec<PatchBlending>,
    blendings_stride: usize,
    patch_tree: Vec<PatchTreeNode>,
    num_patches: Vec<usize>,
    sorted_patches_y0: Vec<(usize, usize)>,
    sorted_patches_y1: Vec<(usize, usize)>,
}

impl PatchesDictionary {
    pub fn new(num_extra_channels: usize) -> Self {
        Self {
            blendings_stride: num_extra_channels + 1,
            ..Default::default()
        }
    }


    fn compute_patch_tree(&mut self) -> Result<()> {
        #[derive(Debug, Clone, Copy)]
        struct PatchInterval {
            idx: usize,
            y0: usize,
            y1: usize,
        }

        self.patch_tree.clear();
        self.num_patches.clear();
        self.sorted_patches_y0.clear();
        self.sorted_patches_y1.clear();

        if self.positions.is_empty() {
            return Ok(());
        }

        let mut intervals: Vec<PatchInterval> = Vec::new_with_capacity(self.positions.len())?;
        for (i, pos) in self.positions.iter().enumerate() {
            let ref_pos = self.ref_positions[pos.ref_pos_idx];
            if ref_pos.xsize > 0 && ref_pos.ysize > 0 {
                intervals.push(PatchInterval {
                    idx: i,
                    y0: pos.y,
                    y1: pos.y + self.ref_positions[pos.ref_pos_idx].ysize,
                });
            }
        }

        let intervals_len = intervals.len();
        let sort_by_y0 = |intervals: &mut Vec<PatchInterval>, start: usize, end: usize| {
            intervals[start..end].sort_unstable_by_key(|i| i.y0);
        };
        let sort_by_y1 = |intervals: &mut Vec<PatchInterval>, start: usize, end: usize| {
            intervals[start..end].sort_unstable_by_key(|i| i.y1);
        };

        sort_by_y1(&mut intervals, 0, intervals_len);
        self.num_patches
            .resize(intervals.last().map_or(0, |iv| iv.y1), 0); 
        for iv in &intervals {
            for y in iv.y0..iv.y1 {
                self.num_patches[y] += 1;
            }
        }

        let root = PatchTreeNode {
            start: 0,
            num: intervals.len(),
            ..Default::default()
        };
        self.patch_tree.push(root);

        let mut next = 0;
        while next < self.patch_tree.len() {
            let node = &mut self.patch_tree[next]; 
            let start = node.start;
            let end = node.start + node.num;

            sort_by_y0(&mut intervals, start, end);
            let middle_idx = start + node.num / 2;
            node.y_center = intervals[middle_idx].y0;

            let mut right_start = middle_idx;
            while right_start < end && intervals[right_start].y0 == node.y_center {
                right_start += 1;
            }

            sort_by_y1(&mut intervals, start, right_start);
            let mut left_end = right_start;
            while left_end > start && intervals[left_end - 1].y1 > node.y_center {
                left_end -= 1;
            }

            node.num = right_start - left_end;
            node.start = self.sorted_patches_y0.len();

            self.sorted_patches_y1
                .try_reserve(right_start.saturating_sub(left_end))?;
            self.sorted_patches_y0
                .try_reserve(right_start.saturating_sub(left_end))?;
            for i in (left_end..right_start).rev() {
                self.sorted_patches_y1
                    .push((intervals[i].y1, intervals[i].idx));
            }
            sort_by_y0(&mut intervals, left_end, right_start);
            for interval in intervals.iter().take(right_start).skip(left_end) {
                self.sorted_patches_y0.push((interval.y0, interval.idx));
            }

            self.patch_tree[next].left_child = -1;
            self.patch_tree[next].right_child = -1;

            if left_end > start {
                let mut left = PatchTreeNode::default();
                left.start = start;
                left.num = left_end - left.start;
                self.patch_tree[next].left_child = self.patch_tree.len() as isize;
                self.patch_tree.try_reserve(1)?;
                self.patch_tree.push(left);
            }
            if right_start < end {
                let mut right = PatchTreeNode::default();
                right.start = right_start;
                right.num = end - right.start;
                self.patch_tree[next].right_child = self.patch_tree.len() as isize;
                self.patch_tree.try_reserve(1)?;
                self.patch_tree.push(right);
            }

            next += 1;
        }
        Ok(())
    }

    #[instrument(level = "debug", skip(br), ret, err)]
    pub fn read(
        br: &mut BitReader,
        xsize: usize,
        ysize: usize,
        num_extra_channels: usize,
        reference_frames: &[Option<ReferenceFrame>],
    ) -> Result<PatchesDictionary> {
        let blendings_stride = num_extra_channels + 1;
        let patches_histograms = Histograms::decode(PatchContext::NUM, br, true)?;
        let mut patches_reader = SymbolReader::new(&patches_histograms, br, None)?;
        let num_ref_patch = patches_reader.read_unsigned(
            &patches_histograms,
            br,
            PatchContext::NumRefPatch as usize,
        ) as usize;
        let num_pixels = xsize * ysize;
        let max_ref_patches = 1024 + num_pixels / 4;
        let max_patches = max_ref_patches * 4;
        let max_blending_infos = max_patches * 4;
        if num_ref_patch > max_ref_patches {
            return Err(Error::PatchesTooMany(
                "reference patches".to_string(),
                num_ref_patch,
                max_ref_patches,
            ));
        }
        let mut total_patches = 0;
        let mut next_size = 1;
        let mut positions: Vec<PatchPosition> = Vec::new();
        let mut blendings = Vec::new();
        let mut ref_positions = Vec::new_with_capacity(num_ref_patch)?;
        for _ in 0..num_ref_patch {
            let reference = patches_reader.read_unsigned(
                &patches_histograms,
                br,
                PatchContext::ReferenceFrame as usize,
            ) as usize;
            if reference >= DecoderState::MAX_STORED_FRAMES {
                return Err(Error::PatchesRefTooLarge(
                    reference,
                    DecoderState::MAX_STORED_FRAMES,
                ));
            }

            let x0 = patches_reader.read_unsigned(
                &patches_histograms,
                br,
                PatchContext::PatchReferencePosition as usize,
            ) as usize;
            let y0 = patches_reader.read_unsigned(
                &patches_histograms,
                br,
                PatchContext::PatchReferencePosition as usize,
            ) as usize;
            let ref_pos_xsize = patches_reader.read_unsigned(
                &patches_histograms,
                br,
                PatchContext::PatchSize as usize,
            ) as usize
                + 1;
            let ref_pos_ysize = patches_reader.read_unsigned(
                &patches_histograms,
                br,
                PatchContext::PatchSize as usize,
            ) as usize
                + 1;
            let reference_frame = &reference_frames[reference];
            match reference_frame {
                None => return Err(Error::PatchesInvalidReference(reference)),
                Some(reference) => {
                    if !reference.saved_before_color_transform {
                        return Err(Error::PatchesPostColorTransform());
                    }
                    if x0 + ref_pos_xsize > reference.frame[0].size().0 {
                        return Err(Error::PatchesInvalidPosition(
                            "x".to_string(),
                            x0,
                            ref_pos_xsize,
                            reference.frame[0].size().0,
                        ));
                    }
                    if y0 + ref_pos_ysize > reference.frame[0].size().1 {
                        return Err(Error::PatchesInvalidPosition(
                            "y".to_string(),
                            y0,
                            ref_pos_ysize,
                            reference.frame[0].size().1,
                        ));
                    }
                }
            }

            let id_count = patches_reader.read_unsigned(
                &patches_histograms,
                br,
                PatchContext::PatchCount as usize,
            ) as usize
                + 1;
            if id_count > max_patches + 1 {
                return Err(Error::PatchesTooMany(
                    "patches".to_string(),
                    id_count,
                    max_patches,
                ));
            }
            total_patches += id_count;

            if total_patches > max_patches {
                return Err(Error::PatchesTooMany(
                    "patches".to_string(),
                    total_patches,
                    max_patches,
                ));
            }

            if next_size < total_patches {
                next_size *= 2;
                next_size = std::cmp::min(next_size, max_patches);
            }
            if next_size * blendings_stride > max_blending_infos {
                return Err(Error::PatchesTooMany(
                    "blending_info".to_string(),
                    total_patches,
                    max_patches,
                ));
            }
            positions.try_reserve(next_size.saturating_sub(positions.len()))?;
            blendings.try_reserve(
                (next_size * PatchBlendMode::NUM_BLEND_MODES as usize)
                    .saturating_sub(blendings.len()),
            )?;

            for i in 0..id_count {
                let mut pos = PatchPosition {
                    x: 0,
                    y: 0,
                    ref_pos_idx: ref_positions.len(),
                };
                if i == 0 {
                    pos.x = patches_reader.read_unsigned(
                        &patches_histograms,
                        br,
                        PatchContext::PatchPosition as usize,
                    ) as usize;
                    pos.y = patches_reader.read_unsigned(
                        &patches_histograms,
                        br,
                        PatchContext::PatchPosition as usize,
                    ) as usize;
                } else {
                    let delta_x = patches_reader.read_signed(
                        &patches_histograms,
                        br,
                        PatchContext::PatchOffset as usize,
                    );
                    if delta_x < 0 && (-delta_x as usize) > positions.last().unwrap().x {
                        return Err(Error::PatchesInvalidDelta(
                            "x".to_string(),
                            positions.last().unwrap().x,
                            delta_x,
                        ));
                    }
                    pos.x = (positions.last().unwrap().x as i32 + delta_x) as usize;

                    let delta_y = patches_reader.read_signed(
                        &patches_histograms,
                        br,
                        PatchContext::PatchOffset as usize,
                    );
                    if delta_y < 0 && (-delta_y as usize) > positions.last().unwrap().y {
                        return Err(Error::PatchesInvalidDelta(
                            "y".to_string(),
                            positions.last().unwrap().y,
                            delta_y,
                        ));
                    }
                    pos.y = (positions.last().unwrap().y as i32 + delta_y) as usize;
                }

                if pos.x + ref_pos_xsize > xsize {
                    return Err(Error::PatchesOutOfBounds(
                        "x".to_string(),
                        pos.x,
                        ref_pos_xsize,
                        xsize,
                    ));
                }
                if pos.y + ref_pos_ysize > ysize {
                    return Err(Error::PatchesOutOfBounds(
                        "y".to_string(),
                        pos.y,
                        ref_pos_ysize,
                        ysize,
                    ));
                }

                for _ in 0..blendings_stride {
                    let mut alpha_channel = 0;
                    let mut clamp = false;
                    let maybe_blend_mode = patches_reader.read_unsigned(
                        &patches_histograms,
                        br,
                        PatchContext::PatchBlendMode as usize,
                    ) as u8;
                    let blend_mode = match PatchBlendMode::from_u8(maybe_blend_mode) {
                        None => {
                            return Err(Error::PatchesInvalidBlendMode(
                                maybe_blend_mode,
                                PatchBlendMode::NUM_BLEND_MODES,
                            ));
                        }
                        Some(blend_mode) => blend_mode,
                    };

                    if PatchBlendMode::uses_alpha(blend_mode) && blendings_stride > 2 {
                        alpha_channel = patches_reader.read_unsigned(
                            &patches_histograms,
                            br,
                            PatchContext::PatchAlphaChannel as usize,
                        ) as usize;
                        if alpha_channel >= num_extra_channels {
                            return Err(Error::PatchesInvalidAlphaChannel(
                                alpha_channel,
                                num_extra_channels,
                            ));
                        }
                    }

                    if PatchBlendMode::uses_clamp(blend_mode) {
                        clamp = patches_reader.read_unsigned(
                            &patches_histograms,
                            br,
                            PatchContext::PatchClamp as usize,
                        ) != 0;
                    }
                    blendings.push(PatchBlending {
                        mode: blend_mode,
                        alpha_channel,
                        clamp,
                    });
                }
                positions.push(pos);
            }

            ref_positions.push(PatchReferencePosition {
                reference,
                x0,
                y0,
                xsize: ref_pos_xsize,
                ysize: ref_pos_ysize,
            })
        }

        let mut patches_dict = PatchesDictionary {
            positions,
            blendings,
            ref_positions,
            blendings_stride,
            num_patches: vec![],
            sorted_patches_y0: vec![],
            sorted_patches_y1: vec![],
            patch_tree: vec![],
        };
        patches_dict.compute_patch_tree()?;
        Ok(patches_dict)
    }

    #[inline(always)]
    pub fn set_patches_for_row(&self, y: usize, patches_for_row_result: &mut Vec<usize>) {
        patches_for_row_result.clear();
        if self.num_patches.len() <= y || self.num_patches[y] == 0 {
            return;
        }

        let mut tree_idx: isize = 0;
        loop {
            if tree_idx == -1 {
                break;
            }

            let node = self.patch_tree.get(tree_idx as usize).unwrap_or_else(|| {
                panic!("Invalid tree_idx: {tree_idx}");
            });

            if y <= node.y_center {
                for i in 0..node.num {
                    let p = self.sorted_patches_y0[node.start + i];
                    if y < p.0 {
                        break;
                    }
                    patches_for_row_result.push(p.1);
                }
                tree_idx = if y < node.y_center {
                    node.left_child
                } else {
                    -1
                };
            } else {
                for i in 0..node.num {
                    let p = self.sorted_patches_y1[node.start + i];
                    if y >= p.0 {
                        break;
                    }
                    patches_for_row_result.push(p.1);
                }
                tree_idx = node.right_child;
            }
        }

        patches_for_row_result.sort();
    }

    #[inline(always)]
    pub fn add_one_row(
        &self,
        row: &mut [&mut [f32]],
        row_pos: (usize, usize),
        xsize: usize,
        extra_channel_info: &[ExtraChannelInfo],
        reference_frames: &[Option<ReferenceFrame>],
        patches_for_row_result: &mut Vec<usize>,
    ) {
        let mut out = row
            .iter_mut()
            .map(|s| &mut s[..xsize])
            .collect::<Vec<&mut [f32]>>();
        let num_ec = extra_channel_info.len();
        assert!(num_ec + 1 == self.blendings_stride);
        let dummy_fg = vec![0f32];
        let mut fg = vec![dummy_fg.as_slice(); 3 + num_ec];
        self.set_patches_for_row(row_pos.1, &mut *patches_for_row_result);
        for pos_idx in patches_for_row_result.iter() {
            let pos = &self.positions[*pos_idx];
            assert!(row_pos.1 >= pos.y); 
            if pos.x >= row_pos.0 + out[0].len() {
                continue;
            }

            let ref_pos = &self.ref_positions[pos.ref_pos_idx];
            assert!(pos.y + ref_pos.ysize > row_pos.1); 
            if pos.x + ref_pos.xsize < row_pos.0 {
                continue;
            }

            let (ref_x0, out_x0, ref_xsize) = if pos.x < row_pos.0 {
                (
                    ref_pos.x0 + row_pos.0 - pos.x,
                    0,
                    ref_pos.xsize + pos.x - row_pos.0,
                )
            } else {
                (ref_pos.x0, pos.x - row_pos.0, ref_pos.xsize)
            };
            let (ref_x1, out_x1) = if out[0].len() - out_x0 < ref_xsize {
                (ref_x0 + out[0].len() - out_x0, out[0].len())
            } else {
                (ref_x0 + ref_xsize, out_x0 + ref_xsize)
            };
            let ref_pos_y = ref_pos.y0 + row_pos.1 - pos.y;

            for (c, fg_ptr) in fg.iter_mut().enumerate().take(3) {
                *fg_ptr = &(reference_frames[ref_pos.reference].as_ref().unwrap().frame[c]
                    .row(ref_pos_y)[ref_x0..ref_x1]);
            }
            for i in 0..num_ec {
                fg[3 + i] = &(reference_frames[ref_pos.reference].as_ref().unwrap().frame[3 + i]
                    .row(ref_pos_y)[ref_x0..ref_x1]);
            }

            let blending_idx = pos_idx * self.blendings_stride;
            perform_blending(
                &mut slice!(&mut out, .., out_x0..out_x1),
                &fg,
                &self.blendings[blending_idx],
                &self.blendings[blending_idx + 1..],
                extra_channel_info,
            );
        }
    }
}
