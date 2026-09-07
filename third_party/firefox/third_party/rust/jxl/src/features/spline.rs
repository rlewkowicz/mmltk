// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use std::{
    f32::consts::{FRAC_1_SQRT_2, PI, SQRT_2},
    iter::{self, zip},
    ops,
};

use crate::{
    bit_reader::BitReader,
    entropy_coding::decode::{Histograms, SymbolReader, unpack_signed},
    error::{Error, Result},
    frame::color_correlation_map::ColorCorrelationParams,
    util::{CeilLog2, NewWithCapacity, fast_cos, fast_erff_simd, tracing_wrappers::*},
};
use jxl_simd::{F32SimdVec, ScalarDescriptor, SimdDescriptor, simd_function};
const MAX_NUM_CONTROL_POINTS: u32 = 1 << 20;
const MAX_NUM_CONTROL_POINTS_PER_PIXEL_RATIO: u32 = 2;
const DELTA_LIMIT: i64 = 1 << 30;
const SPLINE_POS_LIMIT: isize = 1 << 23;

const QUANTIZATION_ADJUSTMENT_CONTEXT: usize = 0;
const STARTING_POSITION_CONTEXT: usize = 1;
const NUM_SPLINES_CONTEXT: usize = 2;
const NUM_CONTROL_POINTS_CONTEXT: usize = 3;
const CONTROL_POINTS_CONTEXT: usize = 4;
const DCT_CONTEXT: usize = 5;
const NUM_SPLINE_CONTEXTS: usize = 6;
const DESIRED_RENDERING_DISTANCE: f32 = 1.0;

#[derive(Debug, Clone, Copy, Default)]
pub struct Point {
    pub x: f32,
    pub y: f32,
}

impl Point {
    fn new(x: f32, y: f32) -> Self {
        Point { x, y }
    }
    fn abs(&self) -> f32 {
        self.x.hypot(self.y)
    }
}

impl PartialEq for Point {
    fn eq(&self, other: &Self) -> bool {
        (self.x - other.x).abs() < 1e-3 && (self.y - other.y).abs() < 1e-3
    }
}

impl ops::Add<Point> for Point {
    type Output = Point;
    fn add(self, rhs: Point) -> Point {
        Point {
            x: self.x + rhs.x,
            y: self.y + rhs.y,
        }
    }
}

impl ops::Sub<Point> for Point {
    type Output = Point;
    fn sub(self, rhs: Point) -> Point {
        Point {
            x: self.x - rhs.x,
            y: self.y - rhs.y,
        }
    }
}

impl ops::Mul<f32> for Point {
    type Output = Point;
    fn mul(self, rhs: f32) -> Point {
        Point {
            x: self.x * rhs,
            y: self.y * rhs,
        }
    }
}

impl ops::Div<f32> for Point {
    type Output = Point;
    fn div(self, rhs: f32) -> Point {
        let inv = 1.0 / rhs;
        Point {
            x: self.x * inv,
            y: self.y * inv,
        }
    }
}

#[derive(Default, Debug)]
pub struct Spline {
    control_points: Vec<Point>,
    color_dct: [Dct32; 3],
    sigma_dct: Dct32,
    estimated_area_reached: u64,
}

impl Spline {
    pub fn validate_adjacent_point_coincidence(&self) -> Result<()> {
        if let Some(((index, p0), p1)) = zip(
            self.control_points
                .iter()
                .take(self.control_points.len() - 1)
                .enumerate(),
            self.control_points.iter().skip(1),
        )
        .find(|((_, p0), p1)| **p0 == **p1)
        {
            return Err(Error::SplineAdjacentCoincidingControlPoints(
                index,
                *p0,
                index + 1,
                *p1,
            ));
        }
        Ok(())
    }
}

#[derive(Debug, Default, Clone)]
pub struct QuantizedSpline {
    pub control_points: Vec<(i64, i64)>,
    pub color_dct: [[i32; 32]; 3],
    pub sigma_dct: [i32; 32],
}

fn inv_adjusted_quant(adjustment: i32) -> f32 {
    if adjustment >= 0 {
        1.0 / (1.0 + 0.125 * adjustment as f32)
    } else {
        1.0 - 0.125 * adjustment as f32
    }
}

