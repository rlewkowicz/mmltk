/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use api::{ColorF, NormalBorder, PremultipliedColorF, RasterSpace, RepeatMode, Shadow};
use api::units::*;
use smallvec::SmallVec;
use crate::border::{build_border_instances, NormalBorderSegment, MAX_BORDER_RESOLUTION};
use crate::border::NinePatchDescriptorExt;
use crate::clip::{ClipChainInstance, ClipIntern};
use crate::command_buffer::CommandBufferIndex;
use crate::gpu_types::ImageBrushPrimitiveData;
use crate::pattern::image::ImagePattern;
use crate::quad::{prepare_repeatable_quad, QuadTransformState};
use crate::render_backend::DataStores;
use crate::render_task_cache::{RenderTaskCacheKey, RenderTaskCacheKeyKind, RenderTaskParent, to_cache_size};
use crate::renderer::{GpuBufferAddress, GpuBufferWriterF};
use crate::scene_building::{CreateShadow, IsVisible};
use crate::frame_builder::{FrameBuildingContext, FrameBuildingState, PictureContext};
use crate::intern::{self, DataStore};
use crate::internal_types::LayoutPrimitiveInfo;
use crate::prim_store::{
    BrushSegment, InternablePrimitive, NinePatchDescriptor, PrimKey, PrimTemplate, PrimTemplateCommonData, PrimitiveInstanceIndex, PrimitiveKind, PrimitiveScratchBuffer, PrimitiveStore, VECS_PER_SEGMENT
};
use crate::resource_cache::ImageRequest;
use crate::render_task::{RenderTask, RenderTaskKind};
use crate::render_task_graph::RenderTaskId;
use crate::segment::EdgeMask;
use crate::spatial_tree::SpatialNodeIndex;
use crate::util::clamp_to_scale_factor;
use crate::visibility::KindScratchHandle;

use crate::prim_store::storage;

pub use api::interned_prims::NormalBorderPrim;

pub type NormalBorderKey = PrimKey<NormalBorderPrim>;

impl NormalBorderKey {
    pub fn new(
        info: &LayoutPrimitiveInfo,
        normal_border: NormalBorderPrim,
    ) -> Self {
        NormalBorderKey {
            common: info.into(),
            kind: normal_border,
        }
    }
}

impl intern::InternDebug for NormalBorderKey {}

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(MallocSizeOf)]
pub struct NormalBorderData {
    pub border: NormalBorder,
    pub widths: LayoutSideOffsets,
}

