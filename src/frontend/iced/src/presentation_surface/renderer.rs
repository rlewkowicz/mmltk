use super::geometry::*;
#[cfg(test)]
use super::surface_for_content_session;
#[cfg(test)]
use super::{Program, WorkspaceViewport};
use super::{Surface, gallery, labels, metadata, pixel_trace};
use iced::widget::shader::{self, Viewport};
#[cfg(test)]
use iced::{Event, Point, mouse};
use iced::{Rectangle, wgpu};
use std::borrow::Cow;
use std::sync::atomic::{AtomicU64, Ordering};
#[cfg(target_arch = "wasm32")]
use wasm_bindgen::JsCast;
#[cfg(target_arch = "wasm32")]
use wasm_bindgen::closure::Closure;

thread_local! {
    static SURFACE_TRACE_ENABLED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}
// Correlates opt-in draw diagnostics only; it never participates in rendering
// eligibility, graphics ownership, or completion scheduling.
static NEXT_DRAW_DIAGNOSTIC: AtomicU64 = AtomicU64::new(1);

pub(crate) fn initialize_diagnostics(surface_trace: bool, pixel_trace: bool) {
    SURFACE_TRACE_ENABLED.with(|flag| flag.set(surface_trace));
    pixel_trace::initialize(surface_trace && pixel_trace);
}

pub(crate) fn surface_trace_enabled() -> bool {
    SURFACE_TRACE_ENABLED.with(std::cell::Cell::get)
}

#[cfg(target_arch = "wasm32")]
pub(crate) fn trace_surface(event: &str, surface: Surface) {
    trace_surface_request(event, surface, surface, "");
}

#[cfg(target_arch = "wasm32")]
pub(crate) fn trace_frame(event: &str, frame: FrameReady) {
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

// One projection of the physical native receipt. Sample notifications carry
// the receipt by value, including when the requested surface has since changed.
#[cfg(target_arch = "wasm32")]
fn frame_trace_fields(frame: Option<FrameReady>) -> String {
    format!(
        ",\"frame_revision\":{},\"presentation_revision\":{},\"content_session\":{},\"layer\":{},\"slot\":{},\"content_width\":{},\"content_height\":{},\"source\":\"{:016x}{:016x}\",\"direct_sampling\":{}",
        frame.map_or(0, |f| f.content_sequence),
        frame.map_or(0, |f| f.presentation_revision),
        frame.map_or(0, |f| f.content_session),
        frame.map_or(0, |f| f.layer),
        frame.map_or(0, |f| f.slot),
        frame.map_or(0, |f| f.content_width),
        frame.map_or(0, |f| f.content_height),
        frame.map_or(0, |f| f.source_high),
        frame.map_or(0, |f| f.source_low),
        frame.is_some_and(|f| f.direct_sampling),
    )
}

#[cfg(target_arch = "wasm32")]
pub(crate) fn surface_trace_fields(surface: Surface, requested: Surface) -> String {
    format!(
        "\"surface\":\"{:016x}{:016x}\",\"requested_surface\":\"{:016x}{:016x}\",\"width\":{},\"height\":{}{}",
        surface.high,
        surface.low,
        requested.high,
        requested.low,
        surface.width,
        surface.height,
        frame_trace_fields(surface.frame),
    )
}

#[cfg(not(target_arch = "wasm32"))]
pub(crate) fn trace_frame(_event: &str, _frame: FrameReady) {}

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
fn trace_draw_receipt(
    event: &str,
    surface: Surface,
    requested: Surface,
    control: &str,
    draw_identity: u64,
) {
    if surface_trace_enabled() {
        emit_surface_trace(&format!(
            "{{\"event\":\"{event}\",\"control\":\"{control}\",\"draw_identity\":{draw_identity},{}}}",
            surface_trace_fields(surface, requested),
        ));
    }
}

#[cfg(target_arch = "wasm32")]
pub(crate) fn emit_surface_trace(line: &str) {
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
pub(crate) fn gallery_trace_fields(
    snapshot: Option<&crate::generated::ExploreImageMetadata>,
) -> String {
    snapshot.map_or_else(String::new, |snapshot| format!(
        ",\"source_kind\":{},\"source_instance\":{},\"source_revision\":{},\"clean_revision\":{},\"source_observation_revision\":{},\"dataset_identity\":{},\"gallery_generation\":{},\"ready_slots\":{:?},\"columns\":{},\"rows\":{},\"first_row\":{},\"matching_count\":{},\"visible_indices\":{:?},\"row_capacity\":{},\"row_origin\":{},\"card_extent\":{},\"augmentation_enabled\":{},\"augmentation_seed\":{}",
        crate::generated::presentation_source_session(snapshot.frame.source.kind), snapshot.frame.source.instance, snapshot.frame.revision,
        crate::generated::visual_clean_content_identity(&snapshot.frame).revision, snapshot.revision,
        snapshot.dataset.identity, snapshot.gallery.generation, snapshot.gallery.slots,
        snapshot.viewport.columns, snapshot.viewport.rowcount, snapshot.viewport.firstrow,
        snapshot.order.matchingcount, snapshot.order.visibleindices,
        snapshot.gallery.layout.rowcapacity, snapshot.gallery.layout.roworigin, snapshot.gallery.layout.cardextent,
        snapshot.augmentation.enabled, snapshot.augmentation.seed,
    ))
}

#[cfg(target_arch = "wasm32")]
fn trace_image(
    event: &str,
    control: &str,
    surface: Surface,
    requested: Surface,
    snapshot: Option<&crate::generated::ExploreImageMetadata>,
    geometry: Option<(Rectangle, Option<(Rectangle, Rectangle)>)>,
    draw_identity: u64,
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
        "{{\"event\":\"iced.surface.{event}\",\"control\":\"{control}\",\"draw_identity\":{draw_identity},{}{}{}{geometry}}}",
        surface_trace_fields(surface, requested),
        gallery_trace_fields(snapshot),
        surface
            .frame
            .map_or_else(String::new, metadata::trace_fields),
    ));
}

#[cfg(not(target_arch = "wasm32"))]
fn trace_image(
    _event: &str,
    _control: &str,
    _surface: Surface,
    _requested: Surface,
    _snapshot: Option<&crate::generated::ExploreImageMetadata>,
    _geometry: Option<(Rectangle, Option<(Rectangle, Rectangle)>)>,
    _draw_identity: u64,
) {
}

fn trace_draw(
    event: &str,
    control: &str,
    draw: &PreparedDraw,
    image: Rectangle,
    clip: Rectangle,
    draw_identity: u64,
) {
    if !surface_trace_enabled() {
        return;
    }
    trace_image(
        event,
        control,
        draw.surface,
        draw.requested,
        draw.gallery.as_ref().map(|value| value.metadata.as_ref()),
        Some((draw.bounds, Some((image, clip)))),
        draw_identity,
    );
}