fn validate_spline_point_pos<T: num_traits::ToPrimitive>(x: T, y: T) -> Result<()> {
    let xi = x.to_i32().unwrap();
    let yi = y.to_i32().unwrap();
    let ok_range = -(1i32 << 23)..(1i32 << 23);
    if !ok_range.contains(&xi) {
        return Err(Error::SplinesPointOutOfRange(
            Point {
                x: xi as f32,
                y: yi as f32,
            },
            xi,
            ok_range,
        ));
    }
    if !ok_range.contains(&yi) {
        return Err(Error::SplinesPointOutOfRange(
            Point {
                x: xi as f32,
                y: yi as f32,
            },
            yi,
            ok_range,
        ));
    }
    Ok(())
}

const CHANNEL_WEIGHT: [f32; 4] = [0.0042, 0.075, 0.07, 0.3333];

fn area_limit(image_size: u64) -> u64 {
    1024u64
        .saturating_mul(image_size)
        .saturating_add(1u64 << 32)
        .min(1u64 << 42)
}

impl QuantizedSpline {
    #[instrument(level = "debug", skip(br), ret, err)]
    pub fn read(
        br: &mut BitReader,
        splines_histograms: &Histograms,
        splines_reader: &mut SymbolReader,
        max_control_points: u32,
        total_num_control_points: &mut u32,
    ) -> Result<QuantizedSpline> {
        let num_control_points =
            splines_reader.read_unsigned(splines_histograms, br, NUM_CONTROL_POINTS_CONTEXT);
        *total_num_control_points += num_control_points;
        if *total_num_control_points > max_control_points {
            return Err(Error::SplinesTooManyControlPoints(
                *total_num_control_points,
                max_control_points,
            ));
        }
        let mut control_points = Vec::new_with_capacity(num_control_points as usize)?;
        for _ in 0..num_control_points {
            let x =
                splines_reader.read_signed(splines_histograms, br, CONTROL_POINTS_CONTEXT) as i64;
            let y =
                splines_reader.read_signed(splines_histograms, br, CONTROL_POINTS_CONTEXT) as i64;
            control_points.push((x, y));
            let max_delta_delta = x.abs().max(y.abs());
            if max_delta_delta >= DELTA_LIMIT {
                return Err(Error::SplinesDeltaLimit(max_delta_delta, DELTA_LIMIT));
            }
        }
        let mut color_dct = [[0; 32]; 3];
        let mut sigma_dct = [0; 32];

        let mut decode_dct = |dct: &mut [i32; 32]| -> Result<()> {
            for value in dct.iter_mut() {
                *value = splines_reader.read_signed(splines_histograms, br, DCT_CONTEXT);
            }
            Ok(())
        };

        for channel in &mut color_dct {
            decode_dct(channel)?;
        }
        decode_dct(&mut sigma_dct)?;

        Ok(QuantizedSpline {
            control_points,
            color_dct,
            sigma_dct,
        })
    }

