use crate::generated::FeatureId;
use crate::integration_control::pixel_checks::{
    AtlasDraw, ProbeOutcome, atlas_pixel_rectangles, displayed_detail, pixel_fixture_enabled,
    sampleable_presentation,
};
use crate::integration_control::probe::{
    SURFACE_DRAW_OBSERVER, ScenarioOutput, SurfaceDrawSubscription, ViewerDraw, current_receipt,
    probe_output, record_probe_draw, report_surface_draw,
};
use crate::integration_control::{
    ANNOTATION_SURFACE, Controller, DRIVER_ENABLED, EXPLORE_GALLERY, Message, Phase,
    annotation_checks, initialize_reporting, notify_driver_draw, reporting, reporting_enabled,
    retained,
};
use crate::view::explore;
use iced::Rectangle;
pub(crate) struct ProbeFixture {
    pub(crate) controller: Controller,
    pub(crate) receiver: iced::futures::channel::mpsc::Receiver<Message>,
    pub(in crate::integration_control) surface: crate::presentation_surface::Surface,
    pub(in crate::integration_control) bounds: Rectangle,
}

impl ProbeFixture {
    pub(crate) fn new(scenario: &str) -> Self {
        initialize_reporting(true, true);
        let controller = Controller::new(
            true,
            false,
            String::new(),
            String::new(),
            "512".into(),
            scenario.into(),
        );
        let (sender, receiver) = iced::futures::channel::mpsc::channel(8);
        SURFACE_DRAW_OBSERVER.with(|observer| {
            observer.borrow_mut().output =
                Some(ScenarioOutput::new(controller.driver.generation, sender))
        });
        let (_, frame) = crate::view_model::test_support::explore_presentation();
        let surface = crate::view_model::test_support::physical_surface(frame);
        Self {
            controller,
            receiver,
            surface,
            bounds: Rectangle::new(iced::Point::ORIGIN, iced::Size::new(640.0, 480.0)),
        }
    }
}

impl Drop for ProbeFixture {
    fn drop(&mut self) {
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = None);
        initialize_reporting(false, false);
    }
}

impl ProbeFixture {
    pub(in crate::integration_control) fn fps_draw_output(
        &self,
        evidence: reporting::FpsEvidence,
    ) -> ScenarioOutput {
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            observer.fps_draw = Some(evidence);
            observer.output_for(EXPLORE_GALLERY).unwrap().clone()
        })
    }

    pub(in crate::integration_control) fn invalidate_fps_draw(&self, change: usize) {
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            let draw = observer.fps_draw.as_mut().unwrap();
            if change == 2 {
                draw.bounds.x += 1.0;
            } else {
                draw.frames += 1;
            }
        });
    }
}