impl NormalBorderData {
    pub fn update(
        &self,
        local_rect: &LayoutRect,
        clip_chain: &ClipChainInstance,
        prim_spatial_node_index: SpatialNodeIndex,
        device_pixel_scale: DevicePixelScale,
        aligned_aa_edges: EdgeMask,
        transformed_aa_edges: EdgeMask,
        prim_instance_index: PrimitiveInstanceIndex,
        quad_transform: &mut QuadTransformState,
        frame_context: &FrameBuildingContext,
        pic_context: &PictureContext,
        targets: &[CommandBufferIndex],
        interned_clips: &DataStore<ClipIntern>,
        frame_state: &mut FrameBuildingState,
        scratch: &mut PrimitiveScratchBuffer,
    ) {
        let scale = frame_context
            .spatial_tree
            .get_world_transform(prim_spatial_node_index)
            .scale_factors();

        let snap_width = |w: f32, s: f32| {
            if w >= 1.0 && s > 0.0 { (w * s).round().max(1.0) / s } else { w }
        };
        let device_scale_x = scale.0 * device_pixel_scale.0;
        let device_scale_y = scale.1 * device_pixel_scale.0;
        let mut widths = self.widths;
        widths.left = snap_width(widths.left, device_scale_x);
        widths.right = snap_width(widths.right, device_scale_x);
        widths.top = snap_width(widths.top, device_scale_y);
        widths.bottom = snap_width(widths.bottom, device_scale_y);

        let scale_width = clamp_to_scale_factor(scale.0, false);
        let scale_height = clamp_to_scale_factor(scale.1, false);
        let world_scale = LayoutToWorldScale::new(scale_width.max(scale_height));
        let mut scale = world_scale * device_pixel_scale;

        let mut segments: SmallVec<[NormalBorderSegment; 8]> = SmallVec::new();
        crate::border::create_border_segments(
            *local_rect,
            &self.border,
            &widths,
            &mut |segment| segments.push(segment.clone()),
        );

        let mut max_dim = 1.0;
        for segment in &segments {
            max_dim = segment.task_size.width.max(segment.task_size.height.max(max_dim));
        }
        let max_scale = LayoutToDeviceScale::new(MAX_BORDER_RESOLUTION as f32 / max_dim);
        scale.0 = scale.0.min(max_scale.0);

        for segment in &segments {
                let cache_size = to_cache_size(segment.task_size, &mut scale);
                let cache_key = RenderTaskCacheKey {
                    kind: RenderTaskCacheKeyKind::BorderSegment(segment.cache_key.clone()),
                    origin: DeviceIntPoint::zero(),
                    size: cache_size,
                };

                let is_opaque = false;

                let task_id = frame_state.resource_cache.request_render_task(
                    Some(cache_key),
                    is_opaque,
                    RenderTaskParent::Surface,
                    &mut frame_state.frame_gpu_data.f32,
                    frame_state.rg_builder,
                    &mut frame_state.surface_builder,
                    &mut |rg_builder, gpu_buffer_builder| {
                        rg_builder.add().init(RenderTask::new_dynamic(
                            cache_size,
                            RenderTaskKind::new_border_segment(
                                build_border_instances(
                                    &segment.cache_key,
                                    cache_size,
                                    &self.border,
                                    scale,
                                    gpu_buffer_builder,
                                )
                            ),
                        ))
                    }
                );

                let pattern = ImagePattern {
                    src_task_id: task_id,
                    src_is_opaque: is_opaque,
                    premultiplied: true,
                    sampler_kind: api::ImageBufferKind::Texture2D,
                    color: ColorF::WHITE,
                };

                let segment_local_rect = segment.local_rect;
                let local_clip_rect = match segment.clip_rect {
                    Some(clip_rect) => clip_chain.local_clip_rect
                        .intersection(&clip_rect)
                        .unwrap_or(LayoutRect::zero()),
                    None => clip_chain.local_clip_rect,
                };

                let mut stretch_size = segment_local_rect.size();
                let mut spacing = LayoutSize::zero();
                let mut _repeat_offset = LayoutVector2D::zero();
                crate::border::compute_border_repetition(
                    segment_local_rect.size(),
                    cache_size.to_f32(),
                    segment.repeat_x,
                    segment.repeat_y,
                    &mut stretch_size,
                    &mut spacing,
                    &mut _repeat_offset,
                );


                if segment.repeat_x == RepeatMode::Repeat {
                    let w = segment_local_rect.width();
                    let sw = stretch_size.width;
                    let scale = w / ((w / sw).round() * sw);

                    stretch_size.width *= scale;
                }

                if segment.repeat_y == RepeatMode::Repeat {
                    let h = segment_local_rect.height();
                    let sh = stretch_size.height;
                    let scale = h / ((h / sh).round() * sh);

                    stretch_size.height *= scale;
                }

                prepare_repeatable_quad(
                    &pattern,
                    &segment_local_rect,
                    &local_clip_rect,
                    stretch_size,
                    spacing,
                    segment.edge_flags & aligned_aa_edges,
                    segment.edge_flags & transformed_aa_edges,
                    prim_instance_index,
                    &None,
                    clip_chain,
                    quad_transform,
                    frame_context,
                    pic_context,
                    targets,
                    interned_clips,
                    frame_state,
                    scratch,
                );
        }
    }
}

pub type NormalBorderTemplate = PrimTemplate<NormalBorderData>;

impl From<NormalBorderKey> for NormalBorderTemplate {
    fn from(key: NormalBorderKey) -> Self {
        let common = PrimTemplateCommonData::with_key_common(key.common);

        let mut border: NormalBorder = key.kind.border.into();
        let widths = LayoutSideOffsets::from_au(key.kind.widths);

        border.normalize(&widths);

        NormalBorderTemplate {
            common,
            kind: NormalBorderData {
                border,
                widths,
            }
        }
    }
}

