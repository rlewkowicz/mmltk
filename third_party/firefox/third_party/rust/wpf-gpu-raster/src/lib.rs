/*!
Converts a 2D path into a set of vertices of a triangle strip mesh that represents the antialiased fill of that path.

```rust
    use wpf_gpu_raster::PathBuilder;
    let mut p = PathBuilder::new();
    p.move_to(10., 10.);
    p.line_to(40., 10.);
    p.line_to(40., 40.);
    let result = p.rasterize_to_tri_list(0, 0, 100, 100);
```

*/
#![allow(unused_parens)]
#![allow(overflowing_literals)]
#![allow(non_snake_case)]
#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]
#![allow(dead_code)]
#![allow(unused_macros)]

#[macro_use]
mod fix;
#[macro_use]
mod helpers;
#[macro_use]
mod real;
mod bezier;
#[macro_use]
mod aarasterizer;
mod hwrasterizer;
mod aacoverage;
mod hwvertexbuffer;

mod types;
mod geometry_sink;
mod matrix;

mod nullable_ref;

#[cfg(feature = "c_bindings")]
pub mod c_bindings;


use aarasterizer::CheckValidRange28_4;
use hwrasterizer::CHwRasterizer;
use hwvertexbuffer::{CHwVertexBuffer, CHwVertexBufferBuilder};
use real::CFloatFPU;
use types::{MilFillMode, PathPointTypeStart, MilPoint2F, MilPointAndSizeL, PathPointTypeLine, MilVertexFormat, MilVertexFormatAttribute, DynArray, BYTE, PathPointTypeBezier, PathPointTypeCloseSubpath, CMILSurfaceRect, POINT};

#[repr(C)]
#[derive(Clone, Debug, Default)]
pub struct OutputVertex {
    pub x: f32,
    pub y: f32,
    pub coverage: f32
}

#[repr(C)]
#[derive(Copy, Clone)]
pub enum FillMode {
    EvenOdd = 0,
    Winding = 1,
}

impl Default for FillMode {
    fn default() -> Self {
        FillMode::EvenOdd
    }
}

#[derive(Clone, Default)]
pub struct OutputPath {
    fill_mode: FillMode,
    points: Box<[POINT]>,
    types: Box<[BYTE]>,
}

impl std::hash::Hash for OutputVertex {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        self.x.to_bits().hash(state);
        self.y.to_bits().hash(state);
        self.coverage.to_bits().hash(state);
    }
}

pub struct PathBuilder {
    points: DynArray<POINT>,
    types: DynArray<BYTE>,
    initial_point: Option<MilPoint2F>,
    current_point: Option<MilPoint2F>,
    in_shape: bool,
    fill_mode: FillMode,
    outside_bounds: Option<CMILSurfaceRect>,
    need_inside: bool,
    valid_range: bool,
    rasterization_truncates: bool,
}

impl PathBuilder {
    pub fn new() -> Self {
        Self {
            points: Vec::new(),
            types: Vec::new(),
            initial_point: None,
            current_point: None,
            in_shape: false,
            fill_mode: FillMode::EvenOdd,
            outside_bounds: None,
            need_inside: true,
            valid_range: true,
            rasterization_truncates: false,
        }
    }
    fn reset(&mut self) {
        *self = Self {
            points: std::mem::take(&mut self.points),
            types: std::mem::take(&mut self.types),
            ..Self::new()
        };
        self.points.clear();
        self.types.clear();
    }
    fn add_point(&mut self, x: f32, y: f32) {
        self.current_point = Some(MilPoint2F{X: x, Y: y});
        let (x, y) = ((x - 0.5) * 16.0, (y - 0.5) * 16.0);
        self.valid_range = self.valid_range && CheckValidRange28_4(x, y);
        self.points.push(POINT {
            x: CFloatFPU::Round(x),
            y: CFloatFPU::Round(y),
        });
    }
    pub fn line_to(&mut self, x: f32, y: f32) {
        if let Some(initial_point) = self.initial_point {
            if !self.in_shape {
                self.types.push(PathPointTypeStart);
                self.add_point(initial_point.X, initial_point.Y);
                self.in_shape = true;
            }
            self.types.push(PathPointTypeLine);
            self.add_point(x, y);
        } else {
            self.initial_point = Some(MilPoint2F{X: x, Y: y})
        }
    }
    pub fn move_to(&mut self, x: f32, y: f32) {
        self.in_shape = false;
        self.initial_point = Some(MilPoint2F{X: x, Y: y});
        self.current_point = self.initial_point;
    }
    pub fn curve_to(&mut self, c1x: f32, c1y: f32, c2x: f32, c2y: f32, x: f32, y: f32) {
        let initial_point = match self.initial_point {
            Some(initial_point) => initial_point,
            None => MilPoint2F{X:c1x, Y:c1y}
        };
        if !self.in_shape {
            self.types.push(PathPointTypeStart);
            self.add_point(initial_point.X, initial_point.Y);
            self.initial_point = Some(initial_point);
            self.in_shape = true;
        }
        self.types.push(PathPointTypeBezier);
        self.add_point(c1x, c1y);
        self.add_point(c2x, c2y);
        self.add_point(x, y);
    }
    pub fn quad_to(&mut self, cx: f32, cy: f32, x: f32, y: f32) {
        let c0 = match self.current_point {
            Some(current_point) => current_point,
            None => MilPoint2F{X:cx, Y:cy}
        };

        let c1x = c0.X + (2./3.) * (cx - c0.X);
        let c1y = c0.Y + (2./3.) * (cy - c0.Y);

        let c2x = x + (2./3.) * (cx - x);
        let c2y = y + (2./3.) * (cy - y);

        self.curve_to(c1x, c1y, c2x, c2y, x, y);
    }
    pub fn close(&mut self) {
        if self.in_shape {
          if let Some(last) = self.types.last_mut() {
              *last |= PathPointTypeCloseSubpath;
          }
          self.in_shape = false;
        }
        self.current_point = self.initial_point;
    }
    pub fn set_fill_mode(&mut self, fill_mode: FillMode) {
        self.fill_mode = fill_mode;
    }
    /// Enables rendering geometry for areas outside the shape but
    /// within the bounds.  These areas will be created with
    /// zero alpha.
    ///
    /// This is useful for creating geometry for other blend modes.
    /// For example:
    /// - `IN(dest, geometry)` can be done with `outside_bounds` and `need_inside = false`
    /// - `IN(dest, geometry, alpha)` can be done with `outside_bounds` and `need_inside = true`
    ///
    /// Note: trapezoidal areas won't be clipped to outside_bounds
    pub fn set_outside_bounds(&mut self, outside_bounds: Option<(i32, i32, i32, i32)>, need_inside: bool) {
        self.outside_bounds = outside_bounds.map(|r| CMILSurfaceRect { left: r.0, top: r.1, right: r.2, bottom: r.3 });
        self.need_inside = need_inside;
    }

