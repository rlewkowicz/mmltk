use crate::generated::{
    AnnotationDocumentState, CaptureRegion, WorkspacePresent, WorkspaceSurfaceInfo,
};
use crate::message::{Message, Tool};
use crate::transport;
use iced::keyboard::{self, Key, key};
use iced::mouse;
use iced::widget::shader::{self, Viewport};
use iced::{Point, Rectangle, wgpu, window};
use std::borrow::Cow;

const HANDLE_HIT_RADIUS: f32 = 10.0;

const WORKSPACE_SHADER: &str = r#"
struct Uniforms {
    uv_offset: vec2<f32>,
    uv_scale: vec2<f32>,
};

@group(0) @binding(0) var workspace_texture: texture_2d<f32>;
@group(0) @binding(1) var workspace_sampler: sampler;
@group(0) @binding(2) var<uniform> uniforms: Uniforms;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    let positions = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>(3.0, -1.0),
        vec2<f32>(-1.0, 3.0),
    );
    let uvs = array<vec2<f32>, 3>(
        vec2<f32>(0.0, 1.0),
        vec2<f32>(2.0, 1.0),
        vec2<f32>(0.0, -1.0),
    );
    var output: VertexOutput;
    output.position = vec4<f32>(positions[index], 0.0, 1.0);
    output.uv = uniforms.uv_offset + uvs[index] * uniforms.uv_scale;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    return textureSample(workspace_texture, workspace_sampler, input.uv);
}
"#;

#[derive(Debug, Clone)]
pub struct Program {
    config: WorkspaceSurfaceInfo,
    present: WorkspacePresent,
    annotation: AnnotationDocumentState,
    crop: Option<CaptureRegion>,
    tool: Tool,
}

impl Program {
    pub fn new(
        config: WorkspaceSurfaceInfo,
        present: WorkspacePresent,
        annotation: AnnotationDocumentState,
        crop: Option<CaptureRegion>,
        tool: Tool,
    ) -> Self {
        Self {
            config,
            present,
            annotation,
            crop,
            tool,
        }
    }

    fn interactive(&self) -> bool {
        self.config.ready
            && !self.present.revision.is_empty()
            && self.present.generation == self.config.generation
            && self.present.slot < self.config.slot_count
    }

    fn mapped_pointer(
        &self,
        bounds: Rectangle,
        point: Point,
        clamp: bool,
    ) -> Option<MappedPointer> {
        let frame_width = self.present.width.max(1) as f32;
        let frame_height = self.present.height.max(1) as f32;
        let scale = (bounds.width / frame_width).min(bounds.height / frame_height);
        if !scale.is_finite() || scale <= 0.0 {
            return None;
        }
        let viewport_width = frame_width * scale;
        let viewport_height = frame_height * scale;
        let viewport_x = bounds.x + (bounds.width - viewport_width) * 0.5;
        let viewport_y = bounds.y + (bounds.height - viewport_height) * 0.5;
        let inside = point.x >= viewport_x
            && point.y >= viewport_y
            && point.x <= viewport_x + viewport_width
            && point.y <= viewport_y + viewport_height;
        if !inside && !clamp {
            return None;
        }
        let local_x = (point.x - viewport_x).clamp(0.0, viewport_width);
        let local_y = (point.y - viewport_y).clamp(0.0, viewport_height);
        let region = &self.present.source_region;
        let source_width = region.width.max(1) as f32;
        let source_height = region.height.max(1) as f32;
        let capture_x = region.x as f32 + local_x * source_width / viewport_width;
        let capture_y = region.y as f32 + local_y * source_height / viewport_height;
        Some(MappedPointer {
            canvas_x: local_x as f64,
            canvas_y: local_y as f64,
            capture_x: capture_x.floor().clamp(
                region.x as f32,
                region
                    .x
                    .saturating_add(region.width.max(1))
                    .saturating_sub(1) as f32,
            ) as i32,
            capture_y: capture_y.floor().clamp(
                region.y as f32,
                region
                    .y
                    .saturating_add(region.height.max(1))
                    .saturating_sub(1) as f32,
            ) as i32,
            capture_units_per_logical_x: source_width / viewport_width,
            capture_units_per_logical_y: source_height / viewport_height,
        })
    }

