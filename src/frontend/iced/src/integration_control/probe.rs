//! Exact receipt and asynchronous callback custody.
#[cfg(target_arch = "wasm32")]
use crate::integration_control::pixel_checks::pixel_result_callback;
use crate::integration_control::pixel_checks::{
    AtlasDraw, FpsPixelOutcome, ProbeOutcome, SampleablePresentation, sampleable_presentation,
};
use crate::integration_control::{
    Driver, EXPLORE_GALLERY, Message, Phase, pixel_checks, reporting, reporting_enabled, widget_ops,
};
#[cfg(target_arch = "wasm32")]
use crate::integration_control::{annotation_swatch_js, capture_probe_js, fps_draw_js, receipt_js};
use crate::view::explore;
use crate::view_model::ApplicationModel;
use iced::Rectangle;
#[derive(Clone)]
pub(super) struct ControlProbe {
    pub(super) output: ScenarioOutput,
    pub(super) color: [f64; 3],
    pub(super) available: bool,
}

impl ControlProbe {
    pub(super) fn sample(self, bounds: Rectangle, control: &str, capability: bool) {
        #[cfg(not(target_arch = "wasm32"))]
        let _ = (self, bounds, control, capability);
        #[cfg(target_arch = "wasm32")]
        {
            let mut output = self.output;
            let canvas_probe = output.canvas_probe.clone();
            let callback = pixel_result_callback(move |outcome| {
                let message = if capability {
                    Message::AnnotationControlPixels { outcome }
                } else {
                    Message::AnnotationPixels {
                        revision: 0,
                        outcome,
                    }
                };
                let _ = output.try_send(message);
            });
            let detail = if capability {
                if self.available {
                    "enabled"
                } else {
                    "disabled"
                }
            } else {
                "native-hsv-completed-canvas"
            };
            annotation_swatch_js(
                &canvas_probe,
                &[
                    f64::from(bounds.x),
                    f64::from(bounds.y),
                    f64::from(bounds.width),
                    f64::from(bounds.height),
                ],
                &self.color,
                control,
                detail,
                capability,
                &callback,
            );
        }
    }
}

