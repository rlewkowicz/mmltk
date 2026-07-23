/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use api::BorderRadius;
use api::units::*;
use euclid::{Point2D, Rect, Box2D, Size2D, Vector2D, point2, point3};
use euclid::{default, Transform2D, Transform3D, Scale, approxeq::ApproxEq};
use plane_split::{Clipper, Polygon};
use std::{i32, f32, fmt, ptr};
use std::borrow::Cow;
use std::num::NonZeroUsize;
use std::sync::Arc;
use std::mem::replace;

use crate::internal_types::FrameVec;

const NEARLY_ZERO: f32 = 1.0 / 4096.0;

/// A typesafe helper that separates new value construction from
/// vector growing, allowing LLVM to ideally construct the element in place.
pub struct Allocation<'a, T: 'a> {
    vec: &'a mut Vec<T>,
    index: usize,
}

impl<'a, T> Allocation<'a, T> {
    #[inline(always)]
    pub fn init(self, value: T) -> usize {
        unsafe {
            ptr::write(self.vec.as_mut_ptr().add(self.index), value);
            self.vec.set_len(self.index + 1);
        }
        self.index
    }
}

/// An entry into a vector, similar to `std::collections::hash_map::Entry`.
pub enum VecEntry<'a, T: 'a> {
    Vacant(Allocation<'a, T>),
    Occupied(&'a mut T),
}

impl<'a, T> VecEntry<'a, T> {
    #[inline(always)]
    pub fn set(self, value: T) {
        match self {
            VecEntry::Vacant(alloc) => { alloc.init(value); }
            VecEntry::Occupied(slot) => { *slot = value; }
        }
    }
}

pub trait VecHelper<T> {
    /// Growns the vector by a single entry, returning the allocation.
    fn alloc(&mut self) -> Allocation<T>;
    /// Either returns an existing elemenet, or grows the vector by one.
    /// Doesn't expect indices to be higher than the current length.
    fn entry(&mut self, index: usize) -> VecEntry<T>;

    /// Equivalent to `mem::replace(&mut vec, Vec::new())`
    fn take(&mut self) -> Self;

    /// Functionally equivalent to `mem::replace(&mut vec, Vec::new())` but tries
    /// to keep the allocation in the caller if it is empty or replace it with a
    /// pre-allocated vector.
    fn take_and_preallocate(&mut self) -> Self;
}

impl<T> VecHelper<T> for Vec<T> {
    fn alloc(&mut self) -> Allocation<T> {
        let index = self.len();
        if self.capacity() == index {
            self.reserve(1);
        }
        Allocation {
            vec: self,
            index,
        }
    }

    fn entry(&mut self, index: usize) -> VecEntry<T> {
        if index < self.len() {
            VecEntry::Occupied(unsafe {
                self.get_unchecked_mut(index)
            })
        } else {
            assert_eq!(index, self.len());
            VecEntry::Vacant(self.alloc())
        }
    }

    fn take(&mut self) -> Self {
        replace(self, Vec::new())
    }

    fn take_and_preallocate(&mut self) -> Self {
        let len = self.len();
        if len == 0 {
            self.clear();
            return Vec::new();
        }
        replace(self, Vec::with_capacity(len + 8))
    }
}

#[repr(C)]
#[derive(Debug, Clone, Copy, MallocSizeOf, PartialEq)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct ScaleOffset {
    pub scale: euclid::Vector2D<f32, euclid::UnknownUnit>,
    pub offset: euclid::Vector2D<f32, euclid::UnknownUnit>,
}

impl ScaleOffset {
    pub fn new(sx: f32, sy: f32, tx: f32, ty: f32) -> Self {
        ScaleOffset {
            scale: Vector2D::new(sx, sy),
            offset: Vector2D::new(tx, ty),
        }
    }

    pub fn identity() -> Self {
        ScaleOffset {
            scale: Vector2D::new(1.0, 1.0),
            offset: Vector2D::zero(),
        }
    }

    pub fn from_transform<F, T>(
        m: &Transform3D<f32, F, T>,
    ) -> Option<ScaleOffset> {


        if m.m12.abs() > NEARLY_ZERO ||
           m.m13.abs() > NEARLY_ZERO ||
           m.m14.abs() > NEARLY_ZERO ||
           m.m21.abs() > NEARLY_ZERO ||
           m.m23.abs() > NEARLY_ZERO ||
           m.m24.abs() > NEARLY_ZERO ||
           m.m31.abs() > NEARLY_ZERO ||
           m.m32.abs() > NEARLY_ZERO ||
           (m.m33 - 1.0).abs() > NEARLY_ZERO ||
           m.m34.abs() > NEARLY_ZERO ||
           m.m43.abs() > NEARLY_ZERO ||
           (m.m44 - 1.0).abs() > NEARLY_ZERO {
            return None;
        }

        Some(ScaleOffset {
            scale: Vector2D::new(m.m11, m.m22),
            offset: Vector2D::new(m.m41, m.m42),
        })
    }

    pub fn from_offset(offset: default::Vector2D<f32>) -> Self {
        ScaleOffset {
            scale: Vector2D::new(1.0, 1.0),
            offset,
        }
    }