    fn hit_test(&self, pointer: &MappedPointer) -> PointerHit {
        let mut hit = PointerHit::default();
        if self.tool != Tool::Select && self.tool != Tool::Direct {
            if self.tool == Tool::Box {
                hit.drag_kind = Some("create");
            }
            return hit;
        }

        let radius_x = HANDLE_HIT_RADIUS * pointer.capture_units_per_logical_x;
        let radius_y = HANDLE_HIT_RADIUS * pointer.capture_units_per_logical_y;
        let normalized_distance = |x: i32, y: i32| {
            let dx = (pointer.capture_x - x) as f32 / radius_x.max(1.0);
            let dy = (pointer.capture_y - y) as f32 / radius_y.max(1.0);
            dx * dx + dy * dy
        };

        if self.tool == Tool::Direct {
            if let Some(handle) = self
                .annotation
                .handles
                .iter()
                .rev()
                .filter_map(|handle| {
                    let distance = normalized_distance(handle.x, handle.y);
                    (distance <= 1.0).then_some((distance, handle))
                })
                .min_by(|left, right| left.0.total_cmp(&right.0))
                .map(|(_, handle)| handle)
            {
                hit.object_index = Some(handle.object_index);
                hit.handle_element_index = Some(handle.element_index);
                hit.handle_role = Some(handle.role.clone());
                return hit;
            }
        }

        for object in self.annotation.objects.iter().rev() {
            let x1 = object.x1.min(object.x2);
            let x2 = object.x1.max(object.x2);
            let y1 = object.y1.min(object.y2);
            let y2 = object.y1.max(object.y2);
            if self.tool == Tool::Direct {
                let corners = [
                    (x1, y1, "resize_top_left"),
                    (x2, y1, "resize_top_right"),
                    (x1, y2, "resize_bottom_left"),
                    (x2, y2, "resize_bottom_right"),
                ];
                if let Some((_, _, drag_kind)) = corners
                    .into_iter()
                    .filter(|(x, y, _)| normalized_distance(*x, *y) <= 1.0)
                    .min_by(|left, right| {
                        normalized_distance(left.0, left.1)
                            .total_cmp(&normalized_distance(right.0, right.1))
                    })
                {
                    hit.object_index = Some(object.object_index);
                    hit.drag_kind = Some(drag_kind);
                    return hit;
                }
            }
            if pointer.capture_x >= x1
                && pointer.capture_x <= x2
                && pointer.capture_y >= y1
                && pointer.capture_y <= y2
            {
                hit.object_index = Some(object.object_index);
                if self.tool == Tool::Direct {
                    hit.drag_kind = Some("move");
                }
                return hit;
            }
        }

        if self.tool == Tool::Direct
            && let Some(crop) = &self.crop
        {
            let x1 = crop.x as i32;
            let y1 = crop.y as i32;
            let x2 = crop.x.saturating_add(crop.width) as i32;
            let y2 = crop.y.saturating_add(crop.height) as i32;
            let corners = [
                (x1, y1, "resize_top_left"),
                (x2, y1, "resize_top_right"),
                (x1, y2, "resize_bottom_left"),
                (x2, y2, "resize_bottom_right"),
            ];
            if let Some((_, _, drag_kind)) = corners
                .into_iter()
                .filter(|(x, y, _)| normalized_distance(*x, *y) <= 1.0)
                .min_by(|left, right| {
                    normalized_distance(left.0, left.1)
                        .total_cmp(&normalized_distance(right.0, right.1))
                })
            {
                hit.drag_kind = Some(drag_kind);
                return hit;
            }
            if pointer.capture_x >= x1
                && pointer.capture_x <= x2
                && pointer.capture_y >= y1
                && pointer.capture_y <= y2
            {
                hit.drag_kind = Some("move");
            }
        }
        hit
    }

    fn pointer_message(
        &self,
        phase: &'static str,
        pointer: MappedPointer,
        hit: PointerHit,
    ) -> Message {
        Message::WorkspacePointer {
            phase,
            canvas_x: pointer.canvas_x,
            canvas_y: pointer.canvas_y,
            capture_x: pointer.capture_x,
            capture_y: pointer.capture_y,
            object_index: hit.object_index,
            drag_kind: hit.drag_kind,
            handle_element_index: hit.handle_element_index,
            handle_role: hit.handle_role,
        }
    }
}