#[derive(Clone)]
pub(super) struct AnnotationProbe {
    pub(super) output: ScenarioOutput,
    pub(super) source: u64,
    pub(super) presentation: u64,
    pub(super) extent: [u32; 2],
    pub(super) pixels: Vec<f64>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ViewerDraw {
    pub crop: [u32; 4],
    pub container: Rectangle,
    pub image: Rectangle,
    pub fit_revision: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ProbeReceipt {
    pub(super) generation: u64,
    pub(super) control: &'static str,
    pub(super) surface: crate::presentation_surface::Surface,
    pub(super) bounds: Rectangle,
    pub(super) image: Rectangle,
    pub(super) clip: Rectangle,
}

#[derive(Clone)]
pub(super) struct ScenarioOutput {
    pub(super) generation: u64,
    pub(super) receipt: Option<ProbeReceipt>,
    pub(super) probe: Option<std::sync::Arc<()>>,
    // Captured with the Rust receipt, before any widget-location task.
    #[cfg(target_arch = "wasm32")]
    pub(super) canvas_probe: wasm_bindgen::JsValue,
    pub(super) sender: iced::futures::channel::mpsc::Sender<Message>,
}

impl ScenarioOutput {
    pub(super) fn new(
        generation: u64,
        sender: iced::futures::channel::mpsc::Sender<Message>,
    ) -> Self {
        Self {
            generation,
            receipt: None,
            probe: None,
            #[cfg(target_arch = "wasm32")]
            canvas_probe: wasm_bindgen::JsValue::UNDEFINED,
            sender,
        }
    }

    pub(super) fn try_send(
        &mut self,
        message: Message,
    ) -> Result<(), iced::futures::channel::mpsc::TrySendError<Message>> {
        let message = if let Some(owner) = &self.probe {
            Message::ProbeCompleted {
                owner: owner.clone(),
                message: Box::new(message),
            }
        } else {
            message
        };
        self.sender.try_send(Message::Scoped {
            generation: self.generation,
            receipt: self.receipt.clone(),
            message: Box::new(message),
        })
    }
    pub(super) fn send(&mut self, message: Message) {
        if let Err(error) = self.try_send(message) {
            if !error.is_full() {
                return;
            }
            // A full observation channel cannot discard its terminal handoff.
            let message = error.into_inner();
            let mut sender = self.sender.clone();
            use iced::Executor;
            iced::executor::Default::new()
                .expect("integration completion executor")
                .spawn(async move {
                    use iced::futures::SinkExt;
                    let _ = sender.send(message).await;
                });
        }
    }
}

#[derive(Default)]
struct SurfaceDrawObserver {
    generation: u64,
    subscription: Option<std::sync::Arc<()>>,
    receipts: std::collections::BTreeMap<&'static str, ProbeReceipt>,
    output: Option<ScenarioOutput>,
    identity: (u64, u64),
    viewer: Option<(u64, u64, ViewerDraw)>,
    gallery: Option<(u64, u64)>,
    atlas: Option<AtlasDraw>,
    atlas_pixels_owner: Option<std::sync::Arc<()>>,
    atlas_composition_owner: Option<std::sync::Arc<()>>,
    fps_sample: Option<iced::time::Instant>,
    fps_draw: Option<reporting::FpsEvidence>,
}

pub(crate) fn report_workspace_fps(
    control: &'static str,
    meter: &crate::workspace_fps::Meter,
    bounds: Rectangle,
    clip: Rectangle,
    dark: bool,
) {
    if !reporting_enabled() || meter.frames == 0 || meter.seconds < 0.5 {
        return;
    }
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        let evidence = reporting::FpsEvidence {
            bounds,
            clip,
            dark,
            frames: meter.frames,
            seconds: meter.seconds,
        };
        #[cfg(target_arch = "wasm32")]
        fps_draw_js(control, &evidence.canvas_values());
        let unchanged = if control == EXPLORE_GALLERY {
            let unchanged = observer.fps_draw == Some(evidence);
            observer.fps_draw = Some(evidence);
            unchanged
        } else {
            observer.fps_draw = None;
            true
        };
        if unchanged && observer.fps_sample == Some(meter.sample_time()) {
            return;
        }
        if let Some(output) = observer.output_for(control)
            && output
                .try_send(Message::WorkspaceFpsDrawn(evidence))
                .is_ok()
        {
            observer.fps_sample = Some(meter.sample_time());
            reporting::workspace_fps(control, meter, evidence);
        }
    });
}

impl SurfaceDrawObserver {
    pub(super) fn output_for(&mut self, control: &str) -> Option<&mut ScenarioOutput> {
        let receipt = self.receipts.get(control)?.clone();
        let output = self.output.as_mut()?;
        output.receipt = Some(receipt);
        Some(output)
    }
}

thread_local! {
    static SURFACE_DRAW_OBSERVER: std::cell::RefCell<SurfaceDrawObserver> =
        std::cell::RefCell::new(SurfaceDrawObserver::default());
}

pub(super) struct SurfaceDrawSubscription(std::sync::Arc<()>);

impl Drop for SurfaceDrawSubscription {
    fn drop(&mut self) {
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            if observer
                .subscription
                .as_ref()
                .is_some_and(|owner| std::sync::Arc::ptr_eq(owner, &self.0))
            {
                let generation = observer.generation;
                *observer = SurfaceDrawObserver {
                    generation,
                    ..Default::default()
                };
            }
        });
    }
}

pub(super) fn surface_draw_stream() -> impl iced::futures::Stream<Item = Message> {
    iced::stream::channel(1, async move |sender| {
        let owner = std::sync::Arc::new(());
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            observer.subscription = Some(owner.clone());
            observer.output = Some(ScenarioOutput::new(observer.generation, sender));
        });
        let _subscription = SurfaceDrawSubscription(owner);
        std::future::pending::<()>().await;
    })
}

pub(super) fn reset_observer() -> u64 {
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        let generation = observer
            .generation
            .checked_add(1)
            .expect("integration scenario generation exhausted");
        let output = observer.output.take().map(|mut output| {
            output.generation = generation;
            output.receipt = None;
            output.probe = None;
            #[cfg(target_arch = "wasm32")]
            {
                output.canvas_probe = wasm_bindgen::JsValue::UNDEFINED;
            }
            output
        });
        let subscription = observer.subscription.take();
        *observer = SurfaceDrawObserver {
            generation,
            output,
            subscription,
            ..Default::default()
        };
        generation
    })
}

