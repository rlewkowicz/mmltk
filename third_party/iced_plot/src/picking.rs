//! Implements picking for the plot widget.
use std::{
    collections::HashMap,
    sync::{Arc, Mutex, OnceLock, atomic::{AtomicBool, Ordering}},
};

use glam::{DVec2, Vec2};
use iced::Rectangle;
use iced::wgpu::*;

use crate::{Point, PointId, Size, camera::Camera, plot_state::{ProjectionOrigin, SeriesSpan}};

/// Threshold for number of points above which GPU picking is used instead of CPU picking.
pub(crate) const CPU_PICK_THRESHOLD: usize = 5000;

// The GPU ID pass contains markers only. Line-only series still have pickable
// vertices, so both hover and click use CPU picking for those data sets.
fn cpu_picking_required(force: bool, points: usize, series: &[SeriesSpan]) -> bool {
    force || points < CPU_PICK_THRESHOLD
        || series.iter().any(|span| span.pickable && span.marker == u32::MAX)
}

// ---- API to the plot widget ----

/// Tracks CPU/GPU picking state for a plot widget.
#[derive(Debug, Clone)]
pub(crate) struct PickingState {
    projection: MarkerProjection,
    // Shared with the retained pass, independent of this projection's sequences.
    settlement: Option<Arc<AtomicBool>>,
    /// Last hover hit, if any.
    pub(crate) last_hover_cache: Option<PointId>,

    /// When set, the matching GPU result is interpreted as a *pick* (click)
    /// instead of a hover.
    pending_gpu_pick_seq: Option<u64>,

    /// Last submitted GPU request sequence number
    pick_seq: u64,

    /// Last processed GPU result sequence number
    pick_result_seq: u64,
}

#[derive(Debug, Clone, Copy)]
pub(crate) enum HoverRequest {
    /// Immediate CPU result.
    CpuHit(PointId),
    /// Immediate CPU miss.
    CpuMiss,
    /// GPU request was submitted; result will arrive in a later frame.
    RequestedGpu,
}

#[derive(Debug, Clone, Copy)]
pub(crate) enum GpuResultEvent {
    Hover(PointId),
    HoverMiss,
    Pick(PointId),
}

impl PickingState {
    pub(crate) fn new(origin: ProjectionOrigin, generation: u64) -> Self {
        Self {
            projection: MarkerProjection { origin, generation },
            settlement: None,
            last_hover_cache: None,
            pending_gpu_pick_seq: None,
            pick_seq: 0,
            pick_result_seq: 0,
        }
    }

    pub(crate) fn projection(&self) -> &MarkerProjection {
        &self.projection
    }

    /// A rebuilt marker projection invalidates both cached and asynchronous hits.
    /// Existing registry storage is reused; CPU-only plots create no entry.
    pub(crate) fn reproject(&mut self, instance_id: u64, generation: u64) {
        self.projection.generation = generation;
        self.last_hover_cache = None;
        self.pending_gpu_pick_seq = None;
        self.pick_seq = 0;
        self.pick_result_seq = 0;
        self.settlement = None;
        if let Some(registry) = REGISTRY.get()
            && let Some(entry) = registry.lock().unwrap().get_mut(&instance_id)
        {
            entry.activate(&self.projection);
            self.settlement = Some(Arc::clone(&entry.settlement));
        }
    }

    fn submit_gpu_request(&mut self, instance_id: u64, cursor: Vec2, radius_px: f32) {
        self.pick_seq = self.pick_seq.wrapping_add(1);
        self.settlement = Some(submit_request(
            instance_id,
            GpuPickRequest {
                projection: self.projection.clone(),
                cursor_x: cursor.x,
                cursor_y: cursor.y,
                radius_px,
                seq: self.pick_seq,
            },
        ));
    }