    pub fn dequantize(
        &self,
        starting_point: &Point,
        quantization_adjustment: i32,
        y_to_x: f32,
        y_to_b: f32,
        image_size: u64,
    ) -> Result<Spline> {
        let area_limit = area_limit(image_size);

        let mut result = Spline {
            control_points: Vec::new_with_capacity(self.control_points.len() + 1)?,
            ..Default::default()
        };

        let px = starting_point.x.round();
        let py = starting_point.y.round();
        validate_spline_point_pos(px, py)?;

        let mut current_x = px as i32;
        let mut current_y = py as i32;
        result
            .control_points
            .push(Point::new(current_x as f32, current_y as f32));

        let mut current_delta_x = 0i32;
        let mut current_delta_y = 0i32;
        let mut manhattan_distance = 0u64;

        for &(dx, dy) in &self.control_points {
            current_delta_x += dx as i32;
            current_delta_y += dy as i32;
            validate_spline_point_pos(current_delta_x, current_delta_y)?;

            manhattan_distance +=
                current_delta_x.unsigned_abs() as u64 + current_delta_y.unsigned_abs() as u64;

            if manhattan_distance > area_limit {
                return Err(Error::SplinesDistanceTooLarge(
                    manhattan_distance,
                    area_limit,
                ));
            }

            current_x += current_delta_x;
            current_y += current_delta_y;
            validate_spline_point_pos(current_x, current_y)?;

            result
                .control_points
                .push(Point::new(current_x as f32, current_y as f32));
        }

        let inv_quant = inv_adjusted_quant(quantization_adjustment);

        for (c, weight) in CHANNEL_WEIGHT.iter().enumerate().take(3) {
            for i in 0..32 {
                let inv_dct_factor = if i == 0 { FRAC_1_SQRT_2 } else { 1.0 };
                result.color_dct[c].0[i] =
                    self.color_dct[c][i] as f32 * inv_dct_factor * weight * inv_quant;
            }
        }

        for i in 0..32 {
            result.color_dct[0].0[i] += y_to_x * result.color_dct[1].0[i];
            result.color_dct[2].0[i] += y_to_b * result.color_dct[1].0[i];
        }

        let mut width_estimate = 0;
        let mut color = [0u64; 3];

        for (c, color_val) in color.iter_mut().enumerate() {
            for i in 0..32 {
                *color_val += (inv_quant * self.color_dct[c][i].abs() as f32).ceil() as u64;
            }
        }

        color[0] += y_to_x.abs().ceil() as u64 * color[1];
        color[2] += y_to_b.abs().ceil() as u64 * color[1];

        let max_color = color[0].max(color[1]).max(color[2]);
        let logcolor = 1u64.max((1u64 + max_color).ceil_log2());

        let weight_limit =
            (((area_limit as f32 / logcolor as f32) / manhattan_distance.max(1) as f32).sqrt())
                .ceil();

        for i in 0..32 {
            let inv_dct_factor = if i == 0 { FRAC_1_SQRT_2 } else { 1.0 };
            result.sigma_dct.0[i] =
                self.sigma_dct[i] as f32 * inv_dct_factor * CHANNEL_WEIGHT[3] * inv_quant;

            let weight_f = (inv_quant * self.sigma_dct[i].abs() as f32).ceil();
            let weight = weight_limit.min(weight_f.max(1.0)) as u64;
            width_estimate += weight * weight * logcolor;
        }

        result.estimated_area_reached = width_estimate * manhattan_distance;

        Ok(result)
    }
}

#[derive(Debug, Clone, Copy, Default)]
struct SplineSegment {
    center_x: f32,
    center_y: f32,
    maximum_distance: f32,
    inv_sigma: f32,
    sigma_over_4_times_intensity: f32,
    color: [f32; 3],
}

#[derive(Debug, Default, Clone)]
pub struct Splines {
    pub quantization_adjustment: i32,
    pub splines: Vec<QuantizedSpline>,
    pub starting_points: Vec<Point>,
    segments: Vec<SplineSegment>,
    segment_indices: Vec<usize>,
    segment_y_start: Vec<u64>,
}

fn draw_centripetal_catmull_rom_spline(points: &[Point]) -> Result<Vec<Point>> {
    if points.is_empty() {
        return Ok(vec![]);
    }
    if points.len() == 1 {
        return Ok(vec![points[0]]);
    }
    const NUM_POINTS: usize = 16;
    let extended_points = iter::once(points[0] + (points[0] - points[1]))
        .chain(points.iter().cloned())
        .chain(iter::once(
            points[points.len() - 1] + (points[points.len() - 1] - points[points.len() - 2]),
        ));
    let points_and_deltas = extended_points
        .chain(iter::once(Point::default()))
        .scan(Point::default(), |previous, p| {
            let result = Some((*previous, (p - *previous).abs().sqrt()));
            *previous = p;
            result
        })
        .skip(1);
    let windowed_points = points_and_deltas
        .scan([(Point::default(), 0.0); 4], |window, p| {
            (window[0], window[1], window[2], window[3]) =
                (window[1], window[2], window[3], (p.0, p.1));
            Some([window[0], window[1], window[2], window[3]])
        })
        .skip(3);
    let result = windowed_points
        .flat_map(|p| {
            let mut window_result = [Point::default(); NUM_POINTS];
            window_result[0] = p[1].0;
            let mut t = [0.0; 4];
            for k in 0..3 {
                t[k + 1] = t[k] + p[k].1;
            }
            for (i, window_point) in window_result.iter_mut().enumerate().skip(1) {
                let tt = p[0].1 + ((i as f32) / (NUM_POINTS as f32)) * p[1].1;
                let mut a = [Point::default(); 3];
                for k in 0..3 {
                    a[k] = p[k].0 + (p[k + 1].0 - p[k].0) * ((tt - t[k]) / p[k].1);
                }
                let mut b = [Point::default(); 2];
                for k in 0..2 {
                    b[k] = a[k] + (a[k + 1] - a[k]) * ((tt - t[k]) / (p[k].1 + p[k + 1].1));
                }
                *window_point = b[0] + (b[1] - b[0]) * ((tt - t[1]) / p[1].1);
            }
            window_result
        })
        .chain(iter::once(points[points.len() - 1]))
        .collect();
    Ok(result)
}