pub(super) fn current_receipt(control: &str) -> Option<ProbeReceipt> {
    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().receipts.get(control).cloned())
}

pub(super) fn probe_output(control: &str) -> Option<ScenarioOutput> {
    let mut output = SURFACE_DRAW_OBSERVER
        .with(|observer| observer.borrow_mut().output_for(control).cloned())?;
    output.probe = Some(std::sync::Arc::new(()));
    #[cfg(target_arch = "wasm32")]
    {
        output.canvas_probe = capture_probe_js(control);
    }
    Some(output)
}

#[cfg(any(target_arch = "wasm32", test))]
pub(super) fn atlas_probe_output(composition: bool) -> Option<ScenarioOutput> {
    let output = probe_output(EXPLORE_GALLERY)?;
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        let pending = if composition {
            &mut observer.atlas_composition_owner
        } else {
            &mut observer.atlas_pixels_owner
        };
        *pending = output.probe.clone();
    });
    Some(output)
}

pub(super) fn same_probe(
    owner: &Option<std::sync::Arc<()>>,
    request: Option<&std::sync::Arc<()>>,
) -> bool {
    match (owner, request) {
        (Some(owner), Some(request)) => std::sync::Arc::ptr_eq(owner, request),
        _ => false,
    }
}

pub(crate) fn record_probe_draw(
    control: &'static str,
    surface: crate::presentation_surface::Surface,
    bounds: Rectangle,
    image: Rectangle,
    clip: Rectangle,
) {
    if !reporting_enabled() {
        return;
    }
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        let receipt = ProbeReceipt {
            generation: observer.generation,
            control,
            surface,
            bounds,
            image,
            clip,
        };
        #[cfg(target_arch = "wasm32")]
        if let Some(frame) = surface.frame {
            receipt_js(
                control,
                &format!("{receipt:?}"),
                frame.content_sequence as f64,
                frame.presentation_revision as f64,
            );
        }
        if observer.receipts.get(control) != Some(&receipt) {
            match control {
                EXPLORE_GALLERY => {
                    observer.gallery = None;
                    observer.atlas = None;
                }
                explore::DETAIL_WORKSPACE_ID => observer.viewer = None,
                crate::view::workspace::STABLE_ID => observer.identity = (0, 0),
                _ => {}
            }
            observer.receipts.insert(control, receipt);
            // Validation can finish its final native update before the paired
            // image is drawable. Wake the waiting scenario from that actual
            // draw, once per receipt; unchanged redraws remain quiet.
            if matches!(
                control,
                crate::view::validate::samples::ATLAS_ID | "validate.detail.image"
            ) && let Some(output) = observer.output_for(control)
            {
                output.send(Message::Advance);
            }
        }
    });
}

/// Mutable observations owned by this scenario or mechanism.
pub(super) struct Requests {
    annotation_sample_baseline: u64,
    annotation_probe: Option<AnnotationProbe>,
    annotation_pixels_receipt: Option<ProbeReceipt>,
    annotation_pixels_pending: Option<ProbeReceipt>,
    annotation_pixels_owner: Option<std::sync::Arc<()>>,
    control_probe_receipt: Option<ProbeReceipt>,
    control_probe_owner: Option<std::sync::Arc<()>>,
    control_probe: Option<ControlProbe>,
    annotation_frame_ready: Option<SampleablePresentation>,
    annotation_drawn: Option<(u64, u64)>,
    viewer_drawn: Option<(u64, u64, ViewerDraw)>,
    upscale_pixel_pending: Option<ProbeReceipt>,
    upscale_pixel_owner: Option<std::sync::Arc<()>>,
    gallery_drawn: Option<(u64, u64)>,
}
impl Default for Requests {
    fn default() -> Self {
        Self {
            annotation_sample_baseline: 0,
            annotation_probe: None,
            annotation_pixels_receipt: None,
            annotation_pixels_pending: None,
            annotation_pixels_owner: None,
            control_probe_receipt: None,
            control_probe_owner: None,
            control_probe: None,
            annotation_frame_ready: None,
            annotation_drawn: None,
            viewer_drawn: None,
            upscale_pixel_pending: None,
            upscale_pixel_owner: None,
            gallery_drawn: None,
        }
    }
}