#[cfg(target_arch = "wasm32")]
pub(super) fn trace_gallery_source(snapshot: &crate::generated::ExploreImageMetadata) {
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
pub(super) fn trace_gallery_source(_snapshot: &crate::generated::ExploreImageMetadata) {}

pub(crate) fn trace_atlas_stage(stage: &str, draw: &crate::integration_control::AtlasDraw) {
    if !surface_trace_enabled() {
        return;
    }
    trace_image(
        "scroll_stage",
        stage,
        draw.surface,
        draw.surface,
        Some(&draw.snapshot),
        Some((draw.bounds, Some((draw.image, draw.clip)))),
        0,
    );
}

#[cfg(not(target_arch = "wasm32"))]
pub(crate) fn trace_surface(_event: &str, _surface: Surface) {}

#[cfg(not(target_arch = "wasm32"))]
fn trace_surface_request(_event: &str, _surface: Surface, _requested: Surface, _control: &str) {}

const MAILBOX_SLOTS: u32 = 2;
// Firefox admits active, candidate, and retiring arenas, each with two slots.
const ARENA_CAPACITY: usize = 3;
pub(super) const SAMPLE_CAPACITY: usize = ARENA_CAPACITY * MAILBOX_SLOTS as usize;
thread_local! {
    static DRAWN_DETAIL: std::cell::Cell<Option<(Surface, [u32; 4])>> = const { std::cell::Cell::new(None) };
    static RELEASED_FRAMES: std::cell::Cell<[Option<FrameReady>; SAMPLE_CAPACITY]> = const { std::cell::Cell::new([None; SAMPLE_CAPACITY]) };
    static BORROWS: std::cell::Cell<[Option<FrameReady>; SAMPLE_CAPACITY]> = const { std::cell::Cell::new([None; SAMPLE_CAPACITY]) };
}

// Publication metadata is copyable; permission to read an external layer is not.
pub(crate) fn accept_publication(frame: FrameReady) -> bool {
    if publication_admission_blocked(frame) {
        return false;
    }
    if RELEASED_FRAMES.with(|released| {
        released.get().iter().flatten().any(|prior| {
            same_mailbox_slot(*prior, frame)
                && prior.presentation_revision >= frame.presentation_revision
        })
    }) {
        return false;
    }
    BORROWS.with(|borrows| {
        let mut slots = borrows.get();
        if slots
            .iter()
            .flatten()
            .any(|prior| same_mailbox_slot(*prior, frame))
        {
            return false;
        }
        let Some(slot) = slots.iter_mut().find(|slot| slot.is_none()) else {
            return false;
        };
        *slot = Some(frame);
        borrows.set(slots);
        true
    })
}

// Read custody, including submitted draws, outlives page publication retirement.
// Recovery waits for that custody to settle instead of repeatedly selecting.
pub(crate) fn publication_admission_blocked(frame: FrameReady) -> bool {
    mailbox_binding(frame).is_none()
        || LIVE_READS.with(|reads| {
            reads
                .borrow()
                .iter()
                .flatten()
                .filter_map(std::sync::Weak::upgrade)
                .any(|read| same_mailbox_slot(read.0, frame))
        })
        || BORROWS.with(|borrows| borrows.get().iter().all(Option::is_some))
}

fn take_publication(frame: FrameReady) -> bool {
    BORROWS.with(|borrows| {
        let mut slots = borrows.get();
        let Some(slot) = slots.iter_mut().find(|slot| **slot == Some(frame)) else {
            return false;
        };
        *slot = None;
        borrows.set(slots);
        true
    })
}

pub(crate) fn retire_publication(frame: FrameReady) {
    // Acquired publications belong to their shared display/read owner.
    if take_publication(frame) {
        release(frame);
    }
}

// Only exact copyable facts cross the unconditional Send callback boundary.
// WebGPU command buffers retain their texture handles; this token retains the
// browser slot permission independently from page-local wrappers.
pub(super) struct SampleRead(FrameReady);

thread_local! {
    static LIVE_READS: std::cell::RefCell<[Option<std::sync::Weak<SampleRead>>; SAMPLE_CAPACITY]> = const { std::cell::RefCell::new([const { None }; SAMPLE_CAPACITY]) };
    static COPY_RECEIPTS: std::cell::Cell<[Option<FrameReady>; SAMPLE_CAPACITY]> = const { std::cell::Cell::new([None; SAMPLE_CAPACITY]) };
}

thread_local! {
    static CAPACITY_ACCEPTANCE: std::cell::RefCell<Option<[Option<std::sync::Arc<SampleRead>>; 2]>> =
        const { std::cell::RefCell::new(None) };
}

fn retain_capacity_sample(read: &std::sync::Arc<SampleRead>) {
    CAPACITY_ACCEPTANCE.with(|owner| {
        let mut owner = owner.borrow_mut();
        let Some(slots) = owner.as_mut() else {
            return;
        };
        if slots.iter().flatten().any(|prior| prior.0 == read.0) {
            return;
        }
        if let Some(slot) = slots.iter_mut().find(|slot| slot.is_none()) {
            *slot = Some(read.clone());
        }
    });
}

pub(crate) fn begin_capacity_acceptance() -> bool {
    if CAPACITY_ACCEPTANCE.with(|owner| owner.borrow().is_some()) {
        return false;
    }
    let read = RENDERER.with(|renderer| {
        renderer
            .borrow()
            .as_ref()
            .and_then(|renderer| renderer.imported.as_ref())
            .and_then(|imported| imported.image.retained_read.clone())
    });
    let Some(read) = read else {
        return false;
    };
    CAPACITY_ACCEPTANCE.with(|owner| *owner.borrow_mut() = Some([Some(read), None]));
    true
}

pub(crate) fn capacity_acceptance_slots() -> usize {
    CAPACITY_ACCEPTANCE.with(|owner| {
        let owner = owner.borrow();
        let Some(slots) = owner.as_ref() else {
            return 0;
        };
        match (&slots[0], &slots[1]) {
            (Some(first), Some(second))
                if first.0.high == second.0.high
                    && first.0.low == second.0.low
                    && first.0.slot != second.0.slot =>
            {
                2
            }
            (Some(_), None) => 1,
            _ => 0,
        }
    })
}

pub(crate) fn release_capacity_sample() -> bool {
    // Take before dropping: release dispatch may re-enter page-local owners.
    let read = CAPACITY_ACCEPTANCE.with(|owner| {
        owner
            .borrow_mut()
            .as_mut()
            .and_then(|slots| slots[0].take())
    });
    read.is_some()
}

pub(crate) fn end_capacity_acceptance() {
    let retired = CAPACITY_ACCEPTANCE.with(|owner| owner.borrow_mut().take());
    drop(retired);
}

pub(super) fn copy_completed(frame: FrameReady) -> bool {
    frame.direct_sampling || COPY_RECEIPTS.with(|receipts| receipts.get().contains(&Some(frame)))
}

impl SampleRead {
    pub(super) fn acquire(frame: FrameReady) -> Option<std::sync::Arc<Self>> {
        if !take_publication(frame) || !reserve_release(frame) {
            return None;
        }
        let read = std::sync::Arc::new(Self(frame));
        LIVE_READS.with(|reads| {
            let mut reads = reads.borrow_mut();
            let slot = reads
                .iter_mut()
                .find(|slot| slot.as_ref().is_none_or(|read| read.strong_count() == 0))
                .expect("bounded active, candidate and retiring arena slots");
            *slot = Some(std::sync::Arc::downgrade(&read));
        });
        retain_capacity_sample(&read);
        Some(read)
    }
}

fn settle_sample(frame: FrameReady) {
    metadata::retire(frame);
    RELEASED_FRAMES.with(|released| {
        let mut slots = released.get();
        remember_frame(&mut slots, frame);
        released.set(slots);
    });
    COPY_RECEIPTS.with(|receipts| {
        let mut slots = receipts.get();
        for slot in &mut slots {
            if *slot == Some(frame) {
                *slot = None;
            }
        }
        receipts.set(slots);
    });
    dispatch_release(frame);
    drain_retired_textures();
}

impl Drop for SampleRead {
    fn drop(&mut self) {
        settle_sample(self.0);
        notify_surface(Notification::Drawn);
    }
}

// Texture wrappers and their destruction stay on the owning page thread. The
// Send completion payload remains SampleRead's copyable publication facts.
thread_local! {
    static RETIRED_TEXTURES: std::cell::RefCell<[Option<RetiredTexture>; SAMPLE_CAPACITY]> =
        const { std::cell::RefCell::new([const { None }; SAMPLE_CAPACITY]) };
}

struct RetiredTexture {
    surface: Surface,
    texture: wgpu::Texture,
    _lifetime: Option<std::sync::Arc<ImportedTextureLifetime>>,
}

struct ImportedTextureLifetime(Surface);

impl Drop for ImportedTextureLifetime {
    fn drop(&mut self) {
        trace_surface("texture_destroyed", self.0);
    }
}

fn trace_source_texture(event: &str, surface: Surface) {
    if surface.frame.is_some_and(|frame| frame.direct_sampling) {
        trace_surface(event, surface);
    }
}

fn arena_has_readers(surface: Surface) -> bool {
    LIVE_READS.with(|reads| {
        reads
            .borrow()
            .iter()
            .flatten()
            .filter_map(std::sync::Weak::upgrade)
            .any(|read| {
                read.0.high == surface.high
                    && read.0.low == surface.low
                    && surface.frame.is_none_or(|frame| {
                        !frame.direct_sampling
                            || (read.0.source_high == frame.source_high
                                && read.0.source_low == frame.source_low)
                    })
            })
    })
}

fn drain_retired_textures() {
    // Remove owners before calling the browser, so destruction cannot re-enter
    // a borrowed retirement list. The fixed local staging allocates nothing.
    let ready = RETIRED_TEXTURES.with(|retired| {
        let mut retired = retired.borrow_mut();
        std::array::from_fn::<_, SAMPLE_CAPACITY, _>(|index| {
            if retired[index]
                .as_ref()
                .is_some_and(|entry| !arena_has_readers(entry.surface))
            {
                retired[index].take()
            } else {
                None
            }
        })
    });
    for retired in ready.into_iter().flatten() {
        retired.texture.destroy();
        trace_source_texture("source_texture_destroyed", retired.surface);
    }
}

// The final field of Imported: its Drop runs after display, view, and probe
// owners are gone. This guard supplies the explicit WebGPU destruction that
// dropping a JS-backed wgpu::Texture handle alone does not perform.
struct ArenaTexture {
    surface: Surface,
    texture: Option<wgpu::Texture>,
    lifetime: Option<std::sync::Arc<ImportedTextureLifetime>>,
}

impl Drop for ArenaTexture {
    fn drop(&mut self) {
        let Some(texture) = self.texture.take() else {
            return;
        };
        if !arena_has_readers(self.surface) {
            texture.destroy();
            trace_source_texture("source_texture_destroyed", self.surface);
            return;
        }
        RETIRED_TEXTURES.with(|retired| {
            let mut retired = retired.borrow_mut();
            let slot = retired
                .iter_mut()
                .find(|slot| slot.is_none())
                .expect("Firefox admits at most six live physical sample slots");
            *slot = Some(RetiredTexture {
                surface: self.surface,
                texture,
                _lifetime: self.lifetime.clone(),
            });
        });
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

pub(crate) fn viewer_annotation_request() -> Option<crate::generated::AnnotationOpen> {
    let (surface, crop) = drawn_detail()?;
    let frame = surface.frame?;
    let paired = metadata::pending(frame)?;
    if let Some(validation) = paired.content.validation() {
        let source = validation.frame();
        if validation.metadata.detail
            && paired.view_ready
            && paired.surface.frame == Some(frame)
            && same_allocation(surface, paired.surface)
            && frame.belongs_to(surface)
            && metadata::product(frame).as_ref() == Some(source)
            && frame.matches_content(source)
            && crop == surface.content_region()
            && metadata::valid_content(source)
            && surface.viewer_identity == paired.surface.viewer_identity
        {
            return Some(surface.annotation_request(source));
        }
        return None;
    }
    let detail = paired.content.detail()?;
    let source = detail.frame();
    if !paired.view_ready
        || paired.surface.frame != Some(frame)
        || !same_allocation(surface, paired.surface)
        || !frame.belongs_to(surface)
        || source.revision == 0
        || !frame.matches_content(source)
        || metadata::product(frame).as_ref() != Some(source)
        || detail.explore.mode != crate::generated::ExploreMode::Detail
        || detail.viewer_identity().is_none()
        || surface.viewer_identity
            != detail
                .viewer_identity()
                .map(|(dataset, image)| (dataset, u64::from(image)))
    {
        return None;
    }
    if !metadata::valid_content(source) || crop != surface.content_region() {
        return None;
    }
    Some(surface.annotation_request(source))
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
const COPY_EVENT: &str = "gpuexternaltexturecopycomplete";
#[cfg(target_arch = "wasm32")]
const IMPORT_EVENT: &str = "gpuexternaltextureimportready";
#[cfg(target_arch = "wasm32")]
const SETTLED_EVENT: &str = "gpuexternaltexturereadsettled";
#[cfg(target_arch = "wasm32")]
const SURFACE_EVENTS: [&str; 4] = [FRAME_EVENT, COPY_EVENT, IMPORT_EVENT, SETTLED_EVENT];
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
    pub source_high: u64,
    pub source_low: u64,
    pub direct_sampling: bool,
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
            source_high: 0,
            source_low: 0,
            direct_sampling: false,
        };
        let source = fields.next()?;
        if source.len() != 32 || !source.is_ascii() {
            return None;
        }
        let mut ready = ready;
        ready.source_high = u64::from_str_radix(&source[..16], 16).ok()?;
        ready.source_low = u64::from_str_radix(&source[16..], 16).ok()?;
        ready.direct_sampling = match fields.next()? {
            "0" => false,
            "1" => true,
            _ => return None,
        };
        (fields.next().is_none()
            && ready.layer == 0
            && (ready.content_session != 0 || ready.content_sequence != 0)
            && ready.presentation_revision != 0
            && ((ready.slot < MAILBOX_SLOTS
                && ready.content_width != 0
                && ready.content_height != 0)
                || (ready.slot == u32::MAX
                    && ready.content_width == 0
                    && ready.content_height == 0
                    && ready.source_high == 0
                    && ready.source_low == 0
                    && !ready.direct_sampling)))
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
    Copied(FrameReady),
    SampleRejected(FrameReady),
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
                Notification::SampleRejected(frame) => {
                    remember_rejection(&mut mailbox.rejected_sample, frame);
                    trace_frame("sample_rejection_enqueued", frame);
                }
                Notification::Drawn => mailbox.drawn = true,
                Notification::Native(_) | Notification::Copied(_) => {
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
    // A malformed later offer cannot supersede failure of an accepted sample.
    // These facts have independent producers and must both reach the controller.
    rejected_sample: Option<FrameReady>,
    rejected_publication: Option<FrameReady>,
    drawn: bool,
    frames: [Option<FrameReady>; 1],
    copied: [Option<FrameReady>; SAMPLE_CAPACITY],
    next_layer: usize,
}

fn remember_frame(slots: &mut [Option<FrameReady>; SAMPLE_CAPACITY], frame: FrameReady) {
    if let Some(slot) = slots
        .iter_mut()
        .find(|slot| slot.is_some_and(|prior| same_mailbox_slot(prior, frame)))
    {
        if slot.is_none_or(|prior| prior.presentation_revision <= frame.presentation_revision) {
            *slot = Some(frame);
        }
        return;
    }
    let index = slots.iter().position(Option::is_none).unwrap_or_else(|| {
        slots
            .iter()
            .enumerate()
            .min_by_key(|(_, slot)| slot.map(|frame| frame.presentation_revision))
            .expect("bounded receipt storage")
            .0
    });
    // Independent arena notifications can arrive out of presentation order.
    slots[index] = Some(frame);
}

fn remember_rejection(slot: &mut Option<FrameReady>, frame: FrameReady) {
    if slot.is_none_or(|prior| prior.presentation_revision < frame.presentation_revision) {
        *slot = Some(frame);
    }
}

impl FrameMailbox {
    fn complete(&mut self, frame: FrameReady) {
        remember_frame(&mut self.copied, frame);
    }
    fn next_notification(&mut self) -> Option<Notification> {
        self.copied
            .iter_mut()
            .find_map(Option::take)
            .map(Notification::Copied)
            .or_else(|| self.pop().map(Notification::Native))
            .or_else(|| {
                self.rejected_sample
                    .take()
                    .map(Notification::SampleRejected)
            })
            .or_else(|| {
                self.rejected_publication
                    .take()
                    .map(Notification::SampleRejected)
            })
            .or_else(|| std::mem::take(&mut self.drawn).then_some(Notification::Drawn))
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
            if event.type_() == IMPORT_EVENT {
                if crate::integration_control::reporting_enabled()
                    && let Some(identity) = event.detail().as_string()
                    && let Some(requested) = parse_arena_identity(&identity)
                {
                    RENDERER.with(|renderer| {
                        if let Some(owner) = renderer.borrow_mut().as_mut() {
                            owner.reconstruct_pending_handoff(requested);
                        }
                    });
                }
                pending.borrow_mut().drawn = true;
                notify.wake();
                return;
            }
            let detail = event.detail();
            let Some(ready) = js_sys::Reflect::get(&detail, &"identity".into())
                .ok()
                .and_then(|identity| {
                    identity
                        .as_string()
                        .and_then(|value| FrameReady::parse(&value))
                })
            else {
                return;
            };
            if ready.slot >= MAILBOX_SLOTS {
                return;
            }
            if event.type_() == SETTLED_EVENT {
                if !ready.direct_sampling {
                    return;
                }
                trace_frame("read_settlement_received", ready);
                pending.borrow_mut().drawn = true;
                notify.wake();
                return;
            }
            if event.type_() == COPY_EVENT {
                pending.borrow_mut().complete(ready);
                notify.wake();
                return;
            }
            // A repeated notification does not own a second physical read.
            // Keep the accepted immutable receipt and its existing custody.
            if metadata::surface(ready).is_some() {
                return;
            }
            let integer = |name: &str| {
                js_sys::Reflect::get(&detail, &name.into())
                    .ok()
                    .and_then(|value| value.as_f64())
                    .filter(|value| {
                        value.is_finite()
                            && value.fract() == 0.0
                            && *value > 0.0
                            && *value <= u32::MAX as f64
                    })
                    .map(|value| value as u32)
            };
            let metadata = js_sys::Reflect::get(&detail, &"metadata".into())
                .ok()
                .and_then(|value| value.dyn_into::<js_sys::Uint8Array>().ok());
            let transfer = js_sys::Reflect::get(&detail, &"transfer".into())
                .ok()
                .and_then(|value| value.as_string())
                .and_then(|value| u64::from_str_radix(&value, 16).ok());
            let decoded = integer("capacityWidth")
                .zip(integer("capacityHeight"))
                .zip(transfer)
                .zip(metadata)
                .is_some_and(|(((width, height), transfer), bytes)| {
                    bytes.length() as usize <= crate::generated::WORKSPACE_METADATA_BYTE_CAPACITY
                        && metadata::install(ready, width, height, transfer, &bytes.to_vec())
                            .is_ok()
                });
            if !decoded {
                release(ready);
                // Preserve any valid queued offer while reporting the lost handoff.
                remember_rejection(&mut pending.borrow_mut().rejected_publication, ready);
                notify.wake();
                return;
            }
            invalidate_drawn_slot(ready);
            let displaced = pending.borrow_mut().push(ready);
            if let Some(displaced) = displaced {
                release(displaced);
            }
            notify.wake();
        }) as Box<dyn FnMut(web_sys::Event)>);
        for (index, name) in SURFACE_EVENTS.iter().enumerate() {
            if window
                .add_event_listener_with_callback(name, callback.as_ref().unchecked_ref())
                .is_err()
            {
                for prior in &SURFACE_EVENTS[..index] {
                    let _ = window.remove_event_listener_with_callback(
                        prior,
                        callback.as_ref().unchecked_ref(),
                    );
                }
                return None;
            }
        }
        Some(FrameListener {
            window,
            callback,
            stop_binding: bind_workspace(),
        })
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
    stop_binding: js_sys::Function,
}

#[cfg(target_arch = "wasm32")]
#[wasm_bindgen::prelude::wasm_bindgen(module = "/src/presentation_surface/graphics.mjs")]
extern "C" {
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = bindWorkspace)]
    fn bind_workspace() -> js_sys::Function;
}

#[cfg(target_arch = "wasm32")]
impl Drop for FrameListener {
    fn drop(&mut self) {
        let _ = self.stop_binding.call0(&wasm_bindgen::JsValue::UNDEFINED);
        COMPLETION_WAKE.with(|notify| *notify.borrow_mut() = None);
        for name in SURFACE_EVENTS {
            let _ = self
                .window
                .remove_event_listener_with_callback(name, self.callback.as_ref().unchecked_ref());
        }
    }
}

#[cfg(all(test, not(target_arch = "wasm32")))]
pub fn subscription() -> iced::Subscription<Notification> {
    iced::Subscription::none()
}

pub(crate) struct Primitive {
    pub(super) submission: Option<std::sync::Arc<dyn Fn() + Send + Sync>>,
    pub(super) surface: Surface,
    pub(super) transform: ViewTransform,
    pub(super) placement: Placement,
    pub(super) control_id: &'static str,
}

impl std::fmt::Debug for Primitive {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("WorkspacePrimitive")
            .field("surface", &self.surface)
            .finish_non_exhaustive()
    }
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
            );
            renderer.prepare_draw(device, queue, self.control_id, self.placement, transform);
        });
    }

    fn render(
        &self,
        pipeline: &Pipeline,
        encoder: &mut wgpu::CommandEncoder,
        target: &wgpu::TextureView,
        clip_bounds: &Rectangle<u32>,
        resources: &mut shader::Resources,
    ) {
        let _ = pipeline;
        RENDERER.with(|renderer| {
            let renderer = renderer.borrow();
            let Some(renderer) = renderer.as_ref() else {
                return;
            };
            renderer.render(
                encoder,
                target,
                *clip_bounds,
                self.control_id,
                self.surface,
                resources,
                self.submission.as_ref(),
            );
        });
    }
}

