use iced::widget::shader::{self, Viewport};
use iced::{Event, Point, Rectangle, mouse, wgpu};
use std::borrow::Cow;
use std::sync::atomic::{AtomicU64, Ordering};
pub(crate) mod gallery;
pub(crate) mod labels;
pub(crate) mod pixel_trace;
#[cfg(target_arch = "wasm32")]
use wasm_bindgen::JsCast;
#[cfg(target_arch = "wasm32")]
use wasm_bindgen::closure::Closure;

thread_local! {
    static SURFACE_TRACE_ENABLED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

pub(crate) fn initialize_diagnostics(surface_trace: bool, pixel_trace: bool) {
    SURFACE_TRACE_ENABLED.with(|flag| flag.set(surface_trace));
    pixel_trace::initialize(surface_trace && pixel_trace);
}

#[cfg(target_arch = "wasm32")]
fn surface_trace_enabled() -> bool {
    SURFACE_TRACE_ENABLED.with(std::cell::Cell::get)
}

#[cfg(target_arch = "wasm32")]
pub(crate) fn trace_surface(event: &str, surface: Surface) {
    trace_surface_request(event, surface, surface, "");
}

#[cfg(target_arch = "wasm32")]
fn trace_frame(event: &str, frame: FrameReady) {
    if surface_trace_enabled() {
        emit_surface_trace(&format!(
            "{{\"event\":\"iced.frame.{event}\",\"surface\":\"{:016x}{:016x}\",\"source_revision\":{}{}}}",
            frame.high,
            frame.low,
            frame.content_sequence,
            frame_trace_fields(Some(frame)),
        ));
    }
}

// One projection of the physical native receipt. Capture notifications carry
// the receipt by value, including when the requested surface has since changed.
#[cfg(target_arch = "wasm32")]
fn frame_trace_fields(frame: Option<FrameReady>) -> String {
    format!(
        ",\"frame_revision\":{},\"presentation_revision\":{},\"content_session\":{},\"layer\":{},\"slot\":{},\"content_width\":{},\"content_height\":{}",
        frame.map_or(0, |f| f.content_sequence),
        frame.map_or(0, |f| f.presentation_revision),
        frame.map_or(0, |f| f.content_session),
        frame.map_or(0, |f| f.layer),
        frame.map_or(0, |f| f.slot),
        frame.map_or(0, |f| f.content_width),
        frame.map_or(0, |f| f.content_height),
    )
}

#[cfg(target_arch = "wasm32")]
pub(crate) fn surface_trace_fields(surface: Surface, requested: Surface) -> String {
    format!(
        "\"surface\":\"{:016x}{:016x}\",\"requested_surface\":\"{:016x}{:016x}\",\"generation\":{},\"width\":{},\"height\":{},\"allocation_generation\":{},\"timeline_ready\":{}{}",
        surface.high,
        surface.low,
        requested.high,
        requested.low,
        surface.generation,
        surface.width,
        surface.height,
        surface.generation,
        surface.timeline_ready,
        frame_trace_fields(surface.frame),
    )
}

#[cfg(not(target_arch = "wasm32"))]
fn trace_frame(_event: &str, _frame: FrameReady) {}

#[cfg(target_arch = "wasm32")]
fn trace_surface_request(event: &str, surface: Surface, requested: Surface, control: &str) {
    if !surface_trace_enabled() {
        return;
    }
    let line = format!(
        "{{\"event\":\"iced.surface.{event}\",\"control\":\"{control}\",{}}}",
        surface_trace_fields(surface, requested),
    );
    emit_surface_trace(&line);
}

#[cfg(target_arch = "wasm32")]
fn emit_surface_trace(line: &str) {
    let global = js_sys::global();
    if let Ok(candidate) =
        js_sys::Reflect::get(global.as_ref(), &wasm_bindgen::JsValue::from_str("dump"))
        && let Some(dump) = candidate.dyn_ref::<js_sys::Function>()
    {
        let output = wasm_bindgen::JsValue::from_str(&(line.to_owned() + "\n"));
        if dump.call1(global.as_ref(), &output).is_ok() {
            return;
        }
    }
    web_sys::console::error_1(&wasm_bindgen::JsValue::from_str(line));
}

#[cfg(target_arch = "wasm32")]
fn gallery_trace_fields(snapshot: Option<&crate::generated::ExploreSnapshot>) -> String {
    snapshot.map_or_else(String::new, |snapshot| format!(
        ",\"source_kind\":{},\"source_instance\":{},\"source_revision\":{},\"clean_revision\":{},\"source_observation_revision\":{},\"dataset_identity\":{},\"gallery_generation\":{},\"ready_slots\":{:?},\"columns\":{},\"rows\":{},\"first_row\":{},\"matching_count\":{},\"visible_indices\":{:?}",
        crate::generated::presentation_source_session(snapshot.frame.source.kind), snapshot.frame.source.instance, snapshot.frame.revision,
        crate::generated::visual_clean_content_identity(&snapshot.frame).revision, snapshot.revision,
        snapshot.dataset.identity, snapshot.gallery.generation, snapshot.gallery.slots,
        snapshot.viewport.columns, snapshot.viewport.rowcount, snapshot.viewport.firstrow,
        snapshot.order.matchingcount, snapshot.order.visibleindices,
    ))
}

#[cfg(target_arch = "wasm32")]
fn trace_image(
    event: &str,
    control: &str,
    surface: Surface,
    requested: Surface,
    snapshot: Option<&crate::generated::ExploreSnapshot>,
    geometry: Option<(Rectangle, Option<(Rectangle, Rectangle)>)>,
) {
    if !surface_trace_enabled() {
        return;
    }
    let Some(_) = surface.frame else {
        return;
    };
    let geometry = geometry.map_or_else(String::new, |(bounds, visible)| {
        let rect = |r: Rectangle| format!("[{},{},{},{}]", r.x, r.y, r.width, r.height);
        let visible = visible.map_or_else(String::new, |(image, clip)| {
            format!(",\"image\":{},\"clip\":{}", rect(image), rect(clip))
        });
        format!(",\"bounds\":{}{visible}", rect(bounds))
    });
    emit_surface_trace(&format!(
        "{{\"event\":\"iced.surface.{event}\",\"control\":\"{control}\",{}{}{geometry}}}",
        surface_trace_fields(surface, requested),
        gallery_trace_fields(snapshot),
    ));
}

#[cfg(not(target_arch = "wasm32"))]
fn trace_image(
    _event: &str,
    _control: &str,
    _surface: Surface,
    _requested: Surface,
    _snapshot: Option<&crate::generated::ExploreSnapshot>,
    _geometry: Option<(Rectangle, Option<(Rectangle, Rectangle)>)>,
) {
}

fn trace_draw(event: &str, control: &str, draw: &PreparedDraw, image: Rectangle, clip: Rectangle) {
    trace_image(
        event,
        control,
        draw.surface,
        draw.requested,
        draw.gallery.as_deref(),
        Some((draw.bounds, Some((image, clip)))),
    );
}

#[cfg(target_arch = "wasm32")]
pub(super) fn trace_gallery_source(snapshot: &crate::generated::ExploreSnapshot) {
    if surface_trace_enabled()
        && snapshot.gallery.generation != 0
        && snapshot.frame.source.instance != 0
        && snapshot.frame.revision != 0
    {
        emit_surface_trace(&format!(
            "{{\"event\":\"iced.gallery.source\",\"content_session\":{},\"content_width\":{},\"content_height\":{}{} }}",
            crate::generated::presentation_source_session(snapshot.frame.source.kind),
            snapshot.frame.extent.width,
            snapshot.frame.extent.height,
            gallery_trace_fields(Some(snapshot)),
        ));
    }
}

#[cfg(not(target_arch = "wasm32"))]
pub(super) fn trace_gallery_source(_snapshot: &crate::generated::ExploreSnapshot) {}

pub(crate) fn trace_atlas_stage(stage: &str, draw: &crate::integration_control::AtlasDraw) {
    trace_image(
        "scroll_stage",
        stage,
        draw.surface,
        draw.surface,
        Some(&draw.snapshot),
        Some((draw.bounds, Some((draw.image, draw.clip)))),
    );
}

#[cfg(not(target_arch = "wasm32"))]
pub(crate) fn trace_surface(_event: &str, _surface: Surface) {}

#[cfg(not(target_arch = "wasm32"))]
fn trace_surface_request(_event: &str, _surface: Surface, _requested: Surface, _control: &str) {}

const MAILBOX_SLOTS: u32 = 2;
const INTEGRATION_REDRAW_PASSES: u8 = 4;
thread_local! {
    static DRAWN_DETAIL: std::cell::Cell<Option<(Surface, [u32; 4])>> = const { std::cell::Cell::new(None) };
    static RELEASED_FRAMES: std::cell::Cell<[Option<FrameReady>; 6]> = const { std::cell::Cell::new([None; 6]) };
    static BORROWS: std::cell::Cell<[Option<FrameReady>; 6]> = const { std::cell::Cell::new([None; 6]) };
}

// Publication metadata is copyable; permission to read an external layer is not.
pub(crate) fn accept_publication(frame: FrameReady) -> bool {
    let Some(binding) = mailbox_binding(frame) else {
        return false;
    };
    if RELEASED_FRAMES.with(|released| {
        released.get()[binding].is_some_and(|prior| {
            same_mailbox_slot(prior, frame)
                && prior.presentation_revision >= frame.presentation_revision
        })
    }) {
        return false;
    }
    BORROWS.with(|borrows| {
        let mut slots = borrows.get();
        if slots[binding].is_some() {
            return false;
        }
        slots[binding] = Some(frame);
        borrows.set(slots);
        true
    })
}

pub(crate) fn retire_publication(frame: FrameReady) {
    let unused = BORROWS.with(|borrows| {
        let mut slots = borrows.get();
        let Some(binding) = mailbox_binding(frame) else {
            return false;
        };
        if slots[binding] != Some(frame) {
            return false;
        }
        slots[binding] = None;
        borrows.set(slots);
        true
    });
    // Captured publications have moved to the GPU completion owner. Metadata
    // retirement must not release that owner's live external sample.
    if unused {
        release(frame);
    }
}

struct CaptureBorrow(FrameReady);

impl CaptureBorrow {
    fn acquire(frame: FrameReady) -> Option<Self> {
        let binding = mailbox_binding(frame)?;
        BORROWS.with(|borrows| {
            let mut slots = borrows.get();
            if slots[binding] != Some(frame) {
                return None;
            }
            slots[binding] = None;
            borrows.set(slots);
            if reserve_release(frame) {
                Some(Self(frame))
            } else {
                None
            }
        })
    }
}

impl Drop for CaptureBorrow {
    fn drop(&mut self) {
        dispatch_release(self.0);
    }
}

pub(crate) fn drawn_detail() -> Option<(Surface, [u32; 4])> {
    DRAWN_DETAIL.with(std::cell::Cell::get)
}

pub(crate) fn clear_drawn_detail() {
    DRAWN_DETAIL.with(|drawn| drawn.set(None));
}

pub(crate) fn record_drawn_detail(surface: Surface, crop: [u32; 4]) {
    if surface.frame.is_none() {
        return;
    }
    DRAWN_DETAIL.with(|drawn| {
        if drawn.replace(Some((surface, crop))) != Some((surface, crop)) {
            notify_surface(Notification::Drawn);
        }
    });
}

pub(crate) fn viewer_copy_matches(
    model: &crate::view_model::ApplicationModel,
    source: &crate::generated::VisualFrame,
) -> bool {
    drawn_detail().is_some_and(|(surface, crop)| {
        let Some(frame) = surface.frame else {
            return false;
        };
        let original = model
            .explore
            .snapshot
            .as_ref()
            .is_some_and(|snapshot| snapshot.detail.showoriginaldimensions);
        let expected = if original {
            [
                source.content.x,
                source.content.y,
                source.content.width,
                source.content.height,
            ]
        } else {
            [0, 0, source.extent.width, source.extent.height]
        };
        model
            .presentation
            .as_ref()
            .is_some_and(|snapshot| frame.matches_completed(snapshot))
            && frame.belongs_to(surface)
            && frame.matches_content(source)
            && crop == expected
    })
}

pub(crate) fn same_mailbox_slot(left: FrameReady, right: FrameReady) -> bool {
    left.high == right.high
        && left.low == right.low
        && left.layer == right.layer
        && left.slot == right.slot
}

pub(crate) fn invalidate_drawn_slot(frame: FrameReady) {
    if drawn_detail()
        .and_then(|(surface, _)| surface.frame)
        .is_some_and(|drawn| same_mailbox_slot(drawn, frame) && drawn != frame)
    {
        clear_drawn_detail();
    }
}
#[cfg(target_arch = "wasm32")]
const FRAME_EVENT: &str = "gpuexternaltextureframe";
#[cfg(target_arch = "wasm32")]
const RELEASE_EVENT: &str = "gpuexternaltextureslotrelease";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct FrameReady {
    pub high: u64,
    pub low: u64,
    pub layer: u32,
    pub slot: u32,
    pub content_session: u64,
    pub content_sequence: u64,
    pub presentation_revision: u64,
    pub content_width: u32,
    pub content_height: u32,
}

impl FrameReady {
    pub(crate) fn matches_content(self, frame: &crate::generated::VisualFrame) -> bool {
        frame.source.instance != 0
            && self.layer == 0
            && self.slot < MAILBOX_SLOTS
            && self.content_session
                == crate::generated::presentation_source_session(frame.source.kind)
            && self.content_sequence == frame.revision
            && self.content_width == frame.extent.width
            && self.content_height == frame.extent.height
    }

    pub(crate) fn matches_completed(
        self,
        snapshot: &crate::generated::PresentationSnapshot,
    ) -> bool {
        // The advertised capability may already be an unpublished replacement.
        // Allocation, layer, and slot remain those of this physical receipt.
        self.presentation_revision == snapshot.presentationrevision
            && self.matches_content(&snapshot.completed)
    }

    pub(crate) fn belongs_to(self, surface: Surface) -> bool {
        self.high == surface.high
            && self.low == surface.low
            && self.content_width <= surface.width
            && self.content_height <= surface.height
    }

    fn parse(detail: &str) -> Option<Self> {
        let mut fields = detail.split(':');
        let id = fields.next()?;
        if id.len() != 32 || !id.bytes().all(|value| value.is_ascii_hexdigit()) {
            return None;
        }
        let ready = Self {
            high: u64::from_str_radix(&id[..16], 16).ok()?,
            low: u64::from_str_radix(&id[16..], 16).ok()?,
            layer: fields.next()?.parse().ok()?,
            slot: fields.next()?.parse().ok()?,
            content_session: fields.next()?.parse().ok()?,
            content_sequence: fields.next()?.parse().ok()?,
            presentation_revision: fields.next()?.parse().ok()?,
            content_width: fields.next()?.parse().ok()?,
            content_height: fields.next()?.parse().ok()?,
        };
        (fields.next().is_none()
            && ready.layer < 3
            && ready.slot < MAILBOX_SLOTS
            && (ready.content_session != 0 || ready.content_sequence != 0)
            && ready.presentation_revision != 0
            && ready.content_width != 0
            && ready.content_height != 0)
            .then_some(ready)
    }
}

#[cfg(target_arch = "wasm32")]
pub fn subscription() -> iced::Subscription<Notification> {
    iced::Subscription::run(frame_stream)
}

#[derive(Debug, Clone, Copy)]
pub enum Notification {
    Native(FrameReady),
    Completed(FrameReady),
    CaptureRejected(FrameReady),
    Drawn,
}

#[cfg(target_arch = "wasm32")]
thread_local! {
    static COMPLETION_WAKE: std::cell::RefCell<Option<(std::rc::Weak<std::cell::RefCell<FrameMailbox>>, std::rc::Rc<iced::futures::task::AtomicWaker>)>> = const { std::cell::RefCell::new(None) };
}

#[cfg(target_arch = "wasm32")]
fn notify_surface(notification: Notification) {
    COMPLETION_WAKE.with(|notify| {
        if let Some((mailbox, wake)) = notify.borrow().as_ref()
            && let Some(mailbox) = mailbox.upgrade()
        {
            let mut mailbox = mailbox.borrow_mut();
            match notification {
                Notification::Completed(frame) => {
                    mailbox.complete(frame);
                    trace_frame("completion_enqueued", frame);
                }
                Notification::CaptureRejected(frame) => {
                    mailbox.rejected = Some(frame);
                    trace_frame("capture_rejection_enqueued", frame);
                }
                Notification::Drawn => mailbox.drawn = true,
                Notification::Native(_) => {
                    unreachable!("native frames arrive through their mailbox")
                }
            }
            wake.wake();
        }
    });
}

#[cfg(not(target_arch = "wasm32"))]
fn notify_surface(_notification: Notification) {}

#[derive(Default)]
struct FrameMailbox {
    completed: Option<FrameReady>,
    rejected: Option<FrameReady>,
    drawn: bool,
    frames: [Option<FrameReady>; 3],
    next_layer: usize,
}

impl FrameMailbox {
    fn complete(&mut self, frame: FrameReady) {
        if self
            .completed
            .is_none_or(|prior| prior.presentation_revision < frame.presentation_revision)
        {
            self.completed = Some(frame);
        }
    }

    fn next_notification(&mut self) -> Option<Notification> {
        self.completed
            .take()
            .map(Notification::Completed)
            .or_else(|| self.rejected.take().map(Notification::CaptureRejected))
            .or_else(|| std::mem::take(&mut self.drawn).then_some(Notification::Drawn))
            .or_else(|| self.pop().map(Notification::Native))
    }

    fn push(&mut self, frame: FrameReady) -> Option<FrameReady> {
        let slot = &mut self.frames[frame.layer as usize];
        if slot.is_some_and(|prior| prior.presentation_revision >= frame.presentation_revision) {
            return (*slot != Some(frame)).then_some(frame);
        }
        slot.replace(frame)
    }

    fn pop(&mut self) -> Option<FrameReady> {
        for offset in 0..self.frames.len() {
            let layer = (self.next_layer + offset) % self.frames.len();
            if let Some(frame) = self.frames[layer].take() {
                self.next_layer = (layer + 1) % self.frames.len();
                return Some(frame);
            }
        }
        None
    }
}

impl Drop for FrameMailbox {
    fn drop(&mut self) {
        for frame in self.frames.iter_mut().filter_map(Option::take) {
            release(frame);
        }
    }
}

#[cfg(target_arch = "wasm32")]
fn frame_stream() -> impl iced::futures::Stream<Item = Notification> {
    use iced::futures::{future::poll_fn, stream::unfold, task::AtomicWaker};
    use std::{cell::RefCell, rc::Rc, task::Poll};

    let mailbox = Rc::new(RefCell::new(FrameMailbox::default()));
    let wake = Rc::new(AtomicWaker::new());
    COMPLETION_WAKE
        .with(|notify| *notify.borrow_mut() = Some((Rc::downgrade(&mailbox), wake.clone())));
    let listener = web_sys::window().and_then(|window| {
        let pending = mailbox.clone();
        let notify = wake.clone();
        let callback = Closure::wrap(Box::new(move |event: web_sys::Event| {
            let Some(event) = event.dyn_ref::<web_sys::CustomEvent>() else {
                return;
            };
            if !event.is_trusted() {
                return;
            }
            let Some(ready) = event
                .detail()
                .as_string()
                .and_then(|value| FrameReady::parse(&value))
            else {
                return;
            };
            invalidate_drawn_slot(ready);
            let displaced = pending.borrow_mut().push(ready);
            if let Some(displaced) = displaced {
                release(displaced);
            }
            notify.wake();
        }) as Box<dyn FnMut(web_sys::Event)>);
        window
            .add_event_listener_with_callback(FRAME_EVENT, callback.as_ref().unchecked_ref())
            .ok()?;
        Some(FrameListener { window, callback })
    });
    unfold(
        (mailbox, wake, listener),
        |(mailbox, wake, listener)| async move {
            let ready = poll_fn(|context| {
                wake.register(context.waker());
                let mut mailbox = mailbox.borrow_mut();
                mailbox
                    .next_notification()
                    .map_or(Poll::Pending, Poll::Ready)
            })
            .await;
            Some((ready, (mailbox, wake, listener)))
        },
    )
}

#[cfg(target_arch = "wasm32")]
struct FrameListener {
    window: web_sys::Window,
    callback: Closure<dyn FnMut(web_sys::Event)>,
}

#[cfg(target_arch = "wasm32")]
impl Drop for FrameListener {
    fn drop(&mut self) {
        COMPLETION_WAKE.with(|notify| *notify.borrow_mut() = None);
        let _ = self.window.remove_event_listener_with_callback(
            FRAME_EVENT,
            self.callback.as_ref().unchecked_ref(),
        );
    }
}

#[cfg(all(test, not(target_arch = "wasm32")))]
pub fn subscription() -> iced::Subscription<Notification> {
    iced::Subscription::none()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Surface {
    pub high: u64,
    pub low: u64,
    pub generation: u64,
    pub width: u32,
    pub height: u32,
    pub timeline_ready: u64,
    // Desired publication geometry/identity, not authority to borrow its slot.
    // Only accept_publication + CaptureBorrow can authorize an external read.
    pub frame: Option<FrameReady>,
    pub integration: bool,
    pub crop: Option<[u32; 4]>,
    pub viewer_identity: Option<(u64, u32)>,
    pub fit_revision: u64,
}

impl Surface {
    pub(crate) fn content_region(self) -> [u32; 4] {
        let (width, height) = self.frame.map_or((self.width, self.height), |frame| {
            (frame.content_width, frame.content_height)
        });
        self.crop
            .filter(|[x, y, w, h]| {
                *w != 0
                    && *h != 0
                    && x.checked_add(*w).is_some_and(|end| end <= width)
                    && y.checked_add(*h).is_some_and(|end| end <= height)
            })
            .unwrap_or([0, 0, width, height])
    }

    fn content_extent(self) -> (u32, u32) {
        let [_, _, width, height] = self.content_region();
        (width, height)
    }

    fn transform_identity(self) -> Option<(u64, u32, u64)> {
        self.viewer_identity
            .or_else(|| self.frame.map(|frame| (frame.content_session, 0)))
            .map(|(session, image)| (session, image, self.fit_revision))
    }
    pub fn valid(self) -> bool {
        (self.high != 0 || self.low != 0)
            && self.generation != 0
            && self.width != 0
            && self.height != 0
    }

    fn label(self) -> String {
        format!("mmltk-surface-v4/{:016x}{:016x}", self.high, self.low)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Placement {
    Contain,
    GalleryGrid {
        columns: u32,
        rows: u32,
        first_row: u32,
    },
}

#[derive(Clone)]
pub(crate) struct Program<Message> {
    pub surface: Surface,
    pub publish: Option<fn(SurfaceGesture) -> Message>,
    pub local: Option<std::sync::Arc<dyn Fn(SurfaceGesture) -> Option<Message> + Send + Sync>>,
    pub placement: Placement,
    pub control_id: &'static str,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct SurfaceSample {
    pub width: u32,
    pub height: u32,
    pub x: u32,
    pub y: u32,
    pub content_x: u32,
    pub content_y: u32,
    pub pressed: bool,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SurfaceGesture {
    pub kind: SurfaceGestureKind,
    pub sample: SurfaceSample,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SurfaceGestureKind {
    Pointer,
    End,
    Cancel,
    Viewport,
}

impl<Message> shader::Program<Message> for Program<Message> {
    type State = ViewportOwner;
    type Primitive = Primitive;

    fn draw(
        &self,
        state: &Self::State,
        _cursor: iced::mouse::Cursor,
        _bounds: Rectangle,
    ) -> Self::Primitive {
        Primitive {
            surface: self.surface,
            transform: state.transform_for(self.surface),
            placement: self.placement,
            control_id: self.control_id,
        }
    }

    fn update(
        &self,
        state: &mut Self::State,
        event: &Event,
        bounds: Rectangle,
        cursor: mouse::Cursor,
    ) -> Option<shader::Action<Message>> {
        let dispatch = |gesture| {
            let message = self.local.as_ref().map_or_else(
                || self.publish.map(|publish| publish(gesture)),
                |local| local(gesture),
            );
            message.map_or_else(shader::Action::capture, shader::Action::publish)
        };
        state.update(
            event,
            bounds,
            cursor,
            self.surface,
            self.placement,
            (self.publish.is_some() || self.local.is_some())
                .then_some(&dispatch as &dyn Fn(SurfaceGesture) -> shader::Action<Message>),
        )
    }

    fn mouse_interaction(
        &self,
        state: &Self::State,
        bounds: Rectangle,
        cursor: mouse::Cursor,
    ) -> mouse::Interaction {
        if state.pan_origin.is_some() {
            mouse::Interaction::Grabbing
        } else if cursor.is_over(bounds) {
            mouse::Interaction::Grab
        } else {
            mouse::Interaction::default()
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct ViewportOwner {
    zoom: f32,
    pan_x: f32,
    pan_y: f32,
    pan_origin: Option<Point>,
    pointer_active: bool,
    last_pointer_sample: Option<SurfaceSample>,
    content_session: Option<(u64, u32, u64)>,
    integration_redraw_revision: Option<(u64, Option<[u32; 4]>, u64)>,
    integration_redraw_remaining: u8,
}

impl Default for ViewportOwner {
    fn default() -> Self {
        Self {
            zoom: 1.0,
            pan_x: 0.0,
            pan_y: 0.0,
            pan_origin: None,
            pointer_active: false,
            last_pointer_sample: None,
            content_session: None,
            integration_redraw_revision: None,
            integration_redraw_remaining: 0,
        }
    }
}

impl ViewportOwner {
    fn transform(self) -> ViewTransform {
        ViewTransform {
            zoom: self.zoom,
            pan_x: self.pan_x,
            pan_y: self.pan_y,
        }
    }

    fn transform_for(self, surface: Surface) -> ViewTransform {
        match surface.transform_identity() {
            Some(session) if self.content_session != Some(session) => ViewTransform::FIT,
            _ => self.transform(),
        }
    }

    fn synchronize_source(&mut self, surface: Surface) -> Option<SurfaceGesture> {
        let Some(session) = surface.transform_identity() else {
            return None;
        };
        if self.content_session == Some(session) {
            return None;
        }
        let cancellation = self
            .pointer_active
            .then(|| self.last_pointer_sample)
            .flatten()
            .map(|mut sample| {
                sample.pressed = false;
                SurfaceGesture {
                    kind: SurfaceGestureKind::Cancel,
                    sample,
                }
            });
        self.zoom = 1.0;
        self.pan_x = 0.0;
        self.pan_y = 0.0;
        self.pan_origin = None;
        self.pointer_active = false;
        self.last_pointer_sample = None;
        self.content_session = Some(session);
        cancellation
    }

    fn finish_pointer(&mut self, sample: Option<SurfaceSample>) -> Option<SurfaceGesture> {
        if !std::mem::take(&mut self.pointer_active) {
            return None;
        }
        let (kind, mut sample) = match sample {
            Some(sample) => (SurfaceGestureKind::Pointer, sample),
            None => (SurfaceGestureKind::End, self.last_pointer_sample?),
        };
        sample.pressed = false;
        self.last_pointer_sample = None;
        Some(SurfaceGesture { kind, sample })
    }

    fn begin_integration_redraw(&mut self, surface: Surface) -> bool {
        if !surface.integration {
            return false;
        }
        if !crate::integration_control::repeated_redraws_enabled() {
            self.integration_redraw_remaining = 0;
            return false;
        }
        let Some(revision) = surface
            .integration
            .then_some(surface.frame)
            .flatten()
            .map(|frame| {
                (
                    frame.presentation_revision,
                    surface.crop,
                    surface.fit_revision,
                )
            })
        else {
            return false;
        };
        if self.integration_redraw_revision != Some(revision) {
            self.integration_redraw_revision = Some(revision);
            self.integration_redraw_remaining = INTEGRATION_REDRAW_PASSES;
        }
        if self.integration_redraw_remaining == 0 {
            return false;
        }
        self.integration_redraw_remaining -= 1;
        true
    }

    fn update<Message>(
        &mut self,
        event: &Event,
        bounds: Rectangle,
        cursor: mouse::Cursor,
        surface: Surface,
        placement: Placement,
        publish: Option<&dyn Fn(SurfaceGesture) -> shader::Action<Message>>,
    ) -> Option<shader::Action<Message>> {
        if let Some(gesture) = self.synchronize_source(surface) {
            return Some(publish.map_or_else(shader::Action::capture, |publish| {
                publish(gesture).and_capture()
            }));
        }
        match event {
            Event::Window(iced::window::Event::RedrawRequested(_))
                if self.begin_integration_redraw(surface) =>
            {
                Some(shader::Action::request_redraw())
            }
            Event::Mouse(mouse::Event::WheelScrolled { delta })
                if placement == Placement::Contain && cursor.is_over(bounds) =>
            {
                let amount = match delta {
                    mouse::ScrollDelta::Lines { y, .. } => *y * 0.12,
                    mouse::ScrollDelta::Pixels { y, .. } => *y * 0.002,
                };
                self.zoom = (self.zoom * (1.0 + amount)).clamp(0.25, 16.0);
                Some(shader::Action::request_redraw().and_capture())
            }
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left))
                if cursor.is_over(bounds) =>
            {
                let publish = publish?;
                let sample = self.sample(bounds, cursor, surface, placement, true);
                self.pointer_active = sample.is_some();
                self.last_pointer_sample = sample;
                Some(sample.map_or_else(shader::Action::capture, |sample| {
                    publish(SurfaceGesture {
                        kind: SurfaceGestureKind::Pointer,
                        sample,
                    })
                    .and_capture()
                }))
            }
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Right))
                if placement == Placement::Contain && cursor.is_over(bounds) =>
            {
                self.pan_origin = cursor.position();
                Some(shader::Action::capture())
            }
            Event::Mouse(mouse::Event::CursorMoved { position }) => {
                if let Some(prior) = self.pan_origin {
                    self.pan_x += position.x - prior.x;
                    self.pan_y += position.y - prior.y;
                    self.pan_origin = Some(*position);
                    Some(shader::Action::request_redraw().and_capture())
                } else if self.pointer_active {
                    let Some(publish) = publish else {
                        self.pointer_active = false;
                        self.last_pointer_sample = None;
                        return Some(shader::Action::capture());
                    };
                    let sample = self.sample(bounds, cursor, surface, placement, true);
                    if sample.is_some() {
                        self.last_pointer_sample = sample;
                    }
                    Some(sample.map_or_else(shader::Action::capture, |sample| {
                        publish(SurfaceGesture {
                            kind: SurfaceGestureKind::Pointer,
                            sample,
                        })
                        .and_capture()
                    }))
                } else {
                    let publish = publish?;
                    self.sample(bounds, cursor, surface, placement, false)
                        .map(|sample| {
                            publish(SurfaceGesture {
                                kind: SurfaceGestureKind::Viewport,
                                sample,
                            })
                        })
                }
            }
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left))
                if self.pointer_active =>
            {
                let sample = self.sample(bounds, cursor, surface, placement, false);
                Some(match (publish, self.finish_pointer(sample)) {
                    (Some(publish), Some(gesture)) => publish(gesture).and_capture(),
                    _ => shader::Action::capture(),
                })
            }
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Right))
                if self.pan_origin.take().is_some() =>
            {
                Some(shader::Action::capture())
            }
            _ => None,
        }
    }

    fn sample(
        self,
        bounds: Rectangle,
        cursor: mouse::Cursor,
        mut surface: Surface,
        mut placement: Placement,
        pressed: bool,
    ) -> Option<SurfaceSample> {
        let point = cursor.position_in(bounds)?;
        let mut origin_y = 0.0;
        if matches!(placement, Placement::GalleryGrid { .. }) {
            let (shown, snapshot) = gallery::displayed()?;
            origin_y = gallery::row_offset(placement, &snapshot, bounds.width);
            surface = shown;
            placement = gallery::placement(&snapshot);
        }
        surface.frame?;
        let geometry = placement_geometry(
            Rectangle {
                x: 0.0,
                y: origin_y,
                width: bounds.width,
                height: bounds.height,
            },
            surface.content_extent(),
            placement,
            self.transform(),
        )?;
        let (content_x, content_y) =
            inverse_content_point(geometry, point, surface.content_extent())?;
        let [crop_x, crop_y, _, _] = surface.content_region();
        Some(SurfaceSample {
            width: bounds.width.max(1.0) as u32,
            height: bounds.height.max(1.0) as u32,
            x: point.x.max(0.0) as u32,
            y: point.y.max(0.0) as u32,
            content_x: content_x + crop_x,
            content_y: content_y + crop_y,
            pressed,
        })
    }
}

#[derive(Debug, Clone, Copy)]
struct ViewTransform {
    zoom: f32,
    pan_x: f32,
    pan_y: f32,
}

impl ViewTransform {
    const FIT: Self = Self {
        zoom: 1.0,
        pan_x: 0.0,
        pan_y: 0.0,
    };
}

#[derive(Debug, Clone, Copy, PartialEq)]
struct PlacementGeometry {
    x: f32,
    y: f32,
    width: f32,
    height: f32,
}

fn placement_geometry(
    bounds: Rectangle,
    content: (u32, u32),
    placement: Placement,
    transform: ViewTransform,
) -> Option<PlacementGeometry> {
    if ![
        bounds.x,
        bounds.y,
        bounds.width,
        bounds.height,
        transform.zoom,
        transform.pan_x,
        transform.pan_y,
    ]
    .iter()
    .all(|value| value.is_finite())
        || bounds.width <= 0.0
        || bounds.height <= 0.0
        || content.0 == 0
        || content.1 == 0
    {
        return None;
    }
    let (base_width, base_height) = match placement {
        Placement::Contain => {
            let aspect = content.0 as f32 / content.1 as f32;
            if bounds.width / bounds.height > aspect {
                (bounds.height * aspect, bounds.height)
            } else {
                (bounds.width, bounds.width / aspect)
            }
        }
        Placement::GalleryGrid { columns, rows, .. }
            if columns != 0
                && rows != 0
                && content.0 % columns == 0
                && content.1 % rows == 0
                && content.0 / columns == content.1 / rows =>
        {
            (bounds.width, bounds.width / columns as f32 * rows as f32)
        }
        Placement::GalleryGrid { .. } => return None,
    };
    let (zoom, pan_x, pan_y) = match placement {
        Placement::Contain => (transform.zoom, transform.pan_x, transform.pan_y),
        Placement::GalleryGrid { .. } => (1.0, 0.0, 0.0),
    };
    let width = base_width * zoom;
    let height = base_height * zoom;
    Some(PlacementGeometry {
        x: bounds.x + (bounds.width - width) * 0.5 + pan_x,
        y: bounds.y
            + if matches!(placement, Placement::Contain) {
                (bounds.height - height) * 0.5 + pan_y
            } else {
                0.0
            },
        width,
        height,
    })
}

fn inverse_content_point(
    geometry: PlacementGeometry,
    point: Point,
    content: (u32, u32),
) -> Option<(u32, u32)> {
    if point.x < geometry.x
        || point.y < geometry.y
        || point.x >= geometry.x + geometry.width
        || point.y >= geometry.y + geometry.height
        || content.0 == 0
        || content.1 == 0
    {
        return None;
    }
    Some((
        (((point.x - geometry.x) / geometry.width) * content.0 as f32)
            .clamp(0.0, content.0.saturating_sub(1) as f32) as u32,
        (((point.y - geometry.y) / geometry.height) * content.1 as f32)
            .clamp(0.0, content.1.saturating_sub(1) as f32) as u32,
    ))
}

#[derive(Debug)]
pub(crate) struct Primitive {
    surface: Surface,
    transform: ViewTransform,
    placement: Placement,
    control_id: &'static str,
}

impl shader::Primitive for Primitive {
    type Pipeline = Pipeline;

    fn prepare(
        &self,
        pipeline: &mut Pipeline,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        bounds: &Rectangle,
        viewport: &Viewport,
    ) {
        let (pan_x, pan_y) = physical_pan(self.transform, viewport.scale_factor());
        let transform = ViewTransform {
            zoom: self.transform.zoom,
            pan_x,
            pan_y,
        };
        RENDERER.with(|renderer| {
            let mut renderer = renderer.borrow_mut();
            // Explicit application retirement can remove the owner while Iced
            // retains its installed wrapper. Restore only that absent owner.
            let renderer = renderer
                .get_or_insert_with(|| SurfaceRenderer::new(device, queue, pipeline.format));
            renderer.prepare(
                device,
                queue,
                self.surface,
                physical_bounds(*bounds, viewport.scale_factor()),
                viewport.scale_factor(),
                self.placement,
                transform,
            );
            renderer.prepare_draw(device, queue, self.control_id, self.placement, transform);
        });
        if self.surface.integration {
            RENDERER.with(|renderer| {
                if let Some(owner) = renderer.borrow_mut().as_mut() {
                    owner.reconstruct_pending_handoff(device, self.surface);
                }
            });
        }
    }

    fn render(
        &self,
        pipeline: &Pipeline,
        encoder: &mut wgpu::CommandEncoder,
        target: &wgpu::TextureView,
        clip_bounds: &Rectangle<u32>,
    ) {
        let _ = pipeline;
        RENDERER.with(|renderer| {
            let renderer = renderer.borrow();
            let Some(renderer) = renderer.as_ref() else {
                return;
            };
            renderer.render(encoder, target, *clip_bounds, self.control_id);
        });
    }
}

pub(crate) fn physical_bounds(bounds: Rectangle, scale: f32) -> Rectangle {
    Rectangle {
        x: bounds.x * scale,
        y: bounds.y * scale,
        width: bounds.width * scale,
        height: bounds.height * scale,
    }
}

fn physical_pan(transform: ViewTransform, scale: f32) -> (f32, f32) {
    (transform.pan_x * scale, transform.pan_y * scale)
}

// Iced pipeline installation establishes this component's device owner.
// Shader reconstruction preserves its single-claim imports and completed copies;
// explicit application retirement can clear it while the wrapper stays installed.
thread_local! {
    static RENDERER: std::cell::RefCell<Option<SurfaceRenderer>> = const { std::cell::RefCell::new(None) };
    // One matching domain/control frontier; independent of GPU completion and
    // external sample custody. It can authorize an already-submitted queue read.
    static DRAW_AUTHORIZATION: std::cell::Cell<Option<FrameReady>> = const { std::cell::Cell::new(None) };
}

pub(crate) fn authorize_draw(frame: Option<FrameReady>) {
    DRAW_AUTHORIZATION.with(|authorization| authorization.set(frame));
}

pub(crate) fn retire_imports() {
    authorize_draw(None);
    gallery::clear();
    RENDERER.with(|renderer| *renderer.borrow_mut() = None);
}

pub(crate) fn completed_content(
    frame: &crate::generated::VisualFrame,
    snapshot: &crate::generated::PresentationSnapshot,
) -> Option<Surface> {
    RENDERER.with(|renderer| {
        let renderer = renderer.borrow();
        let renderer = renderer.as_ref()?;
        [&renderer.imported, &renderer.pending]
            .into_iter()
            .find_map(|imported| {
                let imported = imported.as_ref()?;
                imported.image.completed_content(frame, snapshot)
            })
    })
}

pub(crate) fn retained_surface() -> Option<Surface> {
    RENDERER.with(|renderer| {
        let renderer = renderer.borrow();
        let image = &renderer.as_ref()?.imported.as_ref()?.image;
        image.retained()
    })
}

pub(crate) fn retained_detail() -> Option<(Surface, DetailContent)> {
    RENDERER.with(|renderer| {
        let renderer = renderer.borrow();
        let image = &renderer.as_ref()?.imported.as_ref()?.image;
        Some((image.retained()?, image.detail.clone()?))
    })
}

pub(crate) fn drawable_detail(requested: Surface) -> Option<(Surface, DetailContent)> {
    RENDERER
        .with(|renderer| {
            let renderer = renderer.borrow();
            let renderer = renderer.as_ref()?;
            [&renderer.pending, &renderer.imported]
                .into_iter()
                .flatten()
                .find_map(|imported| {
                    let pending = imported.image.submitted_draw(requested)?;
                    Some((requested, pending.detail.clone()?))
                })
        })
        .or_else(|| {
            retained_detail().map(|(retained, detail)| {
                if retained.frame == requested.frame && same_allocation(retained, requested) {
                    (requested, detail)
                } else {
                    (retained, detail)
                }
            })
        })
}

pub(crate) struct Pipeline {
    format: wgpu::TextureFormat,
}

struct SurfaceRenderer {
    device: wgpu::Device,
    queue: wgpu::Queue,
    format: wgpu::TextureFormat,
    render: wgpu::RenderPipeline,
    capture: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    imported: Option<Imported>,
    pending: Option<Imported>,
    bounds: Rectangle,
    scale_factor: f32,
    requested: Option<Surface>,
    reconstruction_probe: Option<((u64, u64, u64), (u64, u64, u64))>,
    draws: std::collections::HashMap<&'static str, PreparedDraw>,
}

struct Imported {
    image: ImagePublication,
    _texture: wgpu::Texture,
    geometry: wgpu::Buffer,
    mailbox: Option<MailboxBindings>,
    owned: [OwnedImage; 2],
    geometry_key: GeometryKey,
    drawn_revision: AtomicU64,
    draw_count: AtomicU64,
    pixel_trace: Option<pixel_trace::PixelTrace>,
}

// CPU facts travel with their receiver-owned pixels, including across an
// advertised replacement and while a newer capture awaits native metadata.
struct ImagePublication {
    surface: Surface,
    owned_index: usize,
    pending_capture: Option<PendingImage>,
    captured: Option<FrameReady>,
    gallery: Option<std::sync::Arc<crate::generated::ExploreSnapshot>>,
    detail: Option<DetailContent>,
    placement: Placement,
}

#[derive(Clone)]
pub(crate) struct DetailContent {
    explore: std::sync::Arc<crate::generated::ExploreSnapshot>,
    upscale: Option<std::sync::Arc<crate::generated::UpscaleSnapshot>>,
}

impl DetailContent {
    fn from_model(model: &crate::view_model::ApplicationModel) -> Option<Self> {
        let frame = model.viewed_explore_frame()?;
        let explore = model.explore.snapshot.as_ref()?;
        Some(Self {
            explore: std::sync::Arc::new(explore.clone()),
            upscale: model
                .current_upscale()
                .filter(|upscale| upscale.frame == frame)
                .map(|upscale| std::sync::Arc::new(upscale.clone())),
        })
    }

    pub(crate) fn frame(&self) -> &crate::generated::VisualFrame {
        self.upscale
            .as_ref()
            .map_or(&self.explore.frame, |upscale| &upscale.frame)
    }

    pub(crate) fn scene(&self) -> &crate::generated::AnnotationSceneContent {
        self.upscale
            .as_ref()
            .map_or(&self.explore.scene, |upscale| &upscale.scene)
    }

    pub(crate) fn overlay(&self) -> &crate::generated::ExploreOverlay {
        &self.explore.overlay
    }

    fn matches_model(&self, model: &crate::view_model::ApplicationModel) -> bool {
        model
            .explore
            .snapshot
            .as_ref()
            .is_some_and(|snapshot| snapshot.revision == self.explore.revision)
            && self.upscale.as_ref().map(|snapshot| snapshot.revision)
                == model
                    .current_upscale()
                    .filter(|snapshot| snapshot.frame == *self.frame())
                    .map(|snapshot| snapshot.revision)
    }
}

struct OwnedImage {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
}

struct PendingImage {
    surface: Surface,
    gallery: Option<std::sync::Arc<crate::generated::ExploreSnapshot>>,
    detail: Option<DetailContent>,
    placement: Placement,
    index: usize,
    complete: bool,
    view_ready: bool,
}

struct PreparedDraw {
    owned_index: usize,
    surface: Surface,
    requested: Surface,
    bounds: Rectangle,
    geometry: PlacementGeometry,
    placement: Placement,
    gallery: Option<std::sync::Arc<crate::generated::ExploreSnapshot>>,
    uniform: wgpu::Buffer,
    bindings: [wgpu::BindGroup; 2],
    key: GeometryKey,
}

struct MailboxBindings {
    _views: [wgpu::TextureView; 6],
    bind_groups: [wgpu::BindGroup; 6],
}

impl Drop for Imported {
    fn drop(&mut self) {
        trace_surface("retired", self.image.surface);
        self._texture.destroy();
        for owned in &self.owned {
            owned.texture.destroy();
        }
    }
}

impl shader::Pipeline for Pipeline {
    fn new(device: &wgpu::Device, queue: &wgpu::Queue, format: wgpu::TextureFormat) -> Self {
        RENDERER.with(|renderer| {
            let mut owner = renderer.borrow_mut();
            // Iced installs this wrapper once in each engine's primitive
            // storage. A new installation establishes its device boundary.
            drop(owner.take());
            *owner = Some(SurfaceRenderer::new(device, queue, format));
        });
        Self { format }
    }
}

impl SurfaceRenderer {
    fn new(device: &wgpu::Device, queue: &wgpu::Queue, format: wgpu::TextureFormat) -> Self {
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("mmltk presentation layout"),
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
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("mmltk presentation sampler"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..wgpu::SamplerDescriptor::default()
        });
        let (render, capture) = Self::create_pipelines(device, format, &layout);
        Self {
            device: device.clone(),
            queue: queue.clone(),
            format,
            render,
            capture,
            layout,
            sampler,
            imported: None,
            pending: None,
            bounds: Rectangle::default(),
            scale_factor: 1.0,
            requested: None,
            reconstruction_probe: None,
            draws: std::collections::HashMap::new(),
        }
    }

    fn create_pipelines(
        device: &wgpu::Device,
        format: wgpu::TextureFormat,
        layout: &wgpu::BindGroupLayout,
    ) -> (wgpu::RenderPipeline, wgpu::RenderPipeline) {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("mmltk presentation shader"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(SHADER)),
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("mmltk presentation pipeline layout"),
            bind_group_layouts: &[Some(layout)],
            immediate_size: 0,
        });
        let make_pipeline = |format, fragment| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("mmltk presentation pipeline"),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_main"),
                    buffers: &[],
                    compilation_options: wgpu::PipelineCompilationOptions::default(),
                },
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some(fragment),
                    targets: &[Some(wgpu::ColorTargetState {
                        format,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: wgpu::PipelineCompilationOptions::default(),
                }),
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                multiview_mask: None,
                cache: None,
            })
        };
        (
            make_pipeline(format, "fs_main"),
            make_pipeline(wgpu::TextureFormat::Rgba8Unorm, "fs_capture"),
        )
    }

    fn replace_pipelines(&mut self, device: &wgpu::Device, format: wgpu::TextureFormat) {
        let (render, capture) = Self::create_pipelines(device, format, &self.layout);
        self.render = render;
        self.capture = capture;
        self.format = format;
    }

    fn reconstruct_pending_handoff(&mut self, device: &wgpu::Device, requested: Surface) {
        let Some(imported) = self.imported.as_ref() else {
            return;
        };
        if imported.image.captured.is_none() || same_allocation(imported.image.surface, requested) {
            return;
        }
        if !self.pending.as_ref().is_some_and(|pending| {
            same_allocation(pending.image.surface, requested) && pending.image.captured.is_none()
        }) {
            return;
        }
        let completed = imported.image.surface;
        let probe = (
            (completed.high, completed.low, completed.generation),
            (requested.high, requested.low, requested.generation),
        );
        // A pending physical identity is created once and retired when replaced.
        // Retain its pair guard across all redraws until the next pending import.
        if self.reconstruction_probe == Some(probe) {
            return;
        }
        self.replace_pipelines(device, self.format);
        self.reconstruction_probe = Some(probe);
        trace_surface_request("renderer_reconstructed", completed, requested, "");
    }
}