    /// Request hover picking for the current cursor position.
    ///
    /// CPU vs GPU is decided internally based on point count.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn request_hover(
        &mut self,
        instance_id: u64,
        cursor: Vec2,
        hover_radius_px: f32,
        force_cpu: bool,
        points: &[Point],
        series: &[SeriesSpan],
        camera: &Camera,
        bounds: &Rectangle,
        valid_point_id: impl Fn(&PointId) -> bool,
    ) -> HoverRequest {
        if cpu_picking_required(force_cpu, points.len(), series) {
            if let Some(point) =
                cpu_pick_hit(points, series, camera, bounds, cursor, hover_radius_px)
                && valid_point_id(&point)
            {
                self.last_hover_cache = Some(point);
                HoverRequest::CpuHit(point)
            } else {
                HoverRequest::CpuMiss
            }
        } else {
            self.submit_gpu_request(instance_id, cursor, hover_radius_px);
            HoverRequest::RequestedGpu
        }
    }

    /// Request a click-to-pick hit at the current cursor position.
    ///
    /// Returns an immediate hit when using cache/CPU. For GPU, submits a request and returns None.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn request_pick_hit(
        &mut self,
        instance_id: u64,
        cursor: Vec2,
        hover_radius_px: f32,
        force_cpu: bool,
        points: &[Point],
        series: &[SeriesSpan],
        camera: &Camera,
        bounds: &Rectangle,
        valid_point_id: impl Fn(&PointId) -> bool,
    ) -> Option<PointId> {
        if let Some(point) = self.last_hover_cache
            && valid_point_id(&point)
        {
            return Some(point);
        }

        if cpu_picking_required(force_cpu, points.len(), series) {
            if let Some(point) =
                cpu_pick_hit(points, series, camera, bounds, cursor, hover_radius_px)
                && valid_point_id(&point)
            {
                return Some(point);
            }
        } else {
            self.submit_gpu_request(instance_id, cursor, hover_radius_px);
            // Mark this seq as a pick request.
            self.pending_gpu_pick_seq = Some(self.pick_seq);
        }
        None
    }

    /// Consume and interpret a GPU pick result (if available).
    pub(crate) fn consume_gpu_result(
        &mut self,
        instance_id: u64,
        valid_point_id: impl Fn(&PointId) -> bool,
    ) -> Option<GpuResultEvent> {
        let res = take_result(instance_id, &self.projection)?;
        if res.seq <= self.pick_result_seq {
            return None;
        }

        let mut out = None;

        if self.pending_gpu_pick_seq == Some(res.seq) {
            self.pending_gpu_pick_seq = None;
            if let Some(point) = res.hit
                && valid_point_id(&point)
            {
                out = Some(GpuResultEvent::Pick(point));
            }
        } else if let Some(point) = res.hit
            && valid_point_id(&point)
        {
            self.last_hover_cache = Some(point);
            out = Some(GpuResultEvent::Hover(point));
        } else {
            out = Some(GpuResultEvent::HoverMiss);
        }

        self.pick_result_seq = res.seq;
        out
    }

    pub(crate) fn has_outstanding_gpu_request(&self) -> bool {
        self.pick_seq > self.pick_result_seq
            || self.settlement.as_ref().is_some_and(|pending| pending.load(Ordering::Acquire))
    }
}

fn marker_center_world(pt: &Point) -> DVec2 {
    let mut world = DVec2::new(pt.position[0], pt.position[1]);
    if pt.size_mode == crate::point::MARKER_SIZE_WORLD {
        let half = pt.size as f64 * 0.5;
        world.x += half;
        world.y += half;
    }
    world
}

fn cpu_pick_hit(
    points: &[Point],
    series: &[SeriesSpan],
    camera: &Camera,
    bounds: &Rectangle,
    cursor: Vec2,
    hover_radius_px: f32,
) -> Option<PointId> {
    if points.is_empty() || series.is_empty() {
        return None;
    }

    let width = bounds.width.max(1.0) as f64;
    let height = bounds.height.max(1.0) as f64;
    let cursor_x = cursor.x as f64;
    let cursor_y = cursor.y as f64;

    let mut span_idx = 0usize;
    let mut span_start = 0usize;
    let mut best: Option<(usize, f64)> = None;

    for (idx, pt) in points.iter().enumerate() {
        while span_idx < series.len() && idx >= span_start + series[span_idx].len {
            span_start += series[span_idx].len;
            span_idx += 1;
        }
        if span_idx >= series.len() {
            break;
        }
        if !series[span_idx].pickable {
            continue;
        }

        let world = marker_center_world(pt);
        let ndc_x = (world.x - camera.position.x) / camera.half_extents.x;
        let ndc_y = (world.y - camera.position.y) / camera.half_extents.y;
        let screen_x = (ndc_x + 1.0) * 0.5 * width;
        let screen_y = (1.0 - ndc_y) * 0.5 * height;

        let dx = screen_x - cursor_x;
        let dy = screen_y - cursor_y;
        let d2 = dx * dx + dy * dy;
        let marker_px = Size::size_px(pt.size, pt.size_mode, camera, bounds) as f64;
        let radius = hover_radius_px as f64 + marker_px * 0.5;
        if d2 <= radius * radius {
            if let Some((_, best_d2)) = best {
                if d2 < best_d2 {
                    best = Some((idx, d2));
                }
            } else {
                best = Some((idx, d2));
            }
        }
    }

    let (best_idx, _) = best?;
    let mut span_idx = 0usize;
    let mut span_start = 0usize;
    while span_idx < series.len() && best_idx >= span_start + series[span_idx].len {
        span_start += series[span_idx].len;
        span_idx += 1;
    }
    let span = series.get(span_idx)?;
    let local_idx = best_idx - span_start;
    let point_index = *span.point_indices.get(local_idx)?;
    Some(PointId {
        series_id: span.id,
        point_index,
    })
}

