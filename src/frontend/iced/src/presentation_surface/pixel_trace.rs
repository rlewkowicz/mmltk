use super::{SampleRead, Surface, wgpu};
use std::sync::{Arc, Mutex};

thread_local! {
    static ENABLED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

pub(super) fn initialize(enabled: bool) {
    ENABLED.with(|flag| flag.set(enabled));
}

pub(crate) fn enabled() -> bool {
    ENABLED.with(std::cell::Cell::get)
}

// One active mapping and one newest exact sample read. Replacement returns
// the previous pending lease in O(1); mapping never owns display authorization.
pub(super) struct PixelTrace {
    state: Arc<Probe>,
}

struct Request {
    surface: Surface,
    read: Arc<SampleRead>,
}

struct Requests<T> {
    active: bool,
    closed: bool,
    pending: Option<T>,
}

impl<T> Default for Requests<T> {
    fn default() -> Self {
        Self {
            active: false,
            closed: false,
            pending: None,
        }
    }
}

impl<T> Requests<T> {
    fn enqueue(&mut self, request: T) -> Option<T> {
        if self.closed {
            return None;
        }
        if self.active {
            self.pending = Some(request);
            None
        } else {
            self.active = true;
            Some(request)
        }
    }

    fn close(&mut self) {
        self.closed = true;
        self.pending = None;
    }

    fn finish(&mut self, succeeded: bool) -> Option<T> {
        if !succeeded {
            self.close();
        }
        let next = if self.closed {
            None
        } else {
            self.pending.take()
        };
        self.active = next.is_some();
        next
    }
}

struct Probe {
    device: wgpu::Device,
    queue: wgpu::Queue,
    buffer: wgpu::Buffer,
    requests: Mutex<Requests<Request>>,
    pipeline: wgpu::ComputePipeline,
    bindings: wgpu::BindGroup,
    parameters: wgpu::Buffer,
    pixels: wgpu::Buffer,
}

impl PixelTrace {
    pub(super) fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        texture: &wgpu::Texture,
    ) -> Option<Self> {
        if !enabled() {
            return None;
        }
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("mmltk sample pixel evidence"),
            source: wgpu::ShaderSource::Wgsl(std::borrow::Cow::Borrowed(PROBE)),
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("mmltk sample pixel evidence"),
            layout: None,
            module: &module,
            entry_point: Some("probe"),
            compilation_options: Default::default(),
            cache: None,
        });
        let buffer = |label, size, usage| {
            device.create_buffer(&wgpu::BufferDescriptor {
                label: Some(label),
                size,
                usage,
                mapped_at_creation: false,
            })
        };
        let parameters = buffer(
            "mmltk probe extent and slot",
            16,
            wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        );
        let pixels = buffer(
            "mmltk probe pixels",
            25 * 256,
            wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        );
        let view = texture.create_view(&wgpu::TextureViewDescriptor {
            dimension: Some(wgpu::TextureViewDimension::D2Array),
            ..Default::default()
        });
        let bindings = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("mmltk probe sample"),
            layout: &pipeline.get_bind_group_layout(0),
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: parameters.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: pixels.as_entire_binding(),
                },
            ],
        });
        Some(Self {
            state: Arc::new(Probe {
                device: device.clone(),
                queue: queue.clone(),
                pipeline,
                bindings,
                parameters,
                pixels,
                buffer: buffer(
                    "mmltk bounded pixel evidence",
                    25 * 256,
                    wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                ),
                requests: Mutex::new(Requests::default()),
            }),
        })
    }

    pub(super) fn sample(&self, surface: Surface, read: Arc<SampleRead>) {
        let Some(frame) = surface.frame else {
            return;
        };
        if frame.content_width == 0 || frame.content_height == 0 {
            return;
        }
        let request = Request { surface, read };
        let request = self
            .state
            .requests
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .enqueue(request);
        if let Some(request) = request {
            self.state.clone().start(request);
        }
    }
}

impl Drop for PixelTrace {
    fn drop(&mut self) {
        self.state
            .requests
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .close();
    }
}