fn for_each_equally_spaced_point<F: FnMut(Point, f32)>(
    points: &[Point],
    desired_distance: f32,
    mut f: F,
) {
    if points.is_empty() {
        return;
    }
    let mut accumulated_distance = 0.0;
    f(points[0], desired_distance);
    if points.len() == 1 {
        return;
    }
    for index in 0..(points.len() - 1) {
        let mut current = points[index];
        let next = points[index + 1];
        let segment = next - current;
        let segment_length = segment.abs();
        let unit_step = segment / segment_length;
        if accumulated_distance + segment_length >= desired_distance {
            current = current + unit_step * (desired_distance - accumulated_distance);
            f(current, desired_distance);
            accumulated_distance -= desired_distance;
        }
        accumulated_distance += segment_length;
        while accumulated_distance >= desired_distance {
            current = current + unit_step * desired_distance;
            f(current, desired_distance);
            accumulated_distance -= desired_distance;
        }
    }
    f(points[points.len() - 1], accumulated_distance);
}

/// Precomputed multipliers for DCT: PI / 32.0 * i for i in 0..32
const DCT_MULTIPLIERS: [f32; 32] = [
    PI / 32.0 * 0.0,
    PI / 32.0 * 1.0,
    PI / 32.0 * 2.0,
    PI / 32.0 * 3.0,
    PI / 32.0 * 4.0,
    PI / 32.0 * 5.0,
    PI / 32.0 * 6.0,
    PI / 32.0 * 7.0,
    PI / 32.0 * 8.0,
    PI / 32.0 * 9.0,
    PI / 32.0 * 10.0,
    PI / 32.0 * 11.0,
    PI / 32.0 * 12.0,
    PI / 32.0 * 13.0,
    PI / 32.0 * 14.0,
    PI / 32.0 * 15.0,
    PI / 32.0 * 16.0,
    PI / 32.0 * 17.0,
    PI / 32.0 * 18.0,
    PI / 32.0 * 19.0,
    PI / 32.0 * 20.0,
    PI / 32.0 * 21.0,
    PI / 32.0 * 22.0,
    PI / 32.0 * 23.0,
    PI / 32.0 * 24.0,
    PI / 32.0 * 25.0,
    PI / 32.0 * 26.0,
    PI / 32.0 * 27.0,
    PI / 32.0 * 28.0,
    PI / 32.0 * 29.0,
    PI / 32.0 * 30.0,
    PI / 32.0 * 31.0,
];

/// Precomputed cosine values for DCT at a given t value.
/// Computed once and reused for all 4 DCT evaluations (3 color channels + sigma).
struct PrecomputedCosines([f32; 32]);

impl PrecomputedCosines {
    /// Precompute cosines for a given t value.
    /// Call this once per point, then use with continuous_idct_fast for each DCT.
    #[inline]
    fn new(t: f32) -> Self {
        let tandhalf = t + 0.5;
        PrecomputedCosines(core::array::from_fn(|i| {
            fast_cos(DCT_MULTIPLIERS[i] * tandhalf)
        }))
    }
}

#[derive(Default, Clone, Copy, Debug)]
struct Dct32([f32; 32]);

impl Dct32 {
    /// Fast continuous IDCT using precomputed cosines.
    /// This avoids recomputing 32 cosines for each of the 4 DCT calls per point.
    #[inline]
    fn continuous_idct_fast(&self, precomputed: &PrecomputedCosines) -> f32 {
        zip(self.0, precomputed.0)
            .map(|(coeff, cos)| coeff * cos)
            .sum::<f32>()
            * SQRT_2
    }
}

