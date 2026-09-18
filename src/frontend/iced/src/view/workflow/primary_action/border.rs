//! Analytic rounded-perimeter decoration. One shared pipeline per Iced device;
//! one tiny retained uniform binding per widget, never a per-frame mesh.
use iced::widget::shader;
use iced::{Color, Rectangle};
use std::sync::{Arc, Mutex};

#[derive(Clone, Default, Debug)]
pub(super) struct Resources(Arc<Mutex<Option<Binding>>>);
#[derive(Debug)]
struct Binding {
    device: u64,
    uniform: wgpu::Buffer,
    group: wgpu::BindGroup,
}

#[derive(Clone, Debug)]
pub(super) struct Border {
    pub phase: f32,
    pub blue: Color,
    pub resources: Resources,
}
impl<Message> shader::Program<Message> for Border {
    type State = super::Animation;
    type Primitive = Self;
    fn draw(&self, _state: &Self::State, _cursor: iced::mouse::Cursor, _bounds: Rectangle) -> Self {
        self.clone()
    }
}

pub(super) struct Pipeline {
    identity: u64,
    render: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
}
impl shader::Pipeline for Pipeline {
    fn new(device: &wgpu::Device, _queue: &wgpu::Queue, format: wgpu::TextureFormat) -> Self {
        static NEXT_DEVICE: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("primary border uniforms"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("primary border"),
            source: wgpu::ShaderSource::Wgsl(SHADER.into()),
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("primary border"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let render = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("primary border"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &module,
                entry_point: Some("vertex"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            fragment: Some(wgpu::FragmentState {
                module: &module,
                entry_point: Some("fragment"),
                targets: &[Some(wgpu::ColorTargetState {
                    format,
                    blend: Some(wgpu::BlendState::ALPHA_BLENDING),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: Default::default(),
            }),
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
            multiview_mask: None,
            cache: None,
        });
        Self {
            identity: NEXT_DEVICE.fetch_add(1, std::sync::atomic::Ordering::Relaxed),
            render,
            layout,
        }
    }
}
impl shader::Primitive for Border {
    type Pipeline = Pipeline;
    fn prepare(
        &self,
        pipeline: &mut Pipeline,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        bounds: &Rectangle,
        viewport: &shader::Viewport,
    ) {
        let mut retained = self.resources.0.lock().expect("primary border binding");
        if retained
            .as_ref()
            .is_none_or(|binding| binding.device != pipeline.identity)
        {
            let uniform = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("primary border"),
                size: 48,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            let group = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("primary border"),
                layout: &pipeline.layout,
                entries: &[wgpu::BindGroupEntry {
                    binding: 0,
                    resource: uniform.as_entire_binding(),
                }],
            });
            *retained = Some(Binding {
                device: pipeline.identity,
                uniform,
                group,
            });
        }
        let blue = self.blue.into_linear();
        // Retain the aligned 48-byte binding; unused vector lanes are padding.
        let values = [
            bounds.width,
            bounds.height,
            0.0,
            0.0,
            viewport.scale_factor(),
            self.phase,
            0.0,
            0.0,
            blue[0],
            blue[1],
            blue[2],
            blue[3],
        ];
        let mut bytes = [0u8; 48];
        for (value, target) in values.into_iter().zip(bytes.chunks_exact_mut(4)) {
            target.copy_from_slice(&value.to_ne_bytes());
        }
        queue.write_buffer(
            &retained.as_ref().expect("prepared binding").uniform,
            0,
            &bytes,
        );
    }
    fn draw(
        &self,
        pipeline: &Pipeline,
        pass: &mut wgpu::RenderPass<'_>,
        _resources: &mut shader::Resources,
    ) -> bool {
        let retained = self.resources.0.lock().expect("primary border binding");
        let Some(binding) = retained
            .as_ref()
            .filter(|binding| binding.device == pipeline.identity)
        else {
            return true;
        };
        pass.set_pipeline(&pipeline.render);
        pass.set_bind_group(0, &binding.group, &[]);
        pass.draw(0..6, 0..8);
        true
    }
}

// Distances are measured along the centre of the existing 3px band (radius
// 8.5). The straight lengths and quarter-circle lengths form one continuous
// clockwise parameter, including the wrap from the top-left corner to the top.
const SHADER: &str = r#"
struct Uniforms { size: vec4<f32>, motion: vec4<f32>, blue: vec4<f32> }
@group(0) @binding(0) var<uniform> u: Uniforms;
struct Vertex { @builtin(position) position: vec4<f32>, @location(0) point: vec2<f32> }
@vertex fn vertex(@builtin(vertex_index) index: u32, @builtin(instance_index) instance: u32) -> Vertex {
    let corners = array<vec2<f32>,6>(vec2(0.,0.), vec2(1.,0.), vec2(0.,1.), vec2(0.,1.), vec2(1.,0.), vec2(1.,1.));
    let r = min(10., min(u.size.x, u.size.y) * 0.5);
    let w = u.size.x;
    let h = u.size.y;
    // Four tightly sized straight strips plus four corner squares: no core
    // fragment work, mesh construction, or vertex-buffer upload.
    let regions = array<vec4<f32>,8>(
        vec4(r,0.,w-2.*r,4.), vec4(w-4.,r,4.,h-2.*r),
        vec4(r,h-4.,w-2.*r,4.), vec4(0.,r,4.,h-2.*r),
        vec4(0.,0.,r,r), vec4(w-r,0.,r,r),
        vec4(w-r,h-r,r,r), vec4(0.,h-r,r,r));
    let region = regions[instance];
    let point = region.xy + corners[index] * region.zw;
    // Iced already places this primitive in its physical button viewport.
    // Emit viewport-local NDC while preserving logical fragment coordinates.
    let ndc = point / u.size.xy * 2. - 1.;
    return Vertex(vec4(ndc.x, -ndc.y, 0., 1.), point);
}
@fragment fn fragment(v: Vertex) -> @location(0) vec4<f32> {
    let radius = min(10., min(u.size.x, u.size.y) * 0.5);
    let r = max(radius - 1.5, 0.001);
    let half = u.size.xy * 0.5;
    let q = abs(v.point - half) - (half - vec2(radius));
    let distance = length(max(q, vec2(0.))) + min(max(q.x,q.y),0.) - r;
    let coverage = clamp((1.5 - abs(distance)) * u.motion.x + 0.5, 0., 1.);
    if coverage <= 0. { discard; }
    let w = u.size.x - 2. * radius;
    let h = u.size.y - 2. * radius;
    let arc = r * 1.5707963267948966;
    let perimeter = 2. * (w + h) + 4. * arc;
    let p = v.point;
    var distance_along = 0.;
    if p.y < radius && p.x >= radius && p.x <= u.size.x-radius {
        distance_along = p.x-radius;
    } else if p.x > u.size.x-radius && p.y < radius {
        distance_along = w + r * (atan2(p.y-radius, p.x-(u.size.x-radius)) + 1.5707963267948966);
    } else if p.x > u.size.x-radius && p.y <= u.size.y-radius {
        distance_along = w + arc + p.y-radius;
    } else if p.x > u.size.x-radius {
        distance_along = w + arc + h + r * atan2(p.y-(u.size.y-radius), p.x-(u.size.x-radius));
    } else if p.y > u.size.y-radius && p.x >= radius {
        distance_along = w + 2.*arc + h + u.size.x-radius-p.x;
    } else if p.y > u.size.y-radius {
        distance_along = 2.*w + 2.*arc + h + r * (atan2(p.y-(u.size.y-radius), p.x-radius)-1.5707963267948966);
    } else if p.x < radius && p.y >= radius {
        distance_along = 2.*w + 3.*arc + h + u.size.y-radius-p.y;
    } else {
        distance_along = 2.*(w+h) + 3.*arc + r * (atan2(p.y-radius,p.x-radius)+3.141592653589793);
    }
    let interval = perimeter / 10.;
    let within = fract(distance_along / perimeter * 10. - u.motion.y * 10.) * interval;
    let edge = min(within, interval * 0.5 - within);
    let blue = clamp(edge * u.motion.x + 0.5, 0., 1.);
    return vec4(u.blue.rgb, u.blue.a * coverage * blue);
}
"#;
