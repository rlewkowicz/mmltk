/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//!  Primitive segmentation
//!
//! # Overview
//!
//! Segmenting is the process of breaking rectangular primitives into smaller rectangular
//! primitives in order to extract parts that could benefit from a fast paths.
//!
//! Typically this is used to allow fully opaque segments to be rendered in the opaque
//! pass. For example when an opaque rectangle has a non-axis-aligned transform applied,
//! we usually have to apply some anti-aliasing around the edges which requires alpha
//! blending. By segmenting the edges out of the center of the primitive, we can keep a
//! large amount of pixels in the opaque pass.
//! Segmenting also lets us avoids rasterizing parts of clip masks that we know to have
//! no effect or to be fully masking. For example by segmenting the corners of a rounded
//! rectangle clip, we can optimize both rendering the mask and the primitive by only
//! rasterize the corners in the mask and not applying any clipping to the segments of
//! the primitive that don't overlap the borders.
//!
//! It is a flexible system in the sense that different sources of segmentation (for
//! example two rounded rectangle clips) can affect the segmentation, and the possibility
//! to segment some effects such as specific clip kinds does not necessarily mean the
//! primitive will actually be segmented.
//!
//! ## Segments and clipping
//!
//! Segments of a primitive can be either not clipped, fully clipped, or partially clipped.
//! In the first two case we don't need a clip mask. For each partially masked segments, a
//! mask is rasterized using a render task. All of the interesting steps happen during frame
//! building.
//!
//! - The first step is to determine the segmentation and write the associated GPU data.
//!   See `PrimitiveInstance::build_segments_if_needed` and `write_brush_segment_description`
//!   in `prim_store/mod.rs` which uses the segment builder of this module.
//! - The second step is to generate the mask render tasks.
//!   See `BrushSegment::update_clip_task` and `RenderTask::new_mask`. For each segment that
//!   needs a mask, the contribution of all clips that affect the segment is added to the
//!   mask's render task.
//! - Segments are assigned to batches (See `batch.rs`). Segments of a given primitive can
//!   be assigned to different batches.
//!
//! See also the [`clip` module documentation][clip.rs] for details about how clipping
//! information is represented.
//!
//!
//! [clip.rs]: ../clip/index.html
//!

use api::{BorderRadius, ClipMode};
use api::units::*;
use std::{cmp, usize};
use crate::util::extract_inner_rect_safe;
use smallvec::SmallVec;

const MAX_SEGMENTS: usize = 64;

pub use api::key_types::EdgeMask;

bitflags! {
    #[derive(Debug, Copy, PartialEq, Eq, Clone, PartialOrd, Ord, Hash)]
    pub struct ItemFlags: u8 {
        const X_ACTIVE = 0x1;
        const Y_ACTIVE = 0x2;
        const HAS_MASK = 0x4;
    }
}

#[derive(Debug, PartialEq)]
pub struct Segment {
    pub rect: LayoutRect,
    pub has_mask: bool,
    pub edge_flags: EdgeMask,
    pub region_x: usize,
    pub region_y: usize,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, PartialOrd)]
enum EventKind {
    BeginClip,
    EndClip,
    BeginRegion,
}

impl Ord for EventKind {
    fn cmp(&self, other: &EventKind) -> cmp::Ordering {
        match (*self, *other) {
            (EventKind::BeginRegion, EventKind::BeginRegion) => {
                panic!("bug: regions must be non-overlapping")
            }
            (EventKind::EndClip, EventKind::BeginRegion) |
            (EventKind::BeginRegion, EventKind::BeginClip) => {
                cmp::Ordering::Less
            }
            (EventKind::BeginClip, EventKind::BeginRegion) |
            (EventKind::BeginRegion, EventKind::EndClip) => {
                cmp::Ordering::Greater
            }
            (EventKind::BeginClip, EventKind::BeginClip) |
            (EventKind::EndClip, EventKind::EndClip) => {
                cmp::Ordering::Equal
            }
            (EventKind::BeginClip, EventKind::EndClip) => {
                cmp::Ordering::Greater
            }
            (EventKind::EndClip, EventKind::BeginClip) => {
                cmp::Ordering::Less
            }
        }
    }
}

#[derive(Debug, Eq, PartialEq, PartialOrd)]
struct Event {
    value: Au,
    item_index: ItemIndex,
    kind: EventKind,
}