fn retained_draw_admitted(retained: Surface, requested: Surface, placement: Placement) -> bool {
    matches!(placement, Placement::GalleryGrid { .. })
        || (retained.frame.is_some() && retained.frame == requested.frame)
        || retained.viewer_identity == requested.viewer_identity
}

impl SurfaceRenderer {
    fn matching_import<'a>(
        imported: &'a mut Option<Imported>,
        pending: &'a mut Option<Imported>,
        surface: Surface,
    ) -> Option<&'a mut Imported> {
        imported
            .iter_mut()
            .chain(pending.iter_mut())
            .find(|candidate| same_allocation(candidate.image.surface, surface))
    }

    fn reconcile_capture(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        surface: Surface,
        placement: Placement,
    ) {
        if let Some(imported) =
            Self::matching_import(&mut self.imported, &mut self.pending, surface)
        {
            imported.reconcile_capture(
                device,
                queue,
                &self.capture,
                &self.layout,
                &self.sampler,
                surface,
                placement,
            );
            return;
        }
        self.prepare(
            device,
            queue,
            surface,
            self.bounds,
            self.scale_factor,
            placement,
            ViewTransform::FIT,
        );
    }

    fn prepare_draw(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        control: &'static str,
        placement: Placement,
        transform: ViewTransform,
    ) {
        let requested = self.requested.expect("prepared surface request");
        let submitted = [&self.pending, &self.imported]
            .into_iter()
            .flatten()
            .find_map(|imported| {
                imported
                    .image
                    .submitted_draw(requested)
                    .map(|pending| (imported, pending))
            });
        let Some(imported) = submitted.map(|(imported, _)| imported).or_else(|| {
            self.imported
                .as_ref()
                .filter(|value| value.image.retained().is_some())
        }) else {
            self.draws.remove(control);
            return;
        };
        let (surface, owned_index, gallery) = submitted.map_or(
            (
                imported.image.surface,
                imported.image.owned_index,
                imported.image.gallery.as_ref(),
            ),
            |(_, pending)| (pending.surface, pending.index, pending.gallery.as_ref()),
        );
        if !retained_draw_admitted(surface, requested, placement) {
            self.draws.remove(control);
            return;
        }
        let mut bounds = self.bounds;
        let mut placement = placement;
        if matches!(placement, Placement::GalleryGrid { .. }) {
            let Some(snapshot) = gallery else {
                self.draws.remove(control);
                return;
            };
            bounds.y += gallery::row_offset(placement, snapshot, bounds.width);
            placement = gallery::placement(snapshot);
        }
        let Some(geometry) =
            placement_geometry(bounds, surface.content_extent(), placement, transform)
        else {
            trace_image(
                "owned_draw_rejected",
                control,
                surface,
                requested,
                gallery.map(std::sync::Arc::as_ref),
                Some((bounds, None)),
            );
            self.draws.remove(control);
            return;
        };
        let key = geometry_key(surface, bounds, placement, transform);
        let draw = self.draws.entry(control).or_insert_with(|| {
            let uniform = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("mmltk widget image geometry"),
                size: 48,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            write_content_geometry(queue, &uniform, key);
            let bindings = std::array::from_fn(|index| {
                bind_group(
                    device,
                    &self.layout,
                    &self.sampler,
                    &uniform,
                    &imported.owned[index].view,
                )
            });
            PreparedDraw {
                owned_index,
                surface,
                requested,
                bounds,
                geometry,
                placement,
                gallery: matches!(placement, Placement::GalleryGrid { .. })
                    .then(|| gallery.cloned())
                    .flatten(),
                uniform,
                bindings,
                key,
            }
        });
        if !same_allocation(draw.surface, surface) {
            draw.bindings = std::array::from_fn(|index| {
                bind_group(
                    device,
                    &self.layout,
                    &self.sampler,
                    &draw.uniform,
                    &imported.owned[index].view,
                )
            });
        }
        if draw.key != key {
            write_content_geometry(queue, &draw.uniform, key);
            draw.key = key;
        }
        draw.owned_index = owned_index;
        draw.surface = surface;
        draw.requested = requested;
        draw.bounds = bounds;
        draw.geometry = geometry;
        draw.placement = placement;
        draw.gallery = matches!(placement, Placement::GalleryGrid { .. })
            .then(|| gallery.cloned())
            .flatten();
    }

    fn prepare(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        surface: Surface,
        bounds: Rectangle,
        scale_factor: f32,
        placement: Placement,
        transform: ViewTransform,
    ) {
        self.bounds = bounds;
        self.scale_factor = scale_factor;
        self.requested = Some(surface);
        if !surface.valid() {
            return;
        }
        if self
            .imported
            .as_ref()
            .is_some_and(|imported| same_allocation(imported.image.surface, surface))
        {
            self.discard_pending();
        }
        if let Some(imported) =
            Self::matching_import(&mut self.imported, &mut self.pending, surface)
        {
            imported.prepare(
                device,
                queue,
                &self.capture,
                &self.layout,
                &self.sampler,
                surface,
                bounds,
                placement,
                transform,
            );
            return;
        }
        trace_surface("texture_create", surface);
        let label = surface.label();
        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some(&label),
            size: wgpu::Extent3d {
                width: surface.width,
                height: surface.height,
                depth_or_array_layers: 6,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let geometry = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("mmltk presentation content geometry"),
            size: 48,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let geometry_key = geometry_key(surface, bounds, placement, transform);
        write_content_geometry(queue, &geometry, geometry_key);
        // Current and pending images alternate within one physical import.
        // A capture never overwrites pixels whose labels are already recorded.
        let owned = std::array::from_fn(|_| {
            let texture = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("mmltk owned presentation pixels"),
                size: wgpu::Extent3d {
                    width: surface.width,
                    height: surface.height,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8Unorm,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING
                    | if pixel_trace::enabled() {
                        wgpu::TextureUsages::COPY_SRC
                    } else {
                        wgpu::TextureUsages::empty()
                    },
                view_formats: &[],
            });
            let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
            OwnedImage { texture, view }
        });
        let mut imported = Imported {
            image: ImagePublication {
                surface,
                owned_index: 0,
                pending_capture: None,
                captured: None,
                gallery: gallery::matching(surface.frame),
                detail: None,
                placement,
            },
            _texture: texture,
            geometry,
            mailbox: None,
            owned,
            geometry_key,
            drawn_revision: AtomicU64::new(0),
            draw_count: AtomicU64::new(0),
            pixel_trace: pixel_trace::PixelTrace::new(device, queue),
        };
        imported.capture(device, queue, &self.capture, &self.layout, &self.sampler);
        self.discard_pending();
        self.pending = Some(imported);
    }

    fn discard_pending(&mut self) {
        if let Some(pending) = self.pending.take() {
            trace_surface(
                if pending.image.pending_capture.is_some() {
                    "pending_capture_discarded"
                } else {
                    "pending_discarded"
                },
                pending.image.surface,
            );
        }
    }

    fn render(
        &self,
        encoder: &mut wgpu::CommandEncoder,
        target: &wgpu::TextureView,
        clip: Rectangle<u32>,
        control_id: &'static str,
    ) {
        let Some(draw) = self.draws.get(control_id) else {
            if let Some(requested) = self.requested {
                trace_surface_request("owned_draw_missing", requested, requested, control_id);
            }
            return;
        };
        let bounds = draw.bounds;
        let geometry = draw.geometry;
        let frame = draw.surface.frame.expect("prepared completed image");
        let image = Rectangle {
            x: geometry.x,
            y: geometry.y,
            width: geometry.width,
            height: geometry.height,
        };
        let clip = Rectangle {
            x: clip.x as f32,
            y: clip.y as f32,
            width: clip.width as f32,
            height: clip.height as f32,
        };
        let Some(visible) = image.intersection(&clip) else {
            trace_draw("owned_draw_clipped", control_id, draw, image, clip);
            return;
        };
        if visible.width <= 0.0 || visible.height <= 0.0 {
            trace_draw("owned_draw_clipped", control_id, draw, image, clip);
            return;
        }
        let Some(imported) =
            [&self.imported, &self.pending]
                .into_iter()
                .flatten()
                .find(|imported| {
                    (imported.image.captured == Some(frame)
                        && imported.image.owned_index == draw.owned_index)
                        || imported
                            .image
                            .submitted_draw(draw.requested)
                            .is_some_and(|pending| {
                                pending.surface.frame == Some(frame)
                                    && pending.index == draw.owned_index
                            })
                })
        else {
            trace_draw("owned_draw_rejected", control_id, draw, image, clip);
            return;
        };
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("mmltk presentation pass"),
            color_attachments: &[Some(preserving_color_attachment(target))],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_viewport(
            visible.x,
            visible.y,
            visible.width,
            visible.height,
            0.0,
            1.0,
        );
        pass.set_scissor_rect(
            clip.x as u32,
            clip.y as u32,
            clip.width as u32,
            clip.height as u32,
        );
        pass.set_pipeline(&self.render);
        pass.set_bind_group(0, Some(&draw.bindings[draw.owned_index]), &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        trace_surface_request(
            "owned_draw_selected",
            draw.surface,
            draw.requested,
            control_id,
        );
        trace_draw("draw_encoded", control_id, draw, image, clip);
        crate::integration_control::notify_driver_draw(
            control_id,
            frame.content_sequence,
            frame.presentation_revision,
        );
        if draw.surface.integration {
            crate::integration_control::record_probe_draw(
                control_id,
                draw.surface,
                bounds,
                image,
                clip,
            );
            crate::integration_control::sample_boundary_pixels(
                draw.surface,
                control_id,
                image,
                clip,
            );
        }
        if draw.surface.integration {
            if draw.gallery.is_some() {
                crate::integration_control::report_atlas_draw(
                    crate::integration_control::AtlasDraw {
                        surface: draw.surface,
                        snapshot: draw.gallery.as_ref().expect("gallery draw").clone(),
                        bounds,
                        image,
                        clip,
                    },
                    gallery::dark(),
                    self.scale_factor,
                );
            }
        }
        if control_id == crate::view::explore::DETAIL_WORKSPACE_ID {
            record_drawn_detail(draw.surface, draw.surface.content_region());
        }
        if draw.surface.integration {
            let prior_draw = imported
                .drawn_revision
                .swap(frame.presentation_revision, Ordering::AcqRel);
            let draw_count = imported.draw_count.fetch_add(1, Ordering::Relaxed) + 1;
            let redraw = prior_draw == frame.presentation_revision;
            crate::integration_control::report_surface_draw(
                control_id,
                frame.presentation_revision,
                frame.content_sequence,
                redraw,
                frame.content_width,
                frame.content_height,
                draw_count,
                crate::integration_control::ViewerDraw {
                    crop: draw.surface.content_region(),
                    container: bounds,
                    image: Rectangle {
                        x: geometry.x,
                        y: geometry.y,
                        width: geometry.width,
                        height: geometry.height,
                    },
                    fit_revision: draw.surface.fit_revision,
                },
            );
            crate::integration_control::report_surface_geometry(
                control_id,
                frame.presentation_revision,
                frame.content_sequence,
                Rectangle {
                    x: geometry.x,
                    y: geometry.y,
                    width: geometry.width,
                    height: geometry.height,
                },
            );
            if matches!(draw.placement, Placement::Contain) {
                crate::integration_control::report_surface_container(
                    control_id,
                    frame.presentation_revision,
                    frame.content_sequence,
                    bounds,
                    frame.content_width,
                    frame.content_height,
                );
            }
            crate::integration_control::report_surface_scale(
                control_id,
                frame.presentation_revision,
                frame.content_sequence,
                self.scale_factor,
            );
        }
    }
}

