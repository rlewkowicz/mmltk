use crate::core::{self, Rectangle};
use crate::graphics::Viewport;
use crate::graphics::futures::{MaybeSend, MaybeSync};

use rustc_hash::FxHashMap;
use std::any::{Any, TypeId};
use std::fmt::Debug;
use std::sync::{Arc, Mutex};

/// Read custody belonging to one actual command encoder. Only add a resource
/// after encoding its use. The deferred wgpu action retains the batch through
/// submission settlement, or drops it when an unsubmitted encoder is abandoned.
#[derive(Default)]
pub struct Resources {
    holds: Vec<Arc<dyn Any + Send + Sync>>,
    pool: ResourcePool,
    observers: Vec<Box<dyn FnOnce(Settlement) + Send>>,
    submissions: Vec<Arc<dyn Fn() + Send + Sync>>,
}

/// Optional observers of actual queue submission, independent of work completion.
#[derive(Default)]
pub struct Submissions {
    observers: Vec<Arc<dyn Fn() + Send + Sync>>,
    pool: Option<ResourcePool>,
}

impl Submissions {
    /// Called immediately after the queue accepts this encoder's commands.
    pub fn submitted(mut self) {
        for observer in self.observers.drain(..) {
            observer();
        }
    }
}

impl Drop for Submissions {
    fn drop(&mut self) {
        self.observers.clear();
        let Some(pool) = &self.pool else { return; };
        let mut spare = pool.0.lock().expect("submission staging");
        if self.observers.capacity() > spare.submissions.capacity() {
            std::mem::swap(&mut self.observers, &mut spare.submissions);
        }
    }
}

#[derive(Default)]
struct ResourceStaging {
    holds: Vec<Arc<dyn Any + Send + Sync>>,
    submissions: Vec<Arc<dyn Fn() + Send + Sync>>,
}

#[derive(Clone, Default)]
pub(crate) struct ResourcePool(Arc<Mutex<ResourceStaging>>);

impl ResourcePool {
    pub(crate) fn take(&self) -> Resources {
        let mut staging = self.0.lock().expect("resource staging");
        Resources {
            holds: std::mem::take(&mut staging.holds),
            pool: self.clone(),
            observers: Vec::new(),
            submissions: std::mem::take(&mut staging.submissions),
        }
    }
}

/// A work-done callback is settlement, including device failure, not render success.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Settlement {
    Submitted,
    Abandoned,
}

impl Resources {
    /// Register an observer once for this encoder, after encoding its drawing.
    pub fn observe_submission(&mut self, observer: Arc<dyn Fn() + Send + Sync>) {
        if !self.submissions.iter().any(|prior| Arc::ptr_eq(prior, &observer)) {
            self.submissions.push(observer);
        }
    }

    /// Retain a safely shareable resource token, without moving UI state.
    pub fn retain<T: Any + Send + Sync>(&mut self, resource: Arc<T>) {
        self.holds.push(resource);
    }

    /// Optional effect-only evidence attached to this actual encoder's custody.
    pub fn observe_settlement(&mut self, observer: impl FnOnce(Settlement) + Send + 'static) {
        self.observers.push(Box::new(observer));
    }

    fn settle(&mut self, outcome: Settlement) {
        for observer in self.observers.drain(..) {
            observer(outcome);
        }
    }

    pub(crate) fn attach(mut self, encoder: &wgpu::CommandEncoder) -> Submissions {
        let submissions = if self.submissions.is_empty() {
            Submissions::default()
        } else {
            Submissions {
                observers: std::mem::take(&mut self.submissions),
                pool: Some(self.pool.clone()),
            }
        };
        if !self.holds.is_empty() || !self.observers.is_empty() {
            // Callback arrival also includes backend terminal failure. It is
            // resource settlement, never a successful-render notification.
            encoder.on_submitted_work_done(move || {
                self.settle(Settlement::Submitted);
                drop(self);
            });
        }
        submissions
    }
}

impl Drop for Resources {
    fn drop(&mut self) {
        self.settle(Settlement::Abandoned);
        self.holds.clear();
        self.submissions.clear();
        let mut spare = self.pool.0.lock().expect("resource staging");
        if self.holds.capacity() > spare.holds.capacity() {
            std::mem::swap(&mut self.holds, &mut spare.holds);
        }
        if self.submissions.capacity() > spare.submissions.capacity() {
            std::mem::swap(&mut self.submissions, &mut spare.submissions);
        }
    }
}

pub type Batch = Vec<Instance>;

pub trait Primitive: Debug + MaybeSend + MaybeSync + 'static {
    type Pipeline: Pipeline + MaybeSend + MaybeSync;

    fn prepare(
        &self,
        pipeline: &mut Self::Pipeline,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        bounds: &Rectangle,
        viewport: &Viewport,
    );

    fn draw(
        &self,
        _pipeline: &Self::Pipeline,
        _render_pass: &mut wgpu::RenderPass<'_>,
        _resources: &mut Resources,
    ) -> bool {
        false
    }

    fn render(
        &self,
        _pipeline: &Self::Pipeline,
        _encoder: &mut wgpu::CommandEncoder,
        _target: &wgpu::TextureView,
        _clip_bounds: &Rectangle<u32>,
        _resources: &mut Resources,
    ) {
    }
}