impl Requests {
    pub(super) fn accepts_message(
        &self,
        driver: &Driver,
        pixel_checks: &pixel_checks::State,
        message: &Message,
    ) -> bool {
        if !driver.running() {
            return false;
        }
        match message {
            Message::Scoped {
                generation,
                receipt,
                message,
            } => {
                let (probe, message) = match message.as_ref() {
                    Message::ProbeCompleted { owner, message } => (Some(owner), message.as_ref()),
                    message => (None, message),
                };
                let cancellation = matches!(
                    message,
                    Message::ReportingDisabled
                        | Message::WorkspaceFpsPixels(FpsPixelOutcome::Cancelled)
                );
                *generation == driver.generation
                    && (cancellation
                        || *generation
                            == SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation))
                    && match message {
                        Message::ReportingDisabled => {
                            probe.is_none()
                                && matches!(
                                    driver.phase,
                                    Phase::AwaitWorkspaceFps | Phase::AwaitWorkspaceFpsPixels
                                )
                        }
                        Message::WorkspaceFpsPixels(_) => {
                            pixel_checks.accepts_fps_completion(driver, cancellation, probe)
                        }
                        Message::UpscalePixels { .. } => {
                            same_probe(&self.upscale_pixel_owner, probe)
                        }
                        Message::AnnotationControlPixels { .. }
                        | Message::AnnotationPixels { revision: 0, .. } => {
                            same_probe(&self.control_probe_owner, probe)
                        }
                        Message::AnnotationPixels { .. } => {
                            same_probe(&self.annotation_pixels_owner, probe)
                        }
                        Message::AtlasPixels { .. } => SURFACE_DRAW_OBSERVER.with(|observer| {
                            same_probe(&observer.borrow().atlas_pixels_owner, probe)
                        }),
                        Message::AtlasComposition { .. } => {
                            SURFACE_DRAW_OBSERVER.with(|observer| {
                                same_probe(&observer.borrow().atlas_composition_owner, probe)
                            })
                        }
                        _ => probe.is_none(),
                    }
                    && match receipt {
                        // An owned completion must retire its request even when its
                        // physical receipt changed while the browser was sampling.
                        Some(_) if matches!(message, Message::WorkspaceFpsPixels(_)) => true,
                        Some(receipt) => current_receipt(receipt.control).as_ref() == Some(receipt),
                        None => matches!(
                            message,
                            Message::ReportingDisabled
                                | Message::Advance
                                | Message::Located { .. }
                                | Message::NumberWheelDelivered
                                | Message::NumberInvalidDelivered
                                | Message::NumberClipboardPrepared { .. }
                                | Message::NumberPasteDelivered(_)
                                | Message::NumberPasteRead { .. }
                                | Message::GalleryMouseDelivered
                                | Message::WorkflowPixels { .. }
                                | Message::PrimaryActionPixels { .. }
                                | Message::PrimaryActionMeasure { .. }
                                | Message::PrimaryActionMeasured { .. }
                                | Message::ChartInputDelivered(_)
                        ),
                    }
            }
            Message::Advance
            | Message::Located { .. }
            | Message::NumberWheelDelivered
            | Message::NumberInvalidDelivered
            | Message::GalleryMouseDelivered => true,
            _ => false,
        }
    }
    pub(super) fn observe_presentation(
        &mut self,
        driver: &mut Driver,
        model: &ApplicationModel,
        frame: Option<crate::presentation_surface::FrameReady>,
    ) {
        if !matches!(
            driver.phase,
            Phase::AwaitAnnotation
                | Phase::AnnotationTool { .. }
                | Phase::AnnotationSidebar { .. }
                | Phase::AnnotationTimeline { .. }
                | Phase::AnnotationOperation { .. }
                | Phase::AnnotationStop { .. }
                | Phase::AnnotationBrush { .. }
                | Phase::AwaitTool { .. }
                | Phase::AnnotationSurface(_)
                | Phase::AwaitAnnotationFrame(_)
                | Phase::AnnotationPointer(_)
                | Phase::AwaitPointer(_)
                | Phase::CopyProductStart
                | Phase::CopyProductWait
        ) {
            return;
        }
        let Some(annotation) = model.annotation.snapshot.as_ref() else {
            return;
        };
        let Some(sampleable) = sampleable_presentation(
            frame,
            crate::generated::PresentationSourceKind::Annotation,
            annotation.frame.revision,
        ) else {
            return;
        };
        if sampleable.presentation_revision > self.annotation_sample_baseline {
            self.annotation_frame_ready = Some(sampleable);
        }
    }
}