#[derive(Debug, Clone, Default)]
struct PointerHit {
    object_index: Option<u32>,
    drag_kind: Option<&'static str>,
    handle_element_index: Option<u32>,
    handle_role: Option<String>,
}

#[derive(Debug, Clone)]
struct MappedPointer {
    canvas_x: f64,
    canvas_y: f64,
    capture_x: i32,
    capture_y: i32,
    capture_units_per_logical_x: f32,
    capture_units_per_logical_y: f32,
}

impl MappedPointer {
    fn outside() -> Self {
        Self {
            canvas_x: -1.0,
            canvas_y: -1.0,
            capture_x: -1,
            capture_y: -1,
            capture_units_per_logical_x: 1.0,
            capture_units_per_logical_y: 1.0,
        }
    }
}

#[derive(Debug, Clone)]
struct PendingPointer {
    pointer: MappedPointer,
    hit: PointerHit,
    phase: &'static str,
}

impl PendingPointer {
    fn outside_hover() -> Self {
        Self {
            pointer: MappedPointer::outside(),
            hit: PointerHit::default(),
            phase: "hover",
        }
    }

    fn is_inside(&self) -> bool {
        self.pointer.capture_x >= 0 && self.pointer.capture_y >= 0
    }
}

#[derive(Default)]
pub struct State {
    generation: String,
    active: Option<PendingPointer>,
    pending: Option<PendingPointer>,
    native_pointer_inside: bool,
    cursor_outside: bool,
}

impl shader::Program<Message> for Program {
    type State = State;
    type Primitive = Primitive;

    fn draw(
        &self,
        _state: &Self::State,
        _cursor: mouse::Cursor,
        _bounds: Rectangle,
    ) -> Self::Primitive {
        Primitive {
            config: self.config.clone(),
            present: self.present.clone(),
        }
    }