impl Ord for Event {
    fn cmp(&self, other: &Event) -> cmp::Ordering {
        self.value
            .cmp(&other.value)
            .then(self.kind.cmp(&other.kind))
    }
}

impl Event {
    fn begin(value: f32, index: usize) -> Event {
        Event {
            value: Au::from_f32_px(value),
            item_index: ItemIndex(index),
            kind: EventKind::BeginClip,
        }
    }

    fn end(value: f32, index: usize) -> Event {
        Event {
            value: Au::from_f32_px(value),
            item_index: ItemIndex(index),
            kind: EventKind::EndClip,
        }
    }

    fn region(value: f32) -> Event {
        Event {
            value: Au::from_f32_px(value),
            kind: EventKind::BeginRegion,
            item_index: ItemIndex(usize::MAX),
        }
    }

    fn update(
        &self,
        flag: ItemFlags,
        items: &mut [Item],
        region: &mut usize,
    ) {
        let is_active = match self.kind {
            EventKind::BeginClip => true,
            EventKind::EndClip => false,
            EventKind::BeginRegion => {
                *region += 1;
                return;
            }
        };

        items[self.item_index.0].flags.set(flag, is_active);
    }
}

#[derive(Debug)]
struct Item {
    rect: LayoutRect,
    mode: Option<ClipMode>,
    flags: ItemFlags,
}

impl Item {
    fn new(
        rect: LayoutRect,
        mode: Option<ClipMode>,
        has_mask: bool,
    ) -> Item {
        let flags = if has_mask {
            ItemFlags::HAS_MASK
        } else {
            ItemFlags::empty()
        };

        Item {
            rect,
            mode,
            flags,
        }
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, PartialOrd)]
struct ItemIndex(usize);

pub struct SegmentBuilder {
    items: Vec<Item>,
    inner_rect: Option<LayoutRect>,
    bounding_rect: Option<LayoutRect>,
    has_interesting_clips: bool,

    #[cfg(debug_assertions)]
    initialized: bool,
}

impl SegmentBuilder {
    pub fn new() -> SegmentBuilder {
        SegmentBuilder {
            items: Vec::with_capacity(4),
            bounding_rect: None,
            inner_rect: None,
            has_interesting_clips: false,
            #[cfg(debug_assertions)]
            initialized: false,
        }
    }

    pub fn initialize(
        &mut self,
        local_rect: LayoutRect,
        inner_rect: Option<LayoutRect>,
        local_clip_rect: LayoutRect,
    ) {
        self.items.clear();
        self.inner_rect = inner_rect;
        self.bounding_rect = Some(local_rect);

        self.push_clip_rect(local_rect, None, ClipMode::Clip);
        self.push_clip_rect(local_clip_rect, None, ClipMode::Clip);

        self.has_interesting_clips = false;

        #[cfg(debug_assertions)]
        {
            self.initialized = true;
        }
    }

    pub fn push_mask_region(
        &mut self,
        outer_rect: LayoutRect,
        inner_rect: LayoutRect,
        inner_clip_mode: Option<ClipMode>,
    ) {
        self.has_interesting_clips = true;

        if inner_rect.is_empty() {
            self.items.push(Item::new(
                outer_rect,
                None,
                true
            ));
            return;
        }

        debug_assert!(outer_rect.contains_box(&inner_rect));

        let p0 = outer_rect.min;
        let p1 = inner_rect.min;
        let p2 = inner_rect.max;
        let p3 = outer_rect.max;

        let segments = &[
            LayoutRect {
                min: LayoutPoint::new(p0.x, p0.y),
                max: LayoutPoint::new(p1.x, p1.y),
            },
            LayoutRect {
                min: LayoutPoint::new(p2.x, p0.y),
                max: LayoutPoint::new(p3.x, p1.y),
            },
            LayoutRect {
                min: LayoutPoint::new(p2.x, p2.y),
                max: LayoutPoint::new(p3.x, p3.y),
            },
            LayoutRect {
                min: LayoutPoint::new(p0.x, p2.y),
                max: LayoutPoint::new(p1.x, p3.y),
            },
            LayoutRect {
                min: LayoutPoint::new(p1.x, p0.y),
                max: LayoutPoint::new(p2.x, p1.y),
            },
            LayoutRect {
                min: LayoutPoint::new(p2.x, p1.y),
                max: LayoutPoint::new(p3.x, p2.y),
            },
            LayoutRect {
                min: LayoutPoint::new(p1.x, p2.y),
                max: LayoutPoint::new(p2.x, p3.y),
            },
            LayoutRect {
                min: LayoutPoint::new(p0.x, p1.y),
                max: LayoutPoint::new(p1.x, p2.y),
            },
        ];

        self.items.reserve(segments.len() + 1);

        for segment in segments {
            self.items.push(Item::new(
                *segment,
                None,
                true
            ));
        }

        if inner_clip_mode.is_some() {
            self.items.push(Item::new(
                inner_rect,
                inner_clip_mode,
                false,
            ));
        }
    }