fn preserving_color_attachment(view: &wgpu::TextureView) -> wgpu::RenderPassColorAttachment<'_> {
    wgpu::RenderPassColorAttachment {
        view,
        depth_slice: None,
        resolve_target: None,
        ops: wgpu::Operations {
            load: wgpu::LoadOp::Load,
            store: wgpu::StoreOp::Store,
        },
    }
}

pub(crate) fn complete_capture(frame: FrameReady) {
    trace_frame("completion_received", frame);
    RENDERER.with(|renderer| {
        if let Some(renderer) = renderer.borrow_mut().as_mut() {
            for imported in [&mut renderer.imported, &mut renderer.pending]
                .into_iter()
                .flatten()
            {
                imported.image.complete(frame);
            }
        }
    });
}

pub(crate) fn discard_capture(frame: FrameReady) {
    RENDERER.with(|renderer| {
        if let Some(renderer) = renderer.borrow_mut().as_mut() {
            for imported in [&mut renderer.imported, &mut renderer.pending]
                .into_iter()
                .flatten()
            {
                imported.image.discard(frame);
            }
        }
    });
}

pub(crate) fn reconcile_completed(surface: Surface, model: &crate::view_model::ApplicationModel) {
    let Some(frame) = surface.frame else {
        return;
    };
    if model.completed_presentation_reconciliation().ok()
        != Some(crate::view_model::PresentationReconciliation::Matching)
        || model
            .presentation
            .as_ref()
            .is_none_or(|snapshot| !frame.matches_completed(snapshot))
    {
        return;
    }
    authorize_draw(Some(frame));
    RENDERER.with(|renderer| {
        let mut renderer = renderer.borrow_mut();
        let Some(renderer) = renderer.as_mut() else {
            return;
        };
        // Submit the receiver-owned copy before Iced builds labels and layout.
        // Widget preparation remains the sole owner of view identity and geometry.
        let device = renderer.device.clone();
        let queue = renderer.queue.clone();
        let placement = gallery::matching(Some(frame))
            .as_ref()
            .map_or(Placement::Contain, |snapshot| gallery::placement(snapshot));
        renderer.reconcile_capture(&device, &queue, surface, placement);
        for imported in [&mut renderer.imported, &mut renderer.pending]
            .into_iter()
            .flatten()
        {
            imported.image.reconcile_pending(frame, model);
        }
        if let Some(imported) = renderer.imported.as_mut()
            && imported.image.captured == Some(frame)
        {
            imported.image.refresh_detail(model);
            return;
        }
        if renderer
            .imported
            .as_mut()
            .is_some_and(|imported| imported.image.promote(frame, model))
        {
            return;
        }
        if renderer
            .pending
            .as_mut()
            .is_some_and(|pending| pending.image.promote(frame, model))
        {
            if renderer.imported.is_some() {
                trace_surface(
                    "owned_replacement",
                    renderer
                        .pending
                        .as_ref()
                        .expect("completed pending image")
                        .image
                        .surface,
                );
            }
            renderer.imported = renderer.pending.take();
        }
    });
}