#[test]
fn first_native_gallery_draw_reports_placeholders_and_retains_pixels_in_enabled_and_quiet_modes() {
    use crate::presentation_surface as surface;
    use iced::advanced::{
        Layout, layout,
        renderer::{Headless, Renderer as _},
        widget,
    };
    for enabled in [false, true] {
        surface::reset_test_releases();
        surface::initialize_diagnostics(enabled, false);
        let mut fixture = ProbeFixture::new("atlas");
        initialize_reporting(enabled, false);
        while fixture.receiver.try_recv().is_ok() {}
        let frame = fixture.surface.frame.unwrap();
        let mut snapshot = crate::view_model::test_support::explore_snapshot();
        snapshot.mode = crate::generated::ExploreMode::Gallery;
        snapshot.frame = crate::view_model::test_support::visual_frame(
            crate::generated::PresentationSourceKind::Explore,
            frame.content_sequence,
        );
        snapshot.dataset.identity = 1;
        snapshot.viewport.columns = 4;
        snapshot.viewport.rowcount = 3;
        snapshot.viewport.extent = snapshot.frame.extent.clone();
        snapshot.order.matchingcount = 12;
        snapshot.order.visibleindices = (0..12).collect();
        snapshot.gallery.slots = vec![false; 12];
        crate::view_model::test_support::gallery_layout(&mut snapshot);
        surface::metadata::install_explore(frame, &snapshot);
        // Only the graphics receipt and its paired metadata reach the renderer.
        // No application snapshot is installed or reconciled before either draw.
        drop(snapshot);
        assert!(surface::accept_publication(frame));
        surface::authorize_draw(Some(frame));
        surface::complete_sample(frame);
        let received = surface::metadata::surface(frame).unwrap();
        let _renderer_cleanup = surface::TestRendererCleanup;
        let mut renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
            Default::default(),
            Some("wgpu"),
        ))
        .expect("first native gallery draw acceptance requires the container GPU backend");
        let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(128.0, 96.0));
        let viewport =
            iced::widget::shader::Viewport::with_physical_size(iced::Size::new(128, 96), 1.0);
        let node = layout::Node::new(bounds.size());
        let theme = crate::fluent_theme::app_theme(false);
        let style = iced::advanced::renderer::Style::default();
        let mut first_pixels = None;
        for requested in [received, received, surface::Surface::empty(), received] {
            let program = surface::Program::<()> {
                show_fps: false,
                input: None,
                local: None,
                publish: None,
                surface: requested,
                placement: if requested.valid() {
                    surface::Placement::GalleryGrid {
                        columns: 4,
                        rows: 3,
                        row_capacity: 3,
                        row_origin: 0,
                        first_row: 0,
                    }
                } else {
                    surface::Placement::Contain
                },
                control_id: EXPLORE_GALLERY,
            };
            let mut element: crate::fluent_theme::Element<'_, ()> =
                iced::widget::shader(program).width(128).height(96).into();
            let mut tree = widget::Tree::new(&element);
            tree.diff(element.as_widget_mut());
            renderer.reset(bounds);
            element.as_widget().draw(
                &tree,
                &mut renderer,
                &theme,
                &style,
                Layout::new(&node),
                iced::mouse::Cursor::Unavailable,
                &bounds,
            );
            let pixels = renderer.screenshot(&viewport, iced::Color::WHITE);
            if requested.valid() {
                assert_eq!(
                    &pixels[(16 * 128 + 16) * 4..(16 * 128 + 16) * 4 + 3],
                    &[0, 0, 0]
                );
                if let Some(first) = &first_pixels {
                    assert_eq!(&pixels, first);
                } else {
                    first_pixels = Some(pixels);
                }
            } else {
                // An input-only workspace does not sample another component's
                // retained image. Returning to its image preserves custody.
                assert!(pixels.iter().all(|channel| *channel == 255));
            }
            let (displayed, metadata) = surface::gallery::displayed().unwrap();
            assert_eq!(displayed.frame, Some(frame));
            assert_eq!(metadata.gallery.slots, vec![false; 12]);
            assert!(surface::test_releases().is_empty());
        }
        let mut atlas_drawn = false;
        let mut gallery_drawn = false;
        while let Ok(message) = fixture.receiver.try_recv() {
            assert!(fixture.controller.probes.accepts_message(
                &fixture.controller.driver,
                &fixture.controller.pixel_checks,
                &message
            ));
            if let Message::Scoped { message, .. } = &message {
                match message.as_ref() {
                    Message::AtlasDrawn { receipt, .. } => {
                        assert_eq!(receipt.surface.frame, Some(frame));
                        assert_eq!(receipt.snapshot.gallery.slots, vec![false; 12]);
                        assert_eq!(receipt.bounds, bounds);
                        atlas_drawn = true;
                    }
                    Message::GalleryDrawn {
                        presentation_revision,
                        source_revision,
                    } => {
                        assert_eq!(*presentation_revision, frame.presentation_revision);
                        assert_eq!(*source_revision, frame.content_sequence);
                        gallery_drawn = true;
                    }
                    _ => {}
                }
            }
        }
        assert_eq!(atlas_drawn, enabled);
        assert_eq!(gallery_drawn, enabled);
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let observer = observer.borrow();
            assert_eq!(observer.receipts.is_empty(), !enabled);
            assert_eq!(observer.atlas.is_some(), enabled);
        });
        surface::retire_samples();
        surface::retire_publication(frame);
        surface::discard_sample(frame);
        assert_eq!(surface::test_releases(), vec![frame]);
        surface::initialize_diagnostics(false, false);
    }
}