impl Requests {
    pub(super) fn accept_location(&mut self) -> bool {
        if let Some(probe) = &self.annotation_probe {
            if probe.output.receipt != current_receipt("workflow.visual.workspace") {
                if self.annotation_pixels_pending == probe.output.receipt {
                    self.annotation_pixels_pending = None;
                    self.annotation_pixels_owner = None;
                }
                self.annotation_probe = None;
                return false;
            }
        }
        if let Some(probe) = &self.control_probe {
            if probe.output.receipt != current_receipt("workflow.visual.workspace") {
                if self.control_probe_receipt == probe.output.receipt {
                    self.control_probe_receipt = None;
                    self.control_probe_owner = None;
                }
                self.control_probe = None;
                return false;
            }
        }
        true
    }
}

impl Requests {
    pub(super) fn prepare_control_probe(
        &mut self,
        widgets: &widget_ops::RevealState,
        color: [f64; 3],
        available: bool,
    ) -> bool {
        if widgets.location_pending() {
            return false;
        }
        let Some(output) = probe_output("workflow.visual.workspace") else {
            return false;
        };
        self.control_probe_owner = output.probe.clone();
        self.control_probe_receipt = output.receipt.clone();
        self.control_probe = Some(ControlProbe {
            output,
            color: color,
            available: available,
        });
        true
    }
}

impl Requests {
    pub(super) fn prepare_annotation_probe(
        &mut self,
        widgets: &widget_ops::RevealState,
        source: u64,
        presentation: u64,
        extent: [u32; 2],
        pixels: Vec<f64>,
    ) -> bool {
        if widgets.location_pending() {
            return false;
        }
        let Some(output) = probe_output("workflow.visual.workspace") else {
            return false;
        };
        let Some(frame) = output
            .receipt
            .as_ref()
            .and_then(|receipt| receipt.surface.frame)
        else {
            return false;
        };
        if frame.content_sequence != source
            || frame.presentation_revision != presentation
            || [frame.content_width, frame.content_height] != extent
        {
            return false;
        }
        if self.annotation_pixels_pending == output.receipt {
            return false;
        }
        self.annotation_pixels_owner = output.probe.clone();
        self.annotation_pixels_pending = output.receipt.clone();
        self.annotation_probe = Some(AnnotationProbe {
            output,
            source,
            presentation,
            extent,
            pixels,
        });
        true
    }
}