pub type NormalBorderDataHandle = intern::Handle<NormalBorderPrim>;

impl intern::Internable for NormalBorderPrim {
    type Key = NormalBorderKey;
    type StoreData = NormalBorderTemplate;
    type InternData = ();
    const PROFILE_COUNTER: usize = crate::render_stats::INTERNED_NORMAL_BORDERS;
}

impl InternablePrimitive for NormalBorderPrim {
    fn into_key(
        self,
        info: &LayoutPrimitiveInfo,
    ) -> NormalBorderKey {
        NormalBorderKey::new(
            info,
            self,
        )
    }

    fn make_instance_kind(
        _key: NormalBorderKey,
        data_handle: NormalBorderDataHandle,
        _: &mut PrimitiveStore,
    ) -> PrimitiveKind {
        PrimitiveKind::NormalBorder {
            data_handle,
        }
    }
}

impl CreateShadow for NormalBorderPrim {
    fn create_shadow(
        &self,
        shadow: &Shadow,
        _: bool,
        _: RasterSpace,
    ) -> Self {
        let border = self.border.with_color(shadow.color.into());
        NormalBorderPrim {
            border,
            widths: self.widths,
        }
    }
}

impl IsVisible for NormalBorderPrim {
    fn is_visible(&self) -> bool {
        true
    }
}

////////////////////////////////////////////////////////////////////////////////

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(Debug, Clone, Eq, MallocSizeOf, PartialEq, Hash)]
pub struct ImageBorder {
    #[ignore_malloc_size_of = "Arc"]
    pub request: ImageRequest,
    pub nine_patch: NinePatchDescriptor,
}

pub type ImageBorderKey = PrimKey<ImageBorder>;

impl ImageBorderKey {
    pub fn new(
        info: &LayoutPrimitiveInfo,
        image_border: ImageBorder,
    ) -> Self {
        ImageBorderKey {
            common: info.into(),
            kind: image_border,
        }
    }
}

impl intern::InternDebug for ImageBorderKey {}

/// Per-frame scratch data for an ImageBorder primitive.
#[derive(Copy, Clone, Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
pub struct ImageBorderScratch {
    /// Range into `PrimitiveFrameScratch::segments` holding the per-
    /// frame nine-patch brush segments for this border. Built fresh
    /// each frame against the prim's current size in
    /// `prepare_prim_for_render`.
    pub brush_segments_range: storage::Range<BrushSegment>,
    /// Per-instance GPU buffer address for the brush + segment blocks
    /// written by `ImageBorderData::write_brush_gpu_blocks`. Per-instance
    /// because the block contents (stretch_size and segments) depend on
    /// the prim's per-instance size.
    pub gpu_address: GpuBufferAddress,
    /// Per-instance source image render task, recomputed each frame in
    /// `ImageBorderData::update`. Lives here rather than on the now-
    /// immutable template.
    pub src_color: Option<RenderTaskId>,
    /// Whether the source image is opaque. Derived each frame from the
    /// resource-cache image properties.
    pub is_opaque: bool,
}

impl ImageBorderScratch {
    /// Build the per-frame nine-patch brush segments for an ImageBorder
    /// prim, push the resulting `ImageBorderScratch` entry, and wire it
    /// up to the prim's `PrimitiveDrawHeader.kind_scratch`.
    ///
    /// Called from the prep early pass before `update_clip_task` runs,
    /// since `update_clip_task_for_brush` reads the brush segments via
    /// the scratch entry allocated here.
    pub fn build_for_prim(
        data_handle: ImageBorderDataHandle,
        prim_instance_index: PrimitiveInstanceIndex,
        prim_size: LayoutSize,
        data_stores: &DataStores,
        scratch: &mut PrimitiveScratchBuffer,
    ) {
        let prim_data = &data_stores.image_border[data_handle];
        let nine_patch = &prim_data.kind.nine_patch;

        let brush_open = scratch.frame.segments.open_range();
        scratch.frame.segments.data_mut().extend(
            nine_patch.create_brush_segments(prim_size),
        );
        let brush_segments_range = scratch.frame.segments.close_range(brush_open);

        let handle = scratch.frame.image_border.push(ImageBorderScratch {
            brush_segments_range,
            gpu_address: GpuBufferAddress::INVALID,
            src_color: None,
            is_opaque: false,
        });
        scratch.frame.draws[prim_instance_index.0 as usize].kind_scratch =
            KindScratchHandle::ImageBorder(handle);
    }
}

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(MallocSizeOf)]
pub struct ImageBorderData {
    #[ignore_malloc_size_of = "Arc"]
    pub request: ImageRequest,
    pub nine_patch: NinePatchDescriptor,
}

