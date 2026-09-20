//! GPU renderer for PlotWidget.
use super::{
    CROSSHAIR_RGBA, SELECTION_FILL_RGBA, highlight_marker_plot_position,
    highlight_mask_plot_position, highlight_mask_rgba,
};
use crate::LineStyle;
use crate::picking::PickingPass;
use crate::transform::data_value_to_plot_with_axis_range;
use crate::{LineType, Size, camera::CameraUniform, grid::Grid, plot_state::{PlotState, ProjectionOrigin}};
use iced::widget::shader::Viewport;
use iced::{Rectangle, wgpu::*};

/// MSAA sample count for all plot pipelines.
///
/// Android: 1 — plots draw inside iced's surface render pass via
/// `Primitive::draw` (see `draw_in_pass`), and that pass is single-sampled;
/// MSAA pipelines would be incompatible ("Incompatible sample count"
/// validation panic). In-pass rendering is required there because per-plot
/// pass fragmentation corrupts frames on tile-based GPUs (Mali/Adreno).
///
/// Desktop: 4 — plots render through `render()`/`encode` into a dedicated
/// MSAA offscreen target and composite the resolved texture, so
/// `Primitive::draw` must return `false` (see `PlotWidget` shader impl).
pub(crate) const MSAA_SAMPLE_COUNT: u32 = if cfg!(target_os = "android") { 1 } else { 4 };

pub struct RenderParams<'a> {
    pub encoder: &'a mut CommandEncoder,
    pub target: &'a TextureView,
    /// clip_bounds considers crop in scrollable viewport
    pub clip_bounds: &'a Rectangle<u32>,
}

fn msaa_state() -> MultisampleState {
    MultisampleState {
        count: MSAA_SAMPLE_COUNT,
        mask: !0,
        alpha_to_coverage_enabled: false,
    }
}

fn create_color_texture(
    device: &Device,
    label: &'static str,
    width: u32,
    height: u32,
    sample_count: u32,
    format: TextureFormat,
    usage: TextureUsages,
) -> Texture {
    device.create_texture(&TextureDescriptor {
        label: Some(label),
        size: Extent3d {
            width,
            height,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count,
        dimension: TextureDimension::D2,
        format,
        usage,
        view_formats: &[],
    })
}

fn msaa_attachment<'a>(
    targets: &'a MsaaTargets,
    load: LoadOp<Color>,
) -> RenderPassColorAttachment<'a> {
    RenderPassColorAttachment {
        view: &targets.msaa_view,
        resolve_target: Some(&targets.resolved_view),
        ops: Operations {
            load,
            store: StoreOp::Store,
        },
        depth_slice: None,
    }
}

fn target_attachment(target: &TextureView) -> RenderPassColorAttachment<'_> {
    RenderPassColorAttachment {
        view: target,
        resolve_target: None,
        ops: Operations {
            load: LoadOp::Load,
            store: StoreOp::Store,
        },
        depth_slice: None,
    }
}

#[derive(Default, Clone)]
struct LineSegment {
    first_vertex: u32,
    vertex_count: u32,
}

/// Helper struct for managing vertex buffers
struct VertexBuffer {
    buffer: Buffer,
    vertex_count: u32,
}

impl VertexBuffer {
    fn clear(slot: &mut Option<Self>) { if let Some(buffer) = slot { buffer.vertex_count = 0; } }
    fn upload(slot: &mut Option<Self>, device: &Device, queue: &Queue, data: &[u8], count: u32) {
        if data.is_empty() { Self::clear(slot); return; }
        if slot.as_ref().is_none_or(|buffer| buffer.buffer.size() < data.len() as u64) {
            *slot = Some(Self { buffer: device.create_buffer(&BufferDescriptor {
                label: Some("retained plot vertices"), size: data.len() as u64,
                usage: BufferUsages::VERTEX | BufferUsages::COPY_DST, mapped_at_creation: false,
            }), vertex_count: 0 });
        }
        let buffer = slot.as_mut().unwrap();
        queue.write_buffer(&buffer.buffer, 0, data);
        buffer.vertex_count = count;
    }
}

/// Helper struct for managing line vertex buffers
struct LineBuffer {
    buffer: Buffer,
    segments: Vec<LineSegment>,
}

struct LineVertex<'a> {
    start: [f32; 2],
    end: [f32; 2],
    color: &'a iced::Color,
    style: u32,
    distance_start: f32,
    segment_length_world: f32,
    param: f32,
    width: f32,
    width_mode: u32,
    along: f32,
    side: f32,
}

#[derive(Clone, Copy)]
struct LineRenderStyle {
    width: Size,
    line_style: u32,
    style_param: f32,
}

struct PolylineRef<'a> {
    positions: &'a [[f32; 2]],
    distances: &'a [f32],
    colors: &'a [iced::Color],
}

/// Cache for render pipelines
struct PipelineCache {
    marker: Option<RenderPipeline>,
    line: Option<RenderPipeline>,
    fill: Option<RenderPipeline>,
    overlay: Option<RenderPipeline>,
    line_overlay: Option<RenderPipeline>,
    composite: Option<RenderPipeline>,
}

impl PipelineCache {
    fn new() -> Self {
        Self {
            marker: None,
            line: None,
            fill: None,
            overlay: None,
            line_overlay: None,
            composite: None,
        }
    }
}

struct MsaaTargets {
    width: u32,
    height: u32,
    _msaa_texture: Texture,
    msaa_view: TextureView,
    _resolved_texture: Texture,
    resolved_view: TextureView,
    composite_bind_group: BindGroup,
}

/// Cache for vertex buffers
struct BufferCache {
    markers: Option<VertexBuffer>,
    fills: Option<VertexBuffer>,
    lines: Option<LineBuffer>,
    reflines: Option<LineBuffer>,
    selection: Option<VertexBuffer>,
    highlight: Option<VertexBuffer>,
    highlight_markers: Option<VertexBuffer>,
    crosshairs: Option<VertexBuffer>,
}

impl BufferCache {
    fn new() -> Self {
        Self {
            markers: None,
            fills: None,
            lines: None,
            reflines: None,
            selection: None,
            highlight: None,
            highlight_markers: None,
            crosshairs: None,
        }
    }
}

/// Retained dependencies of the uploaded buffers, including the owner of local counters.
struct BufferDependencies {
    origin: Option<ProjectionOrigin>,
    markers: u64,
    fills: u64,
    lines: u64,
    highlight: u64,
    render_offset: glam::DVec2,
    reference: Option<ReferenceKey>,
    crosshair: Option<(bool, glam::Vec2)>,
    selection: Option<(bool, bool, glam::Vec2, glam::Vec2)>,
}

#[derive(Debug, Default, PartialEq)]
struct BufferChanges {
    markers: bool,
    fills: bool,
    lines: bool,
    reference: bool,
    highlight: bool,
    selection: bool,
    crosshair: bool,
}

impl BufferDependencies {
    fn new() -> Self {
        Self {
            origin: None,
            markers: 0,
            fills: 0,
            lines: 0,
            highlight: 0,
            render_offset: glam::DVec2::ZERO,
            reference: None,
            crosshair: None,
            selection: None,
        }
    }

    fn changes(&self, state: &PlotState, reference: ReferenceKey, geometry_changed: bool) -> BufferChanges {
        let origin_changed = self.origin.as_ref().is_none_or(|origin| !origin.same_as(state.origin()));
        let offset_changed = self.render_offset != state.camera.render_offset;
        let axes_changed = self.reference.is_none_or(|previous|
            previous.x_axis != reference.x_axis || previous.y_axis != reference.y_axis);
        BufferChanges {
            markers: origin_changed || offset_changed || self.markers != state.markers_version,
            fills: origin_changed || offset_changed || self.fills != state.fills_version,
            lines: origin_changed || offset_changed || self.lines != state.lines_version,
            reference: origin_changed || self.reference != Some(reference),
            highlight: origin_changed || geometry_changed || axes_changed || self.highlight != state.highlight_version,
            selection: geometry_changed || self.selection != Some(Self::selection(state)),
            crosshair: geometry_changed || self.crosshair != Some((state.crosshairs_enabled, state.crosshairs_position)),
        }
    }

    fn selection(state: &PlotState) -> (bool, bool, glam::Vec2, glam::Vec2) {
        (state.selection.active, state.selection.moved, state.selection.start, state.selection.end)
    }

    /// Commit only after the renderer has refreshed every affected buffer.
    fn synchronized(&mut self, state: &PlotState, reference: ReferenceKey) {
        if self.origin.as_ref().is_none_or(|origin| !origin.same_as(state.origin())) {
            self.origin = Some(state.origin().clone());
        }
        self.markers = state.markers_version;
        self.fills = state.fills_version;
        self.lines = state.lines_version;
        self.highlight = state.highlight_version;
        self.render_offset = state.camera.render_offset;
        self.reference = Some(reference);
        self.selection = Some(Self::selection(state));
        self.crosshair = Some((state.crosshairs_enabled, state.crosshairs_position));
    }
}

/// Helper for writing vertex data
struct VertexWriter {
    data: Vec<u8>,
}

impl VertexWriter {
    fn new() -> Self {
        Self { data: Vec::new() }
    }

    fn write_f32(&mut self, value: f32) {
        self.data.extend_from_slice(&value.to_le_bytes());
    }

    fn write_u32(&mut self, value: u32) {
        self.data.extend_from_slice(&value.to_le_bytes());
    }

    fn write_position(&mut self, pos: [f32; 2]) {
        self.write_f32(pos[0]);
        self.write_f32(pos[1]);
    }

    fn write_color(&mut self, color: &iced::Color) {
        self.write_f32(color.r);
        self.write_f32(color.g);
        self.write_f32(color.b);
        self.write_f32(color.a);
    }

    fn write_line_vertex(&mut self, vertex: LineVertex<'_>) {
        self.write_position(vertex.start);
        self.write_position(vertex.end);
        self.write_color(vertex.color);
        self.write_u32(vertex.style);
        self.write_f32(vertex.distance_start);
        self.write_f32(vertex.segment_length_world);
        self.write_f32(vertex.param);
        self.write_f32(vertex.width);
        self.write_u32(vertex.width_mode);
        self.write_f32(vertex.along);
        self.write_f32(vertex.side);
    }

    fn byte_len(&self) -> usize {
        self.data.len()
    }

    fn is_empty(&self) -> bool {
        self.data.is_empty()
    }

    fn as_slice(&self) -> &[u8] {
        &self.data
    }
}

pub struct PlotRenderer {
    format: TextureFormat,
    camera_buffer: Buffer,
    camera_bind_group: BindGroup,
    camera_bgl: BindGroupLayout,
    composite_bgl: BindGroupLayout,
    composite_sampler: Sampler,
    composite_uniform: Buffer,
    composite_region: Option<([f32; 4], Rectangle)>,
    msaa_targets: Option<MsaaTargets>,
    // Caches
    pipelines: PipelineCache,
    buffers: BufferCache,
    dependencies: BufferDependencies,
    // Support objects
    grid: Grid,
    picking: PickingPass,
    scale_factor: f32,
    bounds_w: u32,
    bounds_h: u32,
    bounds: Rectangle,
    prepared: Option<(crate::camera::Camera, u32, u32, f32)>,
    scratch: VertexWriter,
    float_scratch: Vec<f32>,
    marker_scratch: VertexWriter,
    poly_positions: Vec<[f32; 2]>,
    poly_distances: Vec<f32>,
    poly_colors: Vec<iced::Color>,
}

