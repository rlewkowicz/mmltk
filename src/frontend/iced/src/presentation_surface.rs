use iced::widget::shader;
use iced::{Event, Rectangle, mouse};
pub(crate) mod gallery;
mod geometry;
pub(crate) mod labels;
pub(crate) mod metadata;
pub(crate) mod pixel_trace;
mod renderer;

pub(crate) use geometry::{Placement, SurfaceSample, ViewportOwner, physical_bounds};
use geometry::{PlacementGeometry, placement_geometry};
pub use geometry::{SurfaceGesture, SurfaceGestureKind};
#[cfg(test)]
use geometry::{ViewTransform, inverse_content_point};
#[cfg(test)]
use renderer::ImagePublication;
#[cfg(any(target_arch = "wasm32", test))]
pub use renderer::subscription;
pub(crate) use renderer::{
    AnnotationContent, DetailContent, ExploreDisplay, Primitive, accept_publication,
    authorize_draw, begin_capacity_acceptance, capacity_acceptance_slots, clear_drawn_detail,
    complete_sample, discard_sample, drawable_annotation, drawable_prediction, drawable_validation,
    drawn_detail, end_capacity_acceptance, explore_display, initialize_diagnostics,
    invalidate_drawn_slot, reconcile_completed, release, release_capacity_sample,
    reset_reconstruction_probe, retained_surface, retire_publication, retire_samples,
    same_allocation, trace_atlas_stage, trace_surface, viewer_annotation_request,
};
pub use renderer::{FrameReady, Notification};
use renderer::{PendingImage, SAMPLE_CAPACITY, SampleRead, copy_completed, trace_gallery_source};
#[cfg(test)]
pub(crate) use renderer::{
    TestRendererCleanup, record_drawn_detail, reset_test_releases, test_releases, test_sample_read,
};
#[cfg(target_arch = "wasm32")]
pub(crate) use renderer::{
    emit_surface_trace, gallery_trace_fields, surface_trace_enabled, surface_trace_fields,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Surface {
    pub high: u64,
    pub low: u64,
    pub width: u32,
    pub height: u32,
    // Desired publication geometry/identity, not authority to borrow its slot.
    // Only accept_publication + SampleRead can authorize an external read.
    pub frame: Option<FrameReady>,
    pub crop: Option<[u32; 4]>,
    pub viewer_identity: Option<(u64, u64)>,
    pub fit_revision: u64,
}

impl Surface {
    pub(crate) fn empty() -> Self {
        Self {
            high: 0,
            low: 0,
            width: 0,
            height: 0,
            frame: None,
            crop: None,
            viewer_identity: None,
            fit_revision: 0,
        }
    }

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

    fn transform_identity(self) -> Option<(u64, u64, u64)> {
        self.viewer_identity
            .or_else(|| self.frame.map(|frame| (frame.content_session, 0)))
            .map(|(session, image)| (session, image, self.fit_revision))
    }
    pub fn valid(self) -> bool {
        (self.high != 0 || self.low != 0) && self.width != 0 && self.height != 0
    }

    fn label(self) -> String {
        format!("mmltk-surface-v4/{:016x}{:016x}", self.high, self.low)
    }
}

#[derive(Clone)]
pub(crate) struct Program<Message> {
    pub show_fps: bool,
    pub input: Option<crate::workspace_input::Binding>,
    pub surface: Surface,
    pub publish: Option<fn(SurfaceGesture) -> Message>,
    pub local: Option<std::sync::Arc<dyn Fn(SurfaceGesture) -> Option<Message> + Send + Sync>>,
    pub placement: Placement,
    pub control_id: &'static str,
}

impl<Message> shader::Program<Message> for Program<Message> {
    type State = WorkspaceViewport;
    type Primitive = Primitive;

    fn draw(
        &self,
        state: &Self::State,
        _cursor: iced::mouse::Cursor,
        _bounds: Rectangle,
    ) -> Self::Primitive {
        Primitive {
            submission: state
                .fps
                .as_ref()
                .filter(|_| self.show_fps)
                .map(crate::workspace_fps::Meter::observer),
            surface: self.surface,
            transform: state.viewport.transform_for(self.surface),
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
        crate::workspace_fps::Meter::update(&mut state.fps, self.show_fps, event);
        let dispatch = |gesture| {
            let message = self.local.as_ref().map_or_else(
                || self.publish.map(|publish| publish(gesture)),
                |local| local(gesture),
            );
            message.map_or_else(shader::Action::capture, shader::Action::publish)
        };
        let surface = if self.control_id == crate::view::annotation::WORKSPACE_ID {
            drawable_annotation(self.surface).map_or(self.surface, |(surface, _)| surface)
        } else {
            self.surface
        };
        if let Some(input) = &self.input {
            let point = state
                .viewport
                .sample(bounds, cursor, surface, self.placement, false)
                .map(|sample| crate::generated::WorkspacePoint {
                    x: sample.content_x,
                    y: sample.content_y,
                });
            state.input.event(input, event, bounds, cursor, point);
        }
        state.viewport.update(
            event,
            bounds,
            cursor,
            surface,
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
        if state.viewport.pan_origin.is_some() {
            mouse::Interaction::Grabbing
        } else if cursor.is_over(bounds) {
            mouse::Interaction::Grab
        } else {
            mouse::Interaction::default()
        }
    }
}

#[derive(Default)]
pub(crate) struct WorkspaceViewport {
    viewport: ViewportOwner,
    input: crate::workspace_input::Capture,
    fps: Option<crate::workspace_fps::Meter>,
}

#[cfg(test)]
fn surface_for_content_session(content_session: u64) -> Surface {
    Surface {
        high: 1,
        low: 2,
        width: 640,
        height: 480,
        frame: Some(crate::view_model::test_support::physical_frame(
            content_session,
            1,
            1,
            640,
            480,
        )),
        crop: None,
        viewer_identity: None,
        fit_revision: 0,
    }
}
