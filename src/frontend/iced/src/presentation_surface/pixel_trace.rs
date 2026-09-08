use super::{Surface, wgpu};
use std::sync::{Arc, Mutex};

pub(crate) fn enabled() -> bool {
    #[cfg(target_arch = "wasm32")]
    {
        thread_local! {
            static ENABLED: bool = web_sys::window()
                .and_then(|window| window.location().search().ok())
                .and_then(|search| web_sys::UrlSearchParams::new_with_str(&search).ok())
                .and_then(|params| params.get("mmltk_pixel_trace"))
                .is_some_and(|value| value == "1");
        }
        ENABLED.with(|enabled| *enabled)
    }
    #[cfg(not(target_arch = "wasm32"))]
    {
        false
    }
}

// One active mapping and one newest owned image. No external mailbox borrow
// enters this owner, and replacement drops the previous pending handle in O(1).
pub(super) struct PixelTrace {
    state: Arc<Probe>,
}

struct Request {
    texture: wgpu::Texture,
    surface: Surface,
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
}

impl PixelTrace {
    pub(super) fn new(device: &wgpu::Device, queue: &wgpu::Queue) -> Option<Self> {
        enabled().then(|| Self {
            state: Arc::new(Probe {
                device: device.clone(),
                queue: queue.clone(),
                buffer: device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("mmltk bounded pixel evidence"),
                    size: 25 * 256,
                    usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                    mapped_at_creation: false,
                }),
                requests: Mutex::new(Requests::default()),
            }),
        })
    }

    pub(super) fn sample(&self, texture: &wgpu::Texture, surface: Surface) {
        let Some(frame) = surface.frame else {
            return;
        };
        if frame.content_width == 0 || frame.content_height == 0 {
            return;
        }
        let request = Request {
            texture: texture.clone(),
            surface,
        };
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
        let frame = surface.frame.expect("validated owned probe frame");
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
        for (index, &(x, y)) in coordinates.iter().enumerate() {
            let mut source = request.texture.as_image_copy();
            source.origin = wgpu::Origin3d { x, y, z: 0 };
            encoder.copy_texture_to_buffer(
                source,
                wgpu::TexelCopyBufferInfo {
                    buffer: &self.buffer,
                    layout: wgpu::TexelCopyBufferLayout {
                        offset: index as u64 * 256,
                        bytes_per_row: Some(256),
                        rows_per_image: None,
                    },
                },
                wgpu::Extent3d {
                    width: 1,
                    height: 1,
                    depth_or_array_layers: 1,
                },
            );
        }
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
            // Device-loss/error completion clears the one pending owned handle;
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