    pub fn from_scale(scale: default::Vector2D<f32>) -> Self {
        ScaleOffset {
            scale,
            offset: Vector2D::new(0.0, 0.0),
        }
    }

    pub fn inverse(&self) -> Self {
        if self.scale.x.approx_eq(&0.0) || self.scale.y.approx_eq(&0.0) {
            return ScaleOffset::new(0.0, 0.0, 0.0, 0.0);
        }

        ScaleOffset {
            scale: Vector2D::new(
                1.0 / self.scale.x,
                1.0 / self.scale.y,
            ),
            offset: Vector2D::new(
                -self.offset.x / self.scale.x,
                -self.offset.y / self.scale.y,
            ),
        }
    }

    pub fn pre_offset(&self, offset: default::Vector2D<f32>) -> Self {
        self.pre_transform(
            &ScaleOffset {
                scale: Vector2D::new(1.0, 1.0),
                offset,
            }
        )
    }

    pub fn pre_scale(&self, scale: f32) -> Self {
        ScaleOffset {
            scale: self.scale * scale,
            offset: self.offset,
        }
    }

    pub fn then_scale(&self, scale: f32) -> Self {
        ScaleOffset {
            scale: self.scale * scale,
            offset: self.offset * scale,
        }
    }

    /// Produce a ScaleOffset that includes both self and other.
    /// The 'self' ScaleOffset is applied after `other`.
    /// This is equivalent to `Transform3D::pre_transform`.
    pub fn pre_transform(&self, other: &ScaleOffset) -> Self {
        ScaleOffset {
            scale: Vector2D::new(
                self.scale.x * other.scale.x,
                self.scale.y * other.scale.y,
            ),
            offset: Vector2D::new(
                self.offset.x + self.scale.x * other.offset.x,
                self.offset.y + self.scale.y * other.offset.y,
            ),
        }
    }

    /// Produce a ScaleOffset that includes both self and other.
    /// The 'other' ScaleOffset is applied after `self`.
    /// This is equivalent to `Transform3D::then`.
    #[allow(unused)]
    pub fn then(&self, other: &ScaleOffset) -> Self {
        ScaleOffset {
            scale: Vector2D::new(
                self.scale.x * other.scale.x,
                self.scale.y * other.scale.y,
            ),
            offset: Vector2D::new(
                other.scale.x * self.offset.x + other.offset.x,
                other.scale.y * self.offset.y + other.offset.y,
            ),
        }
    }


    pub fn map_rect<F, T>(&self, rect: &Box2D<f32, F>) -> Box2D<f32, T> {
        let x0 = rect.min.x * self.scale.x + self.offset.x;
        let y0 = rect.min.y * self.scale.y + self.offset.y;
        let x1 = rect.min.x.max(rect.max.x) * self.scale.x + self.offset.x;
        let y1 = rect.min.y.max(rect.max.y) * self.scale.y + self.offset.y;

        Box2D::new(
            Point2D::new(x0.min(x1), y0.min(y1)),
            Point2D::new(x0.max(x1), y0.max(y1)),
        )
    }

    pub fn unmap_rect<F, T>(&self, rect: &Box2D<f32, F>) -> Box2D<f32, T> {
        let x0 = (rect.min.x - self.offset.x) / self.scale.x;
        let y0 = (rect.min.y - self.offset.y) / self.scale.y;
        let x1 = (rect.min.x.max(rect.max.x) - self.offset.x) / self.scale.x;
        let y1 = (rect.min.y.max(rect.max.y) - self.offset.y) / self.scale.y;

        Box2D::new(
            Point2D::new(x0.min(x1), y0.min(y1)),
            Point2D::new(x0.max(x1), y0.max(y1)),
        )
    }

    pub fn map_vector<F, T>(&self, vector: &Vector2D<f32, F>) -> Vector2D<f32, T> {
        Vector2D::new(
            vector.x * self.scale.x,
            vector.y * self.scale.y,
        )
    }

    pub fn map_size<F, T>(&self, size: &Size2D<f32, F>) -> Size2D<f32, T> {
        Size2D::new(
            size.width * self.scale.x,
            size.height * self.scale.y,
        )
    }

    pub fn unmap_vector<F, T>(&self, vector: &Vector2D<f32, F>) -> Vector2D<f32, T> {
        Vector2D::new(
            vector.x / self.scale.x,
            vector.y / self.scale.y,
        )
    }

    pub fn map_point<F, T>(&self, point: &Point2D<f32, F>) -> Point2D<f32, T> {
        Point2D::new(
            point.x * self.scale.x + self.offset.x,
            point.y * self.scale.y + self.offset.y,
        )
    }

    pub fn unmap_point<F, T>(&self, point: &Point2D<f32, F>) -> Point2D<f32, T> {
        Point2D::new(
            (point.x - self.offset.x) / self.scale.x,
            (point.y - self.offset.y) / self.scale.y,
        )
    }

    pub fn to_transform<F, T>(&self) -> Transform3D<f32, F, T> {
        Transform3D::new(
            self.scale.x,
            0.0,
            0.0,
            0.0,

            0.0,
            self.scale.y,
            0.0,
            0.0,

            0.0,
            0.0,
            1.0,
            0.0,

            self.offset.x,
            self.offset.y,
            0.0,
            1.0,
        )
    }