    fn update(
        &self,
        state: &mut Self::State,
        event: &iced::Event,
        bounds: Rectangle,
        cursor: mouse::Cursor,
    ) -> Option<shader::Action<Message>> {
        if state.generation != self.config.generation {
            let clear_pointer = state.native_pointer_inside;
            state.generation.clone_from(&self.config.generation);
            state.pending = None;
            state.native_pointer_inside = false;
            if let Some(active) = state.active.take() {
                return Some(
                    shader::Action::publish(self.pointer_message(
                        "cancel",
                        active.pointer,
                        active.hit,
                    ))
                    .and_capture(),
                );
            }
            if clear_pointer {
                let outside = PendingPointer::outside_hover();
                return Some(shader::Action::publish(self.pointer_message(
                    outside.phase,
                    outside.pointer,
                    outside.hit,
                )));
            }
        }

        if !self.interactive() {
            let clear_pointer = state.native_pointer_inside;
            state.pending = None;
            state.native_pointer_inside = false;
            if let Some(active) = state.active.take() {
                return Some(
                    shader::Action::publish(self.pointer_message(
                        "cancel",
                        active.pointer,
                        active.hit,
                    ))
                    .and_capture(),
                );
            }
            if clear_pointer {
                let outside = PendingPointer::outside_hover();
                return Some(shader::Action::publish(self.pointer_message(
                    outside.phase,
                    outside.pointer,
                    outside.hit,
                )));
            }
            return None;
        }

        match event {
            iced::Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)) => {
                let pointer = self.mapped_pointer(bounds, cursor.position()?, false)?;
                let hit = self.hit_test(&pointer);
                let active = PendingPointer {
                    pointer,
                    hit,
                    phase: "begin",
                };
                state.pending = None;
                state.active = Some(active.clone());
                state.native_pointer_inside = true;
                state.cursor_outside = false;
                Some(
                    shader::Action::publish(self.pointer_message(
                        active.phase,
                        active.pointer,
                        active.hit,
                    ))
                    .and_capture(),
                )
            }
            iced::Event::Mouse(mouse::Event::CursorMoved { position }) => {
                let dragging = state.active.is_some();
                let inside_pointer = self.mapped_pointer(bounds, *position, false);
                state.cursor_outside = inside_pointer.is_none();
                let pointer = if dragging {
                    self.mapped_pointer(bounds, *position, true)
                        .unwrap_or_else(MappedPointer::outside)
                } else {
                    inside_pointer.unwrap_or_else(MappedPointer::outside)
                };
                let hit = if dragging {
                    state.active.as_ref()?.hit.clone()
                } else if pointer.capture_x < 0 || pointer.capture_y < 0 {
                    PointerHit::default()
                } else {
                    self.hit_test(&pointer)
                };
                state.pending = Some(PendingPointer {
                    pointer,
                    hit,
                    phase: if dragging { "update" } else { "hover" },
                });
                let action = shader::Action::request_redraw();
                Some(if dragging {
                    action.and_capture()
                } else {
                    action
                })
            }
            iced::Event::Window(window::Event::RedrawRequested(_)) => {
                let pending = state.pending.take()?;
                state.native_pointer_inside = pending.is_inside();
                if let Some(active) = state.active.as_mut() {
                    active.pointer = pending.pointer.clone();
                }
                let action = shader::Action::publish(self.pointer_message(
                    pending.phase,
                    pending.pointer,
                    pending.hit,
                ));
                Some(if state.active.is_some() {
                    action.and_capture()
                } else {
                    action
                })
            }
            iced::Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left))
                if state.active.is_some() =>
            {
                let mut active = state.active.take()?;
                if let Some(position) = cursor.position()
                    && let Some(pointer) = self.mapped_pointer(bounds, position, true)
                {
                    active.pointer = pointer;
                }
                state.pending = None;
                state.native_pointer_inside = true;
                if state.cursor_outside {
                    state.pending = Some(PendingPointer::outside_hover());
                }
                Some(
                    shader::Action::publish(self.pointer_message(
                        "end",
                        active.pointer,
                        active.hit,
                    ))
                    .and_capture(),
                )
            }
            iced::Event::Window(window::Event::Unfocused | window::Event::CloseRequested)
            | iced::Event::Keyboard(keyboard::Event::KeyPressed {
                key: Key::Named(key::Named::Escape),
                ..
            }) if state.active.is_some() => {
                let active = state.active.take()?;
                state.pending = None;
                state.native_pointer_inside = false;
                state.cursor_outside = true;
                Some(
                    shader::Action::publish(self.pointer_message(
                        "cancel",
                        active.pointer,
                        active.hit,
                    ))
                    .and_capture(),
                )
            }
            iced::Event::Mouse(mouse::Event::CursorLeft) => {
                state.cursor_outside = true;
                let clear_pointer = state.native_pointer_inside;
                state.pending = None;
                state.native_pointer_inside = false;
                clear_pointer.then(|| {
                    let outside = PendingPointer::outside_hover();
                    shader::Action::publish(self.pointer_message(
                        outside.phase,
                        outside.pointer,
                        outside.hit,
                    ))
                })
            }
            iced::Event::Window(window::Event::Unfocused | window::Event::CloseRequested)
                if state.active.is_none()
                    && (state.native_pointer_inside
                        || state
                            .pending
                            .as_ref()
                            .is_some_and(PendingPointer::is_inside)) =>
            {
                let clear_pointer = state.native_pointer_inside;
                state.pending = None;
                state.native_pointer_inside = false;
                state.cursor_outside = true;
                clear_pointer.then(|| {
                    let outside = PendingPointer::outside_hover();
                    shader::Action::publish(self.pointer_message(
                        outside.phase,
                        outside.pointer,
                        outside.hit,
                    ))
                })
            }
            _ => None,
        }
    }

    fn mouse_interaction(
        &self,
        state: &Self::State,
        bounds: Rectangle,
        cursor: mouse::Cursor,
    ) -> mouse::Interaction {
        if !self.interactive() {
            return mouse::Interaction::default();
        }
        if state.active.is_some() {
            return mouse::Interaction::Grabbing;
        }
        let Some(position) = cursor.position() else {
            return mouse::Interaction::default();
        };
        let Some(pointer) = self.mapped_pointer(bounds, position, false) else {
            return mouse::Interaction::default();
        };
        let hit = self.hit_test(&pointer);
        if hit.handle_element_index.is_some() || hit.drag_kind.is_some() {
            mouse::Interaction::Grab
        } else if hit.object_index.is_some() {
            mouse::Interaction::Pointer
        } else {
            mouse::Interaction::Crosshair
        }
    }
}