#[test]
fn quiet_driver_and_disabled_frontend_collect_no_probe_state() {
    initialize_reporting(false, false);
    let mut driver = Controller::new(
        true,
        false,
        String::new(),
        String::new(),
        String::new(),
        String::new(),
    );
    assert!(driver.driver.running());
    assert!(DRIVER_ENABLED.with(std::cell::Cell::get));
    assert!(!reporting_enabled());
    assert!(!pixel_fixture_enabled());
    assert!(driver.driver.reporting.state_is_absent());
    let (mut model, frame) = crate::view_model::test_support::explore_presentation();
    let mut surface = crate::view_model::test_support::physical_surface(frame);
    surface.viewer_identity = model.explore.snapshot.as_ref().and_then(|snapshot| {
        snapshot
            .selectedimage
            .map(|image| (snapshot.dataset.identity, u64::from(image)))
    });
    let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(640.0, 480.0));
    record_probe_draw(EXPLORE_GALLERY, surface, bounds, bounds, bounds);
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let observer = observer.borrow();
        assert!(observer.receipts.is_empty());
        assert!(observer.output.is_none());
        assert!(observer.subscription.is_none());
        assert_eq!(observer.identity, (0, 0));
    });
    let snapshot = model.explore.snapshot.as_mut().unwrap();
    snapshot.dataset.imagewidth = frame.content_width;
    snapshot.dataset.imageheight = frame.content_height;
    snapshot.detail.showoriginaldimensions = false;
    driver.driver.viewer_scenario = "quiet".into();
    driver.driver.phase = Phase::AwaitDetail(0);
    let settings = crate::view::settings::SettingsModel::default();
    let router = crate::view::router::Router::default();
    assert!(
        sampleable_presentation(
            Some(frame),
            crate::generated::PresentationSourceKind::Explore,
            frame.content_sequence,
        )
        .is_some()
    );
    assert!(displayed_detail(Some(surface), model.explore.snapshot.as_ref().unwrap()).is_none());
    assert!(crate::presentation_surface::accept_publication(frame));
    assert!(displayed_detail(Some(surface), model.explore.snapshot.as_ref().unwrap()).is_some());
    crate::presentation_surface::clear_drawn_detail();
    let stale = crate::view_model::test_support::physical_surface(
        crate::presentation_surface::FrameReady {
            content_sequence: frame.content_sequence + 1,
            ..frame
        },
    );
    let crop = surface.content_region();
    for (draw, expected_ready) in [
        (None, false),
        (Some((stale, crop)), false),
        (
            Some((
                surface,
                [1, 0, frame.content_width - 1, frame.content_height],
            )),
            false,
        ),
        (Some((surface, crop)), true),
    ] {
        if let Some((surface, crop)) = draw {
            crate::presentation_surface::record_drawn_detail(surface, crop);
        }
        drop(driver.advance(
            &model,
            &settings,
            1.0,
            &router,
            FeatureId::Explore,
            Some(crate::view_model::test_support::physical_surface(frame)),
        ));
        assert_eq!(driver.widgets.location_pending(), expected_ready);
        assert_eq!(
            driver.driver.phase,
            if expected_ready {
                Phase::OpenAnnotation
            } else {
                Phase::AwaitDetail(0)
            }
        );
        assert!(model.error.is_none());
        assert!(driver.driver.reporting.state_is_absent());
        SURFACE_DRAW_OBSERVER.with(|observer| assert!(observer.borrow().receipts.is_empty()));
    }
    crate::presentation_surface::clear_drawn_detail();
    driver.driver.phase = Phase::Complete;
    driver
        .reset_scenario(String::new(), String::new(), String::new(), String::new())
        .unwrap();
    assert!(driver.driver.running());
    assert!(DRIVER_ENABLED.with(std::cell::Cell::get));
    assert!(!reporting_enabled());
    assert!(driver.driver.reporting.state_is_absent());
    let generation = SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation);
    let disabled = Controller::new(
        false,
        false,
        String::new(),
        String::new(),
        String::new(),
        String::new(),
    );
    assert!(!disabled.driver.running());
    assert!(!DRIVER_ENABLED.with(std::cell::Cell::get));
    notify_driver_draw(
        EXPLORE_GALLERY,
        frame.content_sequence,
        frame.presentation_revision,
    );
    assert_eq!(
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation),
        generation
    );
}

