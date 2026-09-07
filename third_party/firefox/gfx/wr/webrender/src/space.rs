/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


//! Utilities to deal with coordinate spaces.

use std::fmt;

use euclid::{Transform3D, Box2D, Point2D, Vector2D};

use api::units::DeviceRect;
use crate::spatial_tree::{CoordinateSystemId, SpatialTree, CoordinateSpaceMapping, SpatialNodeIndex, VisibleFace};
use crate::surface::SurfaceInfo;
use crate::util::project_rect;
use crate::util::{MatrixHelpers, RectHelpers, ScaleOffset};


#[derive(Debug, Clone)]
pub struct SpaceMapper<F, T> {
    kind: CoordinateSpaceMapping<F, T>,
    pub ref_spatial_node_index: SpatialNodeIndex,
    pub current_target_spatial_node_index: SpatialNodeIndex,
    pub bounds: Box2D<f32, T>,
    visible_face: VisibleFace,
}

impl<F, T> SpaceMapper<F, T> where F: fmt::Debug {
    pub fn new(
        ref_spatial_node_index: SpatialNodeIndex,
        bounds: Box2D<f32, T>,
    ) -> Self {
        SpaceMapper {
            kind: CoordinateSpaceMapping::Local,
            ref_spatial_node_index,
            current_target_spatial_node_index: ref_spatial_node_index,
            bounds,
            visible_face: VisibleFace::Front,
        }
    }

    pub fn new_with_target(
        ref_spatial_node_index: SpatialNodeIndex,
        target_node_index: SpatialNodeIndex,
        bounds: Box2D<f32, T>,
        spatial_tree: &SpatialTree,
    ) -> Self {
        let mut mapper = Self::new(ref_spatial_node_index, bounds);
        mapper.set_target_spatial_node(target_node_index, spatial_tree);
        mapper
    }

    pub fn set_target_spatial_node(
        &mut self,
        target_node_index: SpatialNodeIndex,
        spatial_tree: &SpatialTree,
    ) {
        if target_node_index == self.current_target_spatial_node_index {
            return
        }

        let ref_spatial_node = spatial_tree.get_spatial_node(self.ref_spatial_node_index);
        let target_spatial_node = spatial_tree.get_spatial_node(target_node_index);
        self.visible_face = VisibleFace::Front;

        self.kind = if self.ref_spatial_node_index == target_node_index {
            CoordinateSpaceMapping::Local
        } else if ref_spatial_node.coordinate_system_id == target_spatial_node.coordinate_system_id {
            let scale_offset = target_spatial_node.content_transform
                .then(&ref_spatial_node.content_transform.inverse());
            CoordinateSpaceMapping::ScaleOffset(scale_offset)
        } else {
            let transform = spatial_tree
                .get_relative_transform_with_face(
                    target_node_index,
                    self.ref_spatial_node_index,
                    Some(&mut self.visible_face),
                )
                .into_transform()
                .with_source::<F>()
                .with_destination::<T>();
            CoordinateSpaceMapping::Transform(transform)
        };

        self.current_target_spatial_node_index = target_node_index;
    }

    pub fn get_transform(&self) -> Transform3D<f32, F, T> {
        match self.kind {
            CoordinateSpaceMapping::Local => {
                Transform3D::identity()
            }
            CoordinateSpaceMapping::ScaleOffset(ref scale_offset) => {
                scale_offset.to_transform()
            }
            CoordinateSpaceMapping::Transform(transform) => {
                transform
            }
        }
    }

    pub fn unmap(&self, rect: &Box2D<f32, T>) -> Option<Box2D<f32, F>> {
        match self.kind {
            CoordinateSpaceMapping::Local => {
                Some(rect.cast_unit())
            }
            CoordinateSpaceMapping::ScaleOffset(ref scale_offset) => {
                Some(scale_offset.unmap_rect(rect))
            }
            CoordinateSpaceMapping::Transform(ref transform) => {
                transform.inverse_rect_footprint(rect)
            }
        }
    }