#[derive(Debug)]
pub struct Primitive {
    config: WorkspaceSurfaceInfo,
    present: WorkspacePresent,
}

impl shader::Primitive for Primitive {
    type Pipeline = Pipeline;

    fn prepare(
        &self,
        pipeline: &mut Pipeline,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        _bounds: &Rectangle,
        _viewport: &Viewport,
    ) {
        pipeline.prepare(device, queue, &self.config, &self.present);
    }

    fn render(
        &self,
        pipeline: &Pipeline,
        encoder: &mut wgpu::CommandEncoder,
        target: &wgpu::TextureView,
        clip_bounds: &Rectangle<u32>,
    ) {
        if self.present.revision.is_empty()
            || !transport::mark_workspace_surface_submitted(
                &self.present.generation,
                self.present.slot,
                &self.present.revision,
            )
        {
            return;
        }
        if !pipeline.render(encoder, target, *clip_bounds, &self.present) {
            transport::cancel_workspace_surface_submission(
                self.present.generation.clone(),
                self.present.slot,
                self.present.revision.clone(),
            );
            return;
        }
        transport::confirm_workspace_surface_encoded(
            &self.present.generation,
            self.present.slot,
            &self.present.revision,
        );
        transport::trace_workspace_surface(
            "workspace.encoded",
            &self.present.generation,
            self.present.slot,
            &self.present.revision,
            Some(&self.present.ready_ns),
            Some(true),
        );
        let generation = self.present.generation.clone();
        let revision = self.present.revision.clone();
        let slot = self.present.slot;
        encoder.on_submitted_work_done(move || {
            transport::complete_workspace_surface_submission(generation, slot, revision);
        });
    }
}

pub struct Pipeline {
    render_pipeline: wgpu::RenderPipeline,
    bind_group_layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    uniform_buffer: wgpu::Buffer,
    generation: String,
    capacity: (u32, u32),
    frame_dimensions: (u32, u32),
    bind_groups: Vec<wgpu::BindGroup>,
    views: Vec<wgpu::TextureView>,
    textures: Vec<wgpu::Texture>,
}

impl shader::Pipeline for Pipeline {
    fn new(device: &wgpu::Device, _queue: &wgpu::Queue, format: wgpu::TextureFormat) -> Self {
        let bind_group_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("mmltk workspace bind group layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::VERTEX,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let uniform_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("mmltk workspace uniforms"),
            size: 16,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("mmltk workspace sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..wgpu::SamplerDescriptor::default()
        });
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("mmltk workspace shader"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(WORKSPACE_SHADER)),
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("mmltk workspace pipeline layout"),
            bind_group_layouts: &[Some(&bind_group_layout)],
            immediate_size: 0,
        });
        let render_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("mmltk workspace pipeline"),
            layout: Some(&layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: wgpu::PipelineCompilationOptions::default(),
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                targets: &[Some(wgpu::ColorTargetState {
                    format,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: wgpu::PipelineCompilationOptions::default(),
            }),
            multiview_mask: None,
            cache: None,
        });
        Self {
            render_pipeline,
            bind_group_layout,
            sampler,
            uniform_buffer,
            generation: String::new(),
            capacity: (0, 0),
            frame_dimensions: (0, 0),
            bind_groups: Vec::new(),
            views: Vec::new(),
            textures: Vec::new(),
        }
    }
}