#[test]
fn scenario_reset_isolates_queued_messages_geometry_and_obsolete_subscription_teardown() {
    initialize_reporting(true, true);
    let mut controller = Controller::new(
        true,
        false,
        "old-source".into(),
        "old-output".into(),
        "384".into(),
        "atlas".into(),
    );
    let (sender, mut receiver) = iced::futures::channel::mpsc::channel(8);
    let old_subscription = std::sync::Arc::new(());
    let mut old_output = ScenarioOutput::new(controller.driver.generation, sender);
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        observer.subscription = Some(old_subscription.clone());
        observer.output = Some(old_output.clone());
        observer.identity = (3, 4);
    });
    let (_, frame) = crate::view_model::test_support::explore_presentation();
    let surface = crate::view_model::test_support::physical_surface(frame);
    let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(640.0, 480.0));
    record_probe_draw(EXPLORE_GALLERY, surface, bounds, bounds, bounds);
    let old_receipt = current_receipt(EXPLORE_GALLERY).unwrap();
    old_output
        .try_send(Message::Located {
            control: EXPLORE_GALLERY.into(),
            bounds,
        })
        .unwrap();
    record_probe_draw(
        crate::view::workspace::STABLE_ID,
        surface,
        bounds,
        bounds,
        bounds,
    );
    let viewer = ViewerDraw {
        crop: surface.content_region(),
        container: bounds,
        image: bounds,
        fit_revision: 0,
    };
    report_surface_draw(
        crate::view::workspace::STABLE_ID,
        5,
        1,
        false,
        640,
        480,
        1,
        viewer,
    );
    controller.widgets.begin_location();
    controller
        .driver
        .reporting
        .observe(|reporting| reporting.reported_style_bits = 7);
    controller.probes.annotation_pixels_receipt = Some(old_receipt.clone());
    controller.probes.upscale_pixel_pending = Some(old_receipt);
    retained::tests::replace_scenario_with_retained_baseline(&mut controller);
    assert!(!controller.widgets.location_pending());
    assert!(controller.probes.upscale_pixel_pending.is_none());
    assert!(controller.probes.annotation_pixels_receipt.is_none());
    controller
        .driver
        .reporting
        .observe(|reporting| assert_eq!(reporting.reported_style_bits, 0));
    assert!(current_receipt(EXPLORE_GALLERY).is_none());
    assert_eq!(
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().identity),
        (0, 0)
    );
    controller.widgets.begin_location(); // Same widget may already be armed in the replacement.
    while let Ok(message) = receiver.try_recv() {
        assert!(controller.update(message).is_none());
    }
    assert!(controller.widgets.location_pending());
    assert!(controller.probes.annotation_drawn.is_none());
    // A late result retains the original sender/generation even after reset.
    old_output
        .try_send(Message::SurfaceDrawn {
            presentation_revision: 5,
            source_revision: 1,
            viewer: None,
        })
        .unwrap();
    controller.update(receiver.try_recv().unwrap());
    assert!(controller.probes.annotation_drawn.is_none());
    for control in [
        EXPLORE_GALLERY,
        explore::DETAIL_WORKSPACE_ID,
        crate::view::workspace::STABLE_ID,
    ] {
        assert!(
            probe_output(control).is_none(),
            "no physical receipt cannot schedule a probe"
        );
        report_surface_draw(control, 5, 1, false, 640, 480, 1, viewer);
        assert!(
            receiver.try_recv().is_err(),
            "no physical receipt cannot enqueue a draw"
        );
        record_probe_draw(control, surface, bounds, bounds, bounds);
        report_surface_draw(control, 5, 1, false, 640, 480, 1, viewer);
        let queued = receiver.try_recv().unwrap();
        let moved = Rectangle { x: 17.0, ..bounds };
        record_probe_draw(control, surface, bounds, moved, bounds);
        assert!(!controller.probes.accepts_message(
            &controller.driver,
            &controller.pixel_checks,
            &queued
        ));
        controller.update(queued);
        assert!(controller.probes.annotation_drawn.is_none());
        assert!(controller.probes.viewer_drawn.is_none());
        assert!(controller.probes.gallery_drawn.is_none());
        // Unchanged revisions and viewer fields must not suppress a new geometry receipt.
        report_surface_draw(control, 5, 1, true, 640, 480, 2, viewer);
        let queued = receiver.try_recv().unwrap();
        assert!(controller.probes.accepts_message(
            &controller.driver,
            &controller.pixel_checks,
            &queued
        ));
        controller.update(queued);
        match control {
            EXPLORE_GALLERY => assert_eq!(controller.probes.gallery_drawn.take(), Some((5, 1))),
            explore::DETAIL_WORKSPACE_ID => {
                assert_eq!(controller.probes.viewer_drawn.take(), Some((5, 1, viewer)))
            }
            _ => assert_eq!(controller.probes.annotation_drawn.take(), Some((5, 1))),
        }
    }
    let snapshot = std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(
        &crate::view_model::test_support::explore_snapshot(),
    ));
    let draw = AtlasDraw {
        surface,
        snapshot,
        bounds,
        image: bounds,
        clip: bounds,
    };
    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().receipts.remove(EXPLORE_GALLERY));
    retained::tests::check_reset_atlas_callbacks(&mut controller, &mut receiver, draw);
    old_output.generation = controller.driver.generation;
    old_output
        .try_send(Message::SurfaceDrawn {
            presentation_revision: 5,
            source_revision: 1,
            viewer: None,
        })
        .unwrap();
    assert!(
        !controller.probes.accepts_message(
            &controller.driver,
            &controller.pixel_checks,
            &receiver.try_recv().unwrap()
        ),
        "physical messages cannot use a generation-only output"
    );
    let replacement_subscription = std::sync::Arc::new(());
    SURFACE_DRAW_OBSERVER.with(|observer| {
        observer.borrow_mut().subscription = Some(replacement_subscription.clone())
    });
    drop(SurfaceDrawSubscription(old_subscription));
    assert!(SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.is_some()));
    assert!(current_receipt(EXPLORE_GALLERY).is_some());
    drop(SurfaceDrawSubscription(replacement_subscription));
    assert!(SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.is_none()));
    controller.driver.phase = Phase::Failed;
    assert!(
        controller
            .reset_scenario(String::new(), String::new(), String::new(), String::new())
            .is_err()
    );
    initialize_reporting(false, false);
}