#[inline(always)]
fn draw_segment_inner<D: SimdDescriptor>(
    d: D,
    row: &mut [&mut [f32]],
    row_pos: (usize, usize),
    x_range: (usize, usize),
    segment: &SplineSegment,
) -> usize {
    let (x_start, x_end) = x_range;
    let (row_x0, y) = row_pos;
    let len = D::F32Vec::LEN;
    if x_start + len > x_end {
        return x_start;
    }

    let inv_sigma = D::F32Vec::splat(d, segment.inv_sigma);
    let half = D::F32Vec::splat(d, 0.5);
    let one_over_2s2 = D::F32Vec::splat(d, 0.353_553_38);
    let sigma_over_4_times_intensity = D::F32Vec::splat(d, segment.sigma_over_4_times_intensity);
    let center_x = D::F32Vec::splat(d, segment.center_x);
    let center_y = D::F32Vec::splat(d, segment.center_y);
    let dy = D::F32Vec::splat(d, y as f32) - center_y;
    let dy2 = dy * dy;

    let mut x_base_arr = [0.0f32; 16];
    for (i, val) in x_base_arr.iter_mut().enumerate() {
        *val = i as f32;
    }
    let vx_base = D::F32Vec::load(d, &x_base_arr);

    let start_offset = x_start - row_x0;
    let end_offset = x_end - row_x0;

    let [r0, r1, r2] = row else { unreachable!() };

    let mut it0 = r0[start_offset..end_offset].chunks_exact_mut(len);
    let mut it1 = r1[start_offset..end_offset].chunks_exact_mut(len);
    let mut it2 = r2[start_offset..end_offset].chunks_exact_mut(len);

    let cm0 = D::F32Vec::splat(d, segment.color[0]);
    let cm1 = D::F32Vec::splat(d, segment.color[1]);
    let cm2 = D::F32Vec::splat(d, segment.color[2]);

    let num_chunks = (end_offset - start_offset) / len;
    let mut x = x_start;
    for _ in 0..num_chunks {
        let vx = D::F32Vec::splat(d, x as f32) + vx_base;
        let dx = vx - center_x;
        let sqd = dx.mul_add(dx, dy2);
        let distance = sqd.sqrt();

        let arg1 = distance.mul_add(half, one_over_2s2) * inv_sigma;
        let arg2 = distance.mul_add(half, D::F32Vec::splat(d, -0.353_553_38)) * inv_sigma;
        let one_dimensional_factor = fast_erff_simd(d, arg1) - fast_erff_simd(d, arg2);
        let local_intensity =
            sigma_over_4_times_intensity * one_dimensional_factor * one_dimensional_factor;

        let c0 = it0.next().unwrap();
        cm0.mul_add(local_intensity, D::F32Vec::load(d, c0))
            .store(c0);
        let c1 = it1.next().unwrap();
        cm1.mul_add(local_intensity, D::F32Vec::load(d, c1))
            .store(c1);
        let c2 = it2.next().unwrap();
        cm2.mul_add(local_intensity, D::F32Vec::load(d, c2))
            .store(c2);

        x += len;
    }
    x
}

simd_function!(
    draw_segment_dispatch,
    d: D,
    fn draw_segment_simd(
        row: &mut [&mut [f32]],
        row_pos: (usize, usize),
        xsize: usize,
        segment: &SplineSegment,
    ) {
        let (x0, y) = row_pos;
        let x1 = x0 + xsize;
        let clamped_x0 = x0.max((segment.center_x - segment.maximum_distance).round() as usize);
        let clamped_x1 = x1.min((segment.center_x + segment.maximum_distance).round() as usize + 1);

        if clamped_x1 <= clamped_x0 {
            return;
        }

        let x = clamped_x0;
        let x = draw_segment_inner(d, row, (x0, y), (x, clamped_x1), segment);
        let d = d.maybe_downgrade_256bit();
        let x = draw_segment_inner(d, row, (x0, y), (x, clamped_x1), segment);
        let d = d.maybe_downgrade_128bit();
        let x = draw_segment_inner(d, row, (x0, y), (x, clamped_x1), segment);
        draw_segment_inner(ScalarDescriptor, row, (x0, y), (x, clamped_x1), segment);
    }
);