// ---- GPU picking ----

/// Strong ownership identity plus the marker generation within that tree state.
#[derive(Debug, Clone)]
pub(crate) struct MarkerProjection {
    origin: ProjectionOrigin,
    generation: u64,
}

impl MarkerProjection {
    fn same_as(&self, other: &Self) -> bool {
        self.generation == other.generation && self.origin.same_as(&other.origin)
    }
}

#[derive(Debug, Clone)]
struct GpuPickRequest {
    projection: MarkerProjection,
    pub cursor_x: f32,  // logical px in widget local coordinates
    pub cursor_y: f32,  // logical px in widget local coordinates
    pub radius_px: f32, // logical px
    pub seq: u64,       // monotonically increasing within this projection
}

#[derive(Debug, Clone)]
struct GpuPickResult {
    projection: MarkerProjection,
    pub seq: u64,
    pub hit: Option<PointId>,
}

struct InstanceEntry {
    projection: MarkerProjection,
    settlement: Arc<AtomicBool>,
    latest_req: Option<GpuPickRequest>,
    latest_res: Option<GpuPickResult>,
}

impl InstanceEntry {
    fn new(projection: MarkerProjection) -> Self {
        Self { projection, settlement: Arc::new(AtomicBool::new(false)), latest_req: None, latest_res: None }
    }

    fn activate(&mut self, projection: &MarkerProjection) {
        if !self.projection.same_as(projection) {
            self.projection = projection.clone();
            self.latest_req = None;
            self.latest_res = None;
        }
    }
}

static REGISTRY: OnceLock<Mutex<HashMap<u64, InstanceEntry>>> = OnceLock::new();

fn registry() -> &'static Mutex<HashMap<u64, InstanceEntry>> {
    REGISTRY.get_or_init(|| Mutex::new(HashMap::new()))
}

fn submit_request(instance_id: u64, req: GpuPickRequest) -> Arc<AtomicBool> {
    let mut map = registry().lock().unwrap();
    let entry = map.entry(instance_id)
        .or_insert_with(|| InstanceEntry::new(req.projection.clone()));
    entry.activate(&req.projection);
    // Replace if newer
    if entry.latest_req.as_ref().is_none_or(|r| r.seq < req.seq) {
        entry.latest_req = Some(req);
    }
    Arc::clone(&entry.settlement)
}

fn take_result(instance_id: u64, projection: &MarkerProjection) -> Option<GpuPickResult> {
    let mut map = registry().lock().unwrap();
    let entry = map.get_mut(&instance_id)?;
    entry.projection.same_as(projection).then(|| entry.latest_res.take()).flatten()
}

fn take_latest_request(instance_id: u64, projection: &MarkerProjection) -> Option<(GpuPickRequest, Arc<AtomicBool>)> {
    let mut map = registry().lock().unwrap();
    let entry = map.get_mut(&instance_id)?;
    if !entry.projection.same_as(projection) { return None; }
    entry.latest_req.take().map(|request| (request, Arc::clone(&entry.settlement)))
}

fn publish_result(instance_id: u64, res: GpuPickResult) {
    let mut map = registry().lock().unwrap();
    let Some(entry) = map.get_mut(&instance_id) else { return };
    if !entry.projection.same_as(&res.projection) { return; }
    if entry.latest_res.as_ref().is_none_or(|r| r.seq < res.seq) {
        entry.latest_res = Some(res);
    }
}

// ---- GPU picking pass ----

pub(crate) struct PickingPass {
    // Render target holding u32 IDs
    pick_texture: Option<Texture>,
    pick_view: Option<TextureView>,
    size_w: u32,
    size_h: u32,
    scale_factor: f32,

    // Pipeline for rendering marker IDs
    pipeline: Option<RenderPipeline>,

    // Reusable staging buffer; a pending asynchronous map settles before reuse.
    staging: Option<Buffer>,
    staging_size: u64,

    // Mapping from instance_id (draw instance) -> (span_index, local_pt_index)
    id_map: Vec<(u32, u32)>,
    id_map_projection: Option<MarkerProjection>,

    pending: Option<PendingReadback>,
}