    /// Set this to true if post vertex shader coordinates are converted to fixed point
    /// via truncation. This has been observed with OpenGL on AMD GPUs on macOS. 
    pub fn set_rasterization_truncates(&mut self, rasterization_truncates: bool) {
        self.rasterization_truncates = rasterization_truncates;
    }

    /// Note: trapezoidal areas won't necessarily be clipped to the clip rect
    pub fn rasterize_to_tri_list(&self, clip_x: i32, clip_y: i32, clip_width: i32, clip_height: i32) -> Box<[OutputVertex]> {
        if !self.valid_range {
            return Box::new([]);
        }
        let (x, y, width, height, need_outside) = if let Some(CMILSurfaceRect { left, top, right, bottom }) = self.outside_bounds {
            let x0 = clip_x.max(left);
            let y0 = clip_y.max(top);
            let x1 = (clip_x + clip_width).min(right);
            let y1 = (clip_y + clip_height).min(bottom);
            (x0, y0, x1 - x0, y1 - y0, true)
        } else {
            (clip_x, clip_y, clip_width, clip_height, false)
        };
        rasterize_to_tri_list(self.fill_mode, &self.types, &self.points, x, y, width, height, self.need_inside, need_outside, self.rasterization_truncates, None)
            .flush_output()
    }

    pub fn get_path(&mut self) -> Option<OutputPath> {
        if self.valid_range && !self.points.is_empty() && !self.types.is_empty() {
            Some(OutputPath {
                fill_mode: self.fill_mode,
                points: Box::from(self.points.as_slice()),
                types: Box::from(self.types.as_slice()),
            })
        } else {
            None
        }
    }
}

pub fn rasterize_to_tri_list<'a>(
    fill_mode: FillMode,
    types: &[BYTE],
    points: &[POINT],
    clip_x: i32,
    clip_y: i32,
    clip_width: i32,
    clip_height: i32,
    need_inside: bool,
    need_outside: bool,
    rasterization_truncates: bool,
    output_buffer: Option<&'a mut [OutputVertex]>,
) -> CHwVertexBuffer<'a> {
    let clipRect = MilPointAndSizeL {
        X: clip_x,
        Y: clip_y,
        Width: clip_width,
        Height: clip_height,
    };

    let mil_fill_mode = match fill_mode {
        FillMode::EvenOdd => MilFillMode::Alternate,
        FillMode::Winding => MilFillMode::Winding,
    };

    let m_mvfIn: MilVertexFormat = MilVertexFormatAttribute::MILVFAttrXY as MilVertexFormat;
    let m_mvfGenerated: MilVertexFormat  = MilVertexFormatAttribute::MILVFAttrNone as MilVertexFormat;
    const HWPIPELINE_ANTIALIAS_LOCATION: MilVertexFormatAttribute = MilVertexFormatAttribute::MILVFAttrDiffuse;
    let mvfaAALocation = HWPIPELINE_ANTIALIAS_LOCATION;

    let outside_bounds = if need_outside {
        Some(CMILSurfaceRect {
            left: clip_x,
            top: clip_y,
            right: clip_x + clip_width,
            bottom: clip_y + clip_height,
        })
    } else {
        None
    };

    let mut vertexBuffer = CHwVertexBuffer::new(rasterization_truncates, output_buffer);
    {
        let mut vertexBuilder = CHwVertexBufferBuilder::Create(
            m_mvfIn, m_mvfIn | m_mvfGenerated, mvfaAALocation, &mut vertexBuffer);
        vertexBuilder.SetOutsideBounds(outside_bounds.as_ref(), need_inside);
        vertexBuilder.BeginBuilding();
        {
            let mut rasterizer = CHwRasterizer::new(
                &mut vertexBuilder, mil_fill_mode, None, clipRect);
            rasterizer.SendGeometry(points, types);
        }
        vertexBuilder.EndBuilding();
    }

    vertexBuffer
}