impl ImagePublication {
    fn reconcile_pending(
        &mut self,
        frame: FrameReady,
        model: &crate::view_model::ApplicationModel,
    ) {
        let Some(pending) = self
            .pending_capture
            .as_mut()
            .filter(|pending| pending.surface.frame == Some(frame))
        else {
            return;
        };
        pending.view_ready = true;
        if let Some(current) = gallery::matching(Some(frame))
            && pending
                .gallery
                .as_ref()
                .is_none_or(|previous| previous.dataset.identity == current.dataset.identity)
        {
            pending.gallery = Some(current);
        }
        if pending
            .detail
            .as_ref()
            .is_none_or(|detail| !detail.matches_model(model))
        {
            pending.detail = DetailContent::from_model(model)
                .filter(|detail| frame.matches_content(detail.frame()));
        }
    }

    fn submitted_draw(&self, requested: Surface) -> Option<&PendingImage> {
        self.pending_capture.as_ref().filter(|pending| {
            pending.view_ready
                && pending.surface.frame.is_some()
                && pending.surface.frame == requested.frame
                && same_allocation(pending.surface, requested)
                && DRAW_AUTHORIZATION
                    .with(|authorization| authorization.get() == pending.surface.frame)
                && pending
                    .gallery
                    .as_ref()
                    .is_none_or(|snapshot| gallery::current_source(snapshot))
        })
    }