impl Splines {
    pub fn is_initialized(&self) -> bool {
        !self.segment_y_start.is_empty()
    }

    pub fn draw_segments(&self, row: &mut [&mut [f32]], row_pos: (usize, usize), xsize: usize) {
        let first_segment_index_pos = self.segment_y_start[row_pos.1];
        let last_segment_index_pos = self.segment_y_start[row_pos.1 + 1];
        for segment_index_pos in first_segment_index_pos..last_segment_index_pos {
            draw_segment_dispatch(
                row,
                row_pos,
                xsize,
                &self.segments[self.segment_indices[segment_index_pos as usize]],
            );
        }
    }

    fn add_segment(
        &mut self,
        center: &Point,
        intensity: f32,
        color: [f32; 3],
        sigma: f32,
        high_precision: bool,
        segments_by_y: &mut Vec<(u64, usize)>,
    ) {
        if sigma.is_infinite()
            || sigma == 0.0
            || (1.0 / sigma).is_infinite()
            || intensity.is_infinite()
        {
            return;
        }
        let distance_exp: f32 = if high_precision { 5.0 } else { 3.0 };
        let max_color = [0.01, color[0], color[1], color[2]]
            .iter()
            .map(|chan| (chan * intensity).abs())
            .max_by(|a, b| a.total_cmp(b))
            .unwrap();
        let max_distance =
            (-2.0 * sigma * sigma * (0.1f32.ln() * distance_exp - max_color.ln())).sqrt();
        let segment = SplineSegment {
            center_x: center.x,
            center_y: center.y,
            color,
            inv_sigma: 1.0 / sigma,
            sigma_over_4_times_intensity: 0.25 * sigma * intensity,
            maximum_distance: max_distance,
        };
        let y0 = (center.y - max_distance).round() as i64;
        let y1 = (center.y + max_distance).round() as i64 + 1;
        for y in 0.max(y0)..y1 {
            segments_by_y.push((y as u64, self.segments.len()));
        }
        self.segments.push(segment);
    }

    fn add_segments_from_points(
        &mut self,
        spline: &Spline,
        points_to_draw: &[(Point, f32)],
        length: f32,
        desired_distance: f32,
        high_precision: bool,
        segments_by_y: &mut Vec<(u64, usize)>,
    ) {
        let inv_length = 1.0 / length;
        for (point_index, (point, multiplier)) in points_to_draw.iter().enumerate() {
            let progress = (point_index as f32 * desired_distance * inv_length).min(1.0);
            let t = (32.0 - 1.0) * progress;

            let precomputed = PrecomputedCosines::new(t);

            let mut color = [0.0; 3];
            for (index, coeffs) in spline.color_dct.iter().enumerate() {
                color[index] = coeffs.continuous_idct_fast(&precomputed);
            }
            let sigma = spline.sigma_dct.continuous_idct_fast(&precomputed);

            self.add_segment(
                point,
                *multiplier,
                color,
                sigma,
                high_precision,
                segments_by_y,
            );
        }
    }