pub trait Pipeline: Any + MaybeSend + MaybeSync {
    fn new(device: &wgpu::Device, queue: &wgpu::Queue, format: wgpu::TextureFormat) -> Self
    where
        Self: Sized;

    fn trim(&mut self) {}
}

pub(crate) trait Stored: Debug + MaybeSend + MaybeSync + 'static {
    fn prepare(
        &self,
        storage: &mut Storage,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        format: wgpu::TextureFormat,
        bounds: &Rectangle,
        viewport: &Viewport,
    );

    fn draw(
        &self,
        storage: &Storage,
        render_pass: &mut wgpu::RenderPass<'_>,
        resources: &mut Resources,
    ) -> bool;

    fn render(
        &self,
        storage: &Storage,
        encoder: &mut wgpu::CommandEncoder,
        target: &wgpu::TextureView,
        clip_bounds: &Rectangle<u32>,
        resources: &mut Resources,
    );
}

#[derive(Debug)]
struct BlackBox<P: Primitive> {
    primitive: P,
}

impl<P: Primitive> Stored for BlackBox<P> {
    fn prepare(
        &self,
        storage: &mut Storage,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        format: wgpu::TextureFormat,
        bounds: &Rectangle,
        viewport: &Viewport,
    ) {
        if !storage.has::<P>() {
            storage.store::<P, _>(P::Pipeline::new(device, queue, format));
        }

        let renderer = storage
            .get_mut::<P>()
            .expect("renderer should be initialized")
            .downcast_mut::<P::Pipeline>()
            .expect("renderer should have the proper type");

        self.primitive
            .prepare(renderer, device, queue, bounds, viewport);
    }

    fn draw(&self, storage: &Storage, render_pass: &mut wgpu::RenderPass<'_>, resources: &mut Resources) -> bool {
        let renderer = storage
            .get::<P>()
            .expect("renderer should be initialized")
            .downcast_ref::<P::Pipeline>()
            .expect("renderer should have the proper type");

        self.primitive.draw(renderer, render_pass, resources)
    }

    fn render(
        &self,
        storage: &Storage,
        encoder: &mut wgpu::CommandEncoder,
        target: &wgpu::TextureView,
        clip_bounds: &Rectangle<u32>,
        resources: &mut Resources,
    ) {
        let renderer = storage
            .get::<P>()
            .expect("renderer should be initialized")
            .downcast_ref::<P::Pipeline>()
            .expect("renderer should have the proper type");

        self.primitive
            .render(renderer, encoder, target, clip_bounds, resources);
    }
}

#[derive(Debug)]
pub struct Instance {
    pub(crate) bounds: Rectangle,

    pub(crate) primitive: Box<dyn Stored>,
}

impl Instance {
    pub fn new(bounds: Rectangle, primitive: impl Primitive) -> Self {
        Instance {
            bounds,
            primitive: Box::new(BlackBox { primitive }),
        }
    }
}

pub trait Renderer: core::Renderer {
    fn draw_primitive(&mut self, bounds: Rectangle, primitive: impl Primitive);
}

#[derive(Default)]
pub struct Storage {
    pipelines: FxHashMap<TypeId, Box<dyn Pipeline>>,
}

impl Storage {
    pub fn has<T: 'static>(&self) -> bool {
        self.pipelines.contains_key(&TypeId::of::<T>())
    }

    pub fn store<T: 'static, P: Pipeline>(&mut self, pipeline: P) {
        let _ = self.pipelines.insert(TypeId::of::<T>(), Box::new(pipeline));
    }

    pub fn get<T: 'static>(&self) -> Option<&dyn Any> {
        self.pipelines
            .get(&TypeId::of::<T>())
            .map(|pipeline| pipeline.as_ref() as &dyn Any)
    }

    pub fn get_mut<T: 'static>(&mut self) -> Option<&mut dyn Any> {
        self.pipelines
            .get_mut(&TypeId::of::<T>())
            .map(|pipeline| pipeline.as_mut() as &mut dyn Any)
    }

    pub fn trim(&mut self) {
        for pipeline in self.pipelines.values_mut() {
            pipeline.trim();
        }
    }
}

#[cfg(test)]
mod resource_tests {
    use super::{Resources, Settlement};
    use std::sync::{Arc, Mutex};

    #[test]
    fn abandoned_batch_settles_before_releasing_its_last_reader() {
        struct Reader(Arc<Mutex<Vec<&'static str>>>);
        impl Drop for Reader {
            fn drop(&mut self) {
                self.0.lock().unwrap().push("released");
            }
        }
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut resources = Resources::default();
        resources.retain(Arc::new(Reader(events.clone())));
        let observer = events.clone();
        resources.observe_settlement(move |outcome| {
            assert_eq!(outcome, Settlement::Abandoned);
            observer.lock().unwrap().push("abandoned");
        });
        drop(resources);
        assert_eq!(*events.lock().unwrap(), ["abandoned", "released"]);
    }
}