    fn refresh_detail(&mut self, model: &crate::view_model::ApplicationModel) {
        if self
            .detail
            .as_ref()
            .is_none_or(|detail| !detail.matches_model(model))
        {
            self.detail = DetailContent::from_model(model).filter(|detail| {
                self.surface
                    .frame
                    .is_some_and(|frame| frame.matches_content(detail.frame()))
            });
        }
    }

    fn retained(&self) -> Option<Surface> {
        (self.captured.is_some() && self.captured == self.surface.frame).then_some(self.surface)
    }

    fn completed_content(
        &self,
        frame: &crate::generated::VisualFrame,
        snapshot: &crate::generated::PresentationSnapshot,
    ) -> Option<Surface> {
        self.retained().filter(|surface| {
            surface.frame.is_some_and(|captured| {
                captured.matches_content(frame) && captured.matches_completed(snapshot)
            })
        })
    }

    fn complete(&mut self, frame: FrameReady) {
        if let Some(pending) = self.pending_capture.as_mut()
            && pending.surface.frame == Some(frame)
        {
            pending.complete = true;
        }
    }

    fn discard(&mut self, frame: FrameReady) {
        if self
            .pending_capture
            .as_ref()
            .is_some_and(|pending| pending.surface.frame == Some(frame))
        {
            self.pending_capture = None;
        }
    }

    fn promote(&mut self, frame: FrameReady, model: &crate::view_model::ApplicationModel) -> bool {
        if model.completed_presentation_reconciliation().ok()
            != Some(crate::view_model::PresentationReconciliation::Matching)
            || model
                .presentation
                .as_ref()
                .is_none_or(|snapshot| !frame.matches_completed(snapshot))
            || !self
                .pending_capture
                .as_ref()
                .is_some_and(|pending| pending.complete && pending.surface.frame == Some(frame))
        {
            return false;
        }
        let pending = self
            .pending_capture
            .take()
            .expect("matching completed capture");
        trace_surface("owned_capture_promotion_started", pending.surface);
        if pending
            .gallery
            .as_ref()
            .is_some_and(|snapshot| !gallery::current_source(snapshot))
        {
            trace_surface("owned_capture_promotion_rejected", pending.surface);
            return false;
        }
        self.surface = pending.surface;
        self.gallery = pending.gallery;
        self.detail = pending.detail;
        self.placement = pending.placement;
        self.owned_index = pending.index;
        self.captured = Some(frame);
        self.refresh_detail(model);
        trace_surface("owned_capture_promoted", self.surface);
        true
    }
}

impl Imported {
    fn reconcile_capture(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        pipeline: &wgpu::RenderPipeline,
        layout: &wgpu::BindGroupLayout,
        sampler: &wgpu::Sampler,
        surface: Surface,
        placement: Placement,
    ) {
        let retained_surface = self.image.surface;
        let retained_gallery = self.image.gallery.clone();
        let retained_placement = self.image.placement;
        self.image.surface = surface;
        self.image.gallery = gallery::matching(surface.frame);
        self.image.placement = placement;
        self.capture(device, queue, pipeline, layout, sampler);
        self.image.surface = retained_surface;
        self.image.gallery = retained_gallery;
        self.image.placement = retained_placement;
    }

    fn prepare(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        pipeline: &wgpu::RenderPipeline,
        layout: &wgpu::BindGroupLayout,
        sampler: &wgpu::Sampler,
        surface: Surface,
        bounds: Rectangle,
        placement: Placement,
        transform: ViewTransform,
    ) {
        let retained = self.image.surface;
        let retained_gallery = self.image.gallery.clone();
        let retained_placement = self.image.placement;
        self.image.gallery = gallery::matching(surface.frame).or_else(|| {
            (self.image.captured == surface.frame)
                .then(|| retained_gallery.clone())
                .flatten()
        });
        self.image.placement = placement;
        refresh_import(self, surface, bounds, placement, transform, queue);
        self.capture(device, queue, pipeline, layout, sampler);
        if let Some(pending) = self.image.pending_capture.as_mut()
            && pending.surface.frame == surface.frame
        {
            pending.surface = surface;
            pending.placement = placement;
        }
        if self.image.captured.is_some()
            && (self.image.captured != surface.frame
                || !retained_draw_admitted(retained, surface, placement))
        {
            self.image.gallery = retained_gallery;
            self.image.placement = retained_placement;
            refresh_import(self, retained, bounds, retained_placement, transform, queue);
        }
    }

    fn capture(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        pipeline: &wgpu::RenderPipeline,
        layout: &wgpu::BindGroupLayout,
        sampler: &wgpu::Sampler,
    ) {
        let Some(frame) = self.image.surface.frame else {
            return;
        };
        if self.image.captured == Some(frame)
            || self
                .image
                .pending_capture
                .as_ref()
                .is_some_and(|pending| pending.surface.frame == Some(frame))
        {
            return;
        }
        if matches!(self.image.placement, Placement::GalleryGrid { .. })
            && self.image.gallery.is_none()
        {
            trace_image(
                "capture_rejected",
                "",
                self.image.surface,
                self.image.surface,
                None,
                None,
            );
            notify_surface(Notification::CaptureRejected(frame));
            return;
        }
        if frame.content_width == 0
            || frame.content_height == 0
            || frame.content_width > self.image.surface.width
            || frame.content_height > self.image.surface.height
        {
            retire_publication(frame);
            return;
        }
        let Some(borrow) = CaptureBorrow::acquire(frame) else {
            trace_image(
                "capture_unavailable",
                "",
                self.image.surface,
                self.image.surface,
                self.image.gallery.as_deref(),
                None,
            );
            return;
        };
        let mailbox = self.mailbox.get_or_insert_with(|| {
            let views = std::array::from_fn(|layer| {
                self._texture.create_view(&wgpu::TextureViewDescriptor {
                    dimension: Some(wgpu::TextureViewDimension::D2),
                    base_array_layer: layer as u32,
                    array_layer_count: Some(1),
                    ..wgpu::TextureViewDescriptor::default()
                })
            });
            let bind_groups = std::array::from_fn(|layer| {
                bind_group(device, layout, sampler, &self.geometry, &views[layer])
            });
            MailboxBindings {
                _views: views,
                bind_groups,
            }
        });
        let binding = mailbox_binding(frame).expect("acquired mailbox binding");
        let owned_index = 1 - self.image.owned_index;
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("mmltk receiver-owned capture"),
        });
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("mmltk external sample final use"),
                color_attachments: &[Some(preserving_color_attachment(
                    &self.owned[owned_index].view,
                ))],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_viewport(
                0.0,
                0.0,
                frame.content_width as f32,
                frame.content_height as f32,
                0.0,
                1.0,
            );
            pass.set_scissor_rect(0, 0, frame.content_width, frame.content_height);
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0, Some(&mailbox.bind_groups[binding]), &[]);
            pass.draw(0..3, 0..1);
        }
        queue.submit([encoder.finish()]);
        // Subsequent Iced submissions on this queue see the completed capture.
        // App rejection/replacement cannot return this slot while capture runs.
        // The completion closure owns custody even if this pipeline is dropped.
        let captured_surface = self.image.surface;
        queue.on_submitted_work_done(move || {
            trace_surface("owned_capture_completed", captured_surface);
            drop(borrow);
            notify_surface(Notification::Completed(frame));
            trace_surface("owned_capture_notified", captured_surface);
        });
        if let Some(probe) = &self.pixel_trace {
            probe.sample(&self.owned[owned_index].texture, captured_surface);
        }
        self.image.pending_capture = Some(PendingImage {
            surface: self.image.surface,
            gallery: self.image.gallery.clone(),
            detail: None,
            placement: self.image.placement,
            index: owned_index,
            complete: false,
            view_ready: false,
        });
        trace_image(
            "owned_capture_submitted",
            "",
            self.image.surface,
            self.image.surface,
            self.image.gallery.as_deref(),
            None,
        );
    }
}

pub(crate) fn same_allocation(left: Surface, right: Surface) -> bool {
    left.high == right.high
        && left.low == right.low
        && left.generation == right.generation
        && left.width == right.width
        && left.height == right.height
}

fn refresh_import(
    imported: &mut Imported,
    surface: Surface,
    bounds: Rectangle,
    placement: Placement,
    transform: ViewTransform,
    queue: &wgpu::Queue,
) {
    let geometry_key = geometry_key(surface, bounds, placement, transform);
    if imported.geometry_key != geometry_key {
        write_content_geometry(queue, &imported.geometry, geometry_key);
        imported.geometry_key = geometry_key;
    }
    imported.image.surface = surface;
}

fn mailbox_binding(frame: FrameReady) -> Option<usize> {
    let binding = frame
        .layer
        .checked_mul(MAILBOX_SLOTS)?
        .checked_add(frame.slot)?;
    (binding < 6).then_some(binding as usize)
}

fn bind_group(
    device: &wgpu::Device,
    layout: &wgpu::BindGroupLayout,
    sampler: &wgpu::Sampler,
    geometry: &wgpu::Buffer,
    view: &wgpu::TextureView,
) -> wgpu::BindGroup {
    device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("mmltk presentation bind group"),
        layout,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(view),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::Sampler(sampler),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: geometry.as_entire_binding(),
            },
        ],
    })
}