    pub fn map(&self, rect: &Box2D<f32, F>) -> Option<Box2D<f32, T>> {
        match self.kind {
            CoordinateSpaceMapping::Local => {
                Some(rect.cast_unit())
            }
            CoordinateSpaceMapping::ScaleOffset(ref scale_offset) => {
                Some(scale_offset.map_rect(rect))
            }
            CoordinateSpaceMapping::Transform(ref transform) => {
                match project_rect(transform, rect, &self.bounds) {
                    Some(bounds) => {
                        Some(bounds)
                    }
                    None => {
                        warn!("parent relative transform can't transform the primitive rect for {:?}", rect);
                        None
                    }
                }
            }
        }
    }

    pub fn map_inner_bounds(&self, rect: &Box2D<f32, F>) -> Option<Box2D<f32, T>> {
        match self.kind {
            CoordinateSpaceMapping::Local => {
                Some(rect.cast_unit())
            }
            CoordinateSpaceMapping::ScaleOffset(ref scale_offset) => {
                Some(scale_offset.map_rect(rect))
            }
            CoordinateSpaceMapping::Transform(..) => {
                return None;
            }
        }
    }

    pub fn map_point(&self, p: Point2D<f32, F>) -> Option<Point2D<f32, T>> {
        match self.kind {
            CoordinateSpaceMapping::Local => {
                Some(p.cast_unit())
            }
            CoordinateSpaceMapping::ScaleOffset(ref scale_offset) => {
                Some(scale_offset.map_point(&p))
            }
            CoordinateSpaceMapping::Transform(ref transform) => {
                transform.transform_point2d(p)
            }
        }
    }

    pub fn map_vector(&self, v: Vector2D<f32, F>) -> Vector2D<f32, T> {
        match self.kind {
            CoordinateSpaceMapping::Local => {
                v.cast_unit()
            }
            CoordinateSpaceMapping::ScaleOffset(ref scale_offset) => {
                scale_offset.map_vector(&v)
            }
            CoordinateSpaceMapping::Transform(ref transform) => {
                transform.transform_vector2d(v)
            }
        }
    }

    pub fn as_2d_scale_offset(&self) -> Option<ScaleOffset> {
        self.kind.as_2d_scale_offset()
    }
}


/// Snaps rects to the device pixel grid at frame time, in the space they are
/// actually rasterized in. A snapper is bound to a single raster node (the
/// surface the content is rasterized into) at construction and then reused for
/// many targets via `set_target_spatial_node`, which caches the snapping
/// transform for the last target so re-snapping prims/clips that share a
/// spatial node is cheap.
///
/// The snapping transform is derived from each node's resolved
/// `content_transform` (the node-local -> coordinate-system transform the
/// spatial tree already computed, with device-pixel snapping of reference-frame
/// / scroll offsets baked in), so it is consistent with how content is actually
/// placed. Re-deriving it from raw origins / source transforms would snap rects
/// against a different offset than the node transform renders them at, landing
/// content a sub-pixel off (see bug 1580534).
///
/// Snapping into a surface's raster node (rather than always the root) snaps
/// content in the space it is rasterized in — for a tile cache that excludes
/// the scroll above the raster node, matching the cache's own (scroll-stable)
/// content transform.
///
/// A snapper built for a surface that doesn't snap (`allow_snapping == false`)
/// is disabled and passes every rect through unchanged. Such a surface is a
/// non-snapping raster root (preserve-3d / perspective / huge-scale), where
/// snapping against the surface's own scaled node would use only the tiny local
/// scale and collapse content to zero.
/// Maps a target node's content into the snap node's (device) space so a rect
/// can be snapped to the device grid there.
///
/// A rotation or reflection by a multiple of 90 degrees (the only
/// cross-coordinate-system case we snap across) is fully described by a
/// `ScaleOffset` plus an optional x/y axis swap: the 0/180-degree case is a
/// `ScaleOffset` directly; the 90/270-degree case is the same after swapping x
/// and y. `swap_xy` is always false for a target in the snap node's own
/// coordinate system.
#[derive(Clone, Debug)]
struct SnapTransform {
    scale_offset: ScaleOffset,
    swap_xy: bool,
}