#[test]
fn annotation_and_upscale_consumers_retire_invalidations_without_pixel_evidence() {
    for consumer in 0..4 {
        let mut fixture = ProbeFixture::new("copy");
        let surface = fixture.surface;
        let bounds = fixture.bounds;
        let frame = surface.frame.unwrap();
        let (controller, receiver) = (&mut fixture.controller, &mut fixture.receiver);
        let control = if consumer == 3 {
            explore::DETAIL_WORKSPACE_ID
        } else {
            "workflow.visual.workspace"
        };
        record_probe_draw(control, surface, bounds, bounds, bounds);
        let arm = |controller: &mut Controller, image| match consumer {
            0 | 1 => {
                controller.driver.phase = if consumer == 0 {
                    Phase::CopyCapabilityWait
                } else {
                    Phase::CopySwatchWait
                };
                assert!(annotation_checks::tests::prepare_control_probe(controller));
                controller.probes.take_control_probe().unwrap().output
            }
            2 => {
                controller.driver.phase = Phase::CopyProductWait;
                assert!(controller.probes.prepare_annotation_probe(
                    &controller.widgets,
                    frame.content_sequence,
                    frame.presentation_revision,
                    [frame.content_width, frame.content_height],
                    vec![1.0; 7]
                ));
                controller.probes.take_annotation_probe().unwrap().output
            }
            _ => {
                controller.driver.phase = Phase::AwaitExploreReady;
                controller
                    .retained
                    .prepare_upscale_probe(
                        &mut controller.probes,
                        image,
                        frame.content_sequence,
                        frame.presentation_revision,
                    )
                    .unwrap()
            }
        };
        let message = |outcome| match consumer {
            0 => Message::AnnotationControlPixels { outcome },
            1 => Message::AnnotationPixels {
                revision: 0,
                outcome,
            },
            2 => Message::AnnotationPixels {
                revision: frame.content_sequence,
                outcome,
            },
            _ => Message::UpscalePixels {
                source: frame.content_sequence,
                presentation: frame.presentation_revision,
                outcome,
            },
        };
        let mut old = arm(controller, bounds);
        let phase = controller.driver.phase.clone();
        // JavaScript CSS/backing replacement may invalidate while this exact
        // Rust receipt is still current. Exercise the real message consumer.
        old.try_send(message(ProbeOutcome::Invalidated)).unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert_eq!(controller.driver.phase, phase);
        assert!(controller.probes.annotation_pixels_pending.is_none());
        assert!(controller.probes.control_probe_receipt.is_none());
        assert!(controller.probes.upscale_pixel_pending.is_none());
        annotation_checks::tests::assert_copy_completion(controller, false, false);
        assert!(
            controller.probes.annotation_pixels_receipt.is_none()
                && controller.retained.upscale_observation().pixels.is_none()
        );

        let mut same_frame = arm(controller, bounds);
        old.try_send(message(ProbeOutcome::Invalidated)).unwrap();
        let stale = receiver.try_recv().unwrap();
        assert!(
            !controller.probes.accepts_message(
                &controller.driver,
                &controller.pixel_checks,
                &stale
            ),
            "a new request can own the same physical receipt"
        );
        controller.update(stale);
        same_frame
            .try_send(message(ProbeOutcome::Invalidated))
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert_eq!(controller.driver.phase, phase);

        let moved = Rectangle { x: 17.0, ..bounds };
        record_probe_draw(control, surface, bounds, moved, bounds);
        let mut replacement = arm(controller, moved);
        for outcome in [
            ProbeOutcome::Invalidated,
            ProbeOutcome::Failed,
            ProbeOutcome::Observed(1, 1),
        ] {
            old.try_send(message(outcome)).unwrap();
            let stale = receiver.try_recv().unwrap();
            assert!(!controller.probes.accepts_message(
                &controller.driver,
                &controller.pixel_checks,
                &stale
            ));
            controller.update(stale);
        }
        assert_eq!(controller.driver.phase, phase);
        let observed = &controller.probes;
        let pending = if consumer == 3 {
            &observed.upscale_pixel_pending
        } else if consumer == 2 {
            &observed.annotation_pixels_pending
        } else {
            &observed.control_probe_receipt
        };
        assert_eq!(*pending, replacement.receipt);
        replacement
            .try_send(message(ProbeOutcome::Observed(1, 1)))
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        match consumer {
            0 => annotation_checks::tests::assert_copy_completion(controller, true, false),
            1 => annotation_checks::tests::assert_copy_completion(controller, false, true),
            2 => assert_eq!(
                controller.probes.annotation_pixels_receipt,
                replacement.receipt
            ),
            _ => assert_eq!(
                controller.retained.upscale_observation().pixels,
                Some((frame.content_sequence, frame.presentation_revision, 1, 1))
            ),
        }
        // A new current observation must retain both measured and adapter
        // failures. Upscale's wait phase owns checksum/color validation.
        if consumer == 2 {
            controller.probes.annotation_pixels_pending = None;
        }
        let mut current = arm(controller, moved);
        current
            .try_send(message(ProbeOutcome::Observed(1, 0)))
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        if consumer == 3 {
            assert_eq!(
                controller.retained.upscale_observation().pixels,
                Some((frame.content_sequence, frame.presentation_revision, 1, 0))
            );
            let mut current = arm(controller, moved);
            current.try_send(message(ProbeOutcome::Failed)).unwrap();
            controller.update(receiver.try_recv().unwrap());
        }
        assert_eq!(controller.driver.phase, Phase::Failed);
        let mut malformed = arm(controller, moved);
        malformed
            .try_send(message(ProbeOutcome::decode(
                Some("observed"),
                [None, Some(1.0)],
            )))
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert_eq!(controller.driver.phase, Phase::Failed);
    }
}