impl ImageBorderData {
    /// Update the GPU cache for a given primitive template. This may be called multiple
    /// times per frame, by each primitive reference that refers to this interned
    /// template. The initial request call to the GPU cache ensures that work is only
    /// done if the cache entry is invalid (due to first use or eviction).
    pub fn write_brush_gpu_blocks(
        &self,
        prim_size: LayoutSize,
        brush_segments: &[BrushSegment],
        frame_state: &mut FrameBuildingState,
    ) -> GpuBufferAddress {
        let mut writer = frame_state.frame_gpu_data.f32.write_blocks(3 + brush_segments.len() * VECS_PER_SEGMENT);
        self.write_prim_gpu_blocks(&mut writer, &prim_size);
        Self::write_segment_gpu_blocks(&mut writer, brush_segments);
        writer.finish()
    }


    pub fn update(
        &self,
        frame_state: &mut FrameBuildingState,
    ) -> (RenderTaskId, DeviceIntSize, bool) {
        let size = frame_state.resource_cache.request_image(
            self.request,
            &mut frame_state.frame_gpu_data.f32,
        );

        let task_id = frame_state.rg_builder.add().init(
            RenderTask::new_image(size, self.request, false)
        );

        let is_opaque = frame_state
            .resource_cache
            .get_image_properties(self.request.key)
            .map(|properties| properties.descriptor.is_opaque())
            .unwrap_or(true);

        (task_id, size, is_opaque)
    }

    fn write_prim_gpu_blocks(
        &self,
        writer: &mut GpuBufferWriterF,
        prim_size: &LayoutSize,
    ) {
        writer.push(&ImageBrushPrimitiveData {
            color: PremultipliedColorF::WHITE,
            background_color: PremultipliedColorF::WHITE,
            stretch_size: *prim_size,
        });
    }

    fn write_segment_gpu_blocks(
        writer: &mut GpuBufferWriterF,
        brush_segments: &[BrushSegment],
    ) {
        for segment in brush_segments {
            segment.write_gpu_blocks(writer);
        }
    }
}

pub type ImageBorderTemplate = PrimTemplate<ImageBorderData>;

impl From<ImageBorderKey> for ImageBorderTemplate {
    fn from(key: ImageBorderKey) -> Self {
        let common = PrimTemplateCommonData::with_key_common(key.common);

        ImageBorderTemplate {
            common,
            kind: ImageBorderData {
                request: key.kind.request,
                nine_patch: key.kind.nine_patch,
            }
        }
    }
}

pub type ImageBorderDataHandle = intern::Handle<ImageBorder>;

impl intern::Internable for ImageBorder {
    type Key = ImageBorderKey;
    type StoreData = ImageBorderTemplate;
    type InternData = ();
    const PROFILE_COUNTER: usize = crate::render_stats::INTERNED_IMAGE_BORDERS;
}

impl InternablePrimitive for ImageBorder {
    fn into_key(
        self,
        info: &LayoutPrimitiveInfo,
    ) -> ImageBorderKey {
        ImageBorderKey::new(
            info,
            self,
        )
    }

    fn make_instance_kind(
        _key: ImageBorderKey,
        data_handle: ImageBorderDataHandle,
        _: &mut PrimitiveStore,
    ) -> PrimitiveKind {
        PrimitiveKind::ImageBorder {
            data_handle
        }
    }
}

impl IsVisible for ImageBorder {
    fn is_visible(&self) -> bool {
        true
    }
}