struct PendingReadback {
    // Own the mapped buffer even during pass destruction or map_async unwinding.
    buffer: Buffer,
    unmap_on_drop: bool,
    settlement: Arc<AtomicBool>,
    instance_id: u64,
    projection: MarkerProjection,
    seq: u64,
    needed: u64,
    bytes_per_row: u32,
    max_w: u32,
    max_h: u32,
    min_x: u32,
    min_y: u32,
    cx: u32,
    cy: u32,
    map_status: Arc<Mutex<Option<Result<(), BufferAsyncError>>>>,
}

impl PendingReadback {
    fn unmap(&mut self) {
        let failed = matches!(*self.map_status.lock().unwrap(), Some(Err(_)));
        if std::mem::replace(&mut self.unmap_on_drop, false) && !failed {
            // Release the status lock first: cancelling a pending map may invoke
            // its callback synchronously. Failed maps are already unmapped.
            self.buffer.unmap();
        }
    }
}

impl Drop for PendingReadback {
    fn drop(&mut self) {
        // No mapped view escapes poll_pending. Unmap also cancels an unfinished
        // callback; WGPU retains submitted device work independently.
        self.unmap();
        self.settlement.store(false, Ordering::Release);
    }
}

impl Default for PickingPass {
    fn default() -> Self {
        Self {
            pick_texture: None,
            pick_view: None,
            size_w: 0,
            size_h: 0,
            scale_factor: 1.0,
            pipeline: None,
            staging: None,
            staging_size: 0,
            id_map: Vec::new(),
            id_map_projection: None,
            pending: None,
        }
    }
}