    pub fn push_clip_rect(
        &mut self,
        rect: LayoutRect,
        radius: Option<BorderRadius>,
        mode: ClipMode,
    ) {
        self.has_interesting_clips = true;

        if mode == ClipMode::Clip {
            self.bounding_rect = self.bounding_rect.and_then(|bounding_rect| {
                bounding_rect.intersection(&rect)
            });
        }
        let mode = Some(mode);

        match radius {
            Some(radius) => {
                match extract_inner_rect_safe(&rect, &radius) {
                    Some(inner) => {
                        let p0 = rect.min;
                        let p1 = inner.min;
                        let p2 = inner.max;
                        let p3 = rect.max;

                        self.items.reserve(9);

                        let corner_segments = &[
                            LayoutRect {
                                min: LayoutPoint::new(p0.x, p0.y),
                                max: LayoutPoint::new(p1.x, p1.y),
                            },
                            LayoutRect {
                                min: LayoutPoint::new(p2.x, p0.y),
                                max: LayoutPoint::new(p3.x, p1.y),
                            },
                            LayoutRect {
                                min: LayoutPoint::new(p2.x, p2.y),
                                max: LayoutPoint::new(p3.x, p3.y),
                            },
                            LayoutRect {
                                min: LayoutPoint::new(p0.x, p2.y),
                                max: LayoutPoint::new(p1.x, p3.y),
                            },
                        ];

                        for segment in corner_segments {
                            self.items.push(Item::new(
                                *segment,
                                mode,
                                true
                            ));
                        }

                        let other_segments = &[
                            LayoutRect {
                                min: LayoutPoint::new(p1.x, p0.y),
                                max: LayoutPoint::new(p2.x, p1.y),
                            },
                            LayoutRect {
                                min: LayoutPoint::new(p2.x, p1.y),
                                max: LayoutPoint::new(p3.x, p2.y),
                            },
                            LayoutRect {
                                min: LayoutPoint::new(p1.x, p2.y),
                                max: LayoutPoint::new(p2.x, p3.y),
                            },
                            LayoutRect {
                                min: LayoutPoint::new(p0.x, p1.y),
                                max: LayoutPoint::new(p1.x, p2.y),
                            },
                            LayoutRect {
                                min: LayoutPoint::new(p1.x, p1.y),
                                max: LayoutPoint::new(p2.x, p2.y),
                            },
                        ];

                        for segment in other_segments {
                            self.items.push(Item::new(
                                *segment,
                                mode,
                                false,
                            ));
                        }
                    }
                    None => {
                        self.items.push(Item::new(
                            rect,
                            mode,
                            true,
                        ))
                    }
                }
            }
            None => {
                self.items.push(Item::new(
                    rect,
                    mode,
                    false,
                ))
            }
        }
    }