#[derive(Clone, Copy)]
pub(super) struct Draws {
    pub(super) annotation: Option<(u64, u64)>,
    pub(super) viewer: Option<(u64, u64, ViewerDraw)>,
    pub(super) gallery: Option<(u64, u64)>,
}
impl Draws {
    pub(super) fn upscale(
        self,
        frame: Option<crate::presentation_surface::FrameReady>,
        revision: u64,
    ) -> Option<SampleablePresentation> {
        let sampleable = sampleable_presentation(
            frame,
            crate::generated::PresentationSourceKind::Upscale,
            revision,
        )?;
        self.viewer
            .filter(|(drawn, source, _)| {
                *drawn == sampleable.presentation_revision && *source == revision
            })
            .map(|_| sampleable)
    }
}
pub(super) struct AnnotationObservation<'a> {
    pub(super) sample_baseline: u64,
    pub(super) frame_ready: Option<SampleablePresentation>,
    pub(super) prepared: Option<&'a AnnotationProbe>,
    pub(super) pending: &'a Option<ProbeReceipt>,
    pub(super) receipt: &'a Option<ProbeReceipt>,
    pub(super) control_receipt: &'a Option<ProbeReceipt>,
}
impl Requests {
    pub(super) fn draws(&self) -> Draws {
        Draws {
            annotation: self.annotation_drawn,
            viewer: self.viewer_drawn,
            gallery: self.gallery_drawn,
        }
    }
    pub(super) fn annotation_observation(&self) -> AnnotationObservation<'_> {
        AnnotationObservation {
            sample_baseline: self.annotation_sample_baseline,
            frame_ready: self.annotation_frame_ready,
            prepared: self.annotation_probe.as_ref(),
            pending: &self.annotation_pixels_pending,
            receipt: &self.annotation_pixels_receipt,
            control_receipt: &self.control_probe_receipt,
        }
    }
    pub(super) fn record_gallery_draw(&mut self, presentation: u64, source: u64) {
        self.gallery_drawn = Some((presentation, source));
    }
    pub(super) fn record_surface_draw(
        &mut self,
        presentation: u64,
        source: u64,
        viewer: Option<ViewerDraw>,
    ) {
        if let Some(viewer) = viewer {
            self.viewer_drawn = Some((presentation, source, viewer));
        } else {
            self.annotation_drawn = Some((presentation, source));
        }
    }
    pub(super) fn await_viewer_draw(&mut self) {
        self.viewer_drawn = None;
    }
    pub(super) fn await_annotation_sample(&mut self, baseline: u64) {
        self.annotation_sample_baseline = baseline;
    }
    pub(super) fn restart_annotation_sampling(&mut self, baseline: u64) {
        self.annotation_sample_baseline = baseline;
        self.annotation_frame_ready = None;
    }
    pub(super) fn take_control_probe(&mut self) -> Option<ControlProbe> {
        self.control_probe.take()
    }
    pub(super) fn take_annotation_probe(&mut self) -> Option<AnnotationProbe> {
        self.annotation_probe.take()
    }
    pub(super) fn begin_upscale_probe(&mut self, output: &ScenarioOutput) {
        self.upscale_pixel_owner = output.probe.clone();
        self.upscale_pixel_pending = output.receipt.clone();
    }
    pub(super) fn upscale_pending(&self) -> &Option<ProbeReceipt> {
        &self.upscale_pixel_pending
    }
    pub(super) fn rearm_upscale_probe(&mut self) {
        self.upscale_pixel_pending = None;
    }
    pub(super) fn complete_upscale_probe(
        &mut self,
        receipt: &Option<ProbeReceipt>,
        outcome: &ProbeOutcome,
    ) -> bool {
        if &self.upscale_pixel_pending != receipt {
            return false;
        }
        self.upscale_pixel_owner = None;
        if matches!(outcome, ProbeOutcome::Invalidated) {
            self.upscale_pixel_pending = None;
        }
        true
    }
    pub(super) fn complete_capability_probe(
        &mut self,
        receipt: &Option<ProbeReceipt>,
        outcome: &ProbeOutcome,
    ) -> bool {
        if &self.control_probe_receipt != receipt {
            return false;
        }
        self.control_probe_owner = None;
        if matches!(outcome, ProbeOutcome::Invalidated) {
            self.control_probe_receipt = None;
        }
        true
    }
    pub(super) fn take_annotation_completion(
        &mut self,
        revision: u64,
        receipt: &Option<ProbeReceipt>,
    ) -> bool {
        let pending = if revision == 0 {
            &mut self.control_probe_receipt
        } else {
            &mut self.annotation_pixels_pending
        };
        if &*pending != receipt {
            return false;
        }
        *pending = None;
        if revision == 0 {
            self.control_probe_owner = None;
        } else {
            self.annotation_pixels_owner = None;
        }
        true
    }
    pub(super) fn accept_annotation_pixels(
        &mut self,
        revision: u64,
        receipt: Option<ProbeReceipt>,
    ) {
        if revision == 0 {
            self.control_probe_receipt = receipt;
        } else {
            self.annotation_pixels_receipt = receipt;
        }
    }
}