    pub fn is_identity(&self) -> bool {
        self.scale.x == 1.0 &&
        self.scale.y == 1.0 &&
        self.offset.x == 0.0 &&
        self.offset.y == 0.0
    }

    pub fn is_reflection(&self) -> bool {
        self.scale.x < 0.0 ||
        self.scale.y < 0.0
    }
}

pub trait MatrixHelpers<Src, Dst> {
    /// A port of the preserves2dAxisAlignment function in Skia.
    /// Defined in the SkMatrix44 class.
    fn preserves_2d_axis_alignment(&self) -> bool;
    fn has_perspective_component(&self) -> bool;
    /// Returns true only if the perspective divide varies across the z=0 plane
    /// (`m14`/`m24` non-zero), i.e. a coplanar 2D surface is mapped with a
    /// non-constant `w` (a true keystone). A perspective matrix whose only
    /// perspective terms are `m34`/`m44` still maps a z=0 surface affinely
    /// (constant `w`), so it returns false here even though
    /// `has_perspective_component` is true. Used to decide whether coplanar
    /// content (e.g. text) can still be rasterized and snapped in device space
    /// (bug 2052019).
    fn has_2d_plane_perspective(&self) -> bool;
    fn has_2d_inverse(&self) -> bool;
    /// Check if the matrix post-scaling on either the X or Y axes could cause geometry
    /// transformed by this matrix to have scaling exceeding the supplied limit.
    fn exceeds_2d_scale(&self, limit: f64) -> bool;
    fn inverse_project(&self, target: &Point2D<f32, Dst>) -> Option<Point2D<f32, Src>>;
    fn inverse_rect_footprint(&self, rect: &Box2D<f32, Dst>) -> Option<Box2D<f32, Src>>;
    fn is_simple_translation(&self) -> bool;
    fn is_simple_2d_translation(&self) -> bool;
    fn is_2d_scale_translation(&self) -> bool;
    /// If this transform is a rotation or reflection by a multiple of 90 degrees
    /// (with unit scale, no z-coupling and no perspective), decompose it into a
    /// `ScaleOffset` plus whether the x and y axes are swapped (the 90/270-degree
    /// case, where the `ScaleOffset` applies after the swap). Returns `None`
    /// otherwise. Such a transform keeps content on the same pixel grid, so a
    /// rect can be snapped across it losslessly. Unlike
    /// `preserves_2d_axis_alignment`, this also rejects perspective (`m34`) and
    /// rescaling.
    fn as_grid_aligned_rotation(&self) -> Option<(ScaleOffset, bool)>;
    /// Return the determinant of the 2D part of the matrix.
    fn determinant_2d(&self) -> f32;
    /// Turn Z transformation into identity. This is useful when crossing "flat"
    /// transform styled stacking contexts upon traversing the coordinate systems.
    fn flatten_z_output(&mut self);

    fn cast_unit<NewSrc, NewDst>(&self) -> Transform3D<f32, NewSrc, NewDst>;
}

impl<Src, Dst> MatrixHelpers<Src, Dst> for Transform3D<f32, Src, Dst> {
    fn preserves_2d_axis_alignment(&self) -> bool {
        if self.m14 != 0.0 || self.m24 != 0.0 {
            return false;
        }

        let mut col0 = 0;
        let mut col1 = 0;
        let mut row0 = 0;
        let mut row1 = 0;

        if self.m11.abs() > NEARLY_ZERO {
            col0 += 1;
            row0 += 1;
        }
        if self.m12.abs() > NEARLY_ZERO {
            col1 += 1;
            row0 += 1;
        }
        if self.m21.abs() > NEARLY_ZERO {
            col0 += 1;
            row1 += 1;
        }
        if self.m22.abs() > NEARLY_ZERO {
            col1 += 1;
            row1 += 1;
        }

        col0 < 2 && col1 < 2 && row0 < 2 && row1 < 2
    }

    fn has_perspective_component(&self) -> bool {
         self.m14.abs() > NEARLY_ZERO ||
         self.m24.abs() > NEARLY_ZERO ||
         self.m34.abs() > NEARLY_ZERO ||
         (self.m44 - 1.0).abs() > NEARLY_ZERO
    }

    fn has_2d_plane_perspective(&self) -> bool {
         self.m14.abs() > NEARLY_ZERO ||
         self.m24.abs() > NEARLY_ZERO
    }

    fn has_2d_inverse(&self) -> bool {
        self.determinant_2d() != 0.0
    }

    fn exceeds_2d_scale(&self, limit: f64) -> bool {
        let limit2 = (limit * limit) as f32;
        self.m11 * self.m11 + self.m12 * self.m12 > limit2 ||
        self.m21 * self.m21 + self.m22 * self.m22 > limit2
    }