    pub fn initialize_draw_cache(
        &mut self,
        image_xsize: u64,
        image_ysize: u64,
        color_correlation_params: &ColorCorrelationParams,
        high_precision: bool,
    ) -> Result<()> {
        let mut total_estimated_area_reached = 0u64;
        let mut splines = Vec::new();
        let image_area = image_xsize.saturating_mul(image_ysize);
        let area_limit = area_limit(image_area);
        for (index, qspline) in self.splines.iter().enumerate() {
            let spline = qspline.dequantize(
                &self.starting_points[index],
                self.quantization_adjustment,
                color_correlation_params.y_to_x_lf(),
                color_correlation_params.y_to_b_lf(),
                image_area,
            )?;
            total_estimated_area_reached += spline.estimated_area_reached;
            if total_estimated_area_reached > area_limit {
                return Err(Error::SplinesAreaTooLarge(
                    total_estimated_area_reached,
                    area_limit,
                ));
            }
            spline.validate_adjacent_point_coincidence()?;
            splines.push(spline);
        }

        if total_estimated_area_reached
            > (8 * image_xsize * image_ysize + (1u64 << 25)).min(1u64 << 30)
        {
            warn!(
                "Large total_estimated_area_reached, expect slower decoding:{}",
                total_estimated_area_reached
            );
        }

        let mut segments_by_y = Vec::new();

        self.segments.clear();
        for spline in splines {
            let mut points_to_draw = Vec::<(Point, f32)>::new();
            let intermediate_points = draw_centripetal_catmull_rom_spline(&spline.control_points)?;
            for_each_equally_spaced_point(
                &intermediate_points,
                DESIRED_RENDERING_DISTANCE,
                |p, d| points_to_draw.push((p, d)),
            );
            let length = (points_to_draw.len() as isize - 2) as f32 * DESIRED_RENDERING_DISTANCE
                + points_to_draw[points_to_draw.len() - 1].1;
            if length <= 0.0 {
                continue;
            }
            self.add_segments_from_points(
                &spline,
                &points_to_draw,
                length,
                DESIRED_RENDERING_DISTANCE,
                high_precision,
                &mut segments_by_y,
            );
        }

        segments_by_y.sort_by_key(|segment| segment.0);

        self.segment_indices.clear();
        self.segment_indices.try_reserve(segments_by_y.len())?;
        self.segment_indices.resize(segments_by_y.len(), 0);

        self.segment_y_start.clear();
        self.segment_y_start.try_reserve(image_ysize as usize + 1)?;
        self.segment_y_start.resize(image_ysize as usize + 1, 0);

        for (i, segment) in segments_by_y.iter().enumerate() {
            self.segment_indices[i] = segment.1;
            let y = segment.0;
            if y < image_ysize {
                self.segment_y_start[y as usize + 1] += 1;
            }
        }
        for y in 0..image_ysize {
            self.segment_y_start[y as usize + 1] += self.segment_y_start[y as usize];
        }
        Ok(())
    }

    #[instrument(level = "debug", skip(br), ret, err)]
    pub fn read(br: &mut BitReader, num_pixels: u32) -> Result<Splines> {
        trace!(pos = br.total_bits_read());
        let splines_histograms = Histograms::decode(NUM_SPLINE_CONTEXTS, br, true)?;
        let mut splines_reader = SymbolReader::new(&splines_histograms, br, None)?;
        let num_splines = splines_reader
            .read_unsigned(&splines_histograms, br, NUM_SPLINES_CONTEXT)
            .saturating_add(1);
        let max_control_points =
            MAX_NUM_CONTROL_POINTS.min(num_pixels / MAX_NUM_CONTROL_POINTS_PER_PIXEL_RATIO);
        if num_splines > max_control_points {
            return Err(Error::SplinesTooMany(num_splines, max_control_points));
        }

        let mut starting_points = Vec::new();
        let mut last_x = 0;
        let mut last_y = 0;
        for i in 0..num_splines {
            let unsigned_x =
                splines_reader.read_unsigned(&splines_histograms, br, STARTING_POSITION_CONTEXT);
            let unsigned_y =
                splines_reader.read_unsigned(&splines_histograms, br, STARTING_POSITION_CONTEXT);

            let (x, y) = if i != 0 {
                (
                    unpack_signed(unsigned_x) as isize + last_x,
                    unpack_signed(unsigned_y) as isize + last_y,
                )
            } else {
                (unsigned_x as isize, unsigned_y as isize)
            };
            let max_coordinate = x.abs().max(y.abs());
            if max_coordinate >= SPLINE_POS_LIMIT {
                return Err(Error::SplinesCoordinatesLimit(
                    max_coordinate,
                    SPLINE_POS_LIMIT,
                ));
            }

            starting_points.push(Point {
                x: x as f32,
                y: y as f32,
            });

            last_x = x;
            last_y = y;
        }

        let quantization_adjustment =
            splines_reader.read_signed(&splines_histograms, br, QUANTIZATION_ADJUSTMENT_CONTEXT);

        let mut splines = Vec::new();
        let mut num_control_points = 0u32;
        for _ in 0..num_splines {
            splines.push(QuantizedSpline::read(
                br,
                &splines_histograms,
                &mut splines_reader,
                max_control_points,
                &mut num_control_points,
            )?);
        }
        splines_reader.check_final_state(&splines_histograms, br)?;
        Ok(Splines {
            quantization_adjustment,
            splines,
            starting_points,
            ..Splines::default()
        })
    }
}