// Iced pipeline installation establishes this component's device owner.
// Shader reconstruction preserves its single-claim imports and completed samples;
// explicit application retirement can clear it while the wrapper stays installed.
thread_local! {
    static RENDERER: std::cell::RefCell<Option<SurfaceRenderer>> = const { std::cell::RefCell::new(None) };
    // One matching domain/control frontier; independent of GPU completion and
    // external sample custody. It can authorize an already-submitted queue read.
    static DRAW_AUTHORIZATION: std::cell::RefCell<DrawAuthorization> = const {
        std::cell::RefCell::new(DrawAuthorization { draw: None })
    };
}

struct DrawAuthorization {
    draw: Option<FrameReady>,
}

pub(crate) fn authorize_draw(frame: Option<FrameReady>) {
    DRAW_AUTHORIZATION.with(|authorization| authorization.borrow_mut().draw = frame);
}

pub(crate) fn reset_reconstruction_probe() {
    RENDERER.with(|renderer| {
        if let Some(renderer) = renderer.borrow_mut().as_mut() {
            renderer.reconstruction_admissions = None;
        }
    });
}

pub(crate) fn retire_samples() {
    authorize_draw(None);
    let samples = RENDERER.with(|renderer| {
        let mut renderer = renderer.borrow_mut();
        renderer.as_mut().map(|renderer| {
            // Prepared bindings remain reusable, but render cannot select them
            // without a completed or submitted sample in either image owner.
            [
                renderer
                    .imported
                    .as_mut()
                    .map(|imported| imported.image.retire()),
                renderer
                    .pending
                    .as_mut()
                    .map(|imported| imported.image.retire()),
            ]
        })
    });
    clear_drawn_detail();
    // Last-reader settlement can reenter page ownership. All RefCell borrows
    // must end before any sample lease is dropped.
    drop(samples);
}

pub(crate) fn retained_surface() -> Option<Surface> {
    RENDERER.with(|renderer| {
        let renderer = renderer.borrow();
        let image = &renderer.as_ref()?.imported.as_ref()?.image;
        image.retained()
    })
}

#[derive(Clone)]
pub(crate) enum ExploreDisplay {
    Gallery(Surface, std::sync::Arc<super::labels::GalleryContent>),
    Detail(Surface, DetailContent),
}

impl ExploreDisplay {
    fn paired(
        surface: Surface,
        gallery: Option<&std::sync::Arc<super::labels::GalleryContent>>,
        detail: Option<&DetailContent>,
    ) -> Option<Self> {
        if let Some(gallery) = gallery {
            return Some(Self::Gallery(surface, gallery.clone()));
        }
        detail.map(|detail| Self::Detail(surface, detail.clone()))
    }
}

fn has_live_read(frame: FrameReady) -> bool {
    LIVE_READS.with(|reads| {
        reads
            .borrow()
            .iter()
            .flatten()
            .filter_map(std::sync::Weak::upgrade)
            .any(|read| read.0 == frame)
    })
}

fn can_display_pending(frame: FrameReady, has_content: impl Fn(&ImagePublication) -> bool) -> bool {
    let live = has_live_read(frame);
    let incumbent = RENDERER.with(|renderer| {
        renderer
            .borrow()
            .as_ref()
            .and_then(|renderer| renderer.imported.as_ref())
            .is_some_and(|imported| {
                imported.image.retained().is_some() && has_content(&imported.image)
            })
    });
    // An accepted initial offer constructs the first shader owner. Replacements
    // keep the incumbent until their physical read exists.
    live || (!incumbent && BORROWS.with(|borrows| borrows.get().contains(&Some(frame))))
}

pub(crate) fn explore_display(requested: Option<Surface>) -> Option<ExploreDisplay> {
    if let Some(surface) = requested
        && let Some(frame) = surface.frame
    {
        let accepted = can_display_pending(frame, |image| {
            image.content.gallery().is_some() || image.content.detail().is_some()
        });
        if accepted
            && let Some(paired) = metadata::pending(frame)
            && paired.view_ready
            && same_allocation(surface, paired.surface)
        {
            return ExploreDisplay::paired(
                surface,
                paired.content.gallery(),
                paired.content.detail().as_ref(),
            );
        }
    }
    RENDERER.with(|renderer| {
        let renderer = renderer.borrow();
        let renderer = renderer.as_ref()?;
        for imported in renderer.pending.iter().chain(renderer.imported.iter()) {
            if let Some(pending) = imported.image.pending_sample.as_ref()
                && imported.image.submitted_draw(pending.surface).is_some()
            {
                return ExploreDisplay::paired(
                    pending.surface,
                    pending.content.gallery(),
                    pending.content.detail().as_ref(),
                );
            }
        }
        let image = &renderer.imported.as_ref()?.image;
        image.explore_display(image.retained()?)
    })
}

pub(crate) fn drawable_annotation(requested: Surface) -> Option<(Surface, AnnotationContent)> {
    RENDERER.with(|renderer| {
        let renderer = renderer.borrow();
        let renderer = renderer.as_ref()?;
        SurfaceRenderer::submitted_draws(&renderer.pending, &renderer.imported, requested)
            .find_map(|(_, pending)| Some((requested, pending.content.annotation()?)))
            .or_else(|| {
                let image = &renderer.imported.as_ref()?.image;
                Some((image.retained()?, image.content.annotation()?))
            })
    })
}

fn drawable_content<T>(
    requested: Surface,
    kind: crate::generated::PresentationSourceKind,
    pending_content: impl Fn(&PendingImage) -> Option<std::sync::Arc<T>>,
    retained_content: impl Fn(&ImagePublication) -> Option<std::sync::Arc<T>>,
) -> Option<(Surface, std::sync::Arc<T>)> {
    let frame = requested.frame?;
    if frame.content_session != crate::generated::presentation_source_session(kind) {
        return None;
    }
    if can_display_pending(frame, |image| retained_content(image).is_some())
        && let Some(pending) = metadata::pending(frame)
        && pending.view_ready
        && same_allocation(requested, pending.surface)
        && let Some(content) = pending_content(&pending)
    {
        return Some((requested, content));
    }
    RENDERER.with(|renderer| {
        let renderer = renderer.borrow();
        let renderer = renderer.as_ref()?;
        SurfaceRenderer::submitted_draws(&renderer.pending, &renderer.imported, requested)
            .find_map(|(_, pending)| Some((pending.surface, pending_content(pending)?)))
            .or_else(|| {
                let image = &renderer.imported.as_ref()?.image;
                Some((image.retained()?, retained_content(image)?))
            })
    })
}
pub(crate) fn drawable_prediction(
    requested: Surface,
) -> Option<(Surface, std::sync::Arc<labels::PredictionContent>)> {
    drawable_content(
        requested,
        crate::generated::PresentationSourceKind::Predict,
        |pending| pending.content.prediction(),
        |image| image.content.prediction(),
    )
}
pub(crate) fn drawable_validation(
    requested: Surface,
) -> Option<(Surface, std::sync::Arc<labels::ValidationContent>)> {
    drawable_content(
        requested,
        if requested.frame.is_some_and(|frame| {
            frame.content_session
                == crate::generated::presentation_source_session(
                    crate::generated::PresentationSourceKind::Upscale,
                )
        }) {
            crate::generated::PresentationSourceKind::Upscale
        } else {
            crate::generated::PresentationSourceKind::Validation
        },
        |pending| pending.content.validation(),
        |image| image.content.validation(),
    )
}

pub(crate) struct Pipeline {
    format: wgpu::TextureFormat,
}

struct SurfaceRenderer {
    device: wgpu::Device,
    queue: wgpu::Queue,
    format: wgpu::TextureFormat,
    render: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    imported: Option<Imported>,
    pending: Option<Imported>,
    bounds: Rectangle,
    scale_factor: f32,
    requested: Option<Surface>,
    reconstruction_admissions: Option<std::collections::VecDeque<(u64, u64)>>,
    draws: std::collections::HashMap<&'static str, PreparedDraw>,
}

struct Imported {
    image: ImagePublication,
    views: [wgpu::TextureView; 2],
    diagnostics: Option<DrawDiagnostics>,
    pixel_trace: [Option<std::sync::Arc<pixel_trace::PixelTrace>>; 2],
    _arena: [Option<std::sync::Arc<ArenaTexture>>; 2],
}

#[derive(Default)]
struct DrawDiagnostics {
    drawn_revision: AtomicU64,
    draw_count: AtomicU64,
}

// CPU facts travel with their leased browser pixels, including across an
// advertised replacement and while a newer sample awaits native metadata.
pub(super) struct ImagePublication {
    pub(super) surface: Surface,
    pub(super) pending_sample: Option<PendingImage>,
    pub(super) completed: Option<FrameReady>,
    pub(super) retained_read: Option<std::sync::Arc<SampleRead>>,
    pub(super) content: metadata::Content,
    pub(super) placement: Placement,
}

#[derive(Clone)]
pub(crate) struct AnnotationContent {
    pub(crate) metadata: std::sync::Arc<crate::generated::AnnotationImageMetadata>,
}

#[derive(Clone)]
pub(crate) struct DetailContent {
    pub(super) labels: std::sync::Arc<super::labels::CategoryCaptions>,
    pub(super) explore: std::sync::Arc<crate::generated::ExploreImageMetadata>,
    pub(super) upscale: Option<std::sync::Arc<crate::generated::UpscaleImageMetadata>>,
}

impl DetailContent {
    pub(crate) fn new(
        explore: std::sync::Arc<crate::generated::ExploreImageMetadata>,
        upscale: Option<std::sync::Arc<crate::generated::UpscaleImageMetadata>>,
    ) -> Self {
        let scene = upscale
            .as_ref()
            .map_or(&explore.scene, |value| &value.scene);
        let labels = std::sync::Arc::new(super::labels::CategoryCaptions::new(
            &scene.categories,
            scene.objects.iter().map(|object| object.category),
        ));
        Self {
            explore,
            upscale,
            labels,
        }
    }

    pub(crate) fn configure_surface(
        &self,
        mut surface: Surface,
        original: bool,
        fit_revision: u64,
    ) -> Surface {
        surface.viewer_identity = self
            .viewer_identity()
            .map(|(dataset, image)| (dataset, u64::from(image)));
        surface.fit_revision = fit_revision;
        surface.configure_original(self.frame(), self.input_frame(), original);
        surface
    }

    pub(crate) fn viewer_identity(&self) -> Option<(u64, u32)> {
        self.explore
            .selectedimage
            .map(|image| (self.explore.dataset.identity, image))
    }

    pub(crate) fn input_frame(&self) -> &crate::generated::VisualFrame {
        &self.explore.frame
    }

    pub(crate) fn original_dimensions(&self) -> bool {
        self.explore.detail.showoriginaldimensions
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
}

#[derive(Clone)]
pub(super) struct PendingImage {
    pub(super) read: Option<std::sync::Arc<SampleRead>>,
    pub(super) surface: Surface,
    pub(super) content: metadata::Content,
    pub(super) placement: Placement,
    pub(super) complete: bool,
    pub(super) view_ready: bool,
}

struct PreparedDraw {
    surface: Surface,
    requested: Surface,
    bounds: Rectangle,
    geometry: PlacementGeometry,
    placement: Placement,
    gallery: Option<std::sync::Arc<super::labels::GalleryContent>>,
    uniform: wgpu::Buffer,
    bindings: [wgpu::BindGroup; 2],
    key: GeometryKey,
}

impl Drop for SurfaceRenderer {
    fn drop(&mut self) {
        // Bindings are page-local caches, not encoded readers. Drop them before
        // imported fields begin their last-reader texture retirement.
        self.draws.clear();
    }
}

impl Drop for Imported {
    fn drop(&mut self) {
        trace_surface("import_dropped", self.image.surface);
        // Field destruction drops display/view/probe owners before _arena
        // transfers the texture to exact last-reader retirement.
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
        let render = Self::create_pipeline(device, format, &layout);
        Self {
            device: device.clone(),
            queue: queue.clone(),
            format,
            render,
            layout,
            sampler,
            imported: None,
            pending: None,
            bounds: Rectangle::default(),
            scale_factor: 1.0,
            requested: None,
            reconstruction_admissions: None,
            draws: std::collections::HashMap::new(),
        }
    }

    fn create_pipeline(
        device: &wgpu::Device,
        format: wgpu::TextureFormat,
        layout: &wgpu::BindGroupLayout,
    ) -> wgpu::RenderPipeline {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("mmltk presentation shader"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(SHADER)),
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("mmltk presentation pipeline layout"),
            bind_group_layouts: &[Some(layout)],
            immediate_size: 0,
        });
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
                entry_point: Some("fs_main"),
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
    }

    fn replace_pipelines(&mut self, device: &wgpu::Device, format: wgpu::TextureFormat) {
        self.render = Self::create_pipeline(device, format, &self.layout);
        self.format = format;
    }

    fn reconstruct_pending_handoff(&mut self, requested: (u64, u64)) {
        if !crate::integration_control::reporting_enabled() {
            return;
        }
        let admissions = self
            .reconstruction_admissions
            .get_or_insert_with(|| std::collections::VecDeque::with_capacity(4));
        if admissions.contains(&requested) {
            return;
        }
        // Active, candidate, and the two retiring arenas bound live FD
        // identities. Repeated readiness/reuse edges are not new handoffs.
        if admissions.len() == 4 {
            admissions.pop_front();
        }
        admissions.push_back(requested);
        let Some(imported) = self.imported.as_ref() else {
            return;
        };
        let completed = imported.image.surface;
        let probe = ((completed.high, completed.low), requested);
        if imported.image.completed.is_none() || probe.0 == probe.1 {
            return;
        }
        // The trusted FD arena notification precedes any image metadata.
        // Rebuild only pipeline state; retained samples and already encoded
        // draws keep their independent physical owners throughout the handoff.
        let device = self.device.clone();
        self.replace_pipelines(&device, self.format);
        let requested = Surface {
            high: requested.0,
            low: requested.1,
            ..Surface::empty()
        };
        trace_surface_request("renderer_reconstructed", completed, requested, "");
    }
}

#[cfg(any(target_arch = "wasm32", test))]
fn parse_arena_identity(identity: &str) -> Option<(u64, u64)> {
    if identity.len() != 32 || !identity.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return None;
    }
    let high = u64::from_str_radix(&identity[..16], 16).ok()?;
    let low = u64::from_str_radix(&identity[16..], 16).ok()?;
    (high != 0 || low != 0).then_some((high, low))
}

fn retained_draw_admitted(retained: Surface, requested: Surface, placement: Placement) -> bool {
    matches!(
        placement,
        Placement::GalleryGrid { .. } | Placement::FixedGrid { .. }
    ) || (retained.frame.is_some() && retained.frame == requested.frame)
        || retained.viewer_identity == requested.viewer_identity
}