    /// Find out a point in `Src` that would be projected into the `target`.
    fn inverse_project(&self, target: &Point2D<f32, Dst>) -> Option<Point2D<f32, Src>> {
        let m = Transform2D::<f32, Src, Dst>::new(
            self.m11 - target.x * self.m14, self.m12 - target.y * self.m14,
            self.m21 - target.x * self.m24, self.m22 - target.y * self.m24,
            self.m41 - target.x * self.m44, self.m42 - target.y * self.m44,
        );
        let inv = m.inverse()?;
        if inv.m31 * self.m14 + inv.m32 * self.m24 + self.m44 > 0.0 {
            Some(Point2D::new(inv.m31, inv.m32))
        } else {
            None
        }
    }

    fn inverse_rect_footprint(&self, rect: &Box2D<f32, Dst>) -> Option<Box2D<f32, Src>> {
        Some(Box2D::from_points(&[
            self.inverse_project(&rect.top_left())?,
            self.inverse_project(&rect.top_right())?,
            self.inverse_project(&rect.bottom_left())?,
            self.inverse_project(&rect.bottom_right())?,
        ]))
    }

    fn is_simple_translation(&self) -> bool {
        if (self.m11 - 1.0).abs() > NEARLY_ZERO ||
            (self.m22 - 1.0).abs() > NEARLY_ZERO ||
            (self.m33 - 1.0).abs() > NEARLY_ZERO ||
            (self.m44 - 1.0).abs() > NEARLY_ZERO {
            return false;
        }

        self.m12.abs() < NEARLY_ZERO && self.m13.abs() < NEARLY_ZERO &&
            self.m14.abs() < NEARLY_ZERO && self.m21.abs() < NEARLY_ZERO &&
            self.m23.abs() < NEARLY_ZERO && self.m24.abs() < NEARLY_ZERO &&
            self.m31.abs() < NEARLY_ZERO && self.m32.abs() < NEARLY_ZERO &&
            self.m34.abs() < NEARLY_ZERO
    }

    fn is_simple_2d_translation(&self) -> bool {
        if !self.is_simple_translation() {
            return false;
        }

        self.m43.abs() < NEARLY_ZERO
    }

    fn is_2d_scale_translation(&self) -> bool {
        (self.m33 - 1.0).abs() < NEARLY_ZERO &&
            (self.m44 - 1.0).abs() < NEARLY_ZERO &&
            self.m12.abs() < NEARLY_ZERO && self.m13.abs() < NEARLY_ZERO && self.m14.abs() < NEARLY_ZERO &&
            self.m21.abs() < NEARLY_ZERO && self.m23.abs() < NEARLY_ZERO && self.m24.abs() < NEARLY_ZERO &&
            self.m31.abs() < NEARLY_ZERO && self.m32.abs() < NEARLY_ZERO && self.m34.abs() < NEARLY_ZERO &&
            self.m43.abs() < NEARLY_ZERO
    }

    fn as_grid_aligned_rotation(&self) -> Option<(ScaleOffset, bool)> {
        let is_zero = |v: f32| v.abs() < NEARLY_ZERO;
        let is_one = |v: f32| (v - 1.0).abs() < NEARLY_ZERO;
        let is_unit = |v: f32| (v.abs() - 1.0).abs() < NEARLY_ZERO;

        if !(is_zero(self.m13) && is_zero(self.m14) && is_zero(self.m23) && is_zero(self.m24) &&
            is_zero(self.m31) && is_zero(self.m32) && is_one(self.m33) && is_zero(self.m34) &&
            is_zero(self.m43) && is_one(self.m44)) {
            return None;
        }

        if is_unit(self.m11) && is_unit(self.m22) && is_zero(self.m12) && is_zero(self.m21) {
            Some((ScaleOffset::new(self.m11, self.m22, self.m41, self.m42), false))
        } else if is_unit(self.m12) && is_unit(self.m21) && is_zero(self.m11) && is_zero(self.m22) {
            Some((ScaleOffset::new(self.m21, self.m12, self.m41, self.m42), true))
        } else {
            None
        }
    }

    fn determinant_2d(&self) -> f32 {
        self.m11 * self.m22 - self.m12 * self.m21
    }

    fn flatten_z_output(&mut self) {
        self.m13 = 0.0;
        self.m23 = 0.0;
        self.m33 = 1.0;
        self.m43 = 0.0;
    }

    fn cast_unit<NewSrc, NewDst>(&self) -> Transform3D<f32, NewSrc, NewDst> {
        Transform3D::new(
            self.m11, self.m12, self.m13, self.m14,
            self.m21, self.m22, self.m23, self.m24,
            self.m31, self.m32, self.m33, self.m34,
            self.m41, self.m42, self.m43, self.m44,
        )
    }
}

pub trait VectorHelpers<U>
where
    Self: Sized,
{
    fn snap(&self) -> Self;
}

impl<U> VectorHelpers<U> for Vector2D<f32, U> {
    fn snap(&self) -> Self {
        Vector2D::new(
            self.x.round(),
            self.y.round(),
        )
    }
}

pub trait RectHelpers<U>
where
    Self: Sized,
{
    fn from_floats(x0: f32, y0: f32, x1: f32, y1: f32) -> Self;
    fn snap(&self) -> Self;
}

impl<U> RectHelpers<U> for Rect<f32, U> {
    fn from_floats(x0: f32, y0: f32, x1: f32, y1: f32) -> Self {
        Rect::new(
            Point2D::new(x0, y0),
            Size2D::new(x1 - x0, y1 - y0),
        )
    }