#[test]
fn probe_preparation_keeps_original_frame_through_widget_location() {
    let mut fixture = ProbeFixture::new("copy");
    let surface = fixture.surface;
    let bounds = fixture.bounds;
    let frame = surface.frame.unwrap();
    let controller = &mut fixture.controller;
    for swatch in [false, true] {
        record_probe_draw("workflow.visual.workspace", surface, bounds, bounds, bounds);
        controller.driver.phase = if swatch {
            Phase::CopyCapability
        } else {
            Phase::CopyProductWait
        };
        if swatch {
            annotation_checks::tests::prepare_available_swatch(controller);
            assert!(annotation_checks::tests::prepare_control_probe(controller));
            let prepared = controller.probes.control_probe.as_ref().unwrap();
            assert_eq!(prepared.color, [48.0, 80.0, 112.0]);
            assert!(prepared.available);
        } else {
            for (source, presentation, extent) in [
                (
                    frame.content_sequence + 1,
                    frame.presentation_revision,
                    [frame.content_width, frame.content_height],
                ),
                (
                    frame.content_sequence,
                    frame.presentation_revision + 1,
                    [frame.content_width, frame.content_height],
                ),
                (
                    frame.content_sequence,
                    frame.presentation_revision,
                    [frame.content_width + 1, frame.content_height],
                ),
            ] {
                assert!(!controller.probes.prepare_annotation_probe(
                    &controller.widgets,
                    source,
                    presentation,
                    extent,
                    vec![1.0; 7],
                ));
                assert!(controller.probes.annotation_probe.is_none());
                assert!(controller.probes.annotation_pixels_pending.is_none());
            }
            assert!(controller.probes.prepare_annotation_probe(
                &controller.widgets,
                frame.content_sequence,
                frame.presentation_revision,
                [frame.content_width, frame.content_height],
                vec![1.0; 7]
            ));
            let prepared = controller.probes.annotation_probe.as_ref().unwrap();
            assert_eq!(prepared.source, frame.content_sequence);
            assert_eq!(prepared.presentation, frame.presentation_revision);
            assert_eq!(prepared.extent, [frame.content_width, frame.content_height]);
            assert_eq!(prepared.pixels, vec![1.0; 7]);
        }
        controller.widgets.begin_location();
        let moved = Rectangle { x: 17.0, ..bounds };
        record_probe_draw("workflow.visual.workspace", surface, bounds, moved, bounds);
        assert!(
            !controller.probes.prepare_annotation_probe(
                &controller.widgets,
                999,
                999,
                [1, 1],
                Vec::new()
            ),
            "pending location cannot be overwritten"
        );
        assert!(!annotation_checks::tests::prepare_control_probe(controller));
        let phase = controller.driver.phase.clone();
        // The obsolete location cannot validate bounds or stamp a new
        // output onto the saved old frame. Normal advance can now rearm.
        controller.update(Message::Located {
            control: ANNOTATION_SURFACE.into(),
            bounds: Rectangle::default(),
        });
        assert_eq!(controller.driver.phase, phase);
        assert!(!controller.widgets.location_pending());
        assert!(
            controller.probes.annotation_probe.is_none()
                && controller.probes.control_probe.is_none()
        );
        assert!(
            controller.probes.annotation_pixels_pending.is_none()
                && controller.probes.control_probe_receipt.is_none()
        );
    }
}