#[derive(Clone, Debug)]
pub struct SpaceSnapper {
    /// If false, `snap_rect` passes rects through unchanged.
    enabled: bool,
    /// Node content is snapped against (the root, or the surface's raster node).
    snap_node_index: SpatialNodeIndex,
    /// Inverse of the snap node's `content_transform`, computed once.
    raster_content_inverse: ScaleOffset,
    /// Coordinate system of the snap node. A target in the same coordinate
    /// system snaps with a cheap scale + offset; a target in a different one is
    /// only snappable when the reference frame between them is grid-preserving.
    raster_coord_system_id: CoordinateSystemId,
    /// Last target passed to `set_target_spatial_node`, for the cache below.
    current_target_spatial_node_index: SpatialNodeIndex,
    /// Cached snapping transform for `current_target_spatial_node_index`. `None`
    /// when the target cannot be snapped (a non-axis-aligned reference frame
    /// between it and the snap node).
    snapping_transform: Option<SnapTransform>,
}

impl SpaceSnapper {
    /// Create a snapper that snaps into `surface`'s raster space (the space the
    /// surface's content is rasterized in).
    ///
    /// When the surface snaps (`allow_snapping == true`) content is snapped
    /// against the surface's own raster node.
    ///
    /// A non-snapping raster root (`allow_snapping == false`) whose raster node
    /// is still in the root coordinate system is a resolve target (backdrop
    /// filter): the `DISABLE_SNAPPING` flag keeps it from establishing a
    /// root-snapping raster root, but its content must still be snapped — so we
    /// snap it against the root, mirroring the global snap pass this replaced.
    /// A genuine non-snapping raster root (preserve-3d / perspective, raster
    /// node not in the root coordinate system) stays disabled, since snapping
    /// against its own scaled node would collapse content to zero.
    pub fn new(
        surface: &SurfaceInfo,
        spatial_tree: &SpatialTree,
    ) -> Self {
        let raster_spatial_node_index = surface.raster_spatial_node_index;
        debug_assert!(raster_spatial_node_index != SpatialNodeIndex::INVALID);
        let raster_node = spatial_tree.get_spatial_node(raster_spatial_node_index);
        let raster_in_root = raster_node.coordinate_system_id == CoordinateSystemId::root();

        let (enabled, snap_node_index) = if raster_in_root {
            (true, spatial_tree.root_reference_frame_index())
        } else if surface.allow_snapping {
            (true, raster_spatial_node_index)
        } else {
            (false, raster_spatial_node_index)
        };

        let snap_node = spatial_tree.get_spatial_node(snap_node_index);

        SpaceSnapper {
            enabled,
            snap_node_index,
            raster_content_inverse: snap_node.content_transform.inverse(),
            raster_coord_system_id: snap_node.coordinate_system_id,
            current_target_spatial_node_index: SpatialNodeIndex::INVALID,
            snapping_transform: None,
        }
    }

    /// Set the spatial node whose content subsequent `snap_rect` calls snap.
    /// Cheap to re-set with the same target: the snapping transform is cached.
    pub fn set_target_spatial_node(
        &mut self,
        target_node_index: SpatialNodeIndex,
        spatial_tree: &SpatialTree,
    ) {
        if !self.enabled || target_node_index == self.current_target_spatial_node_index {
            return;
        }

        let target_node = spatial_tree.get_spatial_node(target_node_index);

        self.current_target_spatial_node_index = target_node_index;
        self.snapping_transform = if target_node.coordinate_system_id == self.raster_coord_system_id {
            Some(SnapTransform {
                scale_offset: target_node.content_transform.then(&self.raster_content_inverse),
                swap_xy: false,
            })
        } else {
            let fwd = spatial_tree
                .get_relative_transform(target_node_index, self.snap_node_index)
                .into_transform();
            fwd.as_grid_aligned_rotation()
                .map(|(scale_offset, swap_xy)| SnapTransform { scale_offset, swap_xy })
        };
    }

    /// Snap a rect to the device pixel grid to the nearest pixel. Shorthand for
    /// `snap_rect_rounded(rect, SnapRounding::Nearest)`.
    pub fn snap_rect<F>(&self, rect: &Box2D<f32, F>) -> Box2D<f32, F> where F: fmt::Debug {
        self.snap_rect_rounded(rect, SnapRounding::Nearest)
    }