impl PickingPass {
    /// Service a pick request: draw IDs, copy a small region around cursor, and start an async
    /// map/readback. Completion is handled on later frames without blocking.
    /// Publishes a PickResult via the registry.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn service(
        &mut self,
        instance_id: u64,
        device: &Device,
        queue: &Queue,
        camera_bg: &BindGroup,
        camera_bgl: &BindGroupLayout,
        marker_vb: Option<&Buffer>,
        marker_instances: u32,
        projection: &MarkerProjection,
        series: &[SeriesSpan],
    ) {
        self.poll_pending(device, projection, series);

        if self.pending.is_some() {
            return;
        }

        // Take the latest request, if any
        let (req, settlement) = match take_latest_request(instance_id, projection) {
            Some(r) => r,
            None => return,
        };

        if marker_vb.is_none() || marker_instances == 0
            || !self.id_map_projection.as_ref().is_some_and(|key| key.same_as(projection))
        {
            publish_result(
                instance_id,
                GpuPickResult {
                    projection: req.projection,
                    seq: req.seq,
                    hit: None,
                },
            );
            return;
        }

        // Ensure resources
        self.ensure_target(device);
        self.ensure_pipeline(device, camera_bgl);

        let vb = marker_vb.unwrap();
        let view = self.pick_view.as_ref().unwrap();

        // Draw IDs into pick texture
        let mut encoder = device.create_command_encoder(&CommandEncoderDescriptor {
            label: Some("pick encoder"),
        });
        {
            let mut pass = encoder.begin_render_pass(&RenderPassDescriptor {
                label: Some("pick pass"),
                color_attachments: &[Some(RenderPassColorAttachment {
                    view,
                    resolve_target: None,
                    ops: Operations {
                        load: LoadOp::Clear(Color {
                            r: 0.0,
                            g: 0.0,
                            b: 0.0,
                            a: 0.0,
                        }),
                        store: StoreOp::Store,
                    },
                    depth_slice: None,
                })],
                depth_stencil_attachment: None,
                occlusion_query_set: None,
                timestamp_writes: None,
                multiview_mask: None,
            });

            let w = self.size_w as f32;
            let h = self.size_h as f32;
            pass.set_viewport(0.0, 0.0, w, h, 0.0, 1.0);
            pass.set_scissor_rect(0, 0, self.size_w, self.size_h);
            pass.set_pipeline(self.pipeline.as_ref().unwrap());
            pass.set_bind_group(0, camera_bg, &[]);
            pass.set_vertex_buffer(0, vb.slice(..));
            pass.draw(0..4, 0..marker_instances);
        }

        // Compute copy region in device pixels
        let cx = (req.cursor_x * self.scale_factor)
            .round()
            .clamp(0.0, self.size_w as f32 - 1.0) as u32;
        let cy = (req.cursor_y * self.scale_factor)
            .round()
            .clamp(0.0, self.size_h as f32 - 1.0) as u32;
        let r = (req.radius_px * self.scale_factor).ceil() as i32;
        let win = 2 * r + 1;
        let win = win.max(3) as u32;
        // Clamp to texture bounds
        let min_x = cx.saturating_sub(win / 2);
        let min_y = cy.saturating_sub(win / 2);
        let max_w = (self.size_w - min_x).min(win);
        let max_h = (self.size_h - min_y).min(win);

        let bytes_per_pixel = 4u32; // R32Uint
        let bytes_per_row = (max_w * bytes_per_pixel).div_ceil(256) * 256; // required alignment
        let needed = bytes_per_row as u64 * max_h as u64;
        self.ensure_staging(device, needed);

        let destination = TexelCopyBufferInfo {
            buffer: self.staging.as_ref().unwrap(),
            layout: TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(bytes_per_row),
                rows_per_image: Some(max_h),
            },
        };
        let copy_size = Extent3d {
            width: max_w,
            height: max_h,
            depth_or_array_layers: 1,
        };
        encoder.copy_texture_to_buffer(
            TexelCopyTextureInfo {
                texture: self.pick_texture.as_ref().unwrap(),
                mip_level: 0,
                origin: Origin3d {
                    x: min_x,
                    y: min_y,
                    z: 0,
                },
                aspect: TextureAspect::All,
            },
            destination,
            copy_size,
        );

        // Submit and asynchronously map the tiny buffer
        queue.submit(std::iter::once(encoder.finish()));

        let buf = self.staging.as_ref().unwrap();
        let map_status = Arc::new(Mutex::new(None));
        let status_clone = Arc::clone(&map_status);
        let pending = PendingReadback {
            buffer: buf.clone(),
            unmap_on_drop: true,
            settlement,
            instance_id,
            projection: req.projection,
            seq: req.seq,
            needed,
            bytes_per_row,
            max_w,
            max_h,
            min_x,
            min_y,
            cx,
            cy,
            map_status,
        };
        // Resource creation and submission above cannot strand this fact. From
        // here the local RAII owner covers both map_async failure and pass drop.
        pending.settlement.store(true, Ordering::Release);
        pending.buffer.slice(0..needed).map_async(MapMode::Read, move |res| {
            *status_clone.lock().unwrap() = Some(res);
        });
        self.pending = Some(pending);
    }

    #[cfg(test)]
    pub(crate) fn hold_pending_callback(&mut self, cancel: bool) -> impl FnOnce(&mut Self) + use<> {
        let pending = self.pending.as_mut().expect("real map admission");
        let delivered = std::mem::replace(&mut pending.map_status, Arc::new(Mutex::new(None)));
        if cancel {
            pending.unmap(); // real WGPU cancellation delivers BufferAsyncError
        }
        move |pass| {
            let pending = pass.pending.as_mut().expect("not-ready polling retains custody");
            assert!(pending.map_status.lock().unwrap().is_none());
            assert_eq!(delivered.lock().unwrap().as_ref().expect("real callback readiness").is_err(), cancel);
            pending.map_status = delivered;
        }
    }

    pub(crate) fn set_view(&mut self, w: u32, h: u32, scale: f32) {
        self.size_w = w.max(1);
        self.size_h = h.max(1);
        self.scale_factor = scale;
    }

    pub(crate) fn take_id_map(&mut self) -> Vec<(u32, u32)> { std::mem::take(&mut self.id_map) }

    pub(crate) fn set_id_map(&mut self, map: Vec<(u32, u32)>, projection: &MarkerProjection) {
        self.id_map = map;
        self.id_map_projection = Some(projection.clone());
    }

    pub(crate) fn clear_id_map(&mut self, projection: &MarkerProjection) {
        self.id_map.clear();
        self.id_map_projection = Some(projection.clone());
    }

    fn poll_pending(&mut self, device: &Device, projection: &MarkerProjection, series: &[SeriesSpan]) {
        let Some(pending) = self.pending.as_ref() else {
            return;
        };

        let _ = device.poll(PollType::Poll);
        let Some(mapped) = pending.map_status.lock().unwrap().as_ref().map(Result::is_ok) else {
            return;
        };

        let pending = self.pending.take().unwrap();
        let current = pending.projection.same_as(projection)
            && self.id_map_projection.as_ref().is_some_and(|key| key.same_as(&pending.projection));
        let hit = match mapped {
            true if current => {
                let slice = pending.buffer.slice(0..pending.needed);
                let data = slice.get_mapped_range();
                let best = Self::scan_best_id(
                    &data,
                    pending.bytes_per_row,
                    pending.max_w,
                    pending.max_h,
                    pending.min_x,
                    pending.min_y,
                    pending.cx,
                    pending.cy,
                );
                drop(data);
                best.and_then(|(id, _)| self.decode_id_to_hit(id, series))
            }
            // A stale successful mapping still settles physically; never scan or
            // decode its old IDs through the replacement projection's map.
            true | false => None,
        };
        if !mapped {
            // WGPU's failed-map bookkeeping is not reusable without unmap, but
            // unmapping an already failed buffer is a validation error. Retire
            // only the failed storage; successful readbacks retain capacity.
            self.staging = None;
            self.staging_size = 0;
        }

        let result = current.then(|| GpuPickResult {
            projection: pending.projection.clone(),
            seq: pending.seq,
            hit,
        });
        let instance_id = pending.instance_id;
        drop(pending); // mapped access has ended; unmap before clearing redraw custody
        if let Some(result) = result {
            publish_result(instance_id, result);
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn scan_best_id(
        data: &[u8],
        bytes_per_row: u32,
        max_w: u32,
        max_h: u32,
        min_x: u32,
        min_y: u32,
        cx: u32,
        cy: u32,
    ) -> Option<(u32, i32)> {
        let mut best: Option<(u32, i32)> = None;
        for row in 0..max_h as usize {
            let row_off = row as u64 * bytes_per_row as u64;
            for col in 0..max_w as usize {
                let off = row_off + (col as u64) * 4;
                let id = u32::from_le_bytes([
                    data[off as usize],
                    data[off as usize + 1],
                    data[off as usize + 2],
                    data[off as usize + 3],
                ]);
                if id != 0 {
                    let sx = min_x as i32 + col as i32;
                    let sy = min_y as i32 + row as i32;
                    let dx = sx - cx as i32;
                    let dy = sy - cy as i32;
                    let d2 = dx * dx + dy * dy;
                    if let Some((_, bd2)) = best {
                        if d2 < bd2 {
                            best = Some((id, d2));
                        }
                    } else {
                        best = Some((id, d2));
                    }
                }
            }
        }
        best
    }

    fn ensure_staging(&mut self, device: &Device, needed: u64) {
        if self
            .staging
            .as_ref()
            .map(|b| b.size() >= needed)
            .unwrap_or(false)
        {
            return;
        }
        let size = needed.max(4096);
        self.staging = Some(device.create_buffer(&BufferDescriptor {
            label: Some("pick staging"),
            size,
            usage: BufferUsages::MAP_READ | BufferUsages::COPY_DST,
            mapped_at_creation: false,
        }));
        self.staging_size = size;
    }

    fn ensure_target(&mut self, device: &Device) {
        let need_new = self
            .pick_texture
            .as_ref()
            .map(|t| {
                let size = t.size();
                size.width != self.size_w || size.height != self.size_h
            })
            .unwrap_or(true);
        if need_new {
            let tex = device.create_texture(&TextureDescriptor {
                label: Some("pick texture"),
                size: Extent3d {
                    width: self.size_w,
                    height: self.size_h,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: TextureDimension::D2,
                format: TextureFormat::R32Uint,
                usage: TextureUsages::RENDER_ATTACHMENT | TextureUsages::COPY_SRC,
                view_formats: &[],
            });
            let view = tex.create_view(&TextureViewDescriptor::default());
            self.pick_view = Some(view);
            self.pick_texture = Some(tex);
        }
    }

    fn ensure_pipeline(&mut self, device: &Device, camera_bgl: &BindGroupLayout) {
        if self.pipeline.is_some() {
            return;
        }
        let shader = device.create_shader_module(include_wgsl!("shaders/pick_markers.wgsl"));
        let layout = device.create_pipeline_layout(&PipelineLayoutDescriptor {
            label: Some("pick layout"),
            bind_group_layouts: &[Some(camera_bgl)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&RenderPipelineDescriptor {
            label: Some("pick pipeline"),
            layout: Some(&layout),
            vertex: VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                buffers: &[VertexBufferLayout {
                    // Must match markers: 36 bytes per instance.
                    // Location 4 packs size_mode in bit 0 and pickable in bit 1.
                    array_stride: 36,
                    step_mode: VertexStepMode::Instance,
                    attributes: &[
                        VertexAttribute {
                            offset: 0,
                            shader_location: 0,
                            format: VertexFormat::Float32x2,
                        },
                        VertexAttribute {
                            offset: 8,
                            shader_location: 1,
                            format: VertexFormat::Float32x4,
                        },
                        VertexAttribute {
                            offset: 24,
                            shader_location: 2,
                            format: VertexFormat::Uint32,
                        },
                        VertexAttribute {
                            offset: 28,
                            shader_location: 3,
                            format: VertexFormat::Float32,
                        },
                        VertexAttribute {
                            offset: 32,
                            shader_location: 4,
                            format: VertexFormat::Uint32,
                        },
                    ],
                }],
            },
            fragment: Some(FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                targets: &[Some(ColorTargetState {
                    format: TextureFormat::R32Uint,
                    blend: None,
                    write_mask: ColorWrites::ALL,
                })],
            }),
            primitive: PrimitiveState {
                topology: PrimitiveTopology::TriangleStrip,
                strip_index_format: None,
                front_face: FrontFace::Ccw,
                cull_mode: Some(Face::Back),
                polygon_mode: PolygonMode::Fill,
                unclipped_depth: false,
                conservative: false,
            },
            depth_stencil: None,
            multisample: MultisampleState::default(),
            multiview_mask: None,
            cache: None,
        });
        self.pipeline = Some(pipeline);
    }

    fn decode_id_to_hit(
        &self,
        id: u32,
        series: &[SeriesSpan],
    ) -> Option<PointId> {
        // IDs are 1-based instance index
        let idx = (id as usize).saturating_sub(1);

        if idx >= self.id_map.len() {
            return None;
        }

        let (span_idx_u32, local_idx_u32) = self.id_map[idx];
        let span_idx = span_idx_u32 as usize;
        let local_idx = local_idx_u32 as usize;

        if span_idx >= series.len() {
            return None;
        }

        let span: &SeriesSpan = &series[span_idx];
        if !span.pickable {
            return None;
        }
        if local_idx >= span.point_indices.len() {
            return None;
        }

        let point_index = span.point_indices[local_idx];

        Some(PointId {
            series_id: span.id,
            point_index,
        })
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use glam::{DVec2, Vec2};
    use iced::Rectangle;

    use super::cpu_pick_hit;
    use crate::{
        Color, LineStyle, Point, PointId, ShapeId, camera::Camera, plot_state::SeriesSpan,
    };

    #[test]
    fn line_only_history_keeps_tooltips_above_the_marker_gpu_threshold() {
        let series = crate::Series::line_only(vec![[0.0, 0.0]; super::CPU_PICK_THRESHOLD + 1], LineStyle::solid());
        let id = series.id;
        let plot = crate::PlotWidgetBuilder::new().add_series(series).build().unwrap();
        let mut state = crate::plot_state::PlotState::default();
        state.bounds = Rectangle { x: 0.0, y: 0.0, width: 100.0, height: 100.0 };
        state.camera = Camera { position: DVec2::ZERO, half_extents: DVec2::ONE, render_offset: DVec2::ZERO };
        state.rebuild_from_widget(&plot);
        let mut picking = state.picking.clone();
        let hit = picking.request_hover(1, Vec2::new(50.0, 50.0), 8.0, false,
            &state.points, &state.series, &state.camera, &state.bounds, |_| true);
        assert!(matches!(hit, super::HoverRequest::CpuHit(point) if point.series_id == id));
        assert!(!picking.has_outstanding_gpu_request());
    }

    #[test]
    fn registry_rejects_old_origins_and_generations_without_consuming_new_requests() {
        use super::*;

        let series = crate::Series::markers_only(
            vec![[0.0, 0.0]; CPU_PICK_THRESHOLD + 1],
            crate::MarkerStyle::default(),
        );
        let id = series.id;
        let mut plot = crate::PlotWidgetBuilder::new().add_series(series).build().unwrap();
        let mut old = crate::plot_state::PlotState::default();
        old.rebuild_from_widget(&plot);
        old.picking.submit_gpu_request(plot.instance_id, Vec2::ZERO, 8.0);
        let (pending, _) = take_latest_request(plot.instance_id, old.picking.projection()).unwrap();

        let mut replacement = crate::plot_state::PlotState::default();
        replacement.rebuild_from_widget(&plot);
        replacement.picking.submit_gpu_request(plot.instance_id, Vec2::ZERO, 8.0);
        assert_eq!(pending.seq, replacement.picking.pick_seq);
        assert_eq!(old.markers_version, replacement.markers_version);
        let hit = PointId { series_id: id, point_index: 0 };
        publish_result(plot.instance_id, GpuPickResult {
            projection: pending.projection.clone(), seq: pending.seq, hit: Some(hit),
        });
        assert!(replacement.picking.consume_gpu_result(plot.instance_id, |_| true).is_none());
        assert!(replacement.picking.has_outstanding_gpu_request());
        assert!(take_latest_request(plot.instance_id, &pending.projection).is_none());
        let (current, _) = take_latest_request(plot.instance_id, replacement.picking.projection()).unwrap();
        publish_result(plot.instance_id, GpuPickResult {
            projection: current.projection.clone(), seq: current.seq, hit: Some(hit),
        });
        assert!(old.picking.consume_gpu_result(plot.instance_id, |_| true).is_none());
        // A late old completion cannot overwrite even an already-published result.
        publish_result(plot.instance_id, GpuPickResult {
            projection: pending.projection, seq: u64::MAX, hit: None,
        });
        assert!(matches!(replacement.picking.consume_gpu_result(plot.instance_id, |_| true),
            Some(GpuResultEvent::Hover(point)) if point == hit));
        assert!(!replacement.picking.has_outstanding_gpu_request());

        // One state can also rebuild its marker generation while work is pending.
        replacement.picking.submit_gpu_request(plot.instance_id, Vec2::ZERO, 8.0);
        let (previous_generation, _) = take_latest_request(plot.instance_id, replacement.picking.projection()).unwrap();
        plot.set_series_positions(&id, &[]);
        replacement.rebuild_from_widget(&plot);
        assert!(replacement.points.is_empty());
        assert!(!replacement.picking.has_outstanding_gpu_request());
        assert!(replacement.picking.last_hover_cache.is_none());
        publish_result(plot.instance_id, GpuPickResult {
            projection: previous_generation.projection,
            seq: previous_generation.seq,
            hit: Some(hit),
        });
        assert!(replacement.picking.consume_gpu_result(plot.instance_id, |_| true).is_none());
        let unchanged = replacement.clone();
        assert!(replacement.picking.projection().same_as(unchanged.picking.projection()));
        assert!(!replacement.picking.projection().same_as(old.picking.projection()));
    }

    #[test]
    fn current_generation_preserves_click_interpretation_and_request_coalescing() {
        use super::*;

        let series = crate::Series::markers_only(
            vec![[0.0, 0.0]; CPU_PICK_THRESHOLD + 1],
            crate::MarkerStyle::default(),
        );
        let id = series.id;
        let plot = crate::PlotWidgetBuilder::new().add_series(series).build().unwrap();
        let mut state = crate::plot_state::PlotState::default();
        state.rebuild_from_widget(&plot);
        assert!(plot.pick_hit(&mut state).is_none());
        let (request, _) = take_latest_request(plot.instance_id, state.picking.projection()).unwrap();
        let hit = PointId { series_id: id, point_index: 3 };
        publish_result(plot.instance_id, GpuPickResult {
            projection: request.projection.clone(), seq: request.seq, hit: Some(hit),
        });
        assert!(matches!(state.picking.consume_gpu_result(plot.instance_id, |_| true),
            Some(GpuResultEvent::Pick(point)) if point == hit));
        assert!(!state.picking.has_outstanding_gpu_request());
        state.picking.submit_gpu_request(plot.instance_id, Vec2::new(1.0, 2.0), 8.0);
        state.picking.submit_gpu_request(plot.instance_id, Vec2::new(3.0, 4.0), 8.0);
        let (newest, _) = take_latest_request(plot.instance_id, state.picking.projection()).unwrap();
        assert_eq!(newest.seq, 3);
        assert_eq!((newest.cursor_x, newest.cursor_y), (3.0, 4.0));
        assert!(take_latest_request(plot.instance_id, state.picking.projection()).is_none());
        publish_result(plot.instance_id, GpuPickResult {
            projection: newest.projection, seq: newest.seq, hit: None,
        });
        assert!(matches!(state.picking.consume_gpu_result(plot.instance_id, |_| true), Some(GpuResultEvent::HoverMiss)));
    }

    #[test]
    fn cpu_pick_skips_unpickable_series() {
        let points = [Point::new(0.0, 0.0, 6.0), Point::new(0.1, 0.0, 6.0)];
        let series = [
            SeriesSpan {
                id: ShapeId(1),
                start: 0,
                len: 1,
                point_indices: Arc::from([0usize]),
                line_style: Some(LineStyle::solid()),
                color: Color::BLACK,
                marker: 0,
                pickable: false,
            },
            SeriesSpan {
                id: ShapeId(2),
                start: 1,
                len: 1,
                point_indices: Arc::from([0usize]),
                line_style: Some(LineStyle::solid()),
                color: Color::BLACK,
                marker: 0,
                pickable: true,
            },
        ];
        let camera = Camera {
            position: DVec2::ZERO,
            half_extents: DVec2::ONE,
            render_offset: DVec2::ZERO,
        };
        let bounds = Rectangle {
            x: 0.0,
            y: 0.0,
            width: 100.0,
            height: 100.0,
        };

        let hit = cpu_pick_hit(
            &points,
            &series,
            &camera,
            &bounds,
            Vec2::new(50.0, 50.0),
            8.0,
        );

        assert_eq!(
            hit,
            Some(PointId {
                series_id: ShapeId(2),
                point_index: 0,
            })
        );
    }
}