impl PlotRenderer {
    pub fn new(device: &Device, _queue: &Queue, format: TextureFormat) -> Self {
        let camera_bgl = device.create_bind_group_layout(&BindGroupLayoutDescriptor {
            label: Some("camera_bgl"),
            entries: &[BindGroupLayoutEntry {
                binding: 0,
                visibility: ShaderStages::VERTEX_FRAGMENT,
                ty: BindingType::Buffer {
                    ty: BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let camera_buffer = device.create_buffer(&BufferDescriptor {
            label: Some("camera_buffer"),
            size: std::mem::size_of::<crate::camera::CameraUniform>() as u64,
            usage: BufferUsages::UNIFORM | BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let camera_bind_group = device.create_bind_group(&BindGroupDescriptor {
            label: Some("camera_bg"),
            layout: &camera_bgl,
            entries: &[BindGroupEntry {
                binding: 0,
                resource: camera_buffer.as_entire_binding(),
            }],
        });
        let composite_uniform = device.create_buffer(&BufferDescriptor {
            label: Some("plot composite UV region"), size: 16,
            usage: BufferUsages::UNIFORM | BufferUsages::COPY_DST, mapped_at_creation: false,
        });
        let composite_bgl = device.create_bind_group_layout(&BindGroupLayoutDescriptor {
            label: Some("plot composite bgl"),
            entries: &[
                BindGroupLayoutEntry {
                    binding: 0,
                    visibility: ShaderStages::FRAGMENT,
                    ty: BindingType::Texture {
                        sample_type: TextureSampleType::Float { filterable: false },
                        view_dimension: TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                BindGroupLayoutEntry {
                    binding: 1,
                    visibility: ShaderStages::FRAGMENT,
                    ty: BindingType::Sampler(SamplerBindingType::NonFiltering),
                    count: None,
                },
                BindGroupLayoutEntry {
                    binding: 2, visibility: ShaderStages::VERTEX,
                    ty: BindingType::Buffer { ty: BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None,
                },
            ],
        });
        let composite_sampler = device.create_sampler(&SamplerDescriptor {
            label: Some("plot composite sampler"),
            address_mode_u: AddressMode::ClampToEdge,
            address_mode_v: AddressMode::ClampToEdge,
            address_mode_w: AddressMode::ClampToEdge,
            mag_filter: FilterMode::Nearest,
            min_filter: FilterMode::Nearest,
            mipmap_filter: MipmapFilterMode::Nearest,
            ..SamplerDescriptor::default()
        });
        Self {
            format,
            camera_buffer,
            camera_bind_group,
            camera_bgl,
            composite_bgl,
            composite_sampler, composite_uniform, composite_region: None,
            msaa_targets: None,
            pipelines: PipelineCache::new(),
            buffers: BufferCache::new(),
            dependencies: BufferDependencies::new(),
            grid: Grid::default(),
            picking: PickingPass::default(),
            bounds_w: 0,
            bounds_h: 0,
            bounds: Rectangle::default(),
            scale_factor: 1.0,
            prepared: None,
            float_scratch: Vec::new(), marker_scratch: VertexWriter::new(),
            scratch: VertexWriter::new(), poly_positions: Vec::new(),
            poly_distances: Vec::new(), poly_colors: Vec::new(),
        }
    }

    // Coordinate conversion helpers
    fn screen_to_clip(&self, x: f32, y: f32) -> [f32; 2] {
        let w = self.bounds_w.max(1) as f32;
        let h = self.bounds_h.max(1) as f32;
        [(x / w) * 2.0 - 1.0, 1.0 - (y / h) * 2.0]
    }

    fn world_to_ndc(&self, world: [f64; 2], camera: &crate::camera::Camera) -> [f32; 2] {
        let render_pos = [
            (world[0] - camera.render_offset.x) as f32,
            (world[1] - camera.render_offset.y) as f32,
        ];
        let ndc_x =
            (render_pos[0] - camera.effective_position().x as f32) / camera.half_extents.x as f32;
        let ndc_y =
            (render_pos[1] - camera.effective_position().y as f32) / camera.half_extents.y as f32;
        [ndc_x, ndc_y]
    }

    fn pixels_to_clip_delta(&self, pixels: f32) -> (f32, f32) {
        let w = self.bounds_w.max(1) as f32;
        let h = self.bounds_h.max(1) as f32;
        (2.0 * pixels / w, 2.0 * pixels / h)
    }

    // Helper to convert world position to render position (subtract offset)
    fn world_to_render_pos(&self, world: [f64; 2], camera: &crate::camera::Camera) -> [f32; 2] {
        [
            (world[0] - camera.render_offset.x) as f32,
            (world[1] - camera.render_offset.y) as f32,
        ]
    }

    fn world_per_pixel(&self, camera: &crate::camera::Camera) -> [f32; 2] {
        [
            ((2.0 * camera.half_extents.x) / self.bounds_w.max(1) as f64) as f32,
            ((2.0 * camera.half_extents.y) / self.bounds_h.max(1) as f64) as f32,
        ]
    }

    fn ensure_pipelines_and_update_grid(
        &mut self,
        device: &Device,
        queue: &Queue,
        state: &PlotState,
    ) {
        self.ensure_marker_pipeline(device);
        self.grid
            .ensure_pipeline(device, self.format, &self.camera_bgl, MSAA_SAMPLE_COUNT);
        self.grid.update(device, queue, state);
        if !state.fills.is_empty() {
            self.ensure_fill_pipeline(device);
        }
        if state.series.iter().any(|s| s.line_style.is_some())
            || !state.vlines.is_empty()
            || !state.hlines.is_empty()
        {
            self.ensure_line_pipeline(device);
        }
        self.ensure_overlay_pipeline(device);
        self.ensure_line_overlay_pipeline(device);
        // Composite yalnız `encode`'un resolve→hedef blit'inde kullanılır;
        // pas-içi (MSAA=1) yolda gereksiz.
        if MSAA_SAMPLE_COUNT > 1 {
            self.ensure_composite_pipeline(device);
        }
    }
    fn set_bounds(&mut self, w: u32, h: u32) {
        self.bounds_w = w;
        self.bounds_h = h;
    }
    fn set_scale_factor(&mut self, scale: f32) {
        self.scale_factor = scale;
    }

    fn ensure_msaa_targets(&mut self, device: &Device, width: u32, height: u32) {
        let width = width.max(1);
        let height = height.max(1);

        if self
            .msaa_targets
            .as_ref()
            .is_some_and(|targets| targets.width == width && targets.height == height)
        {
            return;
        }

        let msaa_texture = create_color_texture(
            device,
            "iced_plot msaa color",
            width,
            height,
            MSAA_SAMPLE_COUNT,
            self.format,
            TextureUsages::RENDER_ATTACHMENT,
        );
        let msaa_view = msaa_texture.create_view(&TextureViewDescriptor::default());

        let resolved_texture = create_color_texture(
            device,
            "iced_plot resolved color",
            width,
            height,
            1,
            self.format,
            TextureUsages::RENDER_ATTACHMENT | TextureUsages::TEXTURE_BINDING,
        );
        let resolved_view = resolved_texture.create_view(&TextureViewDescriptor::default());

        let composite_bind_group = device.create_bind_group(&BindGroupDescriptor {
            label: Some("plot composite bind group"),
            layout: &self.composite_bgl,
            entries: &[
                BindGroupEntry {
                    binding: 0,
                    resource: BindingResource::TextureView(&resolved_view),
                },
                BindGroupEntry {
                    binding: 1,
                    resource: BindingResource::Sampler(&self.composite_sampler),
                },
                BindGroupEntry { binding: 2, resource: self.composite_uniform.as_entire_binding() },
            ],
        });

        self.msaa_targets = Some(MsaaTargets {
            width,
            height,
            _msaa_texture: msaa_texture,
            msaa_view,
            _resolved_texture: resolved_texture,
            resolved_view,
            composite_bind_group,
        });
    }

    fn sync(&mut self, device: &Device, queue: &Queue, state: &PlotState, geometry_changed: bool) {
        let reference = ReferenceKey::new(state, self.bounds_w, self.bounds_h, self.scale_factor);
        let changes = self.dependencies.changes(state, reference, geometry_changed);
        if changes.markers {
            self.rebuild_markers(device, queue, state);
        }
        if changes.fills {
            self.rebuild_fills(device, queue, state);
        }
        if changes.lines {
            self.rebuild_lines(device, queue, state);
        }
        if changes.reference {
            self.rebuild_reflines(device, queue, state);
        }
        // Selection uploads follow its actual geometry and interaction dependencies.
        if changes.selection {
            self.rebuild_selection(device, queue, state);
        }
        // Highlights also consume camera/viewport and axis transforms.
        if changes.highlight {
            self.rebuild_highlight(device, queue, state);
        }
        // Unchanged crosshairs reuse their retained buffer.
        if changes.crosshair {
            self.rebuild_crosshairs(device, queue, state);
        }
        self.dependencies.synchronized(state, reference);
    }

    /// Prepare the renderer for a new frame given the viewport and current plot state.
    /// This sets format/viewport/scale, ensures pipelines and grid, and syncs buffers.
    pub(crate) fn prepare_frame(
        &mut self,
        device: &Device,
        queue: &Queue,
        viewport: &Viewport,
        bounds: &Rectangle,
        state: &PlotState,
    ) {
        self.bounds = *bounds;
        let scale = viewport.scale_factor();
        let physical = Rectangle { x: bounds.x * scale, y: bounds.y * scale,
            width: bounds.width * scale, height: bounds.height * scale };
        let window = Rectangle::with_size(iced::Size::new(viewport.physical_size().width as f32, viewport.physical_size().height as f32));
        let region = chart_composite_region(physical, window);
        if self.composite_region != region {
            if let Some((uv, _)) = region { queue.write_buffer(&self.composite_uniform, 0, bytemuck::cast_slice(&uv)); }
            self.composite_region = region;
        }
        let scale_factor = viewport.scale_factor();
        let bounds_width = ((bounds.width * scale_factor).ceil() as u32).max(1);
        let bounds_height = ((bounds.height * scale_factor).ceil() as u32).max(1);


        self.set_bounds(bounds_width, bounds_height);
        self.set_scale_factor(scale_factor);
        // In-pass path (MSAA=1, Android) never runs `encode`, so the offscreen
        // MSAA/resolve textures would only waste memory; `encode` self-guards
        // by early-returning when `msaa_targets` is `None`.
        if MSAA_SAMPLE_COUNT > 1 {
            self.ensure_msaa_targets(device, bounds_width.max(1), bounds_height.max(1));
        }

        // Sync picking viewport
        self.picking
            .set_view(bounds_width, bounds_height, scale_factor);

        // Ensure pipelines/grid and synchronize GPU buffers
        self.ensure_pipelines_and_update_grid(device, queue, state);

        // Upload camera uniform based on current camera and bounds dimensions
        let mut cam_u = CameraUniform::default();
        cam_u.update(&state.camera, bounds_width, bounds_height);
        let geometry = (state.camera, bounds_width, bounds_height, scale_factor);
        let changed = self.prepared != Some(geometry);
        if changed { queue.write_buffer(&self.camera_buffer, 0, bytemuck::bytes_of(&cam_u)); }
        self.sync(device, queue, state, changed);
        self.prepared = Some(geometry);
    }

    pub(crate) fn service_picking(
        &mut self,
        instance_id: u64,
        device: &Device,
        queue: &Queue,
        state: &PlotState,
    ) {
        let marker_buffer = self.buffers.markers.as_ref().map(|vb| &vb.buffer);
        let marker_instances = self
            .buffers
            .markers
            .as_ref()
            .map(|vb| vb.vertex_count)
            .unwrap_or(0);

        self.picking.service(
            instance_id,
            device,
            queue,
            &self.camera_bind_group,
            &self.camera_bgl,
            marker_buffer,
            marker_instances,
            state.picking.projection(),
            &state.series,
        );
    }

    pub fn ensure_marker_pipeline(&mut self, device: &Device) {
        if self.pipelines.marker.is_some() {
            return;
        }
        let shader = device.create_shader_module(include_wgsl!("../shaders/markers.wgsl"));
        let layout = device.create_pipeline_layout(&PipelineLayoutDescriptor {
            label: Some("markers layout"),
            bind_group_layouts: &[Some(&self.camera_bgl)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&RenderPipelineDescriptor {
            label: Some("markers pipeline"),
            layout: Some(&layout),
            vertex: VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                buffers: &[VertexBufferLayout {
                    // Explicit 36-byte stride: vec2<f32> position (8) + vec4<f32> color (16)
                    // + u32 marker (4) + f32 size (4) + u32 size_mode/pickable flags (4) = 36
                    array_stride: 36u64,
                    step_mode: VertexStepMode::Instance,
                    attributes: &[
                        VertexAttribute {
                            offset: 0,
                            shader_location: 0,
                            format: VertexFormat::Float32x2,
                        },
                        VertexAttribute {
                            offset: std::mem::size_of::<[f32; 2]>() as u64,
                            shader_location: 1,
                            format: VertexFormat::Float32x4,
                        },
                        VertexAttribute {
                            offset: std::mem::size_of::<[f32; 6]>() as u64,
                            shader_location: 2,
                            format: VertexFormat::Uint32,
                        },
                        VertexAttribute {
                            offset: std::mem::size_of::<[f32; 6]>() as u64
                                + std::mem::size_of::<u32>() as u64,
                            shader_location: 3,
                            format: VertexFormat::Float32,
                        },
                        VertexAttribute {
                            offset: std::mem::size_of::<[f32; 7]>() as u64
                                + std::mem::size_of::<u32>() as u64,
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
                    format: self.format,
                    blend: Some(BlendState::ALPHA_BLENDING),
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
            multisample: msaa_state(),
            multiview_mask: None,
            cache: None,
        });
        self.pipelines.marker = Some(pipeline);
    }

    pub fn ensure_line_pipeline(&mut self, device: &Device) {
        if self.pipelines.line.is_some() {
            return;
        }
        let shader = device.create_shader_module(include_wgsl!("../shaders/line.wgsl"));
        let layout = device.create_pipeline_layout(&PipelineLayoutDescriptor {
            label: Some("line layout"),
            bind_group_layouts: &[Some(&self.camera_bgl)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&RenderPipelineDescriptor {
            label: Some("line pipeline"),
            layout: Some(&layout),
            vertex: VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                buffers: &[VertexBufferLayout {
                    // vec2<f32> segment_start (8) + vec2<f32> segment_end (8) + vec4<f32> color (16)
                    // + u32 line_style (4) + f32 distance_start (4) + f32 segment_length_world (4)
                    // + f32 style_param (4) + f32 width (4) + u32 width_mode (4)
                    // + f32 along (4) + f32 side (4)
                    array_stride: 64,
                    step_mode: VertexStepMode::Vertex,
                    attributes: &[
                        VertexAttribute {
                            offset: 0,
                            shader_location: 0,
                            format: VertexFormat::Float32x2, // segment_start
                        },
                        VertexAttribute {
                            offset: 8,
                            shader_location: 1,
                            format: VertexFormat::Float32x2, // segment_end
                        },
                        VertexAttribute {
                            offset: 16,
                            shader_location: 2,
                            format: VertexFormat::Float32x4, // color
                        },
                        VertexAttribute {
                            offset: 32,
                            shader_location: 3,
                            format: VertexFormat::Uint32, // line_style
                        },
                        VertexAttribute {
                            offset: 36,
                            shader_location: 4,
                            format: VertexFormat::Float32, // distance_start
                        },
                        VertexAttribute {
                            offset: 40,
                            shader_location: 5,
                            format: VertexFormat::Float32, // segment_length_world
                        },
                        VertexAttribute {
                            offset: 44,
                            shader_location: 6,
                            format: VertexFormat::Float32, // style_param
                        },
                        VertexAttribute {
                            offset: 48,
                            shader_location: 7,
                            format: VertexFormat::Float32, // width
                        },
                        VertexAttribute {
                            offset: 52,
                            shader_location: 8,
                            format: VertexFormat::Uint32, // width_mode
                        },
                        VertexAttribute {
                            offset: 56,
                            shader_location: 9,
                            format: VertexFormat::Float32, // along
                        },
                        VertexAttribute {
                            offset: 60,
                            shader_location: 10,
                            format: VertexFormat::Float32, // side
                        },
                    ],
                }],
            },
            fragment: Some(FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                targets: &[Some(ColorTargetState {
                    format: self.format,
                    blend: Some(BlendState::ALPHA_BLENDING),
                    write_mask: ColorWrites::ALL,
                })],
            }),
            primitive: PrimitiveState {
                topology: PrimitiveTopology::TriangleList,
                strip_index_format: None,
                front_face: FrontFace::Ccw,
                cull_mode: None,
                polygon_mode: PolygonMode::Fill,
                unclipped_depth: false,
                conservative: false,
            },
            depth_stencil: None,
            multisample: msaa_state(),
            multiview_mask: None,
            cache: None,
        });
        self.pipelines.line = Some(pipeline);
    }

    pub fn ensure_fill_pipeline(&mut self, device: &Device) {
        if self.pipelines.fill.is_some() {
            return;
        }
        let shader = device.create_shader_module(include_wgsl!("../shaders/fill.wgsl"));
        let layout = device.create_pipeline_layout(&PipelineLayoutDescriptor {
            label: Some("fill layout"),
            bind_group_layouts: &[Some(&self.camera_bgl)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&RenderPipelineDescriptor {
            label: Some("fill pipeline"),
            layout: Some(&layout),
            vertex: VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                buffers: &[VertexBufferLayout {
                    array_stride: 24,
                    step_mode: VertexStepMode::Vertex,
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
                    ],
                }],
            },
            fragment: Some(FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                targets: &[Some(ColorTargetState {
                    format: self.format,
                    blend: Some(BlendState::ALPHA_BLENDING),
                    write_mask: ColorWrites::ALL,
                })],
            }),
            primitive: PrimitiveState {
                topology: PrimitiveTopology::TriangleList,
                strip_index_format: None,
                front_face: FrontFace::Ccw,
                cull_mode: None,
                polygon_mode: PolygonMode::Fill,
                unclipped_depth: false,
                conservative: false,
            },
            depth_stencil: None,
            multisample: msaa_state(),
            multiview_mask: None,
            cache: None,
        });
        self.pipelines.fill = Some(pipeline);
    }

    pub fn ensure_overlay_pipeline(&mut self, device: &Device) {
        if self.pipelines.overlay.is_some() {
            return;
        }
        let shader = device.create_shader_module(include_wgsl!("../shaders/selection.wgsl"));
        let layout = device.create_pipeline_layout(&PipelineLayoutDescriptor {
            label: Some("overlay layout"),
            bind_group_layouts: &[],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&RenderPipelineDescriptor {
            label: Some("overlay pipeline"),
            layout: Some(&layout),
            vertex: VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                buffers: &[VertexBufferLayout {
                    array_stride: (std::mem::size_of::<[f32; 2]>()
                        + std::mem::size_of::<[f32; 4]>()) as u64,
                    step_mode: VertexStepMode::Vertex,
                    attributes: &[
                        VertexAttribute {
                            offset: 0,
                            shader_location: 0,
                            format: VertexFormat::Float32x2,
                        },
                        VertexAttribute {
                            offset: std::mem::size_of::<[f32; 2]>() as u64,
                            shader_location: 1,
                            format: VertexFormat::Float32x4,
                        },
                    ],
                }],
            },
            fragment: Some(FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                targets: &[Some(ColorTargetState {
                    format: self.format,
                    blend: Some(BlendState::ALPHA_BLENDING),
                    write_mask: ColorWrites::ALL,
                })],
            }),
            primitive: PrimitiveState {
                topology: PrimitiveTopology::TriangleStrip,
                strip_index_format: None,
                front_face: FrontFace::Ccw,
                cull_mode: None,
                polygon_mode: PolygonMode::Fill,
                unclipped_depth: false,
                conservative: false,
            },
            depth_stencil: None,
            multisample: msaa_state(),
            multiview_mask: None,
            cache: None,
        });
        self.pipelines.overlay = Some(pipeline);
    }

    pub fn ensure_line_overlay_pipeline(&mut self, device: &Device) {
        if self.pipelines.line_overlay.is_some() {
            return;
        }
        let shader = device.create_shader_module(include_wgsl!("../shaders/selection.wgsl"));
        let layout = device.create_pipeline_layout(&PipelineLayoutDescriptor {
            label: Some("line overlay layout"),
            bind_group_layouts: &[],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&RenderPipelineDescriptor {
            label: Some("line overlay pipeline"),
            layout: Some(&layout),
            vertex: VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                buffers: &[VertexBufferLayout {
                    array_stride: 24,
                    step_mode: VertexStepMode::Vertex,
                    attributes: &[
                        VertexAttribute {
                            format: VertexFormat::Float32x2,
                            offset: 0,
                            shader_location: 0,
                        },
                        VertexAttribute {
                            format: VertexFormat::Float32x4,
                            offset: 8,
                            shader_location: 1,
                        },
                    ],
                }],
            },
            fragment: Some(FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                targets: &[Some(ColorTargetState {
                    format: self.format,
                    blend: Some(BlendState::ALPHA_BLENDING),
                    write_mask: ColorWrites::ALL,
                })],
            }),
            primitive: PrimitiveState {
                topology: PrimitiveTopology::LineList,
                strip_index_format: None,
                front_face: FrontFace::Ccw,
                cull_mode: None,
                polygon_mode: PolygonMode::Fill,
                unclipped_depth: false,
                conservative: false,
            },
            depth_stencil: None,
            multisample: msaa_state(),
            multiview_mask: None,
            cache: None,
        });
        self.pipelines.line_overlay = Some(pipeline);
    }

    fn ensure_composite_pipeline(&mut self, device: &Device) {
        if self.pipelines.composite.is_some() {
            return;
        }
        let shader = device.create_shader_module(include_wgsl!("../shaders/composite.wgsl"));
        let layout = device.create_pipeline_layout(&PipelineLayoutDescriptor {
            label: Some("plot composite layout"),
            bind_group_layouts: &[Some(&self.composite_bgl)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&RenderPipelineDescriptor {
            label: Some("plot composite pipeline"),
            layout: Some(&layout),
            vertex: VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                buffers: &[],
            },
            fragment: Some(FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                compilation_options: PipelineCompilationOptions::default(),
                targets: &[Some(ColorTargetState {
                    format: self.format,
                    blend: Some(BlendState {
                        color: BlendComponent {
                            src_factor: BlendFactor::One,
                            dst_factor: BlendFactor::OneMinusSrcAlpha,
                            operation: BlendOperation::Add,
                        },
                        alpha: BlendComponent {
                            src_factor: BlendFactor::One,
                            dst_factor: BlendFactor::OneMinusSrcAlpha,
                            operation: BlendOperation::Add,
                        },
                    }),
                    write_mask: ColorWrites::ALL,
                })],
            }),
            primitive: PrimitiveState {
                topology: PrimitiveTopology::TriangleList,
                strip_index_format: None,
                front_face: FrontFace::Ccw,
                cull_mode: None,
                polygon_mode: PolygonMode::Fill,
                unclipped_depth: false,
                conservative: false,
            },
            depth_stencil: None,
            multisample: MultisampleState::default(),
            multiview_mask: None,
            cache: None,
        });
        self.pipelines.composite = Some(pipeline);
    }

    fn rebuild_markers(&mut self, device: &Device, queue: &Queue, state: &PlotState) {
        // Only include series that have markers (marker != u32::MAX)
        let marker_series_count: usize = state
            .series
            .iter()
            .filter(|s| s.marker != u32::MAX)
            .map(|s| s.len)
            .sum();

        if marker_series_count == 0 {
            VertexBuffer::clear(&mut self.buffers.markers);
            self.picking.clear_id_map(state.picking.projection());
            return;
        }

        let mut writer = std::mem::replace(&mut self.marker_scratch, VertexWriter::new());
        writer.data.clear();
        let mut id_map = self.picking.take_id_map();
        id_map.clear();

        // Iterate series so we can pick per-point color/marker for each point.
        for (span_idx, s) in state.series.iter().enumerate() {
            // Skip series without markers
            if s.marker == u32::MAX {
                continue;
            }

            // safety: ensure span indexes are valid with respect to points slice
            let end = s.start + s.len;
            if s.len == 0 || end > state.points.len() {
                continue;
            }

            for (local_i, p) in state.points[s.start..end].iter().enumerate() {
                // Subtract render_offset for high-precision rendering near zero
                let render_pos = self.world_to_render_pos(p.position, &state.camera);
                let color_idx = s.start + local_i;
                let color = state.point_colors.get(color_idx).unwrap_or(&s.color);
                writer.write_position(render_pos);
                writer.write_color(color);
                writer.write_u32(s.marker);
                writer.write_f32(p.size);
                writer.write_u32(crate::point::marker_flags(p.size_mode, s.pickable));

                let original_idx = s.point_indices.get(local_i).copied().unwrap_or(local_i);
                id_map.push((span_idx as u32, original_idx as u32));
            }
        }

        let data = writer.as_slice();
        let needed = data.len() as u64;

        let recreate = match &self.buffers.markers {
            Some(vb) => vb.buffer.size() < needed,
            None => true,
        };

        if recreate {
            self.buffers.markers = Some(VertexBuffer {
                buffer: device.create_buffer(&BufferDescriptor {
                    label: Some("marker vb"),
                    size: needed.max(2048),
                    usage: BufferUsages::VERTEX | BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                }),
                vertex_count: marker_series_count as u32,
            });
        } else if let Some(vb) = &mut self.buffers.markers {
            vb.vertex_count = marker_series_count as u32;
        }

        if let Some(vb) = &self.buffers.markers {
            queue.write_buffer(&vb.buffer, 0, data);
        }

        // Update picking id map
        self.picking.set_id_map(id_map, state.picking.projection());
        self.marker_scratch = writer;
    }

    fn rebuild_fills(&mut self, device: &Device, queue: &Queue, state: &PlotState) {
        VertexBuffer::clear(&mut self.buffers.fills);
        if state.fills.is_empty() {
            return;
        }

        let mut writer = std::mem::replace(&mut self.scratch, VertexWriter::new());
        writer.data.clear();
        for fill in state.fills.iter() {
            for world_pos in fill.vertices.iter() {
                let render_pos = self.world_to_render_pos(*world_pos, &state.camera);
                writer.write_position(render_pos);
                writer.write_color(&fill.color);
            }
        }


        let data = writer.as_slice();
        VertexBuffer::upload(&mut self.buffers.fills, device, queue, data, (data.len() / 24) as u32);
        self.scratch = writer;
    }

    fn rebuild_lines(&mut self, device: &Device, queue: &Queue, state: &PlotState) {
        if let Some(buffer) = self.buffers.lines.as_mut() { buffer.segments.clear(); }
        if state.series.iter().all(|s| s.line_style.is_none()) {
            return;
        }

        let mut writer = std::mem::replace(&mut self.scratch, VertexWriter::new());
        writer.data.clear();
        let mut segs = self.buffers.lines.as_mut().map(|buffer| std::mem::take(&mut buffer.segments)).unwrap_or_default();
        let mut poly_positions = std::mem::take(&mut self.poly_positions);
        let mut poly_distances = std::mem::take(&mut self.poly_distances);
        let mut poly_colors = std::mem::take(&mut self.poly_colors);
        for s in state.series.iter() {
            let Some(line_style) = s.line_style else {
                continue;
            };
            if s.len < 2 {
                continue;
            }
            let (line_style_u32, style_param) = line_style_params(line_style);
            let render_style = LineRenderStyle {
                width: line_style.width,
                line_style: line_style_u32,
                style_param,
            };
            let points_slice = &state.points[s.start..s.start + s.len];
            poly_positions.clear(); poly_distances.clear(); poly_colors.clear();
            let mut cumulative_distance = 0.0f32;

            for (i, point) in points_slice.iter().enumerate() {
                let break_segment = i > 0
                    && s.point_indices
                        .get(i)
                        .zip(s.point_indices.get(i - 1))
                        .is_some_and(|(curr, prev)| *curr != *prev + 1);

                if break_segment {
                    write_polyline_triangles(
                        &mut writer,
                        &mut segs,
                        PolylineRef {
                            positions: &poly_positions,
                            distances: &poly_distances,
                            colors: &poly_colors,
                        },
                        render_style,
                    );
                    poly_positions.clear();
                    poly_distances.clear();
                    poly_colors.clear();
                    cumulative_distance = 0.0;
                }

                let render_pos = self.world_to_render_pos(point.position, &state.camera);
                let color = *state.point_colors.get(s.start + i).unwrap_or(&s.color);

                if let Some(last_pos) = poly_positions.last() {
                    let dx = render_pos[0] - last_pos[0];
                    let dy = render_pos[1] - last_pos[1];
                    let segment_length = (dx * dx + dy * dy).sqrt();
                    if segment_length <= f32::EPSILON {
                        if let Some(last_color) = poly_colors.last_mut() {
                            *last_color = color;
                        }
                        continue;
                    }
                    cumulative_distance += segment_length;
                }

                poly_positions.push(render_pos);
                poly_distances.push(cumulative_distance);
                poly_colors.push(color);
            }

            write_polyline_triangles(
                &mut writer,
                &mut segs,
                PolylineRef {
                    positions: &poly_positions,
                    distances: &poly_distances,
                    colors: &poly_colors,
                },
                render_style,
            );
        }

        let data = writer.as_slice();
        if !data.is_empty() {
            if self.buffers.lines.as_ref().is_none_or(|buffer| buffer.buffer.size() < data.len() as u64) {
                self.buffers.lines = Some(LineBuffer {
                    buffer: device.create_buffer(&BufferDescriptor {
                        label: Some("retained plot line vertices"), size: data.len() as u64,
                        usage: BufferUsages::VERTEX | BufferUsages::COPY_DST, mapped_at_creation: false,
                    }), segments: Vec::new(),
                });
            }
            let buffer = self.buffers.lines.as_mut().unwrap();
            queue.write_buffer(&buffer.buffer, 0, data);
        }
        if let Some(buffer) = self.buffers.lines.as_mut() { buffer.segments = segs; }
        self.scratch = writer;
        self.poly_positions = poly_positions; self.poly_distances = poly_distances; self.poly_colors = poly_colors;
    }

    fn rebuild_reflines(&mut self, device: &Device, queue: &Queue, state: &PlotState) {
        if let Some(buffer) = self.buffers.reflines.as_mut() { buffer.segments.clear(); }

        if state.vlines.is_empty() && state.hlines.is_empty() {
            return;
        }

        let mut writer = std::mem::replace(&mut self.scratch, VertexWriter::new());
        writer.data.clear();
        let mut segs = self.buffers.reflines.as_mut().map(|buffer| std::mem::take(&mut buffer.segments)).unwrap_or_default();
        let world_per_px = self.world_per_pixel(&state.camera);

        // Get visible viewport bounds in world coordinates
        let cam = &state.camera;
        let left = cam.position.x - cam.half_extents.x;
        let right = cam.position.x + cam.half_extents.x;
        let bottom = cam.position.y - cam.half_extents.y;
        let top = cam.position.y + cam.half_extents.y;

        // Add vertical lines
        for vline in state.vlines.iter() {
            let Some(vx_plot) = data_value_to_plot_with_axis_range(
                vline.x,
                state.x_axis_scale,
                vline.transform.as_ref(),
                Some(state.camera.x_range()),
            ) else {
                continue;
            };
            // Check if the stroked vline still overlaps the viewport.
            let half_width = reference_line_half_extent(vline.line_style.width, true, world_per_px);
            if vx_plot + (half_width as f64) < left || vx_plot - (half_width as f64) > right {
                continue;
            }

            let (line_style_u32, style_param) = line_style_params(vline.line_style);
            let render_style = LineRenderStyle {
                width: vline.line_style.width,
                line_style: line_style_u32,
                style_param,
            };
            // Create two endpoints spanning the visible vertical extent.
            let positions = [
                self.world_to_render_pos([vx_plot, bottom], &state.camera),
                self.world_to_render_pos([vx_plot, top], &state.camera),
            ];
            let distances = [0.0, (top - bottom) as f32];
            let colors = [vline.color, vline.color];
            write_polyline_triangles(
                &mut writer,
                &mut segs,
                PolylineRef {
                    positions: &positions,
                    distances: &distances,
                    colors: &colors,
                },
                render_style,
            );
        }

        // Add horizontal lines
        for hline in state.hlines.iter() {
            let Some(hy_plot) = data_value_to_plot_with_axis_range(
                hline.y,
                state.y_axis_scale,
                hline.transform.as_ref(),
                Some(state.camera.y_range()),
            ) else {
                continue;
            };
            // Check if the stroked hline still overlaps the viewport.
            let half_width =
                reference_line_half_extent(hline.line_style.width, false, world_per_px);
            if hy_plot + (half_width as f64) < bottom || hy_plot - (half_width as f64) > top {
                continue;
            }

            let (line_style_u32, style_param) = line_style_params(hline.line_style);
            let render_style = LineRenderStyle {
                width: hline.line_style.width,
                line_style: line_style_u32,
                style_param,
            };
            // Create two endpoints spanning the visible horizontal extent.
            let positions = [
                self.world_to_render_pos([left, hy_plot], &state.camera),
                self.world_to_render_pos([right, hy_plot], &state.camera),
            ];
            let distances = [0.0, (right - left) as f32];
            let colors = [hline.color, hline.color];
            write_polyline_triangles(
                &mut writer,
                &mut segs,
                PolylineRef {
                    positions: &positions,
                    distances: &distances,
                    colors: &colors,
                },
                render_style,
            );
        }

        let data = writer.as_slice();
        if !data.is_empty() {
            if self.buffers.reflines.as_ref().is_none_or(|buffer| buffer.buffer.size() < data.len() as u64) {
                self.buffers.reflines = Some(LineBuffer {
                    buffer: device.create_buffer(&BufferDescriptor {
                        label: Some("retained plot line vertices"), size: data.len() as u64,
                        usage: BufferUsages::VERTEX | BufferUsages::COPY_DST, mapped_at_creation: false,
                    }), segments: Vec::new(),
                });
            }
            let buffer = self.buffers.reflines.as_mut().unwrap();
            queue.write_buffer(&buffer.buffer, 0, data);
        }
        if let Some(buffer) = self.buffers.reflines.as_mut() { buffer.segments = segs; }
        self.scratch = writer;
    }

    fn rebuild_selection(&mut self, device: &Device, queue: &Queue, state: &PlotState) {
        let w = self.bounds_w.max(1) as f32;
        let h = self.bounds_h.max(1) as f32;
        if w <= 1.0 || h <= 1.0 {
            return;
        }
        if state.selection.active || state.selection.moved {
            let p0 = state.selection.start * self.scale_factor;
            let p1 = state.selection.end * self.scale_factor;
            let min_x = p0.x.min(p1.x);
            let max_x = p0.x.max(p1.x);
            let min_y = p0.y.min(p1.y);
            let max_y = p0.y.max(p1.y);
            let tl = self.screen_to_clip(min_x, min_y);
            let br = self.screen_to_clip(max_x, max_y);
            let tr = [br[0], tl[1]];
            let bl = [tl[0], br[1]];
            let mut data = std::mem::take(&mut self.float_scratch);
            data.clear();
            for v in [tl, tr, bl, br] {
                data.extend_from_slice(&v);
                data.extend_from_slice(&SELECTION_FILL_RGBA);
            }
            let raw = bytemuck::cast_slice(&data);
            VertexBuffer::upload(&mut self.buffers.selection, device, queue, raw, 4);
            self.float_scratch = data;
        } else {
            VertexBuffer::clear(&mut self.buffers.selection);
        }
    }

    fn rebuild_highlight(&mut self, device: &Device, queue: &Queue, state: &PlotState) {
        VertexBuffer::clear(&mut self.buffers.highlight);
        VertexBuffer::clear(&mut self.buffers.highlight_markers);

        if state.highlighted_points.is_empty() {
            return;
        }

        let w = self.bounds_w.max(1) as f32;
        let h = self.bounds_h.max(1) as f32;
        if w <= 1.0 || h <= 1.0 {
            return;
        }

        let mut mask_box_data = std::mem::take(&mut self.float_scratch);
        mask_box_data.clear();
        let mut marker_writer = std::mem::replace(&mut self.marker_scratch, VertexWriter::new());
        marker_writer.data.clear();

        for highlight_point in state.highlighted_points.iter() {
            // Build mask box if enabled
            if let Some(mask_padding) = highlight_point.mask_padding
                && let Some(marker_style) = highlight_point.marker_style
            {
                let Some(world_pos) = highlight_mask_plot_position(highlight_point, state) else {
                    continue;
                };

                // Convert world coordinates to NDC
                let ndc = self.world_to_ndc(world_pos, &state.camera);
                // Calculate mask box size based on marker_size
                let mask_box_size_px =
                    marker_style.size.to_px(&state.camera, &state.bounds) + mask_padding;

                let (dx, dy) = self.pixels_to_clip_delta(mask_box_size_px.max(1.0));

                // Build a quad around the point in clip coords
                let tl = [ndc[0] - dx, ndc[1] + dy];
                let tr = [ndc[0] + dx, ndc[1] + dy];
                let bl = [ndc[0] - dx, ndc[1] - dy];
                let br = [ndc[0] + dx, ndc[1] - dy];
                let color = highlight_mask_rgba(highlight_point.color);
                for v in [tl, tr, bl, br] {
                    mask_box_data.extend_from_slice(&v);
                    mask_box_data.extend_from_slice(&color);
                }
            }

            // Build marker if marker_style is Some
            if let Some(marker_style) = highlight_point.marker_style {
                let Some(plot_pos) = highlight_marker_plot_position(highlight_point, state) else {
                    continue;
                };
                let render_pos = self.world_to_render_pos(plot_pos, &state.camera);
                let (size, size_mode) = marker_style.size.to_raw();
                marker_writer.write_position(render_pos);
                marker_writer.write_color(&highlight_point.color);
                marker_writer.write_u32(marker_style.marker_type as u32);
                marker_writer.write_f32(size);
                marker_writer.write_u32(crate::point::marker_flags(size_mode, true));
            }
        }

        // Create mask box buffer if we have any
        if !mask_box_data.is_empty() {
            let raw = bytemuck::cast_slice(&mask_box_data);
            VertexBuffer::upload(&mut self.buffers.highlight, device, queue, raw, (mask_box_data.len() / 6) as u32);
        }

        // Create marker buffer if we have any
        if !marker_writer.is_empty() {
            let data = marker_writer.as_slice();
            let marker_count = (data.len() / 36) as u32; // 36 bytes per marker instance
            VertexBuffer::upload(&mut self.buffers.highlight_markers, device, queue, data, marker_count);
        }
        self.float_scratch = mask_box_data;
        self.marker_scratch = marker_writer;
    }

    fn rebuild_crosshairs(&mut self, device: &Device, queue: &Queue, state: &PlotState) {
        VertexBuffer::clear(&mut self.buffers.crosshairs);

        if !state.crosshairs_enabled {
            return;
        }

        let w = self.bounds_w.max(1) as f32;
        let h = self.bounds_h.max(1) as f32;
        if w <= 1.0 || h <= 1.0 {
            return;
        }

        // Check if cursor is within bounds
        let pos = state.crosshairs_position * self.scale_factor;
        if pos.x < 0.0 || pos.y < 0.0 || pos.x > w || pos.y > h {
            return;
        }

        // Convert cursor position to clip coordinates
        let cursor_clip = self.screen_to_clip(pos.x, pos.y);

        let mut data = std::mem::take(&mut self.float_scratch);
            data.clear();

        // Horizontal line (left to right through cursor)
        let left = [-1.0, cursor_clip[1]];
        let right = [1.0, cursor_clip[1]];

        // Vertical line (top to bottom through cursor)
        let top = [cursor_clip[0], 1.0];
        let bottom = [cursor_clip[0], -1.0];

        // Add horizontal line vertices
        data.extend_from_slice(&left);
        data.extend_from_slice(&CROSSHAIR_RGBA);
        data.extend_from_slice(&right);
        data.extend_from_slice(&CROSSHAIR_RGBA);

        // Add vertical line vertices
        data.extend_from_slice(&top);
        data.extend_from_slice(&CROSSHAIR_RGBA);
        data.extend_from_slice(&bottom);
        data.extend_from_slice(&CROSSHAIR_RGBA);

        let raw = bytemuck::cast_slice(&data);
        VertexBuffer::upload(&mut self.buffers.crosshairs, device, queue, raw, 4);
            self.float_scratch = data;
    }

    /// Draws all plot content into an existing render pass (iced's main
    /// surface pass). The caller (iced's `shader::Primitive::draw` path) has
    /// already set the viewport to the widget bounds and the scissor to the
    /// clip bounds — the same state `encode` sets on its own passes.
    ///
    /// Rendering in-pass avoids tearing the surface render pass down per plot
    /// (`Load`/`Store` round-trips). On tile-based mobile GPUs (Mali/Adreno)
    /// that pass fragmentation triggered driver-level frame corruption: text
    /// drawn by other passes intermittently disappeared on screens containing
    /// plots.
    pub fn draw_in_pass(&self, pass: &mut iced::wgpu::RenderPass<'_>) {
        // Main content (grid, fills, lines, reference lines, markers)
        self.grid.draw(pass, &self.camera_bind_group);
        if let (Some(pipeline), Some(vb)) = (self.pipelines.fill.as_ref(), &self.buffers.fills) {
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0, &self.camera_bind_group, &[]);
            pass.set_vertex_buffer(0, vb.buffer.slice(..));
            pass.draw(0..vb.vertex_count, 0..1);
        }
        if let (Some(pipeline), Some(lb)) = (self.pipelines.line.as_ref(), &self.buffers.lines) {
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0, &self.camera_bind_group, &[]);
            pass.set_vertex_buffer(0, lb.buffer.slice(..));
            for seg in &lb.segments {
                pass.draw(seg.first_vertex..seg.first_vertex + seg.vertex_count, 0..1);
            }
        }
        if let (Some(pipeline), Some(lb)) = (self.pipelines.line.as_ref(), &self.buffers.reflines) {
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0, &self.camera_bind_group, &[]);
            pass.set_vertex_buffer(0, lb.buffer.slice(..));
            for seg in &lb.segments {
                pass.draw(seg.first_vertex..seg.first_vertex + seg.vertex_count, 0..1);
            }
        }
        if let (Some(pipeline), Some(vb)) = (self.pipelines.marker.as_ref(), &self.buffers.markers)
        {
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0, &self.camera_bind_group, &[]);
            pass.set_vertex_buffer(0, vb.buffer.slice(..));
            pass.draw(0..4, 0..vb.vertex_count);
        }
        if let (Some(pipeline), Some(vb)) = (
            self.pipelines.marker.as_ref(),
            &self.buffers.highlight_markers,
        ) {
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0, &self.camera_bind_group, &[]);
            pass.set_vertex_buffer(0, vb.buffer.slice(..));
            pass.draw(0..4, 0..vb.vertex_count);
        }

        // Selection overlay + highlight mask boxes
        if let Some(pipeline) = self.pipelines.overlay.as_ref() {
            pass.set_pipeline(pipeline);
            if let Some(vb) = &self.buffers.selection {
                pass.set_vertex_buffer(0, vb.buffer.slice(..));
                pass.draw(0..vb.vertex_count, 0..1);
            }
            if let Some(vb) = &self.buffers.highlight {
                pass.set_vertex_buffer(0, vb.buffer.slice(..));
                let quad_count = vb.vertex_count / 4;
                for i in 0..quad_count {
                    pass.draw(i * 4..(i + 1) * 4, 0..1);
                }
            }
        }

        // Crosshairs overlay
        if let (Some(pipeline), Some(vb)) = (
            self.pipelines.line_overlay.as_ref(),
            &self.buffers.crosshairs,
        ) {
            pass.set_pipeline(pipeline);
            pass.set_vertex_buffer(0, vb.buffer.slice(..));
            pass.draw(0..vb.vertex_count, 0..1);
        }
    }

    pub fn encode(&self, params: RenderParams) {
        let Some(msaa_targets) = &self.msaa_targets else {
            return;
        };

        // Convert bounds to viewport coordinates
        let Some((_, visible)) = self.composite_region else { return; };
        let width = msaa_targets.width as f32;
        let height = msaa_targets.height as f32;

        // One chart-local pass preserves painter order and resolves only after all overlays.
        {
            let mut pass = params.encoder.begin_render_pass(&RenderPassDescriptor {
                label: Some("iced_plot main"),
                color_attachments: &[Some(msaa_attachment(
                    msaa_targets,
                    LoadOp::Clear(Color::TRANSPARENT),
                ))],
                depth_stencil_attachment: None,
                occlusion_query_set: None,
                timestamp_writes: None,
                multiview_mask: None,
            });

            // Set viewport and scissor to respect bounds
            pass.set_viewport(0.0, 0.0, width, height, 0.0, 1.0);
            pass.set_scissor_rect(
                0, 0, msaa_targets.width, msaa_targets.height,
            );

            self.draw_in_pass(&mut pass);
        }

        if let Some(pipeline) = self.pipelines.composite.as_ref() {
            let mut pass = params.encoder.begin_render_pass(&RenderPassDescriptor {
                label: Some("iced_plot composite"),
                color_attachments: &[Some(target_attachment(params.target))],
                depth_stencil_attachment: None,
                occlusion_query_set: None,
                timestamp_writes: None,
                multiview_mask: None,
            });

            pass.set_viewport(
                visible.x, visible.y, visible.width, visible.height,
                0.0,
                1.0,
            );
            pass.set_scissor_rect(
                params.clip_bounds.x,
                params.clip_bounds.y,
                params.clip_bounds.width,
                params.clip_bounds.height,
            );
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0, &msaa_targets.composite_bind_group, &[]);
            pass.draw(0..3, 0..1);
        }
    }
}

// The offscreen target is chart-local; only its visible UV subsection is
// composited when the chart is partially outside the window.
fn chart_composite_region(physical: Rectangle, window: Rectangle) -> Option<([f32; 4], Rectangle)> {
    if physical.width <= 0.0 || physical.height <= 0.0 { return None; }
    let visible = physical.intersection(&window)?;
    Some(([(visible.x - physical.x) / physical.width, (visible.y - physical.y) / physical.height,
        visible.width / physical.width, visible.height / physical.height], visible))
}

#[cfg(test)]
mod retained_target_tests {
    use super::*;

    #[cfg(not(target_arch = "wasm32"))]
    fn encode_layered_fixture_reference(renderer: &PlotRenderer, params: RenderParams) {
        // Reproduce the three separate passes from 427b5dd5 for this fixed
        // fill/selection/highlight/crosshair fixture, without draw_in_pass.
        let targets = renderer.msaa_targets.as_ref().unwrap();
        let (_, visible) = renderer.composite_region.unwrap();
        for layer in 0..3 {
            let mut pass = params.encoder.begin_render_pass(&RenderPassDescriptor {
                label: Some("prior chart layer reference"),
                color_attachments: &[Some(msaa_attachment(targets,
                    if layer == 0 { LoadOp::Clear(Color::TRANSPARENT) } else { LoadOp::Load }))],
                depth_stencil_attachment: None,
                occlusion_query_set: None,
                timestamp_writes: None,
                multiview_mask: None,
            });
            pass.set_viewport(0.0, 0.0, targets.width as f32, targets.height as f32, 0.0, 1.0);
            pass.set_scissor_rect(0, 0, targets.width, targets.height);
            match layer {
                0 => {
                    renderer.grid.draw(&mut pass, &renderer.camera_bind_group);
                    pass.set_pipeline(renderer.pipelines.fill.as_ref().unwrap());
                    pass.set_bind_group(0, &renderer.camera_bind_group, &[]);
                    let fill = renderer.buffers.fills.as_ref().unwrap();
                    pass.set_vertex_buffer(0, fill.buffer.slice(..));
                    pass.draw(0..fill.vertex_count, 0..1);
                }
                1 => {
                    pass.set_pipeline(renderer.pipelines.overlay.as_ref().unwrap());
                    let selection = renderer.buffers.selection.as_ref().unwrap();
                    pass.set_vertex_buffer(0, selection.buffer.slice(..));
                    pass.draw(0..selection.vertex_count, 0..1);
                    let highlight = renderer.buffers.highlight.as_ref().unwrap();
                    pass.set_vertex_buffer(0, highlight.buffer.slice(..));
                    for quad in 0..highlight.vertex_count / 4 {
                        pass.draw(quad * 4..(quad + 1) * 4, 0..1);
                    }
                }
                _ => {
                    pass.set_pipeline(renderer.pipelines.line_overlay.as_ref().unwrap());
                    let crosshairs = renderer.buffers.crosshairs.as_ref().unwrap();
                    pass.set_vertex_buffer(0, crosshairs.buffer.slice(..));
                    pass.draw(0..crosshairs.vertex_count, 0..1);
                }
            }
        }
        let mut pass = params.encoder.begin_render_pass(&RenderPassDescriptor {
            label: Some("prior chart composite reference"),
            color_attachments: &[Some(target_attachment(params.target))],
            depth_stencil_attachment: None,
            occlusion_query_set: None,
            timestamp_writes: None,
            multiview_mask: None,
        });
        pass.set_viewport(visible.x, visible.y, visible.width, visible.height, 0.0, 1.0);
        pass.set_scissor_rect(params.clip_bounds.x, params.clip_bounds.y,
            params.clip_bounds.width, params.clip_bounds.height);
        pass.set_pipeline(renderer.pipelines.composite.as_ref().unwrap());
        pass.set_bind_group(0, &targets.composite_bind_group, &[]);
        pass.draw(0..3, 0..1);
    }

    #[test]
    #[cfg(not(target_arch = "wasm32"))]
    fn chart_single_resolve_preserves_layer_colors_alpha_and_clip_guards() {
        let instance = Instance::new(InstanceDescriptor {
            backends: Backends::VULKAN,
            ..InstanceDescriptor::new_without_display_handle()
        });
        let adapter = iced::futures::executor::block_on(instance.request_adapter(&Default::default()))
            .expect("plot color acceptance requires the container Vulkan adapter");
        let (device, queue) = iced::futures::executor::block_on(adapter.request_device(&Default::default()))
            .expect("plot color test device");
        let format = TextureFormat::Rgba8Unorm;
        let mut renderer = PlotRenderer::new(&device, &queue, format);
        let mut state = PlotState::default();
        state.bounds = Rectangle::with_size(iced::Size::new(64.0, 64.0));
        state.camera.position = glam::DVec2::ZERO;
        state.camera.half_extents = glam::DVec2::ONE;
        let viewport = Viewport::with_physical_size(iced::Size::new(64, 64), 1.0);
        renderer.prepare_frame(&device, &queue, &viewport, &state.bounds, &state);
        assert_eq!(MSAA_SAMPLE_COUNT, 4);
        renderer.ensure_fill_pipeline(&device);
        // Fractional-alpha pixels must match the prior GPU sequence exactly.
        // Opaque interiors also retain fixed independent color/order expectations.
        let target = create_color_texture(
            &device, "plot color fixture", 64, 64, 1, format,
            TextureUsages::RENDER_ATTACHMENT | TextureUsages::COPY_SRC,
        );
        let view = target.create_view(&Default::default());
        let readback = device.create_buffer(&BufferDescriptor {
            label: Some("plot color pixels"),
            size: 64 * 256,
            usage: BufferUsages::COPY_DST | BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let vertices = |positions: &[[f32; 2]], color: [f32; 4]| {
            let mut values = Vec::new();
            for position in positions {
                values.extend_from_slice(position);
                values.extend_from_slice(&color);
            }
            values
        };
        for alpha in [0.5, 1.0] {
            let fill = vertices(
                &[[-0.75, -0.75], [0.75, -0.75], [-0.75, 0.75],
                  [-0.75, 0.75], [0.75, -0.75], [0.75, 0.75]],
                [1.0, 0.0, 0.0, alpha],
            );
            VertexBuffer::upload(&mut renderer.buffers.fills, &device, &queue, bytemuck::cast_slice(&fill), 6);
            let selection = vertices(
                &[[-0.5, 0.5], [0.5, 0.5], [-0.5, -0.5], [0.5, -0.5]],
                [0.0, 1.0, 0.0, alpha],
            );
            VertexBuffer::upload(&mut renderer.buffers.selection, &device, &queue, bytemuck::cast_slice(&selection), 4);
            let highlight = vertices(
                &[[-0.25, 0.25], [0.25, 0.25], [-0.25, -0.25], [0.25, -0.25]],
                [0.0, 0.0, 1.0, alpha],
            );
            VertexBuffer::upload(&mut renderer.buffers.highlight, &device, &queue, bytemuck::cast_slice(&highlight), 4);
            let crosshairs = vertices(
                &[[0.015625, -0.75], [0.015625, 0.75], [-0.75, -0.015625], [0.75, -0.015625]],
                [1.0, 1.0, 1.0, alpha],
            );
            VertexBuffer::upload(&mut renderer.buffers.crosshairs, &device, &queue, bytemuck::cast_slice(&crosshairs), 4);
            let render_pixels = |prior: bool| {
                let mut encoder = device.create_command_encoder(&Default::default());
                {
                    let _clear = encoder.begin_render_pass(&RenderPassDescriptor {
                        label: Some("plot fixture clear"),
                        color_attachments: &[Some(RenderPassColorAttachment {
                            view: &view,
                            resolve_target: None,
                            depth_slice: None,
                            ops: Operations { load: LoadOp::Clear(Color::TRANSPARENT), store: StoreOp::Store },
                        })],
                        depth_stencil_attachment: None,
                        timestamp_writes: None,
                        occlusion_query_set: None,
                        multiview_mask: None,
                    });
                }
                let clip_bounds = Rectangle { x: 10, y: 10, width: 44, height: 44 };
                let params = RenderParams { encoder: &mut encoder, target: &view, clip_bounds: &clip_bounds };
                if prior {
                    encode_layered_fixture_reference(&renderer, params);
                } else {
                    renderer.encode(params);
                }
                encoder.copy_texture_to_buffer(target.as_image_copy(), TexelCopyBufferInfo {
                    buffer: &readback,
                    layout: TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(256), rows_per_image: Some(64) },
                }, Extent3d { width: 64, height: 64, depth_or_array_layers: 1 });
                let _ = queue.submit([encoder.finish()]);
                let (send, receive) = std::sync::mpsc::channel();
                readback.slice(..).map_async(MapMode::Read, move |result| send.send(result).unwrap());
                device.poll(PollType::Wait {
                    submission_index: None,
                    timeout: Some(std::time::Duration::from_secs(5)),
                }).unwrap();
                receive.recv().unwrap().unwrap();
                let pixels = readback.slice(..).get_mapped_range().to_vec();
                readback.unmap();
                pixels
            };
            let reference = render_pixels(true);
            let pixels = render_pixels(false);
            assert_eq!(pixels.len(), reference.len());
            for (index, (actual, expected)) in pixels.chunks_exact(4).zip(reference.chunks_exact(4)).enumerate() {
                assert_eq!(actual, expected, "prior-sequence pixel at {},{} with alpha {alpha}", index % 64, index / 64);
            }
            let pixel = |x: usize, y: usize| &pixels[(y * 64 + x) * 4..(y * 64 + x + 1) * 4];
            if alpha == 1.0 {
                for (x, y, expected) in [
                    (12, 12, [255, 0, 0, 255]),
                    (20, 20, [0, 255, 0, 255]),
                    (28, 28, [0, 0, 255, 255]),
                    (32, 28, [255, 255, 255, 255]),
                    (32, 32, [255, 255, 255, 255]),
                ] {
                    assert_eq!(pixel(x, y), expected, "opaque interior at {x},{y}");
                }
            }
            for y in 0..64 {
                for x in 0..64 {
                    if !(10..54).contains(&x) || !(10..54).contains(&y) {
                        assert_eq!(pixel(x, y), [0, 0, 0, 0], "untouched clip guard at {x},{y}");
                    }
                }
            }
        }
    }

    #[test]
    fn chart_targets_clip_scroll_and_resize_without_window_sized_storage() {
        let window = Rectangle { x: 0.0, y: 0.0, width: 1920.0, height: 1080.0 };
        let mut chart = Rectangle { x: 400.0, y: -90.0, width: 640.0, height: 360.0 };
        let (uv, visible) = chart_composite_region(chart, window).unwrap();
        assert_eq!(uv, [0.0, 0.25, 1.0, 0.75]);
        assert_eq!(visible, Rectangle { x: 400.0, y: 0.0, width: 640.0, height: 270.0 });
        chart.y = 100.0; chart.width = 800.0; chart.height = 450.0;
        assert_eq!(chart_composite_region(chart, window).unwrap(), ([0.0, 0.0, 1.0, 1.0], chart));
        chart.width = 0.0;
        assert!(chart_composite_region(chart, window).is_none());
        chart.width = 640.0; chart.y = 1200.0;
        assert!(chart_composite_region(chart, window).is_none());
    }
}

// Helper to extract line style parameters
fn line_style_params(style: LineStyle) -> (u32, f32) {
    match style.line_type {
        LineType::Solid => (0u32, 0.0f32),
        LineType::Dotted { spacing } => (1u32, spacing),
        LineType::Dashed { length } => (2u32, length),
    }
}

fn write_polyline_triangles(
    writer: &mut VertexWriter,
    segs: &mut Vec<LineSegment>,
    polyline: PolylineRef<'_>,
    style: LineRenderStyle,
) {
    if polyline.positions.len() < 2
        || polyline.positions.len() != polyline.distances.len()
        || polyline.positions.len() != polyline.colors.len()
    {
        return;
    }

    let (width, width_mode) = line_width_params(style.width);
    let first_vertex = (writer.byte_len() / 64) as u32;
    for index in 0..polyline.positions.len() - 1 {
        let start = polyline.positions[index];
        let end = polyline.positions[index + 1];
        let segment_length_world = polyline.distances[index + 1] - polyline.distances[index];
        if segment_length_world <= f32::EPSILON {
            continue;
        }

        let start_color = &polyline.colors[index];
        let end_color = &polyline.colors[index + 1];
        let distance_start = polyline.distances[index];

        for (color, along, side) in [
            (start_color, 0.0, 1.0),
            (start_color, 0.0, -1.0),
            (end_color, 1.0, 1.0),
            (start_color, 0.0, -1.0),
            (end_color, 1.0, 1.0),
            (end_color, 1.0, -1.0),
        ] {
            writer.write_line_vertex(LineVertex {
                start,
                end,
                color,
                style: style.line_style,
                distance_start,
                segment_length_world,
                param: style.style_param,
                width,
                width_mode,
                along,
                side,
            });
        }
    }

    let vertex_count = (writer.byte_len() / 64) as u32 - first_vertex;
    if vertex_count > 0 {
        segs.push(LineSegment {
            first_vertex,
            vertex_count,
        });
    }
}

// All inputs consumed by reference geometry; series versions are deliberately absent.
#[derive(Clone, Copy, PartialEq, Debug)]
struct ReferenceKey {
    content: u64,
    camera: crate::camera::Camera,
    width: u32,
    height: u32,
    scale: f32,
    x_axis: crate::AxisScale,
    y_axis: crate::AxisScale,
}
impl ReferenceKey {
    fn new(state: &PlotState, width: u32, height: u32, scale: f32) -> Self {
        Self { content: state.reference_version, camera: state.camera, width, height, scale,
            x_axis: state.x_axis_scale, y_axis: state.y_axis_scale }
    }
}

fn reference_line_half_extent(width: Size, vertical: bool, world_per_px: [f32; 2]) -> f32 {
    match width {
        Size::Pixels(size) => {
            let axis_scale = if vertical {
                world_per_px[0]
            } else {
                world_per_px[1]
            };
            size.max(0.5) * axis_scale * 0.5
        }
        Size::World(size) => size.max(f64::EPSILON) as f32 * 0.5,
    }
}

fn line_width_params(width: Size) -> (f32, u32) {
    match width {
        Size::Pixels(size) => (size.max(0.5), crate::point::MARKER_SIZE_PIXELS),
        Size::World(size) => (
            size.max(f64::EPSILON) as f32,
            crate::point::MARKER_SIZE_WORLD,
        ),
    }
}

#[cfg(test)]
mod reference_dependency_tests {
    use super::*;
    use crate::{Fill, HLine, MarkerStyle, PlotWidget, PlotWidgetBuilder, PointId, Series, VLine};
    use crate::plot_widget::HighlightPoint;

    fn project(plot: &PlotWidget) -> PlotState {
        let mut state = PlotState::default();
        let _ = update_projection(plot, &mut state);
        state
    }

    fn update_projection(plot: &PlotWidget, state: &mut PlotState) -> Option<crate::PlotUiMessage> {
        iced::widget::shader::Program::update(
            plot,
            state,
            &iced::Event::Window(iced::window::Event::RedrawRequested(iced::time::Instant::now())),
            Rectangle::with_size(iced::Size::new(640.0, 360.0)),
            iced::mouse::Cursor::Unavailable,
        ).and_then(|action| action.into_inner().0)
    }

    fn reference(state: &PlotState) -> ReferenceKey {
        ReferenceKey::new(state, 640, 360, 1.0)
    }

    fn populated_plot() -> (PlotWidget, crate::ShapeId, VLine, HLine) {
        let mut plot = PlotWidgetBuilder::new().build().unwrap();
        let series = Series::new(
            vec![[0.0, 0.0], [1.0, 0.25], [2.0, 1.0]],
            MarkerStyle::default(),
            LineStyle::solid(),
        );
        let id = series.id;
        plot.add_series(series).unwrap();
        let vertical = VLine::new(0.5);
        let horizontal = HLine::new(0.0);
        plot.add_vline(vertical.clone());
        plot.add_hline(horizontal.clone());
        plot.add_fill(Fill::new(id, horizontal.id)).unwrap();
        plot.picked_points.insert(PointId { series_id: id, point_index: 1 }, (
            HighlightPoint {
                x: 1.0, y: 0.25, transform: Default::default(), color: iced::Color::BLACK,
                marker_style: Some(MarkerStyle::default()), mask_padding: Some(2.0),
            }, None,
        ));
        (plot, id, vertical, horizontal)
    }

    fn assert_projection_refresh(changes: &BufferChanges) {
        assert!(changes.markers);
        assert!(changes.lines);
        assert!(changes.fills);
        assert!(changes.reference);
        assert!(changes.highlight);
    }

    #[test]
    fn retained_buffers_refresh_real_remounts_with_equal_counters_and_extrema() {
        let (mut plot, id, mut vertical, mut horizontal) = populated_plot();
        let instance = plot.instance_id;
        let mut dependencies = BufferDependencies::new();
        let mut previous = project(&plot);
        assert_projection_refresh(&dependencies.changes(&previous, reference(&previous), true));
        dependencies.synchronized(&previous, reference(&previous));

        for interior in [0.75, 0.125, 0.625] {
            plot.set_series_positions(&id, &[[0.0, 0.0], [1.0, interior], [2.0, 1.0]]);
            vertical.x = interior;
            horizontal.y = interior * 0.1;
            plot.add_vline(vertical.clone());
            plot.add_hline(horizontal.clone());
            plot.picked_points.values_mut().next().unwrap().0.y = interior;
            let replacement = project(&plot);
            assert_eq!(plot.instance_id, instance);
            assert_eq!(previous.data_min, replacement.data_min);
            assert_eq!(previous.data_max, replacement.data_max);
            assert_eq!(previous.camera, replacement.camera);
            assert_eq!(previous.bounds, replacement.bounds);
            assert_eq!(previous.markers_version, replacement.markers_version);
            assert_eq!(previous.lines_version, replacement.lines_version);
            assert_eq!(previous.fills_version, replacement.fills_version);
            assert_eq!(previous.reference_version, replacement.reference_version);
            assert_eq!(previous.highlight_version, replacement.highlight_version);
            assert_ne!(previous.points[1].position, replacement.points[1].position);
            assert_ne!(previous.fills[0].vertices, replacement.fills[0].vertices);
            assert_ne!(previous.vlines[0].x, replacement.vlines[0].x);
            assert_ne!(previous.hlines[0].y, replacement.hlines[0].y);
            assert_ne!(previous.highlighted_points, replacement.highlighted_points);
            let changes = dependencies.changes(&replacement, reference(&replacement), false);
            assert_projection_refresh(&changes);
            // These buffers depend on actual interaction geometry, not local counters.
            assert!(!changes.selection);
            assert!(!changes.crosshair);
            dependencies.synchronized(&replacement, reference(&replacement));
            drop(previous);
            let shallow = replacement.clone();
            assert_eq!(dependencies.changes(&shallow, reference(&shallow), false), BufferChanges::default());
            assert_eq!(dependencies.changes(&replacement, reference(&replacement), false), BufferChanges::default());
            previous = replacement;
        }

        // The cached origin stays owned even after every tree/draw state is gone.
        let retained = previous.origin().clone();
        drop(previous);
        assert!(dependencies.origin.as_ref().unwrap().same_as(&retained));
        let replacement = project(&plot);
        assert!(!retained.same_as(replacement.origin()));
        assert_projection_refresh(&dependencies.changes(&replacement, reference(&replacement), false));
    }

    #[test]
    fn empty_remount_requests_every_projection_clear_then_reuses_it() {
        let (mut plot, id, vertical, horizontal) = populated_plot();
        let mut populated = PlotState::default();
        let publication = update_projection(&plot, &mut populated)
            .expect("initial projection publishes its camera and source");
        plot.update(publication);
        let mut dependencies = BufferDependencies::new();
        dependencies.synchronized(&populated, reference(&populated));
        plot.set_series_positions(&id, &[]);
        plot.update(crate::PlotUiMessage::ToggleSeriesVisibility(vertical.id));
        plot.update(crate::PlotUiMessage::ToggleSeriesVisibility(horizontal.id));
        plot.picked_points.clear();
        // Remount through the settled view retained by the actual render message.
        let empty = project(&plot);
        assert_eq!(empty.camera, populated.camera);
        assert_eq!(empty.lines_version, populated.lines_version);
        assert_eq!(empty.reference_version, populated.reference_version);
        assert!(empty.points.is_empty());
        assert!(empty.series.is_empty());
        assert!(empty.fills.is_empty());
        assert!(empty.vlines.is_empty() && empty.hlines.is_empty());
        assert!(empty.highlighted_points.is_empty());
        // Each true branch runs the existing count/segment clearing path in sync.
        assert_projection_refresh(&dependencies.changes(&empty, reference(&empty), false));
        dependencies.synchronized(&empty, reference(&empty));
        assert_eq!(dependencies.changes(&empty, reference(&empty), false), BufferChanges::default());
        let another_empty = project(&plot);
        assert_projection_refresh(&dependencies.changes(&another_empty, reference(&another_empty), false));
    }

    #[test]
    fn synchronized_projection_preserves_independent_geometry_dependencies() {
        let (mut plot, id, mut vertical, _) = populated_plot();
        let mut state = project(&plot);
        let mut dependencies = BufferDependencies::new();
        dependencies.synchronized(&state, reference(&state));
        vertical.label = Some("label only".into());
        plot.add_vline(vertical);
        let _ = update_projection(&plot, &mut state);
        assert_eq!(dependencies.changes(&state, reference(&state), false), BufferChanges::default());
        plot.set_series_positions(&id, &[[0.0, 0.0], [1.0, 0.5], [2.0, 1.0]]);
        let _ = update_projection(&plot, &mut state);
        let changes = dependencies.changes(&state, reference(&state), false);
        assert!(changes.markers && changes.lines && changes.fills);
        assert!(!changes.reference && !changes.highlight);
        dependencies.synchronized(&state, reference(&state));

        state.selection.active = true;
        state.selection.moved = true;
        state.selection.end = glam::Vec2::new(10.0, 20.0);
        state.crosshairs_enabled = true;
        state.crosshairs_position = glam::Vec2::new(20.0, 30.0);
        let changes = dependencies.changes(&state, reference(&state), false);
        assert_eq!(changes, BufferChanges { selection: true, crosshair: true, ..Default::default() });
        dependencies.synchronized(&state, reference(&state));
        for (width, height, scale) in [(800, 360, 1.0), (640, 480, 1.0), (640, 360, 2.0)] {
            let changes = dependencies.changes(&state, ReferenceKey::new(&state, width, height, scale), true);
            assert!(changes.reference && changes.highlight && changes.selection && changes.crosshair);
            assert!(!changes.markers && !changes.lines && !changes.fills);
        }
        state.camera.position.x += 1.0;
        let changes = dependencies.changes(&state, reference(&state), true);
        assert!(changes.reference && changes.highlight && changes.selection && changes.crosshair);
        dependencies.synchronized(&state, reference(&state));
        state.camera.render_offset.x += 1.0;
        assert_projection_refresh(&dependencies.changes(&state, reference(&state), true));
        dependencies.synchronized(&state, reference(&state));
        state.x_axis_scale = crate::AxisScale::Log { base: 10.0 };
        let changes = dependencies.changes(&state, reference(&state), false);
        assert!(changes.reference && changes.highlight);
        assert!(!changes.markers && !changes.lines && !changes.fills);
    }

    #[test]
    #[cfg(not(target_arch = "wasm32"))]
    fn gpu_picking_settles_old_maps_before_servicing_remounted_or_rebuilt_markers() {
        use crate::picking::{CPU_PICK_THRESHOLD, GpuResultEvent, HoverRequest};

        fn request(plot: &PlotWidget, state: &mut PlotState) {
            let cursor = glam::Vec2::new(
                crate::plot_widget::world_to_screen_position_x(1.0, &state.camera, &state.bounds).unwrap(),
                crate::plot_widget::world_to_screen_position_y(1.0, &state.camera, &state.bounds).unwrap(),
            );
            assert!(matches!(state.picking.request_hover(
                plot.instance_id, cursor, 8.0, false, &state.points, &state.series,
                &state.camera, &state.bounds, |_| true,
            ), HoverRequest::RequestedGpu));
        }

        fn settle(device: &Device) {
            device.poll(PollType::Wait {
                submission_index: None,
                timeout: Some(std::time::Duration::from_secs(5)),
            }).expect("GPU picking callback must settle");
        }

        fn redraw(plot: &mut PlotWidget, state: &mut PlotState) -> iced::window::RedrawRequest {
            let action = iced::widget::shader::Program::update(
                plot, state,
                &iced::Event::Window(iced::window::Event::RedrawRequested(iced::time::Instant::now())),
                Rectangle::with_size(iced::Size::new(640.0, 360.0)),
                iced::mouse::Cursor::Unavailable,
            );
            let Some(action) = action else { return iced::window::RedrawRequest::Wait; };
            let (message, redraw, _) = action.into_inner();
            if let Some(message) = message {
                assert!(message.get_hover_pick_event().is_none(), "stale maps publish no UI event");
                plot.update(message);
            }
            redraw
        }

        let instance = Instance::new(InstanceDescriptor {
            backends: Backends::VULKAN,
            ..InstanceDescriptor::new_without_display_handle()
        });
        let adapter = iced::futures::executor::block_on(instance.request_adapter(&Default::default()))
            .expect("browser-app picking acceptance requires the container Vulkan adapter");
        let (device, queue) = iced::futures::executor::block_on(adapter.request_device(&Default::default()))
            .expect("GPU picking test device");
        let count = CPU_PICK_THRESHOLD + 1;
        let mut points = vec![[0.0, 0.0]; count];
        points[count - 1] = [1.0, 1.0];
        let series = Series::markers_only(points.clone(), MarkerStyle::default());
        let id = series.id;
        let mut plot = PlotWidgetBuilder::new().add_series(series).build().unwrap();
        let mut state = project(&plot);
        let viewport = Viewport::with_physical_size(iced::Size::new(640, 360), 1.0);
        let mut renderer = PlotRenderer::new(&device, &queue, TextureFormat::Bgra8UnormSrgb);
        renderer.prepare_frame(&device, &queue, &viewport, &state.bounds, &state);
        let marker_capacity = renderer.buffers.markers.as_ref().unwrap().buffer.size();
        request(&plot, &mut state);
        renderer.service_picking(plot.instance_id, &device, &queue, &state);

        for (visit, selected) in [count - 2, count - 1, count - 3].into_iter().enumerate() {
            points.fill([0.0, 0.0]);
            points[selected] = [1.0, 1.0];
            plot.set_series_positions(&id, &points);
            if visit < 2 {
                let replacement = project(&plot);
                assert_eq!(state.markers_version, replacement.markers_version);
                assert_eq!(state.camera, replacement.camera);
                state = replacement;
            } else {
                // The same tree can change marker generation while a map is pending.
                let _ = update_projection(&plot, &mut state);
            }
            request(&plot, &mut state);
            renderer.prepare_frame(&device, &queue, &viewport, &state.bounds, &state);
            settle(&device);
            renderer.service_picking(plot.instance_id, &device, &queue, &state);
            assert!(state.picking.consume_gpu_result(plot.instance_id, |_| true).is_none());
            assert!(state.picking.has_outstanding_gpu_request());
            // Successful reuse requires the stale mapping to have been unmapped.
            settle(&device);
            renderer.service_picking(plot.instance_id, &device, &queue, &state);
            assert!(matches!(state.picking.consume_gpu_result(plot.instance_id, |_| true),
                Some(GpuResultEvent::Hover(point)) if point.series_id == id && point.point_index == selected));
            assert!(!state.picking.has_outstanding_gpu_request());
            assert_eq!(renderer.buffers.markers.as_ref().unwrap().buffer.size(), marker_capacity);
            let shallow = state.clone();
            assert_eq!(renderer.dependencies.changes(&shallow, reference(&shallow), false), BufferChanges::default());
            request(&plot, &mut state);
            renderer.service_picking(plot.instance_id, &device, &queue, &state);
        }

        plot.camera_bounds = Some((state.camera, state.bounds));
        plot.set_series_positions(&id, &[]);
        state = project(&plot);
        renderer.prepare_frame(&device, &queue, &viewport, &state.bounds, &state);
        settle(&device);
        renderer.service_picking(plot.instance_id, &device, &queue, &state);
        assert!(state.picking.consume_gpu_result(plot.instance_id, |_| true).is_none());
        assert_eq!(renderer.buffers.markers.as_ref().unwrap().vertex_count, 0);
        assert_eq!(renderer.buffers.markers.as_ref().unwrap().buffer.size(), marker_capacity);

        // Refill after the empty visit proves the previous mapping did not strand staging.
        plot.set_series_positions(&id, &points);
        state = project(&plot);
        request(&plot, &mut state);
        renderer.prepare_frame(&device, &queue, &viewport, &state.bounds, &state);
        renderer.service_picking(plot.instance_id, &device, &queue, &state);
        settle(&device);
        renderer.service_picking(plot.instance_id, &device, &queue, &state);
        assert!(matches!(state.picking.consume_gpu_result(plot.instance_id, |_| true),
            Some(GpuResultEvent::Hover(point)) if point.series_id == id && point.point_index == count - 3));

        // Empty, below-threshold and disabled replacements must schedule physical
        // settlement themselves even though none has a logical GPU request.
        for mode in 0..3 {
            for cancel in [false, true] {
                plot.controls = crate::controls::PlotControls::default();
                plot.set_highlight_on_hover(true);
                plot.set_series_positions(&id, &points);
                state = PlotState::default();
                let _ = redraw(&mut plot, &mut state);
                renderer.prepare_frame(&device, &queue, &viewport, &state.bounds, &state);
                request(&plot, &mut state);
                renderer.service_picking(plot.instance_id, &device, &queue, &state);
                // Hold delivery of the actual map callback, so the first real
                // PollType::Poll sees None on every adapter, including fast ones.
                let release = renderer.picking.hold_pending_callback(cancel);
                match mode {
                    0 => plot.set_series_positions(&id, &[]),
                    1 => plot.set_series_positions(&id, &[[0.0, 0.0], [1.0, 1.0]]),
                    _ => {
                        plot.set_highlight_on_hover(false);
                        plot.controls.unbind_click(iced::mouse::Button::Left);
                    }
                }
                state = PlotState::default();
                let _ = redraw(&mut plot, &mut state); // apply normal projection/UI publication
                renderer.prepare_frame(&device, &queue, &viewport, &state.bounds, &state);
                renderer.service_picking(plot.instance_id, &device, &queue, &state);
                assert!(state.picking.has_outstanding_gpu_request());
                assert_eq!(redraw(&mut plot, &mut state), iced::window::RedrawRequest::NextFrame);
                assert!(state.picking.consume_gpu_result(plot.instance_id, |_| true).is_none());
                // Readiness is made deterministic only AFTER ordinary update has
                // requested the service frame; no manually assumed redraw drives it.
                settle(&device);
                release(&mut renderer.picking);
                renderer.service_picking(plot.instance_id, &device, &queue, &state);
                assert!(!state.picking.has_outstanding_gpu_request());
                assert_eq!(redraw(&mut plot, &mut state), iced::window::RedrawRequest::Wait);
                assert!(state.picking.consume_gpu_result(plot.instance_id, |_| true).is_none());
                assert_eq!(renderer.buffers.markers.as_ref().unwrap().buffer.size(), marker_capacity);
            }
        }

        // A current cancellation publishes its ordinary miss, then becomes idle.
        plot.controls = crate::controls::PlotControls::default();
        plot.set_highlight_on_hover(true);
        state = PlotState::default();
        let _ = redraw(&mut plot, &mut state);
        request(&plot, &mut state);
        renderer.prepare_frame(&device, &queue, &viewport, &state.bounds, &state);
        renderer.service_picking(plot.instance_id, &device, &queue, &state);
        let release = renderer.picking.hold_pending_callback(true);
        renderer.service_picking(plot.instance_id, &device, &queue, &state);
        assert_eq!(redraw(&mut plot, &mut state), iced::window::RedrawRequest::NextFrame);
        settle(&device);
        release(&mut renderer.picking);
        renderer.service_picking(plot.instance_id, &device, &queue, &state);
        assert!(matches!(state.picking.consume_gpu_result(plot.instance_id, |_| true), Some(GpuResultEvent::HoverMiss)));
        assert_eq!(redraw(&mut plot, &mut state), iced::window::RedrawRequest::Wait);

        // Pass teardown cancels a real pending map before relinquishing redraw
        // custody. Unwinding exercises the same physical RAII abandonment path.
        for (unwind, cancel) in [(false, false), (true, false), (false, true), (true, true)] {
            request(&plot, &mut state);
            renderer.service_picking(plot.instance_id, &device, &queue, &state);
            let _held = renderer.picking.hold_pending_callback(cancel);
            let mut replacement = PlotState::default();
            let _ = redraw(&mut plot, &mut replacement);
            renderer.service_picking(plot.instance_id, &device, &queue, &replacement);
            assert_eq!(redraw(&mut plot, &mut replacement), iced::window::RedrawRequest::NextFrame);
            if unwind {
                assert!(std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    let _owner = renderer;
                    panic!("abandon admitted map during renderer failure");
                })).is_err());
            } else {
                drop(renderer);
            }
            assert!(!replacement.picking.has_outstanding_gpu_request(),
                "unwind={unwind} cancel={cancel} state={:?}", replacement.picking);
            assert_eq!(redraw(&mut plot, &mut replacement), iced::window::RedrawRequest::Wait);
            state = replacement;
            renderer = PlotRenderer::new(&device, &queue, TextureFormat::Bgra8UnormSrgb);
            // Missing renderer geometry declines the request before any physical
            // admission. It must publish the ordinary miss without setting custody.
            request(&plot, &mut state);
            renderer.service_picking(plot.instance_id, &device, &queue, &state);
            assert!(matches!(state.picking.consume_gpu_result(plot.instance_id, |_| true), Some(GpuResultEvent::HoverMiss)));
            assert_eq!(redraw(&mut plot, &mut state), iced::window::RedrawRequest::Wait);
            renderer.prepare_frame(&device, &queue, &viewport, &state.bounds, &state);
        }
    }

    #[test]
    fn reference_upload_key_tracks_content_camera_viewport_and_axes_only() {
        let mut state = PlotState::default();
        let original = ReferenceKey::new(&state, 640, 360, 1.0);
        state.lines_version += 1; state.markers_version += 1;
        assert_eq!(original, ReferenceKey::new(&state, 640, 360, 1.0));
        state.reference_version += 1;
        assert_ne!(original, ReferenceKey::new(&state, 640, 360, 1.0));
        state.reference_version -= 1;
        assert_ne!(original, ReferenceKey::new(&state, 800, 360, 1.0));
        assert_ne!(original, ReferenceKey::new(&state, 640, 480, 1.0));
        assert_ne!(original, ReferenceKey::new(&state, 640, 360, 2.0));
        state.camera.position.x += 1.0;
        assert_ne!(original, ReferenceKey::new(&state, 640, 360, 1.0));
        state.camera.position.x -= 1.0;
        state.x_axis_scale = crate::AxisScale::Log { base: 10.0 };
        assert_ne!(original, ReferenceKey::new(&state, 640, 360, 1.0));
        state.x_axis_scale = crate::AxisScale::Linear;
        state.y_axis_scale = crate::AxisScale::Log { base: 10.0 };
        assert_ne!(original, ReferenceKey::new(&state, 640, 360, 1.0));
    }
}