    pub fn build<F>(&mut self, mut f: F) where F: FnMut(&Segment) {
        #[cfg(debug_assertions)]
        debug_assert!(self.initialized);

        #[cfg(debug_assertions)]
        {
            self.initialized = false;
        }

        let bounding_rect = match self.bounding_rect {
            Some(bounding_rect) => bounding_rect,
            None => return,
        };

        if !self.has_interesting_clips {
            f(&Segment {
                edge_flags: EdgeMask::all(),
                region_x: 0,
                region_y: 0,
                has_mask: false,
                rect: bounding_rect,
            });
            return
        }

        self.items.retain(|item| item.rect.intersects(&bounding_rect));

        let mut x_events : SmallVec<[Event; 4]> = SmallVec::new();
        let mut y_events : SmallVec<[Event; 4]> = SmallVec::new();

        for (item_index, item) in self.items.iter().enumerate() {
            let p0 = item.rect.min;
            let p1 = item.rect.max;

            x_events.push(Event::begin(p0.x, item_index));
            x_events.push(Event::end(p1.x, item_index));
            y_events.push(Event::begin(p0.y, item_index));
            y_events.push(Event::end(p1.y, item_index));
        }

        if let Some(inner_rect) = self.inner_rect {
            x_events.push(Event::region(inner_rect.min.x));
            x_events.push(Event::region(inner_rect.max.x));

            y_events.push(Event::region(inner_rect.min.y));
            y_events.push(Event::region(inner_rect.max.y));
        }

        let p0 = LayoutPointAu::new(
            Au::from_f32_px(bounding_rect.min.x),
            Au::from_f32_px(bounding_rect.min.y),
        );

        let p1 = LayoutPointAu::new(
            Au::from_f32_px(bounding_rect.max.x),
            Au::from_f32_px(bounding_rect.max.y),
        );

        x_events.sort();
        y_events.sort();



        let mut prev_y = clamp(p0.y, y_events[0].value, p1.y);
        let mut region_y = 0;
        let mut segments : SmallVec<[_; 16]> = SmallVec::new();
        let mut x_count = 0;
        let mut y_count = 0;

        for ey in &y_events {
            let cur_y = clamp(p0.y, ey.value, p1.y);

            if cur_y != prev_y {
                let mut prev_x = clamp(p0.x, x_events[0].value, p1.x);
                let mut region_x = 0;

                for ex in &x_events {
                    let cur_x = clamp(p0.x, ex.value, p1.x);

                    if cur_x != prev_x {
                        segments.push(emit_segment_if_needed(
                            prev_x,
                            prev_y,
                            cur_x,
                            cur_y,
                            region_x,
                            region_y,
                            &self.items,
                        ));

                        prev_x = cur_x;
                        if y_count == 0 {
                            x_count += 1;
                        }
                    }

                    ex.update(
                        ItemFlags::X_ACTIVE,
                        &mut self.items,
                        &mut region_x,
                    );
                }

                prev_y = cur_y;
                y_count += 1;
            }

            ey.update(
                ItemFlags::Y_ACTIVE,
                &mut self.items,
                &mut region_y,
            );
        }

        if segments.len() > MAX_SEGMENTS {
            f(&Segment {
                edge_flags: EdgeMask::all(),
                region_x: 0,
                region_y: 0,
                has_mask: true,
                rect: bounding_rect,
            });
            return
        }

        debug_assert_eq!(segments.len(), x_count * y_count);
        for y in 0 .. y_count {
            for x in 0 .. x_count {
                let mut edge_flags = EdgeMask::empty();

                if x == 0 || segments[y * x_count + x - 1].is_none() {
                    edge_flags |= EdgeMask::LEFT;
                }
                if x == x_count-1 || segments[y * x_count + x + 1].is_none() {
                    edge_flags |= EdgeMask::RIGHT;
                }
                if y == 0 || segments[(y-1) * x_count + x].is_none() {
                    edge_flags |= EdgeMask::TOP;
                }
                if y == y_count-1 || segments[(y+1) * x_count + x].is_none() {
                    edge_flags |= EdgeMask::BOTTOM;
                }

                if let Some(ref mut segment) = segments[y * x_count + x] {
                    segment.edge_flags = edge_flags;
                    f(segment);
                }
            }
        }
    }
}

fn clamp(low: Au, value: Au, high: Au) -> Au {
    value.max(low).min(high)
}

fn emit_segment_if_needed(
    x0: Au,
    y0: Au,
    x1: Au,
    y1: Au,
    region_x: usize,
    region_y: usize,
    items: &[Item],
) -> Option<Segment> {
    debug_assert!(x1 > x0);
    debug_assert!(y1 > y0);

    let mut has_clip_mask = false;

    for item in items {
        if item.flags.contains(ItemFlags::X_ACTIVE | ItemFlags::Y_ACTIVE) {
            has_clip_mask |= item.flags.contains(ItemFlags::HAS_MASK);

            if item.mode == Some(ClipMode::ClipOut) && !item.flags.contains(ItemFlags::HAS_MASK) {
                return None;
            }
        }
    }

    let segment_rect = LayoutRect {
        min: LayoutPoint::new(
            x0.to_f32_px(),
            y0.to_f32_px(),
        ),
        max: LayoutPoint::new(
            x1.to_f32_px(),
            y1.to_f32_px(),
        ),
    };

    Some(Segment {
        rect: segment_rect,
        has_mask: has_clip_mask,
        edge_flags: EdgeMask::empty(),
        region_x,
        region_y,
    })
}