impl Probe {
    fn start(self: Arc<Self>, request: Request) {
        let surface = request.surface;
        let frame = surface.frame.expect("validated sample probe frame");
        let coordinate = |index: usize, size: u32| {
            [
                0,
                191.min(size - 1),
                383.min(size - 1),
                (size - 1) / 2,
                size - 1,
            ][index]
        };
        let coordinates: [(u32, u32); 25] = std::array::from_fn(|index| {
            (
                coordinate(index % 5, frame.content_width),
                coordinate(index / 5, frame.content_height),
            )
        });
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("mmltk receiver pixel evidence"),
            });
        let mut parameters = [0u8; 16];
        for (index, value) in [
            frame.content_width,
            frame.content_height,
            if frame.direct_sampling { 0 } else { frame.slot },
            0,
        ]
        .into_iter()
        .enumerate()
        {
            parameters[index * 4..index * 4 + 4].copy_from_slice(&value.to_le_bytes());
        }
        self.queue.write_buffer(&self.parameters, 0, &parameters);
        {
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("mmltk sample read evidence"),
                timestamp_writes: None,
            });
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, &self.bindings, &[]);
            pass.dispatch_workgroups(1, 1, 1);
        }
        // The external image remains a shader resource, including on hidden
        // pages. A texture-to-buffer copy would leave it in COPY_SRC layout.
        encoder.copy_buffer_to_buffer(&self.pixels, 0, &self.buffer, 0, 25 * 256);
        // The read's physical custody follows the copy's encoder separately
        // from optional mapping success, request coalescing and owner disposal.
        encoder.on_submitted_work_done(move || drop(request.read));
        self.queue.submit([encoder.finish()]);
        let buffer = self.buffer.clone();
        let callback_buffer = buffer.clone();
        buffer.slice(..).map_async(wgpu::MapMode::Read, move |result| {
            if result.is_ok() {
                {
                    let bytes = callback_buffer.slice(..).get_mapped_range();
                    for (index, &(x, y)) in coordinates.iter().enumerate() {
                        let offset = index * 256;
                        let rgba = u32::from_le_bytes(bytes[offset..offset + 4].try_into().expect("four-byte pixel"));
                        #[cfg(target_arch = "wasm32")]
                        super::emit_surface_trace(&format!(
                            "{{\"event\":\"iced.surface.pixel\",{},\"sample_index\":{index},\"sample_x\":{x},\"sample_y\":{y},\"sample_rgba\":{rgba}}}",
                            super::surface_trace_fields(surface, surface),
                        ));
                        #[cfg(not(target_arch = "wasm32"))]
                        let _ = (x, y, rgba);
                    }
                }
                callback_buffer.unmap();
            }
            // Device-loss/error completion clears the one pending sample hold;
            // successful unmapping immediately admits the newest request.
            let next = self.requests.lock().unwrap_or_else(|error| error.into_inner()).finish(result.is_ok());
            if let Some(next) = next { self.start(next); }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::Requests;

    #[test]
    fn overlapping_probes_keep_only_the_latest_owned_request() {
        let mut requests = Requests::default();
        assert_eq!(requests.enqueue(1), Some(1));
        assert_eq!(requests.enqueue(2), None);
        assert_eq!(requests.enqueue(3), None);
        assert_eq!(requests.finish(true), Some(3));
        assert!(requests.active);
        assert_eq!(requests.finish(true), None);
        assert!(!requests.active);
        assert_eq!(requests.enqueue(4), Some(4));
        assert_eq!(requests.enqueue(5), None);
        assert_eq!(requests.finish(false), None);
        assert_eq!(requests.enqueue(6), None);
        assert!(requests.pending.is_none());
    }

    #[test]
    fn owner_drop_discards_pending_work_without_restarting_the_active_probe() {
        let mut requests = Requests::default();
        assert_eq!(requests.enqueue(1), Some(1));
        assert_eq!(requests.enqueue(2), None);
        requests.close();
        assert_eq!(requests.finish(true), None);
        assert_eq!(requests.enqueue(3), None);
    }
}

const PROBE: &str = r#"
@group(0) @binding(0) var sample: texture_2d_array<f32>;
@group(0) @binding(1) var<uniform> parameters: vec4<u32>;
@group(0) @binding(2) var<storage, read_write> pixels: array<u32>;
fn coordinate(index: u32, size: u32) -> u32 {
    switch index {
        case 0u: { return 0u; }
        case 1u: { return min(191u, size - 1u); }
        case 2u: { return min(383u, size - 1u); }
        case 3u: { return (size - 1u) / 2u; }
        default: { return size - 1u; }
    }
}
@compute @workgroup_size(25)
fn probe(@builtin(local_invocation_index) index: u32) {
    let xy = vec2<i32>(i32(coordinate(index % 5u, parameters.x)), i32(coordinate(index / 5u, parameters.y)));
    pixels[index * 64u] = pack4x8unorm(textureLoad(sample, xy, i32(parameters.z), 0));
}
"#;