    fn snap(&self) -> Self {
        let origin = Point2D::new(
            (self.origin.x + 0.5).floor(),
            (self.origin.y + 0.5).floor(),
        );
        Rect::new(
            origin,
            Size2D::new(
                (self.origin.x + self.size.width + 0.5).floor() - origin.x,
                (self.origin.y + self.size.height + 0.5).floor() - origin.y,
            ),
        )
    }
}

impl<U> RectHelpers<U> for Box2D<f32, U> {
    fn from_floats(x0: f32, y0: f32, x1: f32, y1: f32) -> Self {
        Box2D {
            min: Point2D::new(x0, y0),
            max: Point2D::new(x1, y1),
        }
    }

    fn snap(&self) -> Self {
        self.round()
    }
}

pub fn lerp(a: f32, b: f32, t: f32) -> f32 {
    (b - a) * t + a
}

#[repr(u32)]
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub enum TransformedRectKind {
    AxisAligned = 0,
    Complex = 1,
}

#[inline(always)]
pub fn pack_as_float(value: u32) -> f32 {
    value as f32 + 0.5
}

#[inline]
fn extract_inner_rect_impl<U>(
    rect: &Box2D<f32, U>,
    radii: &BorderRadius,
    k: f32,
) -> Option<Box2D<f32, U>> {

    let xl = (k * radii.top_left.width.max(radii.bottom_left.width)).ceil();
    let xr = (rect.width() - k * radii.top_right.width.max(radii.bottom_right.width)).floor();
    let yt = (k * radii.top_left.height.max(radii.top_right.height)).ceil();
    let yb =
        (rect.height() - k * radii.bottom_left.height.max(radii.bottom_right.height)).floor();

    if xl <= xr && yt <= yb {
        Some(Box2D::from_origin_and_size(
            Point2D::new(rect.min.x + xl, rect.min.y + yt),
            Size2D::new(xr - xl, yb - yt),
        ))
    } else {
        None
    }
}

/// Return an aligned rectangle that is inside the clip region and doesn't intersect
/// any of the bounding rectangles of the rounded corners.
pub fn extract_inner_rect_safe<U>(
    rect: &Box2D<f32, U>,
    radii: &BorderRadius,
) -> Option<Box2D<f32, U>> {
    extract_inner_rect_impl(rect, radii, 1.0)
}

/// Return an aligned rectangle that is inside the clip region and doesn't intersect
/// any of the bounding rectangles of the rounded corners, with a specific k factor
/// to control how much of the rounded corner is included.
pub fn extract_inner_rect_k<U>(
    rect: &Box2D<f32, U>,
    radii: &BorderRadius,
    k: f32,
) -> Option<Box2D<f32, U>> {
    extract_inner_rect_impl(rect, radii, k)
}


pub trait MaxRect {
    fn max_rect() -> Self;
}

impl MaxRect for DeviceIntRect {
    fn max_rect() -> Self {
        DeviceIntRect::from_origin_and_size(
            DeviceIntPoint::new(i32::MIN / 2, i32::MIN / 2),
            DeviceIntSize::new(i32::MAX, i32::MAX),
        )
    }
}

impl<U> MaxRect for Rect<f32, U> {
    fn max_rect() -> Self {
        const MAX_COORD: f32 = 1.0e9;

        Rect::new(
            Point2D::new(-MAX_COORD, -MAX_COORD),
            Size2D::new(2.0 * MAX_COORD, 2.0 * MAX_COORD),
        )
    }
}

impl<U> MaxRect for Box2D<f32, U> {
    fn max_rect() -> Self {
        const MAX_COORD: f32 = 1.0e9;

        Box2D::new(
            Point2D::new(-MAX_COORD, -MAX_COORD),
            Point2D::new(MAX_COORD, MAX_COORD),
        )
    }
}

/// An enum that tries to avoid expensive transformation matrix calculations
/// when possible when dealing with non-perspective axis-aligned transformations.
#[derive(Debug, MallocSizeOf)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub enum FastTransform<Src, Dst> {
    /// A simple offset, which can be used without doing any matrix math.
    Offset(Vector2D<f32, Src>),

    /// A 2D transformation with an inverse.
    Transform {
        transform: Transform3D<f32, Src, Dst>,
        inverse: Option<Transform3D<f32, Dst, Src>>,
        is_2d: bool,
    },
}

impl<Src, Dst> Clone for FastTransform<Src, Dst> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<Src, Dst> Copy for FastTransform<Src, Dst> { }

impl<Src, Dst> FastTransform<Src, Dst> {
    pub fn identity() -> Self {
        FastTransform::Offset(Vector2D::zero())
    }

    pub fn with_vector(offset: Vector2D<f32, Src>) -> Self {
        FastTransform::Offset(offset)
    }

    pub fn with_scale_offset(scale_offset: ScaleOffset) -> Self {
        if scale_offset.scale == Vector2D::new(1.0, 1.0) {
            FastTransform::Offset(Vector2D::from_untyped(scale_offset.offset))
        } else {
            FastTransform::Transform {
                transform: scale_offset.to_transform(),
                inverse: Some(scale_offset.inverse().to_transform()),
                is_2d: true,
            }
        }
    }