impl Pipeline {
    fn prepare(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        config: &WorkspaceSurfaceInfo,
        present: &WorkspacePresent,
    ) {
        let slot_count = config.slot_count.min(16);
        if config.generation.is_empty()
            || config.format != "rgba8unorm"
            || config.orientation != "upright"
            || config.capacity_width == 0
            || config.capacity_height == 0
            || slot_count == 0
        {
            return;
        }
        let capacity = (config.capacity_width, config.capacity_height);
        if self.generation != config.generation
            || self.capacity != capacity
            || self.textures.len() != slot_count as usize
        {
            self.bind_groups.clear();
            self.views.clear();
            self.textures.clear();
            self.textures.reserve(slot_count as usize);
            self.views.reserve(slot_count as usize);
            self.bind_groups.reserve(slot_count as usize);
            for slot in 0..slot_count {
                let label = format!("mmltk-workspace-v1/{}/{slot}", config.generation);
                self.textures
                    .push(device.create_texture(&wgpu::TextureDescriptor {
                        label: Some(&label),
                        size: wgpu::Extent3d {
                            width: config.capacity_width,
                            height: config.capacity_height,
                            depth_or_array_layers: 1,
                        },
                        mip_level_count: 1,
                        sample_count: 1,
                        dimension: wgpu::TextureDimension::D2,
                        format: wgpu::TextureFormat::Rgba8Unorm,
                        usage: wgpu::TextureUsages::TEXTURE_BINDING,
                        view_formats: &[],
                    }));
                self.views.push(
                    self.textures
                        .last()
                        .expect("workspace texture was inserted")
                        .create_view(&wgpu::TextureViewDescriptor::default()),
                );
                self.bind_groups
                    .push(device.create_bind_group(&wgpu::BindGroupDescriptor {
                        label: Some("mmltk workspace bind group"),
                        layout: &self.bind_group_layout,
                        entries: &[
                            wgpu::BindGroupEntry {
                                binding: 0,
                                resource: wgpu::BindingResource::TextureView(
                                    self.views.last().expect("workspace view was inserted"),
                                ),
                            },
                            wgpu::BindGroupEntry {
                                binding: 1,
                                resource: wgpu::BindingResource::Sampler(&self.sampler),
                            },
                            wgpu::BindGroupEntry {
                                binding: 2,
                                resource: self.uniform_buffer.as_entire_binding(),
                            },
                        ],
                    }));
            }
            self.generation.clone_from(&config.generation);
            self.capacity = capacity;
            self.frame_dimensions = (0, 0);
        }

        let frame_dimensions = (
            present.width.max(1).min(config.capacity_width),
            present.height.max(1).min(config.capacity_height),
        );
        if self.frame_dimensions != frame_dimensions {
            let values = [
                0.5 / config.capacity_width as f32,
                0.5 / config.capacity_height as f32,
                frame_dimensions.0.saturating_sub(1) as f32 / config.capacity_width as f32,
                frame_dimensions.1.saturating_sub(1) as f32 / config.capacity_height as f32,
            ];
            let mut bytes = [0_u8; 16];
            for (index, value) in values.into_iter().enumerate() {
                bytes[index * 4..index * 4 + 4].copy_from_slice(&value.to_ne_bytes());
            }
            queue.write_buffer(&self.uniform_buffer, 0, &bytes);
            self.frame_dimensions = frame_dimensions;
        }
    }

    fn render(
        &self,
        encoder: &mut wgpu::CommandEncoder,
        target: &wgpu::TextureView,
        clip_bounds: Rectangle<u32>,
        present: &WorkspacePresent,
    ) -> bool {
        if self.generation != present.generation
            || clip_bounds.width == 0
            || clip_bounds.height == 0
        {
            return false;
        }
        let Some(bind_group) = self.bind_groups.get(present.slot as usize) else {
            return false;
        };
        let frame_width = present.width.max(1) as f32;
        let frame_height = present.height.max(1) as f32;
        let scale =
            (clip_bounds.width as f32 / frame_width).min(clip_bounds.height as f32 / frame_height);
        let viewport_width = (frame_width * scale).max(1.0);
        let viewport_height = (frame_height * scale).max(1.0);
        let viewport_x = clip_bounds.x as f32 + (clip_bounds.width as f32 - viewport_width) * 0.5;
        let viewport_y = clip_bounds.y as f32 + (clip_bounds.height as f32 - viewport_height) * 0.5;
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("mmltk workspace pass"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: target,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Load,
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_viewport(
            viewport_x,
            viewport_y,
            viewport_width,
            viewport_height,
            0.0,
            1.0,
        );
        pass.set_scissor_rect(
            clip_bounds.x,
            clip_bounds.y,
            clip_bounds.width,
            clip_bounds.height,
        );
        pass.set_pipeline(&self.render_pipeline);
        pass.set_bind_group(0, Some(bind_group), &[]);
        pass.draw(0..3, 0..1);
        true
    }
}