pub(in crate::integration_control) fn atlas_observation_matches(
    expected: Option<&AtlasDraw>,
) -> bool {
    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().atlas.as_ref() == expected)
}

pub(in crate::integration_control) fn check_empty_atlas_draws(
    driver: &mut Controller,
    model: &mut crate::view_model::ApplicationModel,
    frame: crate::presentation_surface::FrameReady,
    source: u64,
    drive: impl Fn(&mut Controller, &crate::view_model::ApplicationModel),
) {
    let current_draw = (frame.presentation_revision, source);
    for (logical_matches, displayed_matches, drawn, ready) in [
        (0, None, Some(current_draw), false),
        (0, Some(2), Some(current_draw), false),
        (2, Some(0), Some(current_draw), false),
        (0, Some(0), None, false),
        (
            0,
            Some(0),
            Some((frame.presentation_revision - 1, source)),
            false,
        ),
        (0, Some(0), Some(current_draw), true),
    ] {
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.order.matchingcount = logical_matches;
        crate::presentation_surface::metadata::retire(frame);
        if let Some(matching) = displayed_matches {
            let mut product = snapshot.clone();
            product.mode = crate::generated::ExploreMode::Gallery;
            product.order.matchingcount = matching;
            product.viewport.columns = 4;
            product.viewport.rowcount = 3;
            product.viewport.firstrow = 0;
            product.viewport.extent = product.frame.extent.clone();
            crate::view_model::test_support::gallery_layout(&mut product);
            crate::presentation_surface::metadata::install_explore(frame, &product);
        }
        driver.probes.gallery_drawn = drawn;
        driver.driver.phase = Phase::AwaitAtlasEmpty;
        drive(driver, model);
        assert_eq!(
            driver.driver.phase,
            if ready {
                Phase::AtlasEmpty
            } else {
                Phase::AwaitAtlasEmpty
            }
        );
        assert_eq!(driver.widgets.location_pending(), ready);
        driver.widgets.location_completed();
    }
}