fn content_uv_scale(surface: Surface) -> [f32; 2] {
    let width_height = surface.content_extent();
    [
        width_height.0 as f32 / surface.width as f32,
        width_height.1 as f32 / surface.height as f32,
    ]
}

#[derive(Debug, Clone, Copy, PartialEq)]
struct GeometryKey {
    uv_scale: [f32; 2],
    uv_offset: [f32; 2],
    draw_extent: [f32; 2],
    grid: [u32; 2],
    gallery: u32,
    image_origin: [f32; 2],
}

fn geometry_key(
    surface: Surface,
    bounds: Rectangle,
    placement: Placement,
    transform: ViewTransform,
) -> GeometryKey {
    let content = surface.content_extent();
    let [x, y, _, _] = surface.content_region();
    let draw_extent = placement_geometry(bounds, content, placement, transform)
        .map_or([0.0, 0.0], |geometry| [geometry.width, geometry.height]);
    let (grid, gallery) = match placement {
        Placement::Contain => ([0, 0], 0),
        Placement::GalleryGrid { columns, rows, .. } => ([columns, rows], 1),
    };
    GeometryKey {
        uv_scale: content_uv_scale(surface),
        uv_offset: [
            x as f32 / surface.width as f32,
            y as f32 / surface.height as f32,
        ],
        draw_extent,
        grid,
        gallery,
        image_origin: placement_geometry(bounds, content, placement, transform)
            .map_or([0.0, 0.0], |g| [g.x, g.y]),
    }
}

fn write_content_geometry(queue: &wgpu::Queue, geometry: &wgpu::Buffer, key: GeometryKey) {
    let mut bytes = [0_u8; 48];
    bytes[..4].copy_from_slice(&key.uv_scale[0].to_ne_bytes());
    bytes[4..8].copy_from_slice(&key.uv_scale[1].to_ne_bytes());
    bytes[8..12].copy_from_slice(&key.draw_extent[0].to_ne_bytes());
    bytes[12..16].copy_from_slice(&key.draw_extent[1].to_ne_bytes());
    bytes[16..20].copy_from_slice(&key.grid[0].to_ne_bytes());
    bytes[20..24].copy_from_slice(&key.grid[1].to_ne_bytes());
    bytes[24..28].copy_from_slice(&key.gallery.to_ne_bytes());
    bytes[32..36].copy_from_slice(&key.uv_offset[0].to_ne_bytes());
    bytes[36..40].copy_from_slice(&key.uv_offset[1].to_ne_bytes());
    bytes[40..44].copy_from_slice(&key.image_origin[0].to_ne_bytes());
    bytes[44..48].copy_from_slice(&key.image_origin[1].to_ne_bytes());
    queue.write_buffer(geometry, 0, &bytes);
}

pub(crate) fn release(frame: FrameReady) {
    BORROWS.with(|borrows| {
        let mut slots = borrows.get();
        if let Some(binding) = mailbox_binding(frame)
            && slots[binding] == Some(frame)
        {
            slots[binding] = None;
            borrows.set(slots);
        }
    });
    if reserve_release(frame) {
        dispatch_release(frame);
    }
}

// Reserve the notification at transfer of custody, not at callback dispatch.
// A capture's completion token then owns exactly one eventual notification.
fn reserve_release(frame: FrameReady) -> bool {
    RELEASED_FRAMES.with(|released| {
        let mut slots = released.get();
        let Some(slot) = slots.get_mut((frame.layer * MAILBOX_SLOTS + frame.slot) as usize) else {
            return true;
        };
        if slot.is_some_and(|prior| {
            same_mailbox_slot(prior, frame)
                && prior.presentation_revision >= frame.presentation_revision
        }) {
            return false;
        }
        *slot = Some(frame);
        released.set(slots);
        true
    })
}

#[cfg(target_arch = "wasm32")]
fn dispatch_release(frame: FrameReady) {
    let detail = wasm_bindgen::JsValue::from_str(&format!(
        "{:016x}{:016x}:{}:{}:{}:{}:{}",
        frame.high,
        frame.low,
        frame.layer,
        frame.slot,
        frame.content_session,
        frame.content_sequence,
        frame.presentation_revision,
    ));
    let init = web_sys::CustomEventInit::new();
    init.set_detail(&detail);
    if let Ok(event) = web_sys::CustomEvent::new_with_event_init_dict(RELEASE_EVENT, &init)
        && let Some(window) = web_sys::window()
    {
        let _ = window.dispatch_event(&event);
    }
}

#[cfg(all(not(target_arch = "wasm32"), not(test)))]
fn dispatch_release(_frame: FrameReady) {}

#[cfg(test)]
thread_local! {
    static TEST_RELEASES: std::cell::RefCell<Vec<FrameReady>> = const { std::cell::RefCell::new(Vec::new()) };
}

#[cfg(all(not(target_arch = "wasm32"), test))]
fn dispatch_release(frame: FrameReady) {
    TEST_RELEASES.with(|released| released.borrow_mut().push(frame));
}

#[cfg(test)]
pub(crate) fn reset_test_releases() {
    authorize_draw(None);
    BORROWS.with(|borrows| borrows.set([None; 6]));
    RELEASED_FRAMES.with(|released| released.set([None; 6]));
    TEST_RELEASES.with(|released| released.borrow_mut().clear());
    clear_drawn_detail();
}

#[cfg(test)]
pub(crate) fn test_capture_borrow(frame: FrameReady) -> impl Drop {
    CaptureBorrow::acquire(frame).expect("accepted physical publication")
}

#[cfg(test)]
pub(crate) fn test_releases() -> Vec<FrameReady> {
    TEST_RELEASES.with(|released| released.borrow().clone())
}

const SHADER: &str = r#"
@group(0) @binding(0) var image: texture_2d<f32>;
@group(0) @binding(1) var image_sampler: sampler;
struct Geometry {
    uv_scale: vec2<f32>,
    draw_extent: vec2<f32>,
    grid: vec2<u32>,
    gallery: u32,
    padding: u32,
    uv_offset: vec2<f32>,
    image_origin: vec2<f32>,
};
@group(0) @binding(2) var<uniform> geometry: Geometry;

struct Output {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> Output {
    let positions = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>(3.0, -1.0),
        vec2<f32>(-1.0, 3.0)
    );
    let uv = array<vec2<f32>, 3>(
        vec2<f32>(0.0, 1.0),
        vec2<f32>(2.0, 1.0),
        vec2<f32>(0.0, -1.0)
    );
    return Output(vec4<f32>(positions[index], 0.0, 1.0), uv[index]);
}

@fragment
fn fs_capture(input: Output) -> @location(0) vec4<f32> {
    return textureLoad(image, vec2<i32>(input.position.xy), 0);
}