    /// Snap a rect to the device pixel grid using the current target's snapping
    /// transform: map the rect into device space, round it to the grid per
    /// `rounding`, then map it back. A target that can't be snapped (or a
    /// disabled snapper) leaves the rect unchanged. See `SnapRounding` for what
    /// each mode is for.
    pub fn snap_rect_rounded<F>(&self, rect: &Box2D<f32, F>, rounding: SnapRounding) -> Box2D<f32, F> where F: fmt::Debug {
        debug_assert!(!self.enabled || self.current_target_spatial_node_index != SpatialNodeIndex::INVALID);
        match self.snapping_transform {
            Some(SnapTransform { ref scale_offset, swap_xy }) => {
                let rect = if swap_xy { swap_box_xy(rect) } else { *rect };
                let device_rect: DeviceRect = scale_offset.map_rect(&rect);
                let snapped: DeviceRect = match rounding {
                    SnapRounding::Nearest => device_rect.snap(),
                    SnapRounding::RoundOut => device_rect.round_out(),
                    SnapRounding::Line { horizontal } =>
                        snap_line_device_rect(&device_rect, horizontal ^ swap_xy),
                };
                let unmapped: Box2D<f32, F> = scale_offset.unmap_rect(&snapped);
                if swap_xy { swap_box_xy(&unmapped) } else { unmapped }
            }
            None => *rect,
        }
    }
}

/// How a rect is rounded to the device pixel grid by `snap_rect_rounded`.
#[derive(Debug, Copy, Clone, PartialEq)]
pub enum SnapRounding {
    /// Round each edge to the nearest device pixel. Crisp fills / borders that
    /// tile seamlessly with neighbours (`round(max_of_A) == round(min_of_B)`).
    Nearest,
    /// Round the device rect *outward* (floor min, ceil max). The result still
    /// lands on the grid but fully contains the mapped rect and never shifts an
    /// edge inward. Grid-aligns the footprint of a prim that is not itself
    /// snapped (device-space text): the content stays at its exact sub-pixel
    /// position while its bounding rect stays conservative and pixel-aligned for
    /// surface / cluster allocation (bug 2050692).
    RoundOut,
    /// Round the long (length) axis to nearest, but snap the thin (thickness)
    /// axis to a single phase-independent whole-pixel extent (floored at 1 for a
    /// non-empty line). Rounding both thin-axis edges independently makes a
    /// sub-device-pixel-thick decoration line vanish (extent rounds to 0) or
    /// double (rounds to 2) depending on the sub-pixel position it lands at for
    /// the current scale (bug 1783779); snapping the extent once removes that
    /// phase dependence. `horizontal` is the line orientation in the target's
    /// own space (a horizontal line is thin in Y).
    Line { horizontal: bool },
}

/// Snap a device-space decoration-line rect: round both edges of the long axis
/// to the grid, and snap the thin axis to a phase-independent whole-pixel
/// extent (floored at 1 for a non-empty line). See `SnapRounding::Line`.
fn snap_line_device_rect(r: &DeviceRect, thin_is_y: bool) -> DeviceRect {
    let snap_extent = |e: f32| if e > 0.0 { e.round().max(1.0) } else { 0.0 };
    if thin_is_y {
        let min_y = r.min.y.round();
        DeviceRect::from_floats(
            r.min.x.round(),
            min_y,
            r.max.x.round(),
            min_y + snap_extent(r.max.y - r.min.y),
        )
    } else {
        let min_x = r.min.x.round();
        DeviceRect::from_floats(
            min_x,
            r.min.y.round(),
            min_x + snap_extent(r.max.x - r.min.x),
            r.max.y.round(),
        )
    }
}

/// Swap the x and y coordinates of a rect, mapping it through the `(x, y) ->
/// (y, x)` reflection. Used to fold a 90/270-degree axis swap into a
/// `ScaleOffset` snap; it is its own inverse.
fn swap_box_xy<F>(r: &Box2D<f32, F>) -> Box2D<f32, F> {
    Box2D::new(
        Point2D::new(r.min.y, r.min.x),
        Point2D::new(r.max.y, r.max.x),
    )
}