pub(crate) fn report_surface_draw(
    control: &'static str,
    revision: u64,
    source_revision: u64,
    redraw: bool,
    content_width: u32,
    content_height: u32,
    count: u64,
    viewer: ViewerDraw,
) {
    reporting::emit(|sink| {
        sink.record(
            "integration.surface_draw",
            control,
            if redraw { "redraw" } else { "draw" },
            [
                revision as f64,
                source_revision as f64,
                content_width as f64,
                content_height as f64,
            ],
        )
    });
    if control == explore::DETAIL_WORKSPACE_ID {
        reporting::emit(|sink| {
            sink.record(
                "integration.viewer_sample",
                control,
                "actual-draw",
                [
                    viewer.crop[2] as f64,
                    viewer.crop[3] as f64,
                    viewer.image.width as f64,
                    viewer.image.height as f64,
                ],
            )
        });
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            let identity = (revision, source_revision, viewer);
            if observer.viewer != Some(identity)
                && let Some(output) = observer.output_for(control)
            {
                if output
                    .try_send(Message::SurfaceDrawn {
                        presentation_revision: revision,
                        source_revision,
                        viewer: Some(viewer),
                    })
                    .is_ok()
                {
                    observer.viewer = Some(identity);
                }
            }
        });
    }
    reporting::emit(|sink| {
        sink.record(
            "integration.surface_draw_ordinal",
            control,
            if redraw { "redraw" } else { "draw" },
            [
                revision as f64,
                source_revision as f64,
                count as f64,
                if redraw { 1.0 } else { 0.0 },
            ],
        )
    });
    if control == EXPLORE_GALLERY {
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            let identity = (revision, source_revision);
            if observer.gallery != Some(identity)
                && observer.output_for(control).is_some_and(|output| {
                    output
                        .try_send(Message::GalleryDrawn {
                            presentation_revision: revision,
                            source_revision,
                        })
                        .is_ok()
                })
            {
                observer.gallery = Some(identity);
            }
        });
    }
    if control == crate::view::workspace::STABLE_ID {
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            let identity = (revision, source_revision);
            if observer.identity != identity
                && observer.output_for(control).is_some_and(|output| {
                    output
                        .try_send(Message::SurfaceDrawn {
                            presentation_revision: revision,
                            source_revision,
                            viewer: None,
                        })
                        .is_ok()
                })
            {
                observer.identity = identity;
            }
        });
    }
}

pub(super) fn observe_atlas_draw(draw: &AtlasDraw, visibility: u8, scale: f32) -> bool {
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        let frame = draw.surface.frame.expect("drawn atlas publication");
        let image = draw.image;
        let clip = draw.clip;
        let snapshot = &draw.snapshot;
        let same_rendered_draw = observer.atlas.as_ref().is_some_and(|previous| {
            previous.surface == draw.surface
                && previous.bounds == draw.bounds
                && previous.image == draw.image
                && previous.clip == draw.clip
                && previous.snapshot.overlay == draw.snapshot.overlay
        });
        if !same_rendered_draw
            && observer.output_for(EXPLORE_GALLERY).is_some_and(|output| {
                output
                    .try_send(Message::AtlasDrawn {
                        source_revision: frame.content_sequence,
                        visibility,
                        clipped_top: clip.y - image.y,
                        clipped_bottom: image.y + image.height - clip.y - clip.height,
                        row_extent: image.width / snapshot.viewport.columns.max(1) as f32 / scale,
                        receipt: draw.clone(),
                    })
                    .is_ok()
            })
        {
            observer.atlas = Some(draw.clone());
            true
        } else {
            false
        }
    })
}
pub(super) fn scenario_output() -> Option<ScenarioOutput> {
    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.clone())
}
#[cfg(target_arch = "wasm32")]
pub(super) fn observer_generation() -> u64 {
    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation)
}
pub(super) fn current_fps_draw() -> Option<reporting::FpsEvidence> {
    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().fps_draw)
}
pub(super) fn rearm_fps_sampling() {
    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().fps_sample = None);
}
pub(super) fn rearm_viewer_observation() {
    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().viewer = None);
}
pub(super) fn invalidate_atlas_observation() {
    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().atlas = None);
}
pub(super) fn complete_atlas_probe(composition: bool) {
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        if composition {
            observer.atlas_composition_owner = None;
        } else {
            observer.atlas_pixels_owner = None;
        }
    });
}

#[cfg(test)]
pub(in crate::integration_control) mod tests;