impl SurfaceRenderer {
    fn submitted_draws<'a>(
        pending: &'a Option<Imported>,
        imported: &'a Option<Imported>,
        requested: Surface,
    ) -> impl Iterator<Item = (&'a Imported, &'a PendingImage)> {
        [pending, imported]
            .into_iter()
            .flatten()
            .filter_map(move |imported| {
                imported
                    .image
                    .submitted_draw(requested)
                    .map(|image| (imported, image))
            })
    }

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

    fn reconcile_sample(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        surface: Surface,
        placement: Placement,
    ) {
        if let Some(imported) =
            Self::matching_import(&mut self.imported, &mut self.pending, surface)
        {
            imported.ensure_source(device, queue, surface);
            imported.reconcile_sample(surface, placement);
            return;
        }
        self.prepare(
            device,
            queue,
            surface,
            self.bounds,
            self.scale_factor,
            placement,
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
        if !requested.valid() {
            self.draws.remove(control);
            return;
        }
        let submitted = Self::submitted_draws(&self.pending, &self.imported, requested).next();
        let Some(imported) = submitted.map(|(imported, _)| imported).or_else(|| {
            self.imported
                .as_ref()
                .filter(|value| value.image.retained().is_some())
        }) else {
            self.draws.remove(control);
            return;
        };
        let (surface, gallery) = submitted.map_or(
            (imported.image.surface, imported.image.content.gallery()),
            |(_, pending)| (pending.surface, pending.content.gallery()),
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
            bounds.y += gallery::row_offset(placement, &snapshot.metadata, bounds.width);
            placement = gallery::placement(&snapshot.metadata);
        }
        let Some(geometry) =
            placement_geometry(bounds, surface.display_extent(), placement, transform)
        else {
            trace_image(
                "sample_draw_rejected",
                control,
                surface,
                requested,
                gallery.map(|value| value.metadata.as_ref()),
                Some((bounds, None)),
                0,
            );
            self.draws.remove(control);
            return;
        };
        let key = geometry_key(surface, bounds, placement, transform);
        let draw = self.draws.entry(control).or_insert_with(|| {
            let uniform = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("mmltk widget image geometry"),
                size: 64,
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
                    &imported.views[index],
                )
            });
            PreparedDraw {
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
        if !same_allocation(draw.surface, surface)
            || surface.frame.is_some_and(|frame| frame.direct_sampling)
                && draw.surface.frame != surface.frame
        {
            draw.bindings = std::array::from_fn(|index| {
                bind_group(
                    device,
                    &self.layout,
                    &self.sampler,
                    &draw.uniform,
                    &imported.views[index],
                )
            });
        }
        if draw.key != key {
            write_content_geometry(queue, &draw.uniform, key);
            draw.key = key;
        }
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
            imported.ensure_source(device, queue, surface);
            imported.prepare(surface, placement);
            return;
        }
        let Some(frame) = surface.frame else {
            return;
        };
        trace_surface("texture_create", surface);
        let label = if frame.direct_sampling {
            format!(
                "mmltk-surface-v4/{:016x}{:016x}",
                frame.source_high, frame.source_low
            )
        } else {
            surface.label()
        };
        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some(&label),
            size: wgpu::Extent3d {
                width: surface.width,
                height: surface.height,
                depth_or_array_layers: if frame.direct_sampling { 1 } else { 2 },
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let arena = ArenaTexture {
            surface,
            texture: Some(texture),
            lifetime: surface_trace_enabled()
                .then(|| std::sync::Arc::new(ImportedTextureLifetime(surface))),
        };
        trace_source_texture("source_texture_create", surface);
        let texture = arena.texture.as_ref().expect("new page texture");
        let views = std::array::from_fn(|slot| {
            texture.create_view(&wgpu::TextureViewDescriptor {
                dimension: Some(wgpu::TextureViewDimension::D2),
                base_array_layer: if frame.direct_sampling {
                    0
                } else {
                    slot as u32
                },
                array_layer_count: Some(1),
                ..wgpu::TextureViewDescriptor::default()
            })
        });
        let mut pixel_trace = [None, None];
        let texture_index = if frame.direct_sampling {
            frame.slot as usize
        } else {
            0
        };
        pixel_trace[texture_index] =
            pixel_trace::PixelTrace::new(device, queue, texture, surface).map(std::sync::Arc::new);
        let mut textures = [None, None];
        textures[texture_index] = Some(std::sync::Arc::new(arena));
        let mut imported = Imported {
            image: ImagePublication {
                surface,
                pending_sample: None,
                completed: None,
                retained_read: None,
                content: metadata::Content::from_gallery(gallery::matching(surface.frame)),
                placement,
            },
            views,
            diagnostics: crate::integration_control::reporting_enabled()
                .then(DrawDiagnostics::default),
            pixel_trace,
            _arena: textures,
        };
        imported.sample();
        self.discard_pending();
        self.pending = Some(imported);
    }

    fn prune_draws(&mut self) {
        self.draws.retain(|_, draw| {
            [&self.imported, &self.pending]
                .into_iter()
                .flatten()
                .any(|imported| same_allocation(imported.image.surface, draw.surface))
        });
    }

    fn discard_pending(&mut self) {
        let pending = self.pending.take();
        self.prune_draws();
        if let Some(pending) = pending {
            trace_surface(
                if pending.image.pending_sample.is_some() {
                    "pending_sample_discarded"
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
        requested: Surface,
        resources: &mut shader::Resources,
        submission: Option<&std::sync::Arc<dyn Fn() + Send + Sync>>,
    ) {
        let Some(draw) = self.draws.get(control_id) else {
            if requested.valid() {
                trace_surface_request("sample_draw_missing", requested, requested, control_id);
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
            trace_draw("sample_draw_clipped", control_id, draw, image, clip, 0);
            return;
        };
        if visible.width <= 0.0 || visible.height <= 0.0 {
            trace_draw("sample_draw_clipped", control_id, draw, image, clip, 0);
            return;
        }
        let Some(imported) =
            [&self.imported, &self.pending]
                .into_iter()
                .flatten()
                .find(|imported| {
                    imported.image.completed == Some(frame)
                        || imported
                            .image
                            .submitted_draw(draw.requested)
                            .is_some_and(|pending| pending.surface.frame == Some(frame))
                })
        else {
            trace_draw("sample_draw_rejected", control_id, draw, image, clip, 0);
            return;
        };
        let read = if imported.image.completed == Some(frame) {
            imported.image.retained_read.as_ref()
        } else {
            imported
                .image
                .pending_sample
                .as_ref()
                .and_then(|pending| pending.read.as_ref())
        };
        let Some(read) = read.filter(|read| read.0 == frame) else {
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
        pass.set_bind_group(0, Some(&draw.bindings[frame.slot as usize]), &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        resources.retain(read.clone());
        if let Some(observer) = submission {
            resources.observe_submission(observer.clone());
        }
        let draw_identity = if surface_trace_enabled() {
            NEXT_DRAW_DIAGNOSTIC.fetch_add(1, Ordering::Relaxed)
        } else {
            0
        };
        #[cfg(target_arch = "wasm32")]
        if surface_trace_enabled() {
            let selected = draw.surface;
            let requested = draw.requested;
            // A fresh observer belongs to this primitive's exact encoder.
            // Unlike terminal callbacks, this runs synchronously after submit.
            resources.observe_submission(std::sync::Arc::new(move || {
                trace_draw_receipt(
                    "iced.surface.draw_submitted",
                    selected,
                    requested,
                    control_id,
                    draw_identity,
                );
            }));
            // The closure carries exact immutable publication facts only. The
            // generic batch owns submission/abandonment, not application state.
            resources.observe_settlement(move |outcome| {
                trace_draw_receipt(
                    match outcome {
                        shader::Settlement::Submitted => "iced.frame.draw_settled",
                        shader::Settlement::Abandoned => "iced.frame.draw_abandoned",
                    },
                    selected,
                    requested,
                    control_id,
                    draw_identity,
                )
            });
        }
        trace_surface_request(
            "sample_draw_selected",
            draw.surface,
            draw.requested,
            control_id,
        );
        trace_draw("draw_encoded", control_id, draw, image, clip, draw_identity);
        crate::integration_control::notify_driver_draw(
            control_id,
            frame.content_sequence,
            frame.presentation_revision,
        );
        if control_id == crate::view::explore::DETAIL_WORKSPACE_ID
            || control_id == "validate.detail.image"
        {
            record_drawn_detail(draw.surface, draw.surface.content_region());
        }
        if crate::integration_control::reporting_enabled() {
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
            if let Some(gallery) = &draw.gallery {
                crate::integration_control::report_atlas_draw(
                    crate::integration_control::AtlasDraw {
                        surface: draw.surface,
                        snapshot: gallery.metadata.clone(),
                        bounds,
                        image,
                        clip,
                    },
                    gallery::dark(),
                    self.scale_factor,
                );
            }
            let (redraw, draw_count) =
                imported
                    .diagnostics
                    .as_ref()
                    .map_or((false, 0), |diagnostics| {
                        let previous = diagnostics
                            .drawn_revision
                            .swap(frame.presentation_revision, Ordering::Relaxed);
                        (
                            previous == frame.presentation_revision,
                            diagnostics.draw_count.fetch_add(1, Ordering::Relaxed) + 1,
                        )
                    });
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

pub(crate) fn complete_sample(frame: FrameReady) {
    // A disposed publication's late copy receipt cannot resurrect its lease.
    let live = has_live_read(frame);
    let released = RELEASED_FRAMES.with(|receipts| {
        receipts.get().iter().flatten().any(|prior| {
            same_mailbox_slot(*prior, frame)
                && prior.presentation_revision >= frame.presentation_revision
        })
    });
    if released && !live {
        return;
    }
    COPY_RECEIPTS.with(|receipts| {
        let mut slots = receipts.get();
        remember_frame(&mut slots, frame);
        receipts.set(slots);
    });
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

pub(crate) fn discard_sample(frame: FrameReady) {
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
    authorize_draw(Some(frame));
    RENDERER.with(|renderer| {
        let mut renderer = renderer.borrow_mut();
        let Some(renderer) = renderer.as_mut() else {
            return;
        };
        // Acquire the submitted sample before Iced builds labels and layout.
        // Widget preparation remains the sole owner of view identity and geometry.
        let device = renderer.device.clone();
        let queue = renderer.queue.clone();
        let placement = gallery::matching(Some(frame))
            .as_ref()
            .map_or(Placement::Contain, |snapshot| {
                gallery::placement(&snapshot.metadata)
            });
        renderer.reconcile_sample(&device, &queue, surface, placement);
        for imported in [&mut renderer.imported, &mut renderer.pending]
            .into_iter()
            .flatten()
        {
            imported.image.reconcile_pending(frame, model);
        }
        if let Some(imported) = renderer.imported.as_mut()
            && imported.image.completed == Some(frame)
        {
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
                    "sample_replacement",
                    renderer
                        .pending
                        .as_ref()
                        .expect("completed pending image")
                        .image
                        .surface,
                );
            }
            let retired = renderer.imported.take();
            renderer.imported = renderer.pending.take();
            renderer.prune_draws();
            drop(retired);
        }
    });
}

impl ImagePublication {
    pub(super) fn retire(&mut self) -> (Option<PendingImage>, Option<std::sync::Arc<SampleRead>>) {
        self.surface.frame = None;
        self.completed = None;
        self.content = metadata::Content::default();
        (self.pending_sample.take(), self.retained_read.take())
    }

    pub(super) fn reconcile_pending(
        &mut self,
        frame: FrameReady,
        _model: &crate::view_model::ApplicationModel,
    ) {
        if let Some(pending) = self
            .pending_sample
            .as_mut()
            .filter(|pending| pending.surface.frame == Some(frame))
        {
            pending.complete |= copy_completed(frame);
        }
    }

    pub(super) fn submitted_draw(&self, requested: Surface) -> Option<&PendingImage> {
        self.pending_sample.as_ref().filter(|pending| {
            pending.view_ready
                && pending.surface.frame.is_some()
                && pending.surface.frame == requested.frame
                && same_allocation(pending.surface, requested)
                && DRAW_AUTHORIZATION
                    .with(|authorization| authorization.borrow().draw == pending.surface.frame)
        })
    }

    pub(super) fn explore_display(&self, requested: Surface) -> Option<ExploreDisplay> {
        if let Some(pending) = self.submitted_draw(requested) {
            return ExploreDisplay::paired(
                requested,
                pending.content.gallery(),
                pending.content.detail().as_ref(),
            );
        }
        let retained = self.retained()?;
        let surface = if retained.frame == requested.frame && same_allocation(retained, requested) {
            requested
        } else {
            retained
        };
        ExploreDisplay::paired(
            surface,
            self.content.gallery(),
            self.content.detail().as_ref(),
        )
    }

    pub(super) fn retained(&self) -> Option<Surface> {
        (self.completed.is_some() && self.completed == self.surface.frame).then_some(self.surface)
    }

    pub(super) fn complete(&mut self, frame: FrameReady) {
        if let Some(pending) = self.pending_sample.as_mut()
            && pending.surface.frame == Some(frame)
        {
            pending.complete = true;
        }
    }

    pub(super) fn discard(&mut self, frame: FrameReady) {
        if self
            .pending_sample
            .as_ref()
            .is_some_and(|pending| pending.surface.frame == Some(frame))
        {
            self.pending_sample = None;
        }
    }

    pub(super) fn promote(
        &mut self,
        frame: FrameReady,
        _model: &crate::view_model::ApplicationModel,
    ) -> bool {
        let authorized = self
            .pending_sample
            .as_ref()
            .is_some_and(|pending| pending.view_ready);
        if !authorized
            || !self
                .pending_sample
                .as_ref()
                .is_some_and(|pending| pending.complete && pending.surface.frame == Some(frame))
        {
            return false;
        }
        let pending = self
            .pending_sample
            .take()
            .expect("matching completed sample");
        trace_surface("sample_promotion_started", pending.surface);
        self.surface = pending.surface;
        self.content = pending.content;
        self.placement = pending.placement;
        self.completed = Some(frame);
        self.retained_read = pending.read;
        trace_surface("sample_promoted", self.surface);
        true
    }
}

impl Imported {
    fn ensure_source(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, surface: Surface) {
        let Some(frame) = surface.frame.filter(|frame| frame.direct_sampling) else {
            return;
        };
        let index = frame.slot as usize;
        let matches_source = |arena: &Option<std::sync::Arc<ArenaTexture>>| {
            arena.as_ref().is_some_and(|arena| {
                arena.surface.frame.is_some_and(|prior| {
                    prior.source_high == frame.source_high && prior.source_low == frame.source_low
                })
            })
        };
        if matches_source(&self._arena[index]) {
            return;
        }
        if let Some(existing) = self._arena.iter().position(matches_source) {
            // A direct source can return in either free mailbox slot. Both
            // bindings share its one texture and probe owner; explicit texture
            // destruction still waits for the final binding and actual readers.
            self.views[index] = self.views[existing].clone();
            self.pixel_trace[index] = self.pixel_trace[existing].clone();
            self._arena[index] = self._arena[existing].clone();
            return;
        }
        let label = format!(
            "mmltk-surface-v4/{:016x}{:016x}",
            frame.source_high, frame.source_low
        );
        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some(&label),
            size: wgpu::Extent3d {
                width: surface.width,
                height: surface.height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        self.views[index] = texture.create_view(&wgpu::TextureViewDescriptor {
            dimension: Some(wgpu::TextureViewDimension::D2),
            array_layer_count: Some(1),
            ..wgpu::TextureViewDescriptor::default()
        });
        self.pixel_trace[index] =
            pixel_trace::PixelTrace::new(device, queue, &texture, surface).map(std::sync::Arc::new);
        let lifetime = self
            ._arena
            .iter()
            .flatten()
            .next()
            .expect("import owns a physical texture")
            .lifetime
            .clone();
        trace_source_texture("source_texture_create", surface);
        self._arena[index] = Some(std::sync::Arc::new(ArenaTexture {
            surface,
            texture: Some(texture),
            lifetime,
        }));
    }

    fn reconcile_sample(&mut self, surface: Surface, placement: Placement) {
        let retained_surface = self.image.surface;
        let retained_content = self.image.content.clone();
        let retained_placement = self.image.placement;
        self.image.surface = surface;
        self.image.content = surface
            .frame
            .and_then(metadata::pending)
            .map_or_else(metadata::Content::default, |pending| pending.content);
        self.image.placement = placement;
        self.sample();
        self.image.surface = retained_surface;
        self.image.content = retained_content;
        self.image.placement = retained_placement;
    }

    fn prepare(&mut self, surface: Surface, placement: Placement) {
        let retained = self.image.surface;
        let retained_content = self.image.content.clone();
        let retained_placement = self.image.placement;
        self.image.content = surface
            .frame
            .and_then(metadata::pending)
            .map(|pending| pending.content)
            .unwrap_or_else(|| {
                if self.image.completed == surface.frame {
                    retained_content.clone()
                } else {
                    metadata::Content::default()
                }
            });
        self.image.placement = placement;
        self.image.surface = surface;
        self.sample();
        if let Some(pending) = self.image.pending_sample.as_mut()
            && pending.surface.frame == surface.frame
        {
            pending.surface = surface;
            pending.placement = placement;
        }
        if self.image.completed.is_some()
            && (self.image.completed != surface.frame
                || !retained_draw_admitted(retained, surface, placement))
        {
            self.image.content = retained_content;
            self.image.placement = retained_placement;
            self.image.surface = retained;
        }
    }

    fn sample(&mut self) {
        let Some(frame) = self.image.surface.frame else {
            return;
        };
        if self.image.completed == Some(frame)
            || self
                .image
                .pending_sample
                .as_ref()
                .is_some_and(|pending| pending.surface.frame == Some(frame))
        {
            return;
        }
        if matches!(self.image.placement, Placement::GalleryGrid { .. })
            && self.image.content.gallery().is_none()
        {
            trace_image(
                "sample_rejected",
                "",
                self.image.surface,
                self.image.surface,
                None,
                None,
                0,
            );
            notify_surface(Notification::SampleRejected(frame));
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
        let Some(borrow) = SampleRead::acquire(frame) else {
            trace_image(
                "sample_unavailable",
                "",
                self.image.surface,
                self.image.surface,
                self.image
                    .content
                    .gallery()
                    .map(|value| value.metadata.as_ref()),
                None,
                0,
            );
            return;
        };
        let trace_index = if frame.direct_sampling {
            frame.slot as usize
        } else {
            0
        };
        if let Some(probe) = &self.pixel_trace[trace_index] {
            probe.sample(self.image.surface, borrow.clone());
        }
        let Some(mut acquired) = metadata::pending(frame) else {
            drop(borrow);
            notify_surface(Notification::SampleRejected(frame));
            return;
        };
        acquired.read = Some(borrow);
        acquired.surface = self.image.surface;
        acquired.complete = copy_completed(frame);
        self.image.placement = acquired.placement;
        self.image.pending_sample = Some(acquired);
        if surface_trace_enabled()
            && let Some(sample) = self.image.pending_sample.as_ref()
        {
            trace_image(
                "sample_acquired",
                "",
                sample.surface,
                self.image.surface,
                sample
                    .content
                    .gallery()
                    .map(|value| value.metadata.as_ref()),
                None,
                0,
            );
        }
    }
}

pub(crate) fn same_allocation(left: Surface, right: Surface) -> bool {
    left.high == right.high
        && left.low == right.low
        && left.width == right.width
        && left.height == right.height
}

fn mailbox_binding(frame: FrameReady) -> Option<usize> {
    let binding = frame
        .layer
        .checked_mul(MAILBOX_SLOTS)?
        .checked_add(frame.slot)?;
    (binding < 2).then_some(binding as usize)
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

fn write_content_geometry(queue: &wgpu::Queue, geometry: &wgpu::Buffer, key: GeometryKey) {
    let mut bytes = [0_u8; 64];
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
    bytes[48..52].copy_from_slice(&key.atlas[0].to_ne_bytes());
    bytes[52..56].copy_from_slice(&key.atlas[1].to_ne_bytes());
    queue.write_buffer(geometry, 0, &bytes);
}

pub(crate) fn release(frame: FrameReady) {
    if has_live_read(frame) {
        return;
    }
    take_publication(frame);
    if reserve_release(frame) {
        settle_sample(frame);
    }
}

// Reserve the notification at transfer of custody, not at callback dispatch.
// A sample's shared read token then owns exactly one eventual notification.
fn reserve_release(frame: FrameReady) -> bool {
    RELEASED_FRAMES.with(|released| {
        let mut slots = released.get();
        if slots.iter().flatten().any(|prior| {
            same_mailbox_slot(*prior, frame)
                && prior.presentation_revision >= frame.presentation_revision
        }) {
            return false;
        }
        remember_frame(&mut slots, frame);
        released.set(slots);
        true
    })
}

#[cfg(target_arch = "wasm32")]
fn dispatch_release(frame: FrameReady) {
    trace_frame("sample_released", frame);
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
pub(crate) struct TestRendererCleanup;

#[cfg(test)]
impl Drop for TestRendererCleanup {
    fn drop(&mut self) {
        // Construct before the local headless renderer so its stack ownership
        // drops first, including on assertion unwind. Retire the remaining GPU
        // owner while wgpu's thread-local state is still accessible.
        let renderer = RENDERER.with(|owner| owner.borrow_mut().take());
        // Queue settlement can invoke callbacks that reenter component state.
        drop(renderer);
    }
}

#[cfg(test)]
pub(crate) fn reset_test_releases() {
    metadata::reset();
    authorize_draw(None);
    BORROWS.with(|borrows| borrows.set([None; SAMPLE_CAPACITY]));
    RELEASED_FRAMES.with(|released| released.set([None; SAMPLE_CAPACITY]));
    LIVE_READS.with(|reads| *reads.borrow_mut() = [const { None }; SAMPLE_CAPACITY]);
    COPY_RECEIPTS.with(|receipts| receipts.set([None; SAMPLE_CAPACITY]));
    TEST_RELEASES.with(|released| released.borrow_mut().clear());
    clear_drawn_detail();
}

#[cfg(test)]
pub(crate) fn test_sample_read(frame: FrameReady) -> impl Drop {
    SampleRead::acquire(frame).expect("accepted physical publication")
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
    atlas: vec2<u32>,
    reserved: vec2<u32>,
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
fn fs_main(input: Output) -> @location(0) vec4<f32> {
    let image_uv = (input.position.xy - geometry.image_origin) / geometry.draw_extent;
    let half_texel = vec2<f32>(0.5) / vec2<f32>(textureDimensions(image));
    var source_uv = image_uv;
    if geometry.gallery != 0u && geometry.atlas.x != 0u {
        // A visible range occupies at most two continuous physical row spans.
        let edge = half_texel.y / geometry.uv_scale.y * f32(geometry.atlas.x) / f32(geometry.grid.y);
        let logical_y = clamp(image_uv.y, edge, 1.0 - edge);
        source_uv.y = fract((f32(geometry.atlas.y) + logical_y * f32(geometry.grid.y))
                            / f32(geometry.atlas.x));
    }
    let uv = clamp(geometry.uv_offset + source_uv * geometry.uv_scale,
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

    #[test]
    fn arena_notifications_validate_the_complete_physical_identity() {
        assert_eq!(
            parse_arena_identity("00000000000000010000000000000002"),
            Some((1, 2))
        );
        for invalid in [
            "",
            "1",
            "00000000000000000000000000000000",
            "0000000000000001000000000000000g",
            "000000000000000100000000000000020",
        ] {
            assert_eq!(parse_arena_identity(invalid), None);
        }
    }

    #[test]
    fn workspace_fps_observes_real_iced_gpu_submissions_and_retained_pixels() {
        use iced::advanced::{
            Layout, layout,
            renderer::{Headless, Renderer as _},
            widget,
        };
        reset_test_releases();
        initialize_diagnostics(false, false);
        crate::integration_control::initialize_reporting(false, false);
        let (model, frame) = crate::view_model::test_support::explore_presentation();
        assert!(accept_publication(frame));
        authorize_draw(Some(frame));
        complete_sample(frame);
        let surface = crate::view_model::test_support::physical_surface(frame);
        let _renderer_cleanup = TestRendererCleanup;
        let mut renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
            Default::default(),
            Some("wgpu"),
        ))
        .expect("workspace FPS functional acceptance requires the container GPU backend");
        let bounds = Rectangle::new(Point::ORIGIN, iced::Size::new(128.0, 96.0));
        let viewport = Viewport::with_physical_size(iced::Size::new(128, 96), 1.0);
        let program = Program::<()> {
            show_fps: true,
            input: None,
            local: None,
            publish: None,
            surface,
            placement: Placement::Contain,
            control_id: crate::view::workspace::STABLE_ID,
        };
        let mut element: crate::fluent_theme::Element<'_, ()> =
            iced::widget::shader(program).width(128).height(96).into();
        let mut tree = widget::Tree::new(&element);
        tree.diff(element.as_widget_mut());
        let node = layout::Node::new(bounds.size());
        let theme = crate::fluent_theme::app_theme(false);
        let style = iced::advanced::renderer::Style::default();
        let start = iced::time::Instant::now();
        let sample = |tree: &mut widget::Tree, milliseconds| {
            let state = tree.state.downcast_mut::<WorkspaceViewport>();
            crate::workspace_fps::Meter::update(
                &mut state.fps,
                true,
                &Event::Window(iced::window::Event::RedrawRequested(
                    start + iced::time::Duration::from_millis(milliseconds),
                )),
            );
            state.fps.as_ref().unwrap().frames
        };
        assert_eq!(sample(&mut tree, 0), 0);
        let draw = |renderer: &mut iced::Renderer, node: &layout::Node| {
            element.as_widget().draw(
                &tree,
                renderer,
                &theme,
                &style,
                Layout::new(node),
                mouse::Cursor::Unavailable,
                &bounds,
            );
        };
        renderer.reset(bounds);
        draw(&mut renderer, &node);
        draw(&mut renderer, &node);
        let pixels = renderer.screenshot(&viewport, iced::Color::WHITE);
        // Both primitives encoded the same retained workspace; one actual
        // screenshot queue submission counted, and its native test texture drew.
        assert_eq!(
            &pixels[(48 * 128 + 64) * 4..(48 * 128 + 64) * 4 + 3],
            &[0, 0, 0]
        );
        assert_eq!(sample(&mut tree, 500), 1);
        reconcile_completed(surface, &model);
        let (device, format) = RENDERER.with(|owner| {
            let owner = owner.borrow();
            let owner = owner.as_ref().unwrap();
            (owner.device.clone(), owner.format)
        });
        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("workspace FPS functional target"),
            size: wgpu::Extent3d {
                width: 128,
                height: 96,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        });
        let target = texture.create_view(&Default::default());
        let _ = renderer.present(Some(iced::Color::WHITE), format, &target, &viewport);
        assert_eq!(sample(&mut tree, 1000), 1);
        // Encoding the retained image and then abandoning the exact encoder
        // exercises physical custody without counting a queue submission.
        let unsubmitted = renderer.draw(Some(iced::Color::WHITE), &target, &viewport);
        for enabled in [false, true] {
            crate::integration_control::initialize_reporting(enabled, false);
            // Keep the exact encoded draw alive during pipeline reconstruction
            // for an FD-admitted arena with no image or page texture.
            RENDERER.with(|owner| {
                let mut owner = owner.borrow_mut();
                let owner = owner.as_mut().unwrap();
                let imported = owner.imported.as_ref().unwrap();
                let retained = imported.image.retained_read.clone().unwrap();
                let completed = imported.image.completed;
                let requested = (surface.high, surface.low + 1);
                owner.reconstruct_pending_handoff(requested);
                owner.reconstruct_pending_handoff(requested);
                assert_eq!(owner.reconstruction_admissions.is_some(), enabled);
                if enabled {
                    assert_eq!(owner.reconstruction_admissions.as_ref().unwrap().len(), 1);
                }
                let imported = owner.imported.as_ref().unwrap();
                assert_eq!(imported.image.completed, completed);
                assert!(std::sync::Arc::ptr_eq(
                    imported.image.retained_read.as_ref().unwrap(),
                    &retained
                ));
                assert!(owner.pending.is_none());
                assert!(test_releases().is_empty());
            });
        }
        crate::integration_control::initialize_reporting(false, false);
        RENDERER.with(|owner| {
            assert!(
                owner
                    .borrow()
                    .as_ref()
                    .unwrap()
                    .reconstruction_admissions
                    .is_none()
            )
        });
        drop(unsubmitted);
        renderer.reset(bounds);
        let clipped = layout::Node::new(bounds.size()).move_to(Point::new(300.0, 300.0));
        element.as_widget().draw(
            &tree,
            &mut renderer,
            &theme,
            &style,
            Layout::new(&clipped),
            mouse::Cursor::Unavailable,
            &bounds,
        );
        let _ = renderer.screenshot(&viewport, iced::Color::WHITE);
        assert_eq!(sample(&mut tree, 1500), 0);
        renderer.reset(bounds);
        element.as_widget().draw(
            &tree,
            &mut renderer,
            &theme,
            &style,
            Layout::new(&node),
            mouse::Cursor::Unavailable,
            &bounds,
        );
        let _ = renderer.screenshot(&viewport, iced::Color::WHITE);
        assert_eq!(sample(&mut tree, 2000), 1);
    }

    #[test]
    fn paired_empty_gallery_replaces_detail_only_through_normal_read_custody() {
        for direct_sampling in [false, true] {
            for logical_first in [false, true] {
                reset_test_releases();
                let (mut model, mut old) = crate::view_model::test_support::explore_presentation();
                old.direct_sampling = direct_sampling;
                metadata::install_explore(old, model.explore.snapshot.as_ref().unwrap());
                assert!(accept_publication(old));
                let initial = metadata::pending(old).unwrap();
                let held_encoder = SampleRead::acquire(old).unwrap();
                let mut image = ImagePublication {
                    surface: initial.surface,
                    completed: Some(old),
                    pending_sample: None,
                    retained_read: Some(held_encoder.clone()),
                    content: initial.content,
                    placement: initial.placement,
                };
                let mut empty = model.explore.snapshot.clone().unwrap();
                empty.mode = crate::generated::ExploreMode::Gallery;
                empty.order.matchingcount = 0;
                empty.order.visibleindices.clear();
                empty.gallery.slots.clear();
                empty.viewport.columns = 4;
                empty.viewport.rowcount = 3;
                empty.viewport.firstrow = 0;
                empty.viewport.extent = empty.frame.extent.clone();
                crate::view_model::test_support::gallery_layout(&mut empty);
                empty.frame.revision += 1;
                let candidate = FrameReady {
                    content_sequence: empty.frame.revision,
                    presentation_revision: old.presentation_revision + 1,
                    slot: 1,
                    ..old
                };
                if logical_first {
                    model.explore.snapshot = Some(empty.clone());
                }
                assert!(matches!(
                    image.explore_display(image.surface),
                    Some(ExploreDisplay::Detail(..))
                ));
                assert!(test_releases().is_empty());
                metadata::install_explore(candidate, &empty);
                assert!(accept_publication(candidate));
                let requested = metadata::surface(candidate).unwrap();
                let Some(ExploreDisplay::Gallery(shown, paired)) = explore_display(Some(requested))
                else {
                    panic!("accepted empty graphics must select gallery composition");
                };
                assert_eq!(shown.frame, Some(candidate));
                assert_eq!(paired.metadata.order.matchingcount, 0);
                if !logical_first {
                    model.explore.snapshot = Some(empty);
                }
                let mut pending = metadata::pending(candidate).unwrap();
                pending.read = SampleRead::acquire(candidate);
                pending.complete = false;
                image.pending_sample = Some(pending);
                authorize_draw(Some(candidate));
                assert!(matches!(
                    image.explore_display(requested),
                    Some(ExploreDisplay::Gallery(..))
                ));
                image.complete(candidate);
                assert!(image.promote(candidate, &model));
                assert!(test_releases().is_empty());
                drop(held_encoder);
                assert_eq!(test_releases(), vec![old]);
                assert!(matches!(
                    image.explore_display(requested),
                    Some(ExploreDisplay::Gallery(..))
                ));
                drop(image.retire());
                assert_eq!(test_releases(), vec![old, candidate]);
            }
        }
    }

    #[test]
    fn malformed_detail_candidates_release_only_their_own_receipt() {
        for upscale in [false, true] {
            for invalid in 0..(if upscale { 10 } else { 8 }) {
                reset_test_releases();
                let (model, old) = crate::view_model::test_support::explore_presentation();
                assert!(accept_publication(old));
                let initial = metadata::pending(old).unwrap();
                let image = ImagePublication {
                    surface: initial.surface,
                    completed: Some(old),
                    pending_sample: None,
                    retained_read: SampleRead::acquire(old),
                    content: initial.content,
                    placement: initial.placement,
                };
                let mut source = crate::generated::ExploreImageMetadata::from(
                    model.explore.snapshot.as_ref().unwrap(),
                );
                match invalid {
                    0 => source.dataset.identity = 0,
                    1 => source.selectedimage = None,
                    2 => source.frame.content.width = 0,
                    3 => source.frame.content.height = 0,
                    4 => source.frame.content.x = source.frame.extent.width,
                    5 => source.frame.content.y = u32::MAX,
                    6 => source.frame.content.width = u32::MAX,
                    7 => {
                        if upscale {
                            source.mode = crate::generated::ExploreMode::Gallery;
                        } else {
                            source.frame.content.y = source.frame.extent.height;
                        }
                    }
                    _ => {}
                }
                let mut output = source.frame.clone();
                if invalid == 8 {
                    output.content.width = 0;
                }
                if invalid == 9 {
                    output.content.x = u32::MAX;
                }
                let (product, provenance) = if upscale {
                    output.source.kind = crate::generated::PresentationSourceKind::Upscale;
                    (
                        metadata::encode_product(
                            crate::generated::ApplicationSystem::Upscale,
                            crate::generated::UpscaleImageMetadata {
                                frame: output.clone(),
                                preparedextent: source.frame.extent.clone(),
                                preparedcontent: source.frame.content.clone(),
                                input: source.frame.clone(),
                                scene: source.scene.clone(),
                            },
                        ),
                        Some(metadata::encode_product(
                            crate::generated::ApplicationSystem::Explore,
                            source,
                        )),
                    )
                } else {
                    (
                        metadata::encode_product(
                            crate::generated::ApplicationSystem::Explore,
                            source,
                        ),
                        None,
                    )
                };
                let candidate = FrameReady {
                    content_session: crate::generated::presentation_source_session(
                        output.source.kind,
                    ),
                    presentation_revision: old.presentation_revision + 1,
                    slot: 1,
                    ..old
                };
                let bytes = metadata::encode(output, product, provenance);
                assert!(
                    metadata::install(
                        candidate,
                        candidate.content_width,
                        candidate.content_height,
                        2,
                        &bytes
                    )
                    .is_err()
                );
                // The native-event decoder takes this exact path before publication acceptance.
                release(candidate);
                assert!(metadata::surface(candidate).is_none());
                assert!(matches!(
                    image.explore_display(image.surface),
                    Some(ExploreDisplay::Detail(..))
                ));
                assert_eq!(test_releases(), vec![candidate]);
                drop(image);
                assert_eq!(test_releases(), vec![candidate, old]);
            }
        }
    }

    #[test]
    fn detail_crop_choice_survives_independent_acknowledgements_and_retained_upscale() {
        for logical_first in [false, true] {
            reset_test_releases();
            let (model, frame) = crate::view_model::test_support::explore_presentation();
            let mut logical = model.explore.snapshot.unwrap();
            logical.detail.showoriginaldimensions = false;
            let captured =
                std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(&logical));
            let old = DetailContent::new(captured.clone(), None);
            let mut state = crate::view::explore::state::State::default();
            assert!(!state.detail_original(&old));
            state.choose_detail_original(true);
            state.submit_detail(true);
            assert!(state.detail_original(&old));
            state.settle_detail(true);
            logical.detail.showoriginaldimensions = true;
            let paired = DetailContent::new(
                std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(&logical)),
                None,
            );
            if logical_first {
                state.rebase(Some(&logical), false);
            }
            assert!(state.detail_original(&old));
            assert!(state.detail_original(&paired));
            if !logical_first {
                state.rebase(Some(&logical), false);
            }
            assert!(state.detail_original(&paired));
            let mut output = old.frame().clone();
            output.source.kind = crate::generated::PresentationSourceKind::Upscale;
            let upscale = DetailContent::new(
                captured.clone(),
                Some(std::sync::Arc::new(
                    crate::generated::UpscaleImageMetadata {
                        frame: output,
                        preparedextent: old.frame().extent.clone(),
                        preparedcontent: old.frame().content.clone(),
                        input: old.frame().clone(),
                        scene: captured.scene.clone(),
                    },
                )),
            );
            assert!(!upscale.original_dimensions());
            assert!(state.detail_original(&upscale));
            assert!(!captured.detail.showoriginaldimensions);
            // Transport restoration retains the accepted same-image choice.
            state.rebase(None, true);
            state.rebase(Some(&logical), true);
            assert!(state.detail_original(&upscale));
            let mut replacement = old.clone();
            std::sync::Arc::make_mut(&mut replacement.explore).selectedimage = Some(99);
            assert!(!state.detail_original(&replacement));
            state.choose_detail_original(true);
            state.submit_detail(true);
            state.settle_detail(true);
            state.choose_detail_original(false);
            state.submit_detail(false);
            state.settle_detail(false);
            assert!(state.detail_original(&replacement));
            state.choose_detail_original(false);
            state.rebase(None, true);
            state.rebase(Some(&logical), true); // Another logical image cannot settle this choice.
            assert!(state.detail_original(&replacement));
            state.choose_detail_original(false);
            state.submit_detail(false);
            let mut restored = logical.clone();
            restored.selectedimage = Some(99);
            restored.detail.showoriginaldimensions = false;
            state.rebase(None, true);
            state.rebase(Some(&restored), true); // Native commit survived a lost reply.
            assert!(!state.detail_original(&replacement));
            assert!(metadata::pending(frame).is_some());
        }
    }

    #[test]
    fn drawn_detail_projection_classifies_only_exact_paired_crops() {
        for upscale in [false, true] {
            for original in [false, true] {
                reset_test_releases();
                let (model, old) = crate::view_model::test_support::explore_presentation();
                let mut source = crate::generated::ExploreImageMetadata::from(
                    model.explore.snapshot.as_ref().unwrap(),
                );
                source.detail.showoriginaldimensions = !original;
                source.frame.content = crate::generated::VisualRegion {
                    x: 8,
                    y: 4,
                    width: 320,
                    height: 240,
                };
                let mut product = source.frame.clone();
                let (encoded, provenance) = if upscale {
                    product.source.kind = crate::generated::PresentationSourceKind::Upscale;
                    product.extent.width *= 4;
                    product.extent.height *= 4;
                    product.content.x *= 4;
                    product.content.y *= 4;
                    product.content.width *= 4;
                    product.content.height *= 4;
                    (
                        metadata::encode_product(
                            crate::generated::ApplicationSystem::Upscale,
                            crate::generated::UpscaleImageMetadata {
                                frame: product.clone(),
                                preparedextent: source.frame.extent.clone(),
                                preparedcontent: source.frame.content.clone(),
                                input: source.frame.clone(),
                                scene: source.scene.clone(),
                            },
                        ),
                        Some(metadata::encode_product(
                            crate::generated::ApplicationSystem::Explore,
                            source.clone(),
                        )),
                    )
                } else {
                    (
                        metadata::encode_product(
                            crate::generated::ApplicationSystem::Explore,
                            source.clone(),
                        ),
                        None,
                    )
                };
                let physical = FrameReady {
                    content_session: crate::generated::presentation_source_session(
                        product.source.kind,
                    ),
                    presentation_revision: old.presentation_revision + 1,
                    content_width: product.extent.width,
                    content_height: product.extent.height,
                    slot: 1,
                    ..old
                };
                let bytes = metadata::encode(product.clone(), encoded, provenance);
                metadata::install(
                    physical,
                    physical.content_width,
                    physical.content_height,
                    2,
                    &bytes,
                )
                .unwrap();
                let pending = metadata::pending(physical).unwrap();
                let detail = pending.content.detail().unwrap();
                let surface = detail.configure_surface(pending.surface, original, 1);
                let crop = surface.content_region();
                record_drawn_detail(surface, crop);
                let expected = surface.annotation_request(&product);
                assert_eq!(viewer_annotation_request(), Some(expected.clone()));
                if !original {
                    record_drawn_detail(
                        Surface {
                            crop: Some(crop),
                            ..surface
                        },
                        crop,
                    );
                    assert_eq!(viewer_annotation_request(), Some(expected.clone()));
                }
                for invalid in 0..5 {
                    let mut mixed = surface;
                    let mut recorded = crop;
                    match invalid {
                        0 => mixed.width += 1,
                        1 => mixed.viewer_identity = Some((999, 999)),
                        2 => mixed.frame.as_mut().unwrap().presentation_revision += 1,
                        3 => recorded[0] += 1,
                        _ => mixed.crop = Some([1, 1, 1, 1]),
                    }
                    record_drawn_detail(mixed, recorded);
                    assert!(viewer_annotation_request().is_none());
                }
                record_drawn_detail(surface, crop);
                metadata::retire(physical);
                assert!(viewer_annotation_request().is_none());
                assert!(
                    metadata::install(
                        physical,
                        physical.content_width,
                        physical.content_height,
                        2,
                        &bytes[..bytes.len() - 1]
                    )
                    .is_err()
                );
                assert!(viewer_annotation_request().is_none());
            }
        }
    }

    #[test]
    fn a_gallery_draw_cannot_supply_an_annotation_detail_source() {
        reset_test_releases();
        let (model, frame) = crate::view_model::test_support::explore_presentation();
        let mut snapshot = model.explore.snapshot.unwrap();
        snapshot.mode = crate::generated::ExploreMode::Gallery;
        snapshot.viewport.columns = 4;
        snapshot.viewport.extent = snapshot.frame.extent.clone();
        snapshot.viewport.rowcount = 3;
        snapshot.viewport.firstrow = 0;
        snapshot.gallery.slots.clear();
        snapshot.order.visibleindices.clear();
        crate::view_model::test_support::gallery_layout(&mut snapshot);
        metadata::install_explore(frame, &snapshot);
        let surface = Surface {
            viewer_identity: snapshot
                .selectedimage
                .map(|image| (snapshot.dataset.identity, u64::from(image))),
            ..metadata::surface(frame).unwrap()
        };
        record_drawn_detail(surface, surface.content_region());
        assert!(viewer_annotation_request().is_none());
    }

    #[test]
    fn detail_draws_require_paired_metadata_across_gallery_transitions_and_rejection() {
        for from_gallery in [false, true] {
            for metadata_first in [false, true] {
                for reject in [false, true] {
                    reset_test_releases();
                    let (model, old) = crate::view_model::test_support::explore_presentation();
                    let mut snapshot = model.explore.snapshot.clone().unwrap();
                    snapshot.viewport.columns = 4;
                    snapshot.viewport.extent = snapshot.frame.extent.clone();
                    snapshot.viewport.rowcount = 3;
                    snapshot.viewport.firstrow = 0;
                    snapshot.gallery.slots.clear();
                    snapshot.order.visibleindices.clear();
                    crate::view_model::test_support::gallery_layout(&mut snapshot);
                    snapshot.mode = if from_gallery {
                        crate::generated::ExploreMode::Gallery
                    } else {
                        crate::generated::ExploreMode::Detail
                    };
                    metadata::install_explore(old, &snapshot);
                    assert!(accept_publication(old));
                    let initial = metadata::pending(old).unwrap();
                    let mut image = ImagePublication {
                        surface: initial.surface,
                        completed: Some(old),
                        pending_sample: None,
                        retained_read: SampleRead::acquire(old),
                        content: initial.content,
                        placement: initial.placement,
                    };
                    snapshot.mode = if from_gallery {
                        crate::generated::ExploreMode::Detail
                    } else {
                        crate::generated::ExploreMode::Gallery
                    };
                    snapshot.frame.revision += 1;
                    snapshot.frame.content.x = 7;
                    snapshot.frame.content.width -= 7;
                    let next = FrameReady {
                        content_sequence: snapshot.frame.revision,
                        presentation_revision: old.presentation_revision + 1,
                        slot: 1,
                        ..old
                    };
                    let requested = Surface {
                        frame: Some(next),
                        ..image.surface
                    };
                    if metadata_first {
                        metadata::install_explore(next, &snapshot);
                    }
                    // Neither an unclassified image nor metadata alone is a draw.
                    assert_eq!(
                        matches!(
                            image.explore_display(requested),
                            Some(ExploreDisplay::Detail(..))
                        ),
                        !from_gallery
                    );
                    if !metadata_first {
                        metadata::install_explore(next, &snapshot);
                    }
                    if reject {
                        metadata::retire(next);
                        assert_eq!(
                            matches!(
                                image.explore_display(requested),
                                Some(ExploreDisplay::Detail(..))
                            ),
                            !from_gallery
                        );
                        continue;
                    }
                    assert!(accept_publication(next));
                    let mut pending = metadata::pending(next).unwrap();
                    pending.read = SampleRead::acquire(next);
                    pending.complete = false;
                    image.pending_sample = Some(pending);
                    authorize_draw(Some(next));
                    let display = image.explore_display(requested).unwrap();
                    if let ExploreDisplay::Detail(shown, content) = display {
                        assert_eq!(shown.frame, Some(next));
                        let original = content.configure_surface(shown, true, 19);
                        let region = &content.frame().content;
                        assert_eq!(
                            original.crop,
                            Some([region.x, region.y, region.width, region.height])
                        );
                        assert_eq!(
                            original.viewer_identity,
                            content
                                .viewer_identity()
                                .map(|(dataset, image)| (dataset, u64::from(image)))
                        );
                        assert_eq!(original.fit_revision, 19);
                        assert_eq!(content.configure_surface(shown, false, 20).crop, None);
                    } else {
                        assert!(!from_gallery);
                    }
                    image.complete(next);
                    assert!(image.promote(next, &model));
                    assert_eq!(
                        matches!(
                            image.explore_display(requested),
                            Some(ExploreDisplay::Detail(..))
                        ),
                        from_gallery
                    );
                }
            }
        }
    }

    #[test]
    fn upscale_detail_crop_and_identity_follow_its_retained_source_projection() {
        reset_test_releases();
        let (model, frame) = crate::view_model::test_support::explore_presentation();
        let mut source =
            crate::generated::ExploreImageMetadata::from(model.explore.snapshot.as_ref().unwrap());
        source.frame.content = crate::generated::VisualRegion {
            x: 2,
            y: 1,
            width: 8,
            height: 6,
        };
        let mut upscale_frame = source.frame.clone();
        upscale_frame.extent.width *= 4;
        upscale_frame.extent.height *= 4;
        upscale_frame.source.kind = crate::generated::PresentationSourceKind::Upscale;
        upscale_frame.revision += 1;
        upscale_frame.content = crate::generated::VisualRegion {
            x: 8,
            y: 4,
            width: 32,
            height: 24,
        };
        let upscale = crate::generated::UpscaleImageMetadata {
            frame: upscale_frame,
            preparedextent: source.frame.extent.clone(),
            preparedcontent: source.frame.content.clone(),
            input: source.frame.clone(),
            scene: source.scene.clone(),
        };
        let physical = FrameReady {
            content_session: crate::generated::presentation_source_session(
                upscale.frame.source.kind,
            ),
            content_sequence: upscale.frame.revision,
            content_width: upscale.frame.extent.width,
            content_height: upscale.frame.extent.height,
            presentation_revision: frame.presentation_revision + 1,
            slot: 1,
            ..frame
        };
        let bytes = metadata::encode(
            upscale.frame.clone(),
            metadata::encode_product(crate::generated::ApplicationSystem::Upscale, upscale),
            Some(metadata::encode_product(
                crate::generated::ApplicationSystem::Explore,
                source,
            )),
        );
        metadata::install(
            physical,
            physical.content_width,
            physical.content_height,
            2,
            &bytes,
        )
        .unwrap();
        let pending = metadata::pending(physical).unwrap();
        let detail = pending.content.detail().unwrap();
        let surface = pending.surface;
        assert!(physical.matches_content(detail.frame()));
        let configured = detail.configure_surface(surface, true, 2);
        assert_eq!(configured.crop, Some([8, 4, 32, 24]));
        assert_eq!(
            configured.viewer_identity,
            detail
                .viewer_identity()
                .map(|(dataset, image)| (dataset, u64::from(image)))
        );
        assert_eq!(
            detail.frame().source.kind,
            crate::generated::PresentationSourceKind::Upscale
        );
    }

    use crate::view_model::test_support::physical_frame as frame_ready;

    #[test]
    fn graphics_metadata_matches_exact_content_and_capacity() {
        reset_test_releases();
        let (model, frame) = crate::view_model::test_support::explore_presentation();
        let snapshot = model.explore.snapshot.unwrap();
        let bytes = metadata::encode(
            snapshot.frame.clone(),
            metadata::encode_product(
                crate::generated::ApplicationSystem::Explore,
                crate::generated::ExploreImageMetadata::from(&snapshot),
            ),
            None,
        );
        for field in 0..4 {
            let mut changed = frame;
            match field {
                0 => changed.content_session += 1,
                1 => changed.content_sequence += 1,
                2 => changed.content_width += 1,
                _ => changed.content_height += 1,
            }
            assert!(metadata::install(changed, 4096, 4096, 1, &bytes).is_err());
        }
        assert!(metadata::install(frame, 1, 1, 1, &bytes).is_err());
        assert!(metadata::install(frame, 640, 480, 0, &bytes).is_err());
        assert!(metadata::install(frame, 640, 480, 1, &bytes[..bytes.len() - 1]).is_err());
        let mut trailing = bytes.clone();
        trailing.push(0);
        assert!(metadata::install(frame, 640, 480, 1, &trailing).is_err());
        assert!(metadata::pending(frame).is_some());
    }

    #[test]
    fn completed_predecessor_survives_every_metadata_and_copy_notification_order() {
        use crate::view_model::test_support::{annotation_object, explore_presentation};
        for replacement in [false, true] {
            for [
                domain_position,
                control_position,
                physical_position,
                copy_position,
            ] in crate::view_model::test_support::presentation_arrival_orders()
            {
                reset_test_releases();
                let (mut model, old) = explore_presentation();
                let previous = model.explore.snapshot.as_mut().unwrap();
                previous.dataset.identity = 9;
                previous.overlay.showlabels = true;
                previous.scene.categories = vec![crate::generated::ClassName {
                    value: "predecessor".into(),
                }];
                previous.scene.objects = vec![annotation_object(0)];
                metadata::install_explore(old, previous);
                let mut successor = previous.clone();
                successor.revision += 10;
                successor.frame.revision += 1;
                successor.scene.categories[0].value = "successor".into();
                successor.scene.objects[0].box_.first.x += 17.0;
                let next = FrameReady {
                    content_sequence: successor.frame.revision,
                    presentation_revision: old.presentation_revision + 1,
                    slot: 1,
                    low: old.low + u64::from(replacement),
                    ..old
                };
                assert!(accept_publication(old));
                let previous = metadata::pending(old).unwrap();
                let mut image = ImagePublication {
                    surface: previous.surface,
                    completed: Some(old),
                    pending_sample: None,
                    retained_read: SampleRead::acquire(old),
                    content: previous.content,
                    placement: previous.placement,
                };
                let mut copy_notified = false;
                for position in 0..4 {
                    if position == domain_position {
                        model.explore.snapshot = Some(successor.clone());
                    } else if position == control_position {
                        // No application observation is needed to consume a graphics receipt.
                        model.presentation = None;
                        model.peer_disconnected(crate::view_model::UiError::transport(
                            "independent FD graphics",
                        ));
                    } else if position == physical_position {
                        metadata::install_explore(next, &successor);
                        assert!(accept_publication(next));
                        let mut pending = metadata::pending(next).unwrap();
                        pending.read = SampleRead::acquire(next);
                        pending.complete = copy_notified;
                        image.pending_sample = Some(pending);
                    } else {
                        assert_eq!(position, copy_position);
                        complete_sample(next);
                        image.complete(next);
                        copy_notified = true;
                    }
                    let _ = image.promote(next, &model);
                    let promoted = copy_notified && position >= physical_position;
                    assert_eq!(image.completed, Some(if promoted { next } else { old }));
                    let meaning = image.content.detail().unwrap();
                    assert_eq!(
                        meaning.scene().categories[0].value,
                        if promoted { "successor" } else { "predecessor" }
                    );
                    assert_eq!(
                        meaning.scene().objects[0].box_.first.x,
                        successor.scene.objects[0].box_.first.x - if promoted { 0.0 } else { 17.0 }
                    );
                    assert_eq!(test_releases(), if promoted { vec![old] } else { vec![] });
                }
                model
                    .explore
                    .snapshot
                    .get_or_insert_with(|| successor.clone())
                    .overlay
                    .showlabels = false;
                image.reconcile_pending(next, &model);
                assert!(
                    image
                        .content
                        .detail()
                        .as_ref()
                        .unwrap()
                        .overlay()
                        .showlabels
                );
                image.complete(next);
                assert!(!image.promote(next, &model));
                let obsolete = FrameReady {
                    presentation_revision: next.presentation_revision + 1,
                    slot: 0,
                    ..next
                };
                assert!(accept_publication(obsolete));
                let encoded_reader = SampleRead::acquire(obsolete).unwrap();
                let mut pending = metadata::pending(next).unwrap();
                pending.surface.frame = Some(obsolete);
                pending.read = Some(encoded_reader.clone());
                pending.complete = false;
                image.pending_sample = Some(pending);
                image.discard(obsolete);
                retire_publication(obsolete);
                assert_eq!(test_releases(), vec![old]);
                drop(encoded_reader);
                image.complete(obsolete);
                assert!(!image.promote(obsolete, &model));
                assert_eq!(image.completed, Some(next));
                assert_eq!(test_releases(), vec![old, obsolete]);
                drop(image);
                assert_eq!(test_releases(), vec![old, obsolete, next]);
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
    fn mailbox_pressure_retains_the_latest_arena_frame() {
        reset_test_releases();
        let mut mailbox = FrameMailbox::default();
        for revision in 1..=20 {
            let mut frame = frame_ready(1, revision, revision, 640, 480);
            frame.layer = 0;
            if let Some(displaced) = mailbox.push(frame) {
                release(displaced);
            }
        }
        let mut revisions = Vec::new();
        while let Some(frame) = mailbox.pop() {
            revisions.push(frame.presentation_revision);
        }
        revisions.sort_unstable();
        assert_eq!(revisions, vec![20]);
        assert_eq!(test_releases().len(), 19);
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
        remember_rejection(&mut mailbox.rejected_sample, old);
        let malformed = frame_ready(1, 22, 22, 400, 500);
        remember_rejection(&mut mailbox.rejected_publication, malformed);
        assert!(
            matches!(mailbox.next_notification(), Some(Notification::Copied(frame)) if frame == newest)
        );
        assert!(
            matches!(mailbox.next_notification(), Some(Notification::Native(frame)) if frame == newest)
        );
        assert!(
            matches!(mailbox.next_notification(), Some(Notification::SampleRejected(frame)) if frame == old)
        );
        assert!(
            matches!(mailbox.next_notification(), Some(Notification::SampleRejected(frame)) if frame == malformed)
        );
        assert!(mailbox.next_notification().is_none());
        drop(mailbox);
        assert!(test_releases().is_empty());
    }

    #[test]
    fn stale_mailbox_metadata_cannot_release_an_active_sample() {
        reset_test_releases();
        let current = frame_ready(1, 10, 10, 640, 480);
        assert!(accept_publication(current));
        let read = SampleRead::acquire(current).unwrap();
        let mut mailbox = FrameMailbox::default();
        assert!(mailbox.push(current).is_none());
        assert!(mailbox.push(current).is_none());
        let newer = frame_ready(1, 11, 11, 640, 480);
        release(mailbox.push(newer).unwrap());
        assert!(test_releases().is_empty());
        assert_eq!(mailbox.pop(), Some(newer));
        drop(read);
        assert_eq!(test_releases(), vec![current]);
    }

    #[test]
    fn sample_retirement_invalidates_draws_but_preserves_independent_readers() {
        for direct_sampling in [false, true] {
            reset_test_releases();
            let (_, mut frame) = crate::view_model::test_support::explore_presentation();
            frame.direct_sampling = direct_sampling;
            assert!(accept_publication(frame));
            let read = SampleRead::acquire(frame).unwrap();
            let encoded = read.clone();
            let probe = read.clone();
            let surface = Surface {
                frame: Some(frame),
                ..surface_for_content_session(frame.content_session)
            };
            let next = FrameReady {
                slot: 1,
                presentation_revision: frame.presentation_revision + 1,
                content_sequence: frame.content_sequence + 1,
                ..frame
            };
            assert!(accept_publication(next));
            let pending_read = SampleRead::acquire(next).unwrap();
            let submitted = pending_read.clone();
            let pending_surface = Surface {
                frame: Some(next),
                ..surface
            };
            let mut image = ImagePublication {
                surface,
                pending_sample: Some(PendingImage {
                    read: Some(pending_read),
                    surface: pending_surface,
                    content: metadata::Content::default(),
                    placement: Placement::Contain,
                    complete: true,
                    view_ready: true,
                }),
                completed: Some(frame),
                retained_read: Some(read),
                content: metadata::Content::from_detail(
                    metadata::pending(frame).and_then(|image| image.content.detail()),
                ),
                placement: Placement::Contain,
            };
            DRAW_AUTHORIZATION.with(|authorization| {
                let mut authorization = authorization.borrow_mut();
                authorization.draw = Some(next);
            });
            let retired = image.retire();
            assert!(image.retained().is_none());
            assert!(image.submitted_draw(pending_surface).is_none());
            assert!(image.completed.is_none() && image.surface.frame.is_none());
            assert!(image.content.detail().is_none());
            assert_eq!(image.surface.high, surface.high);
            assert_eq!(image.surface.low, surface.low);
            assert_eq!(image.surface.width, surface.width);
            assert_eq!(image.surface.height, surface.height);
            retire_samples();
            retire_samples();
            DRAW_AUTHORIZATION.with(|authorization| {
                let authorization = authorization.borrow();
                assert!(authorization.draw.is_none());
            });
            drop(retired);
            drop(encoded);
            assert!(test_releases().is_empty());
            drop(submitted);
            assert_eq!(test_releases(), vec![next]);
            drop(probe);
            assert_eq!(test_releases(), vec![next, frame]);
        }
    }

    #[test]
    fn publication_custody_is_consumed_once_and_release_waits_for_readers() {
        reset_test_releases();
        let frame = frame_ready(1, 1, 1, 640, 480);
        assert!(SampleRead::acquire(frame).is_none());
        assert!(accept_publication(frame));
        let read = SampleRead::acquire(frame).unwrap();
        assert!(SampleRead::acquire(frame).is_none());
        assert!(!accept_publication(frame));
        // Replacement, navigation and shutdown may all ask to retire a sample
        // while an encoded/submitted read still owns the external GPU use.
        retire_publication(frame);
        retire_publication(frame);
        assert!(test_releases().is_empty());
        drop(read);
        assert_eq!(test_releases(), vec![frame]);
        assert!(!accept_publication(frame));
        assert!(SampleRead::acquire(frame).is_none());
        release(frame);
        assert_eq!(test_releases(), vec![frame]);
    }

    #[test]
    fn cancellation_and_slot_reuse_never_reacquire_a_receipt() {
        reset_test_releases();
        let old = frame_ready(1, 1, 1, 640, 480);
        assert!(accept_publication(old));
        release(old); // Rejected before any GPU work.
        assert!(SampleRead::acquire(old).is_none());
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
        assert!(SampleRead::acquire(old).is_none());
        let read = SampleRead::acquire(current).unwrap();
        release(old);
        assert_eq!(test_releases(), vec![old]);
        // Dropping the completion owner (including device teardown) settles
        // custody, even when the surface/pipeline no longer exists.
        drop(read);
        assert_eq!(test_releases(), vec![old, current]);
        assert!(!accept_publication(old));
        assert!(!accept_publication(current));
    }

    #[test]
    fn allocation_replacement_keeps_each_sample_read_independent() {
        reset_test_releases();
        let old = frame_ready(1, 1, 1, 640, 480);
        assert!(accept_publication(old));
        let old_read = SampleRead::acquire(old).unwrap();
        let replacement = FrameReady {
            high: 3,
            low: 4,
            ..old
        };
        assert!(accept_publication(replacement));
        let new_read = SampleRead::acquire(replacement).unwrap();
        retire_publication(old);
        retire_publication(replacement);
        assert!(test_releases().is_empty());
        assert!(SampleRead::acquire(old).is_none());
        assert!(SampleRead::acquire(replacement).is_none());
        drop(old_read);
        assert_eq!(test_releases(), vec![old]);
        assert!(!accept_publication(replacement));
        drop(new_read);
        assert_eq!(test_releases(), vec![old, replacement]);
    }

    #[test]
    fn display_draw_batches_and_probe_keep_independent_exact_sample_reads() {
        reset_test_releases();
        let frame = frame_ready(1, 1, 1, 640, 480);
        assert!(accept_publication(frame));
        let display = SampleRead::acquire(frame).unwrap();
        let probe = display.clone();
        let mut first_encoder = shader::Resources::default();
        let mut second_encoder = shader::Resources::default();
        let settlements = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
        let observed = settlements.clone();
        first_encoder.observe_settlement(move |outcome| observed.lock().unwrap().push(outcome));
        // Multiple widgets share one publication and one batch callback.
        for _ in 0..32 {
            first_encoder.retain(display.clone());
        }
        second_encoder.retain(display.clone());
        complete_sample(frame);
        drop(display);
        retire_publication(frame);
        release(frame);
        assert!(test_releases().is_empty());
        // An abandoned encoder drops only its own encoded reads. The same RAII
        // batch settles when wgpu invokes its containing submission callback.
        drop(first_encoder);
        assert_eq!(
            *settlements.lock().unwrap(),
            [shader::Settlement::Abandoned]
        );
        assert!(test_releases().is_empty());
        drop(probe);
        assert!(test_releases().is_empty());
        drop(second_encoder);
        assert_eq!(test_releases(), vec![frame]);
        complete_sample(frame);
        assert!(!copy_completed(frame));
    }

    #[test]
    fn both_slots_wait_for_their_own_readers_across_arena_retirement() {
        reset_test_releases();
        let first = frame_ready(1, 1, 1, 640, 480);
        let second = FrameReady {
            slot: 1,
            content_sequence: 2,
            presentation_revision: 2,
            ..first
        };
        assert!(accept_publication(first));
        let fallback = SampleRead::acquire(first).unwrap();
        assert!(accept_publication(second));
        let incoming = SampleRead::acquire(second).unwrap();
        let mut encoded = shader::Resources::default();
        encoded.retain(fallback.clone());
        encoded.retain(incoming.clone());
        let newer = FrameReady {
            content_sequence: 3,
            presentation_revision: 3,
            ..first
        };
        assert!(!accept_publication(newer));
        drop(fallback);
        drop(incoming);
        // A predecessor's actual encoder remains live after its display owner.
        let replacement = FrameReady {
            high: 7,
            presentation_revision: 4,
            ..first
        };
        assert!(accept_publication(replacement));
        let display = SampleRead::acquire(replacement).unwrap();
        complete_sample(replacement);
        complete_sample(second);
        complete_sample(first);
        assert!(copy_completed(replacement) && copy_completed(second) && copy_completed(first));
        release(first);
        assert!(test_releases().is_empty());
        drop(encoded);
        assert_eq!(test_releases(), vec![first, second]);
        assert!(copy_completed(replacement));
        assert!(accept_publication(newer));
        let current = SampleRead::acquire(newer).unwrap();
        release(first);
        complete_sample(first);
        assert!(!copy_completed(first));
        drop(current);
        drop(display);
        assert_eq!(test_releases(), vec![first, second, newer, replacement]);
    }

    #[test]
    fn replaced_undrawn_pending_and_prepublication_copy_receipts_are_exact() {
        reset_test_releases();
        let first = frame_ready(1, 1, 1, 640, 480);
        let other_arena = FrameReady {
            high: 7,
            presentation_revision: 2,
            ..first
        };
        complete_sample(other_arena);
        complete_sample(first);
        assert!(copy_completed(first) && copy_completed(other_arena));
        assert!(accept_publication(first));
        let pending = SampleRead::acquire(first).unwrap();
        drop(pending); // Prepared/metadata-only work encoded no read.
        assert_eq!(test_releases(), vec![first]);
        assert!(copy_completed(other_arena));
        assert!(accept_publication(other_arena));
        retire_publication(other_arena); // No widget is needed to return capacity.
        assert_eq!(test_releases(), vec![first, other_arena]);
    }

    #[test]
    fn arena_retirement_waits_for_both_slots_draw_batches_and_probe() {
        reset_test_releases();
        let first = frame_ready(1, 1, 1, 640, 480);
        let second = FrameReady {
            slot: 1,
            presentation_revision: 2,
            ..first
        };
        let arena = crate::view_model::test_support::physical_surface(first);
        assert!(!arena_has_readers(arena)); // An undrawn import can retire now.
        assert!(accept_publication(first));
        let fallback = SampleRead::acquire(first).unwrap();
        assert!(accept_publication(second));
        let pending = SampleRead::acquire(second).unwrap();
        let probe = pending.clone();
        let mut submitted = shader::Resources::default();
        let mut abandoned = shader::Resources::default();
        for _ in 0..16 {
            submitted.retain(fallback.clone());
        }
        submitted.retain(pending.clone());
        abandoned.retain(pending.clone());
        assert!(arena_has_readers(arena));
        drop(fallback);
        drop(pending); // Removal of Imported's display state alone is insufficient.
        drop(abandoned); // No queue callback or successful frame is needed.
        assert!(arena_has_readers(arena));
        drop(submitted);
        assert!(arena_has_readers(arena)); // Probe custody is independent.
        assert_eq!(test_releases(), vec![first]);
        drop(probe);
        assert!(!arena_has_readers(arena)); // Exact last-reader destroy frontier.
        assert_eq!(test_releases(), vec![first, second]);
        release(first);
        release(second);
        complete_sample(first);
        assert!(!arena_has_readers(arena));
        assert_eq!(test_releases(), vec![first, second]);
    }

    #[test]
    fn retiring_predecessors_and_new_arena_have_independent_destroy_frontiers() {
        reset_test_releases();
        let first = frame_ready(1, 1, 1, 640, 480);
        let frames = std::array::from_fn::<_, ARENA_CAPACITY, _>(|index| FrameReady {
            high: first.high + index as u64,
            presentation_revision: first.presentation_revision + index as u64,
            ..first
        });
        let arenas = frames.map(crate::view_model::test_support::physical_surface);
        let mut readers = frames.map(|frame| {
            assert!(accept_publication(frame));
            Some(SampleRead::acquire(frame).unwrap())
        });
        let readiness = || arenas.map(|arena| !arena_has_readers(arena));
        assert_eq!(readiness(), [false, false, false]);
        drop(readers[1].take());
        assert_eq!(readiness(), [false, true, false]);
        release(frames[1]);
        complete_sample(frames[1]);
        assert_eq!(readiness(), [false, true, false]);
        drop(readers[0].take());
        assert_eq!(readiness(), [true, true, false]);
        // Stale predecessor settlement cannot destroy the newest arena,
        // despite identical physical slot numbers and content dimensions.
        release(frames[0]);
        complete_sample(frames[0]);
        assert_eq!(readiness(), [true, true, false]);
        drop(readers[2].take());
        assert_eq!(readiness(), [true, true, true]);
        assert_eq!(test_releases(), vec![frames[1], frames[0], frames[2]]);
    }

    #[test]
    fn undisplayed_publication_does_not_require_a_draw_to_retire_its_arena() {
        reset_test_releases();
        let frame = frame_ready(1, 1, 1, 640, 480);
        let arena = crate::view_model::test_support::physical_surface(frame);
        assert!(accept_publication(frame));
        assert!(!arena_has_readers(arena));
        retire_publication(frame);
        assert!(!arena_has_readers(arena));
        assert_eq!(test_releases(), vec![frame]);
    }

    #[test]
    fn native_frame_payload_is_direct_and_bounded() {
        let ready = FrameReady::parse(
            "00000000000000010000000000000002:0:1:3:4:5:640:480:00000000000000060000000000000007:1",
        )
        .expect("frame");
        assert_eq!(ready.slot, 1);
        assert_eq!(ready.presentation_revision, 5);
        assert!(ready.direct_sampling);
        assert_eq!((ready.source_high, ready.source_low), (6, 7));
        assert!(FrameReady::parse("00000000000000010000000000000002:0:2:3:4:5:640:480:00000000000000060000000000000007:1",).is_none());
    }

    #[test]
    fn native_mailbox_binding_is_bounded_to_two_retained_slots() {
        let mut frame = frame_ready(1, 2, 3, 640, 360);
        for layer in 0..1 {
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
    #[test]
    fn prediction_metadata_keeps_identity_with_retained_pixels_and_video_revisions() {
        reset_test_releases();
        let model = crate::view_model::test_support::bootstrapped();
        let mut snapshot = model.predict_snapshot.clone().unwrap();
        snapshot.contentidentity = (1_u64 << 40) + 9;
        let session = crate::generated::presentation_source_session(
            crate::generated::PresentationSourceKind::Predict,
        );
        let install = |snapshot: &crate::generated::PredictSnapshot, sequence| {
            let mut frame = frame_ready(session, sequence, sequence, 640, 480);
            // The completed first image retains its physical read until promotion.
            frame.slot = u32::from(sequence != 1);
            let mut product = crate::generated::PredictImageMetadata::from(snapshot);
            product.frame = crate::view_model::test_support::visual_frame(
                crate::generated::PresentationSourceKind::Predict,
                sequence,
            );
            product.frame.extent.width = 640;
            product.frame.extent.height = 480;
            product.frame.content.width = 640;
            product.frame.content.height = 480;
            metadata::install(
                frame,
                640,
                480,
                sequence,
                &metadata::encode(
                    product.frame.clone(),
                    metadata::encode_product(crate::generated::ApplicationSystem::Predict, product),
                    None,
                ),
            )
            .unwrap();
            frame
        };
        let first = install(&snapshot, 1);
        assert!(accept_publication(first));
        let initial = metadata::pending(first).unwrap();
        let mut image = ImagePublication {
            surface: initial.surface,
            completed: Some(first),
            pending_sample: None,
            retained_read: SampleRead::acquire(first),
            content: initial.content,
            placement: Placement::Contain,
        };
        let mut viewport = ViewportOwner::default();
        viewport.synchronize_source(image.retained().unwrap());
        viewport.zoom = 2.0;
        viewport.pan_x = 17.0;
        let retained_identity = image.retained().unwrap().viewer_identity;
        for sequence in [2, 3] {
            // same-image overlay or sequential video frame
            let frame = install(&snapshot, sequence);
            let paired = metadata::surface(frame).unwrap();
            assert_eq!(paired.viewer_identity, retained_identity);
            assert_eq!(viewport.transform_for(paired).zoom, 2.0);
            metadata::retire(frame);
        }
        snapshot.contentidentity += 1; // different compiled/ordinary image
        let next = install(&snapshot, 4);
        assert!(accept_publication(next));
        let mut pending = metadata::pending(next).unwrap();
        pending.read = SampleRead::acquire(next);
        pending.complete = false;
        let next_surface = pending.surface;
        image.pending_sample = Some(pending);
        assert!(!image.promote(next, &model));
        assert_eq!(image.retained().unwrap().viewer_identity, retained_identity);
        assert_eq!(viewport.transform_for(image.retained().unwrap()).zoom, 2.0);
        assert_ne!(next_surface.viewer_identity, retained_identity);
        assert_eq!(viewport.transform_for(next_surface).zoom, 1.0);
        image.complete(next);
        assert!(image.promote(next, &model));
        assert_eq!(
            image.retained().unwrap().viewer_identity,
            next_surface.viewer_identity
        );
        viewport.synchronize_source(image.retained().unwrap());
        assert_eq!(viewport.zoom, 1.0);
        assert_eq!(viewport.pan_x, 0.0);
        drop(image);
    }
}