@fragment
fn fs_main(input: Output) -> @location(0) vec4<f32> {
    let image_uv = (input.position.xy - geometry.image_origin) / geometry.draw_extent;
    let half_texel = vec2<f32>(0.5) / vec2<f32>(textureDimensions(image));
    let uv = clamp(geometry.uv_offset + image_uv * geometry.uv_scale,
        geometry.uv_offset + half_texel, geometry.uv_offset + geometry.uv_scale - half_texel);
    let sampled = textureSample(image, image_sampler, uv);
    if geometry.gallery == 0u || geometry.grid.x == 0u || geometry.grid.y == 0u {
        return sampled;
    }
    let cell = geometry.draw_extent / vec2<f32>(geometry.grid);
    let pixel = image_uv * geometry.draw_extent;
    let boundary = round(pixel / cell) * cell;
    let outer_center = min(vec2<f32>(1.5), geometry.draw_extent * 0.5);
    let center = clamp(
        boundary + vec2<f32>(0.5),
        outer_center,
        geometry.draw_extent - outer_center
    );
    let line_pixel = abs(round(pixel - center));
    if line_pixel.x < 0.5 || line_pixel.y < 0.5 {
        return vec4<f32>(1.0, 1.0, 1.0, 1.0);
    }
    if line_pixel.x < 1.5 || line_pixel.y < 1.5 {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    return sampled;
}
"#;

#[cfg(test)]
mod tests {
    use super::*;

    use crate::view_model::test_support::physical_frame as frame_ready;

    #[test]
    fn completion_authorization_matches_all_available_native_identity() {
        let source = crate::view_model::test_support::visual_frame(
            crate::generated::PresentationSourceKind::Explore,
            7,
        );
        let mut model = crate::view_model::test_support::bootstrapped();
        let snapshot = model.presentation.as_mut().unwrap();
        snapshot.completed = source.clone();
        snapshot.presentationrevision = 11;
        snapshot.capability.surfacehigh = 1;
        snapshot.capability.surfacelow = 2;
        let original = frame_ready(
            crate::generated::presentation_source_session(source.source.kind),
            source.revision,
            11,
            source.extent.width,
            source.extent.height,
        );
        let surface = surface_for_content_session(original.content_session);
        assert!(original.matches_completed(snapshot));
        for field in 0..8 {
            let mut different = original;
            match field {
                0 => different.high += 1,
                1 => different.low += 1,
                2 => different.layer += 1,
                3 => different.slot = MAILBOX_SLOTS,
                4 => different.content_session += 1,
                5 => different.content_sequence += 1,
                6 => different.presentation_revision += 1,
                _ => different.content_width += 1,
            }
            assert!(!different.matches_completed(snapshot) || !different.belongs_to(surface));
            assert_ne!(different, original);
        }
        assert!(
            !FrameReady {
                content_height: original.content_height + 1,
                ..original
            }
            .matches_completed(snapshot)
        );
        let reused = FrameReady {
            content_sequence: 8,
            presentation_revision: 12,
            ..original
        };
        assert!(same_mailbox_slot(original, reused));
        assert_ne!(original, reused);
        snapshot.capability.surfacelow += 1;
        snapshot.capability.generation += 1;
        snapshot.capability.condition = crate::generated::PresentationCapabilityCondition::Admitted;
        assert!(original.matches_completed(snapshot));
        assert!(original.belongs_to(surface));
    }

    #[test]
    fn captured_predecessor_survives_every_causal_metadata_and_capture_order() {
        use crate::view_model::test_support::{
            annotation_object, explore_presentation, physical_surface,
        };

        for replacement in [false, true] {
            for [
                domain_position,
                control_position,
                physical_position,
                capture_position,
            ] in crate::view_model::test_support::presentation_arrival_orders()
            {
                reset_test_releases();
                let (mut model, old) = explore_presentation();
                let mut previous = physical_surface(old);
                previous.viewer_identity = Some((9, 0));
                let explore = model.explore.snapshot.as_mut().unwrap();
                explore.dataset.identity = 9;
                explore.overlay.showlabels = true;
                explore.scene.categories = vec![crate::generated::ArtifactClassName {
                    value: "predecessor".into(),
                }];
                explore.scene.objects = vec![annotation_object(0)];
                let mut next_explore = explore.clone();
                next_explore.revision = 20;
                next_explore.frame.revision = 2;
                next_explore.scene.categories[0].value = "successor".into();
                next_explore.scene.objects[0].box_.first.x += 17.0;
                let snapshot = model.presentation.as_mut().unwrap();
                let mut next = FrameReady {
                    content_sequence: 2,
                    presentation_revision: 6,
                    slot: 1,
                    ..old
                };
                let mut target = previous;
                if replacement {
                    next.low += 1;
                    target.low = next.low;
                    target.generation += 1;
                    target.width *= 2;
                    target.height *= 2;
                }
                target.frame = Some(next);
                // Capability B is already advertised while completion still
                // identifies A/F0. It cannot change the retained image.
                snapshot.capability.surfacehigh = target.high;
                snapshot.capability.surfacelow = target.low;
                snapshot.capability.generation = target.generation;
                snapshot.capability.extent.width = target.width;
                snapshot.capability.extent.height = target.height;
                snapshot.capability.condition = if replacement {
                    crate::generated::PresentationCapabilityCondition::Admitted
                } else {
                    crate::generated::PresentationCapabilityCondition::Ready
                };
                let mut next_control = snapshot.clone();
                next_control.completed = next_explore.frame.clone();
                next_control.completedsourcerevision = 20;
                next_control.presentationrevision = 6;
                next_control.capability.condition =
                    crate::generated::PresentationCapabilityCondition::Ready;
                assert!(accept_publication(old));
                drop(CaptureBorrow::acquire(old).unwrap());
                let mut image = ImagePublication {
                    surface: previous,
                    owned_index: 0,
                    pending_capture: None,
                    captured: Some(old),
                    gallery: None,
                    detail: DetailContent::from_model(&model),
                    placement: Placement::Contain,
                };
                record_drawn_detail(image.surface, image.surface.content_region());
                assert!(viewer_copy_matches(
                    &model,
                    image.detail.as_ref().unwrap().frame()
                ));
                assert_eq!(
                    image.completed_content(
                        image.detail.as_ref().unwrap().frame(),
                        model.presentation.as_ref().unwrap(),
                    ),
                    Some(previous)
                );
                assert!(
                    image
                        .completed_content(
                            &next_explore.frame,
                            model.presentation.as_ref().unwrap()
                        )
                        .is_none()
                );
                let mut copy = None;
                let mut replacement_image: Option<ImagePublication> = None;
                let mut domain_arrived = false;
                let mut control_arrived = false;
                let mut capture_completed = false;
                for position in 0..4 {
                    if position == domain_position {
                        model.explore.snapshot = Some(next_explore.clone());
                        domain_arrived = true;
                    } else if position == control_position {
                        model.presentation = Some(next_control.clone());
                        control_arrived = true;
                    } else if position == physical_position {
                        assert!(accept_publication(next));
                        copy = CaptureBorrow::acquire(next);
                        assert!(copy.is_some());
                        let pending = Some(PendingImage {
                            surface: target,
                            gallery: None,
                            detail: None,
                            placement: Placement::Contain,
                            index: 1,
                            complete: false,
                            view_ready: false,
                        });
                        if replacement {
                            replacement_image = Some(ImagePublication {
                                surface: target,
                                owned_index: 0,
                                pending_capture: pending,
                                captured: None,
                                gallery: None,
                                detail: None,
                                placement: Placement::Contain,
                            });
                        } else {
                            image.pending_capture = pending;
                        }
                    } else {
                        assert_eq!(position, capture_position);
                        drop(copy.take());
                        replacement_image
                            .as_mut()
                            .unwrap_or(&mut image)
                            .complete(next);
                        capture_completed = true;
                    }
                    if replacement {
                        if replacement_image
                            .as_mut()
                            .is_some_and(|candidate| candidate.promote(next, &model))
                        {
                            image = replacement_image.take().unwrap();
                        }
                    } else {
                        let _ = image.promote(next, &model);
                    }
                    let promoted = domain_arrived && control_arrived && capture_completed;
                    assert_eq!(image.surface, if promoted { target } else { previous });
                    assert_eq!(image.captured, Some(if promoted { next } else { old }));
                    assert_eq!(image.owned_index, usize::from(promoted));
                    let meaning = image.detail.as_ref().unwrap();
                    assert_eq!(
                        meaning.scene().categories[0].value,
                        if promoted { "successor" } else { "predecessor" }
                    );
                    assert_eq!(
                        meaning.scene().objects[0].box_.first.x,
                        if promoted {
                            next_explore.scene.objects[0].box_.first.x
                        } else {
                            next_explore.scene.objects[0].box_.first.x - 17.0
                        }
                    );
                    assert!(meaning.overlay().showlabels);
                    record_drawn_detail(image.surface, image.surface.content_region());
                    let authorized = model
                        .viewed_explore_frame()
                        .is_some_and(|source| viewer_copy_matches(&model, &source));
                    assert_eq!(
                        authorized,
                        promoted || (!domain_arrived && !control_arrived)
                    );
                    assert_eq!(
                        test_releases(),
                        if capture_completed {
                            vec![old, next]
                        } else {
                            vec![old]
                        }
                    );
                }
                image.complete(next); // A duplicate completion cannot promote or release twice.
                assert!(!image.promote(next, &model));
                retire_publication(next);
                assert_eq!(test_releases(), vec![old, next]);
                // Label-only metadata advances no physical frame. Update
                // its coherent semantics without recapturing or releasing.
                let labels = model.explore.snapshot.as_mut().unwrap();
                labels.revision += 1;
                labels.overlay.showlabels = false;
                assert_eq!(
                    model.completed_presentation_reconciliation().unwrap(),
                    crate::view_model::PresentationReconciliation::Matching
                );
                image.refresh_detail(&model);
                assert!(!image.detail.as_ref().unwrap().overlay().showlabels);
                assert_eq!(image.retained(), Some(target));
                assert_eq!(test_releases(), vec![old, next]);
                let obsolete = FrameReady {
                    content_sequence: 3,
                    presentation_revision: 7,
                    ..next
                };
                assert!(accept_publication(obsolete));
                let unsettled = CaptureBorrow::acquire(obsolete).unwrap();
                image.pending_capture = Some(PendingImage {
                    surface: Surface {
                        frame: Some(obsolete),
                        ..target
                    },
                    gallery: None,
                    detail: None,
                    placement: Placement::Contain,
                    index: 0,
                    complete: false,
                    view_ready: false,
                });
                let current = model.explore.snapshot.as_mut().unwrap();
                current.revision = 40;
                current.frame.revision = 4;
                let completed = model.presentation.as_mut().unwrap();
                completed.completed.revision = 3;
                completed.completedsourcerevision = 30;
                completed.presentationrevision = 7;
                assert_eq!(
                    model.completed_presentation_reconciliation().unwrap(),
                    crate::view_model::PresentationReconciliation::Superseded
                );
                image.discard(obsolete);
                retire_publication(obsolete);
                assert_eq!(test_releases(), vec![old, next]); // GPU custody still owns this release.
                drop(unsettled);
                image.complete(obsolete);
                assert!(!image.promote(obsolete, &model));
                assert_eq!(image.retained(), Some(target));
                assert_eq!(
                    image.detail.as_ref().unwrap().scene().categories[0].value,
                    "successor"
                );
                assert_eq!(test_releases(), vec![old, next, obsolete]);
            }
        }
    }

    #[test]
    fn receiver_capture_is_reconciled_after_transport_continuity_recovery() {
        use crate::generated::{ExploreMode, FeatureId, PresentationSourceKind};
        use crate::view_model::test_support::{bootstrapped, physical_surface, visual_frame};
        for matching in [false, true] {
            for completed_before_disconnect in [false, true] {
                reset_test_releases();
                let mut model = bootstrapped();
                model.set_foreground_feature(FeatureId::Explore);
                let source = visual_frame(PresentationSourceKind::Explore, 1);
                let frame = frame_ready(1, 1, 5, source.extent.width, source.extent.height);
                let surface = physical_surface(frame);
                let explore = model.explore.snapshot.as_mut().unwrap();
                explore.ready = true;
                explore.mode = ExploreMode::Detail;
                explore.selectedimage = Some(0);
                explore.revision = 10;
                explore.frame = source.clone();
                let control = model.presentation.as_mut().unwrap();
                control.completed = source;
                control.completedsourcerevision = 10;
                control.presentationrevision = 5;
                let restored_explore = model.explore.snapshot.clone();
                let restored_control = model.presentation.clone();
                assert!(accept_publication(frame));
                let mut borrow = Some(CaptureBorrow::acquire(frame).unwrap());
                let mut image = ImagePublication {
                    surface,
                    owned_index: 0,
                    captured: None,
                    pending_capture: Some(PendingImage {
                        surface,
                        index: 1,
                        gallery: None,
                        detail: None,
                        placement: Placement::Contain,
                        complete: false,
                        view_ready: false,
                    }),
                    gallery: None,
                    detail: None,
                    placement: Placement::Contain,
                };
                authorize_draw(None);
                assert!(image.submitted_draw(surface).is_none());
                authorize_draw(Some(frame));
                assert!(image.submitted_draw(surface).is_none());
                image.pending_capture.as_mut().unwrap().view_ready = true;
                assert!(image.submitted_draw(surface).is_some());
                assert!(!image.pending_capture.as_ref().unwrap().complete);
                assert!(image.retained().is_none());
                authorize_draw(None);
                if completed_before_disconnect {
                    drop(borrow.take());
                    image.complete(frame);
                }
                model
                    .peer_disconnected(crate::view_model::UiError::transport("capture continuity"));
                assert!(!image.promote(frame, &model));
                model.peer_connected();
                if !completed_before_disconnect {
                    drop(borrow.take());
                    image.complete(frame);
                }
                assert!(!image.promote(frame, &model));
                assert_eq!(test_releases(), vec![frame]);
                assert!(image.pending_capture.as_ref().unwrap().complete);
                model.presentation = restored_control;
                model.explore.snapshot = restored_explore;
                model.set_foreground_feature(FeatureId::Explore);
                if !matching {
                    let current = model.explore.snapshot.as_mut().unwrap();
                    current.revision += 1;
                    current.frame.revision += 1;
                }
                assert_eq!(image.promote(frame, &model), matching);
                if matching {
                    assert_eq!(image.retained(), Some(surface));
                    record_drawn_detail(surface, surface.content_region());
                    assert!(viewer_copy_matches(
                        &model,
                        image.detail.as_ref().unwrap().frame()
                    ));
                } else {
                    image.discard(frame);
                    assert!(image.retained().is_none());
                    assert!(image.pending_capture.is_none());
                }
                retire_publication(frame);
                assert_eq!(test_releases(), vec![frame]);
            }
        }
    }

    #[test]
    fn retained_detail_draw_requires_the_same_image_but_survives_pending_work() {
        let mut retained = surface_for_content_session(3);
        retained.viewer_identity = Some((9, 2));
        let mut pending = retained;
        pending.frame = None;
        assert!(retained_draw_admitted(
            retained,
            pending,
            Placement::Contain
        ));
        pending.frame = retained.frame;
        pending.viewer_identity = Some((10, 3));
        assert!(retained_draw_admitted(
            retained,
            pending,
            Placement::Contain
        ));
        pending.frame = None;
        pending.viewer_identity = retained.viewer_identity;
        pending.high += 1;
        pending.generation += 1;
        assert!(retained_draw_admitted(
            retained,
            pending,
            Placement::Contain
        ));
        pending.viewer_identity = Some((9, 3));
        assert!(!retained_draw_admitted(
            retained,
            pending,
            Placement::Contain
        ));
        pending.viewer_identity = Some((10, 2));
        assert!(!retained_draw_admitted(
            retained,
            pending,
            Placement::Contain
        ));
        pending.viewer_identity = None;
        assert!(!retained_draw_admitted(
            retained,
            pending,
            Placement::Contain
        ));
    }

    #[test]
    fn mailbox_pressure_retains_the_latest_frame_for_each_layer() {
        reset_test_releases();
        let mut mailbox = FrameMailbox::default();
        for revision in 1..=20 {
            let mut frame = frame_ready(1, revision, revision, 640, 480);
            frame.layer = (revision % 3) as u32;
            if let Some(displaced) = mailbox.push(frame) {
                release(displaced);
            }
        }
        let mut revisions = Vec::new();
        while let Some(frame) = mailbox.pop() {
            revisions.push(frame.presentation_revision);
        }
        revisions.sort_unstable();
        assert_eq!(revisions, vec![18, 19, 20]);
        assert_eq!(test_releases().len(), 17);
    }

    #[test]
    fn completed_notifications_coalesce_without_consuming_native_publications() {
        reset_test_releases();
        let old = frame_ready(1, 20, 20, 400, 400);
        let newest = frame_ready(1, 21, 21, 400, 500);
        let mut mailbox = FrameMailbox::default();
        mailbox.push(newest);
        mailbox.complete(old);
        mailbox.complete(newest);
        mailbox.complete(old);
        mailbox.rejected = Some(old);
        assert!(
            matches!(mailbox.next_notification(), Some(Notification::Completed(frame)) if frame == newest)
        );
        assert!(
            matches!(mailbox.next_notification(), Some(Notification::CaptureRejected(frame)) if frame == old)
        );
        assert!(
            matches!(mailbox.next_notification(), Some(Notification::Native(frame)) if frame == newest)
        );
        assert!(mailbox.next_notification().is_none());
        drop(mailbox);
        assert!(test_releases().is_empty());
    }

    #[test]
    fn stale_mailbox_metadata_cannot_release_an_active_capture() {
        reset_test_releases();
        let current = frame_ready(1, 10, 10, 640, 480);
        assert!(accept_publication(current));
        let capture = CaptureBorrow::acquire(current).unwrap();
        let mut mailbox = FrameMailbox::default();
        assert!(mailbox.push(current).is_none());
        assert!(mailbox.push(current).is_none());
        let newer = frame_ready(1, 11, 11, 640, 480);
        release(mailbox.push(newer).unwrap());
        assert!(test_releases().is_empty());
        assert_eq!(mailbox.pop(), Some(newer));
        drop(capture);
        assert_eq!(test_releases(), vec![current]);
    }

    #[test]
    fn publication_custody_is_consumed_once_and_release_waits_for_capture() {
        reset_test_releases();
        let frame = frame_ready(1, 1, 1, 640, 480);
        assert!(CaptureBorrow::acquire(frame).is_none());
        assert!(accept_publication(frame));
        let capture = CaptureBorrow::acquire(frame).unwrap();
        assert!(CaptureBorrow::acquire(frame).is_none());
        assert!(!accept_publication(frame));
        // Replacement, navigation and shutdown may all ask to retire a sample
        // while its submitted capture still owns the final external GPU use.
        retire_publication(frame);
        retire_publication(frame);
        assert!(test_releases().is_empty());
        drop(capture);
        assert_eq!(test_releases(), vec![frame]);
        assert!(!accept_publication(frame));
        assert!(CaptureBorrow::acquire(frame).is_none());
        release(frame);
        assert_eq!(test_releases(), vec![frame]);
    }

    #[test]
    fn cancellation_and_slot_reuse_never_reacquire_a_receipt() {
        reset_test_releases();
        let old = frame_ready(1, 1, 1, 640, 480);
        assert!(accept_publication(old));
        release(old); // Rejected before any GPU work.
        assert!(CaptureBorrow::acquire(old).is_none());
        let current = FrameReady {
            presentation_revision: 2,
            content_sequence: 2,
            ..old
        };
        assert!(accept_publication(current));
        record_drawn_detail(
            crate::view_model::test_support::physical_surface(old),
            [0, 0, 640, 480],
        );
        invalidate_drawn_slot(current);
        assert!(drawn_detail().is_none());
        assert!(CaptureBorrow::acquire(old).is_none());
        let capture = CaptureBorrow::acquire(current).unwrap();
        release(old);
        assert_eq!(test_releases(), vec![old]);
        // Dropping the completion owner (including device teardown) settles
        // custody, even when the surface/pipeline no longer exists.
        drop(capture);
        assert_eq!(test_releases(), vec![old, current]);
        assert!(!accept_publication(old));
        assert!(!accept_publication(current));
    }

    #[test]
    fn allocation_replacement_keeps_each_capture_completion_independent() {
        reset_test_releases();
        let old = frame_ready(1, 1, 1, 640, 480);
        assert!(accept_publication(old));
        let old_capture = CaptureBorrow::acquire(old).unwrap();
        let replacement = FrameReady {
            high: 3,
            low: 4,
            ..old
        };
        assert!(accept_publication(replacement));
        let new_capture = CaptureBorrow::acquire(replacement).unwrap();
        retire_publication(old);
        retire_publication(replacement);
        assert!(test_releases().is_empty());
        assert!(CaptureBorrow::acquire(old).is_none());
        assert!(CaptureBorrow::acquire(replacement).is_none());
        drop(old_capture);
        assert_eq!(test_releases(), vec![old]);
        assert!(!accept_publication(replacement));
        drop(new_capture);
        assert_eq!(test_releases(), vec![old, replacement]);
    }

    fn surface_for_content_session(content_session: u64) -> Surface {
        Surface {
            high: 1,
            low: 2,
            generation: 1,
            width: 640,
            height: 480,
            timeline_ready: 1,
            frame: Some(frame_ready(content_session, 1, 1, 640, 480)),
            integration: false,
            crop: None,
            viewer_identity: None,
            fit_revision: 0,
        }
    }

    #[test]
    fn native_frame_payload_is_direct_and_bounded() {
        let ready =
            FrameReady::parse("00000000000000010000000000000002:0:1:3:4:5:640:480").expect("frame");
        assert_eq!(ready.slot, 1);
        assert_eq!(ready.presentation_revision, 5);
        assert!(FrameReady::parse("00000000000000010000000000000002:0:2:3:4:5:640:480",).is_none());
    }

    #[test]
    fn viewport_keeps_fit_pan_and_zoom_local() {
        let mut viewport = ViewportOwner::default();
        viewport.zoom = 2.0;
        viewport.pan_x = 12.0;
        viewport.pan_y = -4.0;
        let transform = viewport.transform();
        assert_eq!(transform.zoom, 2.0);
        assert_eq!(transform.pan_x, 12.0);
        assert_eq!(transform.pan_y, -4.0);
        assert_eq!(physical_pan(transform, 1.5), (18.0, -6.0));
    }

    #[test]
    fn viewport_transform_is_local_to_one_presentation_source() {
        let mut viewport = ViewportOwner::default();
        assert!(
            viewport
                .synchronize_source(surface_for_content_session(1))
                .is_none()
        );
        viewport.zoom = 0.25;
        viewport.pan_x = 12.0;
        viewport.pan_y = -4.0;
        assert_eq!(
            viewport.transform_for(surface_for_content_session(1)).zoom,
            0.25
        );

        let reset = viewport.transform_for(surface_for_content_session(2));
        assert_eq!(reset.zoom, 1.0);
        assert_eq!((reset.pan_x, reset.pan_y), (0.0, 0.0));
        assert!(
            viewport
                .synchronize_source(surface_for_content_session(2))
                .is_none()
        );
        assert_eq!(viewport.content_session, Some((2, 0, 0)));
        assert_eq!(viewport.transform().zoom, 1.0);
    }

    #[test]
    fn captured_release_outside_surface_uses_last_valid_sample_once() {
        let sample = SurfaceSample {
            width: 640,
            height: 480,
            x: 120,
            y: 80,
            content_x: 60,
            content_y: 40,
            pressed: true,
        };
        let mut viewport = ViewportOwner {
            pointer_active: true,
            last_pointer_sample: Some(sample),
            ..ViewportOwner::default()
        };
        let end = viewport.finish_pointer(None).expect("local end");
        assert_eq!(end.kind, SurfaceGestureKind::End);
        assert_eq!(end.sample.content_x, sample.content_x);
        assert_eq!(end.sample.content_y, sample.content_y);
        assert!(!end.sample.pressed);
        assert!(viewport.finish_pointer(None).is_none());
    }

    #[test]
    fn captured_content_session_replacement_cancels_once() {
        let sample = SurfaceSample {
            width: 640,
            height: 480,
            x: 120,
            y: 80,
            content_x: 60,
            content_y: 40,
            pressed: true,
        };
        let mut viewport = ViewportOwner::default();
        assert!(
            viewport
                .synchronize_source(surface_for_content_session(1))
                .is_none()
        );
        viewport.pointer_active = true;
        viewport.last_pointer_sample = Some(sample);
        let cancel = viewport
            .synchronize_source(surface_for_content_session(2))
            .expect("local cancellation");
        assert_eq!(cancel.kind, SurfaceGestureKind::Cancel);
        assert!(!cancel.sample.pressed);
        assert!(
            viewport
                .synchronize_source(surface_for_content_session(2))
                .is_none()
        );
    }

    #[test]
    fn integration_requests_one_initial_and_three_same_revision_redraw_passes() {
        let surface = |presentation_revision| Surface {
            high: 1,
            low: 2,
            generation: 1,
            width: 640,
            height: 480,
            timeline_ready: 1,
            frame: Some(frame_ready(1, 1, presentation_revision, 640, 480)),
            integration: true,
            crop: None,
            viewer_identity: None,
            fit_revision: 0,
        };
        let mut viewport = ViewportOwner::default();
        for _ in 0..INTEGRATION_REDRAW_PASSES {
            assert!(viewport.begin_integration_redraw(surface(1)));
        }
        assert!(!viewport.begin_integration_redraw(surface(1)));
        for _ in 0..INTEGRATION_REDRAW_PASSES {
            assert!(viewport.begin_integration_redraw(surface(2)));
        }
        assert!(!viewport.begin_integration_redraw(surface(2)));
        assert!(!viewport.begin_integration_redraw(Surface {
            integration: false,
            ..surface(3)
        }));
    }

    #[test]
    fn content_geometry_samples_only_the_completed_high_water_rectangle() {
        let scale = content_uv_scale(Surface {
            high: 1,
            low: 2,
            generation: 3,
            width: 1_280,
            height: 720,
            timeline_ready: 4,
            frame: Some(frame_ready(1, 2, 3, 640, 360)),
            integration: false,
            crop: None,
            viewer_identity: None,
            fit_revision: 0,
        });
        assert_eq!(scale, [0.5, 0.5]);
    }

    #[test]
    fn stored_crop_changes_sampling_without_replacing_the_native_frame() {
        let full = surface_for_content_session(1);
        let cropped = Surface {
            crop: Some([80, 60, 480, 360]),
            ..full
        };
        let bounds = Rectangle::with_size(iced::Size::new(1000.0, 700.0));
        let transform = ViewTransform::FIT;
        let key = geometry_key(cropped, bounds, Placement::Contain, transform);
        assert_eq!(cropped.frame, full.frame);
        assert_eq!(key.uv_offset, [0.125, 0.125]);
        assert_eq!(key.uv_scale, [0.75, 0.75]);
        assert_ne!(
            key,
            geometry_key(full, bounds, Placement::Contain, transform)
        );
        let mut state = ViewportOwner::default();
        let selected = Surface {
            viewer_identity: Some((9, 2)),
            ..full
        };
        state.synchronize_source(selected);
        state.zoom = 2.0;
        state.pan_x = 17.0;
        let upscale = Surface {
            frame: Some(frame_ready(5, 99, 88, 2560, 1920)),
            ..selected
        };
        assert_eq!(state.transform_for(upscale).zoom, 2.0);
        assert_eq!(state.transform_for(upscale).pan_x, 17.0);
        assert_eq!(
            state
                .transform_for(Surface {
                    fit_revision: 1,
                    ..upscale
                })
                .zoom,
            1.0
        );
        assert_eq!(
            state
                .transform_for(Surface {
                    viewer_identity: Some((9, 3)),
                    ..upscale
                })
                .zoom,
            1.0
        );
    }

    #[test]
    fn square_wide_and_tall_products_fit_actual_remaining_space() {
        let bounds = Rectangle {
            x: 31.0,
            y: 47.0,
            width: 913.0,
            height: 517.0,
        };
        for content in [(512, 512), (768, 384), (192, 384)] {
            let geometry =
                placement_geometry(bounds, content, Placement::Contain, ViewTransform::FIT)
                    .unwrap();
            assert!((geometry.x + geometry.width * 0.5 - bounds.center().x).abs() < 0.001);
            assert!((geometry.y + geometry.height * 0.5 - bounds.center().y).abs() < 0.001);
            assert!(geometry.width <= bounds.width && geometry.height <= bounds.height);
            assert!(
                (geometry.width - bounds.width).abs() < 0.001
                    || (geometry.height - bounds.height).abs() < 0.001
            );
            assert!(
                (geometry.width / geometry.height - content.0 as f32 / content.1 as f32).abs()
                    < 0.001
            );
        }
    }

    #[test]
    fn contain_and_gallery_share_forward_and_inverse_geometry() {
        let bounds = Rectangle {
            x: 0.0,
            y: 0.0,
            width: 100.0,
            height: 100.0,
        };
        let transform = ViewTransform::FIT;
        let contain =
            placement_geometry(bounds, (200, 100), Placement::Contain, transform).unwrap();
        assert_eq!(
            contain,
            PlacementGeometry {
                x: 0.0,
                y: 25.0,
                width: 100.0,
                height: 50.0,
            }
        );
        assert_eq!(
            inverse_content_point(contain, Point::new(50.0, 50.0), (200, 100)),
            Some((100, 50))
        );
        assert!(inverse_content_point(contain, Point::new(50.0, 10.0), (200, 100)).is_none());

        let gallery = placement_geometry(
            bounds,
            (80, 60),
            Placement::GalleryGrid {
                first_row: 0,
                columns: 4,
                rows: 3,
            },
            ViewTransform {
                zoom: 4.0,
                pan_x: 20.0,
                pan_y: -10.0,
            },
        )
        .unwrap();
        assert_eq!(
            gallery,
            PlacementGeometry {
                x: 0.0,
                y: 0.0,
                width: 100.0,
                height: 75.0,
            }
        );
        assert_eq!(
            inverse_content_point(gallery, Point::new(75.0, 50.0), (80, 60)),
            Some((60, 40))
        );
    }

    #[test]
    fn contain_maps_full_square_and_non_square_frames_in_every_workspace_ratio() {
        let frames = [(640, 640), (640, 360), (480, 640)];
        for aspect in crate::generated::WORKSPACE_ASPECT_RATIO_VALUES
            .iter()
            .copied()
        {
            let (width, height) = crate::view::aspect_ratio::extent_for_width(800.0, aspect);
            let bounds = Rectangle {
                x: 0.0,
                y: 0.0,
                width,
                height,
            };
            for content in frames {
                let geometry =
                    placement_geometry(bounds, content, Placement::Contain, ViewTransform::FIT)
                        .unwrap();
                assert!(geometry.width <= bounds.width && geometry.height <= bounds.height);
                assert!(
                    (geometry.width / geometry.height - content.0 as f32 / content.1 as f32).abs()
                        < 0.000_1
                );
                assert_eq!(
                    inverse_content_point(geometry, Point::new(geometry.x, geometry.y), content,),
                    Some((0, 0))
                );
                assert_eq!(
                    inverse_content_point(
                        geometry,
                        Point::new(
                            geometry.x + geometry.width - 0.001,
                            geometry.y + geometry.height - 0.001,
                        ),
                        content,
                    ),
                    Some((content.0 - 1, content.1 - 1))
                );
            }
        }
    }

    #[test]
    fn gallery_scale_and_hit_boundaries_agree_after_fractional_scroll_and_dpi() {
        for dpi in [1.0, 1.5, 2.0] {
            let bounds = Rectangle {
                x: 15.0 * dpi,
                y: -37.25 * dpi,
                width: 600.0 * dpi,
                height: 450.0 * dpi,
            };
            let geometry = placement_geometry(
                bounds,
                (400, 400),
                Placement::GalleryGrid {
                    first_row: 0,
                    columns: 4,
                    rows: 4,
                },
                ViewTransform::FIT,
            )
            .unwrap();
            let scale = geometry.width / 400.0;
            assert!((scale - geometry.height / 400.0).abs() < 0.0001);
            let cell_edge = Point::new(geometry.x + 100.0 * scale, geometry.y + 100.0 * scale);
            assert_eq!(
                inverse_content_point(geometry, cell_edge, (400, 400)),
                Some((100, 100))
            );
            assert_eq!(
                inverse_content_point(
                    geometry,
                    Point::new(cell_edge.x - scale * 0.5, cell_edge.y - scale * 0.5),
                    (400, 400)
                ),
                Some((99, 99))
            );
            assert!(
                inverse_content_point(
                    geometry,
                    Point::new(geometry.x + geometry.width, geometry.y),
                    (400, 400)
                )
                .is_none()
            );
            assert!(
                inverse_content_point(
                    geometry,
                    Point::new(geometry.x, geometry.y + geometry.height),
                    (400, 400)
                )
                .is_none()
            );
        }
        assert!(
            placement_geometry(
                Rectangle {
                    width: 100.0,
                    height: 100.0,
                    ..Rectangle::default()
                },
                (64, 64),
                Placement::GalleryGrid {
                    first_row: 0,
                    columns: 2,
                    rows: 1
                },
                ViewTransform::FIT
            )
            .is_none()
        );
    }

    #[test]
    fn translated_gallery_bounds_preserve_uvs_when_the_gpu_viewport_is_clipped() {
        let mut surface = surface_for_content_session(1);
        surface.width = 400;
        surface.height = 500;
        surface.frame = Some(frame_ready(1, 41, 51, 400, 500));
        let placement = Placement::GalleryGrid {
            columns: 4,
            rows: 5,
            first_row: 2,
        };
        for scale in [1.0, 1.5, 2.0] {
            // These are the bounds supplied by Iced after its scroll transform,
            // not the shader widget's original document-space layout.
            let translated = physical_bounds(
                Rectangle {
                    x: 30.0,
                    y: -37.25,
                    width: 600.0,
                    height: 750.0,
                },
                scale,
            );
            let key = geometry_key(surface, translated, placement, ViewTransform::FIT);
            let geometry = placement_geometry(
                translated,
                surface.content_extent(),
                placement,
                ViewTransform::FIT,
            )
            .unwrap();
            let image = Rectangle {
                x: geometry.x,
                y: geometry.y,
                width: geometry.width,
                height: geometry.height,
            };
            let clip = physical_bounds(
                Rectangle {
                    x: 30.0,
                    y: 0.0,
                    width: 600.0,
                    height: 500.0,
                },
                scale,
            );
            let visible = image.intersection(&clip).unwrap();
            assert_eq!(visible.y, 0.0);
            assert!(key.image_origin[1] < 0.0);
            let uv_y = (visible.y - key.image_origin[1]) / key.draw_extent[1];
            assert!((uv_y - 37.25 / 750.0).abs() < 0.00001);
            assert_eq!(
                inverse_content_point(geometry, Point::new(visible.x, visible.y), (400, 500)),
                Some((0, 24)),
            );
            assert!(
                placement_geometry(translated, (400, 400), placement, ViewTransform::FIT,)
                    .is_none()
            );
        }
    }

    #[test]
    fn resize_changes_only_uniform_geometry_for_one_import_identity() {
        let surface = surface_for_content_session(1);
        let transform = ViewTransform::FIT;
        let placement = Placement::GalleryGrid {
            first_row: 0,
            columns: 4,
            rows: 3,
        };
        let first = geometry_key(
            surface,
            Rectangle {
                width: 400.0,
                height: 300.0,
                ..Rectangle::default()
            },
            placement,
            transform,
        );
        let resized = geometry_key(
            surface,
            Rectangle {
                width: 800.0,
                height: 600.0,
                ..Rectangle::default()
            },
            placement,
            transform,
        );
        assert!(same_allocation(surface, surface));
        assert_ne!(first, resized);
        assert_eq!(first.gallery, 1);
        assert_eq!(first.grid, [4, 3]);
        let contain = geometry_key(
            surface,
            Rectangle {
                width: 400.0,
                height: 300.0,
                ..Rectangle::default()
            },
            Placement::Contain,
            transform,
        );
        assert_eq!(contain.gallery, 0);
        assert_eq!(contain.grid, [0, 0]);
    }

    #[test]
    fn native_mailbox_binding_is_bounded_to_six_retained_slots() {
        let mut frame = frame_ready(1, 2, 3, 640, 360);
        for layer in 0..3 {
            for slot in 0..MAILBOX_SLOTS {
                frame.layer = layer;
                frame.slot = slot;
                assert_eq!(
                    mailbox_binding(frame),
                    Some((layer * MAILBOX_SLOTS + slot) as usize)
                );
            }
        }
        frame.layer = 3;
        assert_eq!(mailbox_binding(frame), None);
    }
}