    #[inline(always)]
    pub fn with_transform(transform: Transform3D<f32, Src, Dst>) -> Self {
        if transform.is_simple_2d_translation() {
            return FastTransform::Offset(Vector2D::new(transform.m41, transform.m42));
        }
        let inverse = transform.inverse();
        let is_2d = transform.is_2d();
        FastTransform::Transform { transform, inverse, is_2d}
    }

    pub fn to_transform(&self) -> Cow<Transform3D<f32, Src, Dst>> {
        match *self {
            FastTransform::Offset(offset) => Cow::Owned(
                Transform3D::translation(offset.x, offset.y, 0.0)
            ),
            FastTransform::Transform { ref transform, .. } => Cow::Borrowed(transform),
        }
    }

    /// Return true if this is an identity transform
    #[allow(unused)]
    pub fn is_identity(&self)-> bool {
        match *self {
            FastTransform::Offset(offset) => {
                offset == Vector2D::zero()
            }
            FastTransform::Transform { ref transform, .. } => {
                *transform == Transform3D::identity()
            }
        }
    }

    pub fn then<NewDst>(&self, other: &FastTransform<Dst, NewDst>) -> FastTransform<Src, NewDst> {
        match *self {
            FastTransform::Offset(offset) => match *other {
                FastTransform::Offset(other_offset) => {
                    FastTransform::Offset(offset + other_offset * Scale::<_, _, Src>::new(1.0))
                }
                FastTransform::Transform { transform: ref other_transform, .. } => {
                    FastTransform::with_transform(
                        other_transform
                            .with_source::<Src>()
                            .pre_translate(offset.to_3d())
                    )
                }
            }
            FastTransform::Transform { ref transform, ref inverse, is_2d } => match *other {
                FastTransform::Offset(other_offset) => {
                    FastTransform::with_transform(
                        transform
                            .then_translate(other_offset.to_3d())
                            .with_destination::<NewDst>()
                    )
                }
                FastTransform::Transform { transform: ref other_transform, inverse: ref other_inverse, is_2d: other_is_2d } => {
                    FastTransform::Transform {
                        transform: transform.then(other_transform),
                        inverse: inverse.as_ref().and_then(|self_inv|
                            other_inverse.as_ref().map(|other_inv| other_inv.then(self_inv))
                        ),
                        is_2d: is_2d & other_is_2d,
                    }
                }
            }
        }
    }

    pub fn pre_transform<NewSrc>(
        &self,
        other: &FastTransform<NewSrc, Src>
    ) -> FastTransform<NewSrc, Dst> {
        other.then(self)
    }

    pub fn pre_translate(&self, other_offset: Vector2D<f32, Src>) -> Self {
        match *self {
            FastTransform::Offset(offset) =>
                FastTransform::Offset(offset + other_offset),
            FastTransform::Transform { transform, .. } =>
                FastTransform::with_transform(transform.pre_translate(other_offset.to_3d()))
        }
    }

    pub fn then_translate(&self, other_offset: Vector2D<f32, Dst>) -> Self {
        match *self {
            FastTransform::Offset(offset) => {
                FastTransform::Offset(offset + other_offset * Scale::<_, _, Src>::new(1.0))
            }
            FastTransform::Transform { ref transform, .. } => {
                let transform = transform.then_translate(other_offset.to_3d());
                FastTransform::with_transform(transform)
            }
        }
    }

    #[inline(always)]
    pub fn is_backface_visible(&self) -> bool {
        match *self {
            FastTransform::Offset(..) => false,
            FastTransform::Transform { inverse: None, .. } => false,
            FastTransform::Transform { inverse: Some(ref inverse), .. } => inverse.m33 < 0.0,
        }
    }

    #[inline(always)]
    pub fn transform_point2d(&self, point: Point2D<f32, Src>) -> Option<Point2D<f32, Dst>> {
        match *self {
            FastTransform::Offset(offset) => {
                let new_point = point + offset;
                Some(Point2D::from_untyped(new_point.to_untyped()))
            }
            FastTransform::Transform { ref transform, .. } => transform.transform_point2d(point),
        }
    }

    #[inline(always)]
    pub fn project_point2d(&self, point: Point2D<f32, Src>) -> Option<Point2D<f32, Dst>> {
        match* self {
            FastTransform::Offset(..) => self.transform_point2d(point),
            FastTransform::Transform{ref transform, ..} => {


                let z = -(point.x * transform.m13 + point.y * transform.m23 + transform.m43) / transform.m33;

                transform.transform_point3d(point3(point.x, point.y, z)).map(| p3 | point2(p3.x, p3.y))
            }
        }
    }

    #[inline(always)]
    pub fn inverse(&self) -> Option<FastTransform<Dst, Src>> {
        match *self {
            FastTransform::Offset(offset) =>
                Some(FastTransform::Offset(Vector2D::new(-offset.x, -offset.y))),
            FastTransform::Transform { transform, inverse: Some(inverse), is_2d, } =>
                Some(FastTransform::Transform {
                    transform: inverse,
                    inverse: Some(transform),
                    is_2d
                }),
            FastTransform::Transform { inverse: None, .. } => None,

        }
    }
}