#[test]
fn atlas_pixel_evidence_covers_only_ready_interiors_inside_the_real_clip() {
    let mut snapshot = crate::view_model::test_support::explore_snapshot();
    snapshot.viewport.columns = 2;
    snapshot.viewport.rowcount = 2;
    snapshot.gallery.slots = vec![true, true, false, true];
    let mut draw = AtlasDraw {
        surface: crate::presentation_surface::Surface {
            high: 1,
            low: 2,
            width: 200,
            height: 200,
            frame: None,
            crop: None,
            viewer_identity: None,
            fit_revision: 0,
        },
        snapshot: std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(&snapshot)),
        bounds: Rectangle {
            x: 10.0,
            y: 20.0,
            width: 200.0,
            height: 200.0,
        },
        image: Rectangle {
            x: 10.0,
            y: 20.0,
            width: 200.0,
            height: 200.0,
        },
        clip: Rectangle {
            x: 10.0,
            y: 20.0,
            width: 200.0,
            height: 200.0,
        },
    };
    assert_eq!(atlas_pixel_rectangles(&draw).len(), 12);
    draw.clip = Rectangle {
        x: 10.0,
        y: 120.0,
        width: 100.0,
        height: 100.0,
    };
    assert!(atlas_pixel_rectangles(&draw).is_empty());
    draw.clip = Rectangle {
        x: 150.0,
        y: 150.0,
        width: 50.0,
        height: 30.0,
    };
    let rectangles = atlas_pixel_rectangles(&draw);
    assert_eq!(rectangles.len(), 4);
    for (actual, expected) in rectangles.iter().zip([150.0, 150.0, 40.0, 30.0]) {
        assert!((actual - expected).abs() < 0.001);
    }
    initialize_reporting(true, true);
    let mut controller = Controller::new(
        true,
        false,
        String::new(),
        String::new(),
        String::new(),
        String::new(),
    );
    controller.driver.phase = Phase::AwaitExploreReady;
    let (sender, mut receiver) = iced::futures::channel::mpsc::channel(8);
    SURFACE_DRAW_OBSERVER.with(|observer| {
        observer.borrow_mut().output =
            Some(ScenarioOutput::new(controller.driver.generation, sender))
    });
    retained::tests::check_atlas_pixel_callbacks(&mut controller, &mut receiver, draw);
    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = None);
    initialize_reporting(false, false);
}