impl<Src, Dst> From<Transform3D<f32, Src, Dst>> for FastTransform<Src, Dst> {
    fn from(transform: Transform3D<f32, Src, Dst>) -> Self {
        FastTransform::with_transform(transform)
    }
}

impl<Src, Dst> From<Vector2D<f32, Src>> for FastTransform<Src, Dst> {
    fn from(vector: Vector2D<f32, Src>) -> Self {
        FastTransform::with_vector(vector)
    }
}

pub type LayoutFastTransform = FastTransform<LayoutPixel, LayoutPixel>;
pub type LayoutToWorldFastTransform = FastTransform<LayoutPixel, WorldPixel>;

pub fn project_rect<F, T>(
    transform: &Transform3D<f32, F, T>,
    rect: &Box2D<f32, F>,
    bounds: &Box2D<f32, T>,
) -> Option<Box2D<f32, T>>
 where F: fmt::Debug
{
    let homogens = [
        transform.transform_point2d_homogeneous(rect.top_left()),
        transform.transform_point2d_homogeneous(rect.top_right()),
        transform.transform_point2d_homogeneous(rect.bottom_left()),
        transform.transform_point2d_homogeneous(rect.bottom_right()),
    ];

    if homogens.iter().any(|h| h.w <= 0.0 || h.w.is_nan()) {
        let mut clipper = Clipper::new();
        let polygon = Polygon::from_rect(rect.to_rect().cast().cast_unit(), 1);

        let planes = match Clipper::<usize>::frustum_planes(
            &transform.cast_unit().cast(),
            Some(bounds.to_rect().cast_unit().to_f64()),
        ) {
            Ok(planes) => planes,
            Err(..) => return None,
        };

        for plane in planes {
            clipper.add(plane);
        }

        let results = clipper.clip(polygon);
        if results.is_empty() {
            return None
        }

        Some(Box2D::from_points(results
            .into_iter()
            .flat_map(|poly| &poly.points)
            .map(|p| {
                let mut homo = transform.transform_point2d_homogeneous(p.to_2d().to_f32().cast_unit());
                homo.w = homo.w.max(0.00000001); 
                homo.to_point2d().unwrap()
            })
        ))
    } else {
        Some(Box2D::from_points(&[
            homogens[0].to_point2d().unwrap(),
            homogens[1].to_point2d().unwrap(),
            homogens[2].to_point2d().unwrap(),
            homogens[3].to_point2d().unwrap(),
        ]))
    }
}

/// Run the first callback over all elements in the array. If the callback returns true,
/// the element is removed from the array and moved to a second callback.
///
/// This is a simple implementation waiting for Vec::drain_filter to be stable.
/// When that happens, code like:
///
/// let filter = |op| {
///     match *op {
///         Enum::Foo | Enum::Bar => true,
///         Enum::Baz => false,
///     }
/// };
/// drain_filter(
///     &mut ops,
///     filter,
///     |op| {
///         match op {
///             Enum::Foo => { foo(); }
///             Enum::Bar => { bar(); }
///             Enum::Baz => { unreachable!(); }
///         }
///     },
/// );
///
/// Can be rewritten as:
///
/// let filter = |op| {
///     match *op {
///         Enum::Foo | Enum::Bar => true,
///         Enum::Baz => false,
///     }
/// };
/// for op in ops.drain_filter(filter) {
///     match op {
///         Enum::Foo => { foo(); }
///         Enum::Bar => { bar(); }
///         Enum::Baz => { unreachable!(); }
///     }
/// }
///
/// See https://doc.rust-lang.org/std/vec/struct.Vec.html#method.drain_filter
pub fn drain_filter<T, Filter, Action>(
    vec: &mut Vec<T>,
    mut filter: Filter,
    mut action: Action,
)
where
    Filter: FnMut(&mut T) -> bool,
    Action: FnMut(T)
{
    let mut i = 0;
    while i != vec.len() {
        if filter(&mut vec[i]) {
            action(vec.remove(i));
        } else {
            i += 1;
        }
    }
}


#[derive(Debug)]
pub struct Recycler {
    pub num_allocations: usize,
}

impl Recycler {
    /// Maximum extra capacity that a recycled vector is allowed to have. If the actual capacity
    /// is larger, we re-allocate the vector storage with lower capacity.
    const MAX_EXTRA_CAPACITY_PERCENT: usize = 200;
    /// Minimum extra capacity to keep when re-allocating the vector storage.
    const MIN_EXTRA_CAPACITY_PERCENT: usize = 20;
    /// Minimum sensible vector length to consider for re-allocation.
    const MIN_VECTOR_LENGTH: usize = 16;

    pub fn new() -> Self {
        Recycler {
            num_allocations: 0,
        }
    }

    /// Clear a vector for re-use, while retaining the backing memory buffer. May shrink the buffer
    /// if it's currently much larger than was actually used.
    pub fn recycle_vec<T>(&mut self, vec: &mut Vec<T>) {
        let extra_capacity = (vec.capacity() - vec.len()) * 100 / vec.len().max(Self::MIN_VECTOR_LENGTH);

        if extra_capacity > Self::MAX_EXTRA_CAPACITY_PERCENT {
            *vec = Vec::with_capacity(vec.len() + vec.len() * Self::MIN_EXTRA_CAPACITY_PERCENT / 100);
            self.num_allocations += 1;
        } else {
            vec.clear();
        }
    }
}

/// Record the size of a data structure to preallocate a similar size
/// at the next frame and avoid growing it too many time.
#[derive(Copy, Clone, Debug)]
pub struct Preallocator {
    size: usize,
}

impl Preallocator {
    pub fn new(initial_size: usize) -> Self {
        Preallocator {
            size: initial_size,
        }
    }

    /// Record the size of a vector to preallocate it the next frame.
    pub fn record_vec<T>(&mut self, vec: &[T]) {
        let len = vec.len();
        if len > self.size {
            self.size = len;
        } else {
            self.size = (self.size + len) / 2;
        }
    }

    /// The size that we'll preallocate the vector with.
    pub fn preallocation_size(&self) -> usize {
        (self.size + 15) & !15
    }

    /// Preallocate vector storage.
    ///
    /// The preallocated amount depends on the length recorded in the last
    /// record_vec call.
    pub fn preallocate_vec<T>(&self, vec: &mut Vec<T>) {
        let len = vec.len();
        let cap = self.preallocation_size();
        if len < cap {
            vec.reserve(cap - len);
        }
    }

    /// Preallocate vector storage.
    ///
    /// The preallocated amount depends on the length recorded in the last
    /// record_vec call.
    pub fn preallocate_framevec<T>(&self, vec: &mut FrameVec<T>) {
        let len = vec.len();
        let cap = self.preallocation_size();
        if len < cap {
            vec.reserve(cap - len);
        }
    }
}

impl Default for Preallocator {
    fn default() -> Self {
        Self::new(0)
    }
}

/// Computes the scale factors of this matrix; that is,
/// the amounts each basis vector is scaled by.
///
/// This code comes from gecko gfx/2d/Matrix.h with the following
/// modifications:
///
/// * Removed `xMajor` parameter.
/// * All arithmetics is done with double precision.
pub fn scale_factors<Src, Dst>(
    mat: &Transform3D<f32, Src, Dst>
) -> (f32, f32) {
    let m11 = mat.m11 as f64;
    let m12 = mat.m12 as f64;
    let det = m11 * mat.m22 as f64 - m12 * mat.m21 as f64;
    if det == 0.0 {
        return (0.0, 0.0);
    }

    let det = det.abs();

    let major = (m11 * m11 + m12 * m12).sqrt();
    let minor = if major != 0.0 { det / major } else { 0.0 };

    (major as f32, minor as f32)
}


/// Clamp scaling factor to a power of two.
///
/// This code comes from gecko gfx/thebes/gfxUtils.cpp with the following
/// modification:
///
/// * logs are taken in base 2 instead of base e.
pub fn clamp_to_scale_factor(val: f32, round_down: bool) -> f32 {
    const SCALE_RESOLUTION: f32 = 2.0;

    let val = val.abs();

    let (val, inverse) = if val < 1.0 {
        (1.0 / val, true)
    } else {
        (val, false)
    };

    let power = val.log2() / SCALE_RESOLUTION.log2();

    let power = if (power - power.round()).abs() < 1e-5 {
        power.round()
    } else if inverse != round_down {
        power.floor()
    } else {
        power.ceil()
    };

    let scale = SCALE_RESOLUTION.powf(power);

    if inverse {
        1.0 / scale
    } else {
        scale
    }
}

/// Rounds a value up to the nearest multiple of mul
pub fn round_up_to_multiple(val: usize, mul: NonZeroUsize) -> usize {
    match val % mul.get() {
        0 => val,
        rem => val - rem + mul.get(),
    }
}


#[macro_export]
macro_rules! c_str {
    ($lit:expr) => {
        unsafe {
            std::ffi::CStr::from_ptr(concat!($lit, "\0").as_ptr()
                                     as *const std::os::raw::c_char)
        }
    }
}

/// This is inspired by the `weak-table` crate.
/// It holds a Vec of weak pointers that are garbage collected as the Vec
pub struct WeakTable {
    inner: Vec<std::sync::Weak<Vec<u8>>>
}

impl WeakTable {
    pub fn new() -> WeakTable {
        WeakTable { inner: Vec::new() }
    }
    pub fn insert(&mut self, x: std::sync::Weak<Vec<u8>>) {
        if self.inner.len() == self.inner.capacity() {
            self.remove_expired();

            if self.inner.len() * 3 < self.inner.capacity() {
                self.inner.shrink_to_fit();
            } else {
                self.inner.reserve(self.inner.len())
            }
        }
        self.inner.push(x);
    }

    fn remove_expired(&mut self) {
        self.inner.retain(|x| x.strong_count() > 0)
    }

    pub fn iter(&self) -> impl Iterator<Item = Arc<Vec<u8>>> + '_ {
        self.inner.iter().filter_map(|x| x.upgrade())
    }
}
