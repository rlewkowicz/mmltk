use crate::generated::FeatureId;
use crate::integration_control::pixel_checks::tests::fps_settings;
use crate::integration_control::pixel_checks::{AtlasDraw, ProbeOutcome, report_atlas_draw};
use crate::integration_control::probe::{atlas_probe_output, record_probe_draw};
use crate::integration_control::retained::{
    cold_gallery_scroll_offset, explore_integer_id, explore_scenario_preparation,
};
use crate::integration_control::tests::advance_receipt;
use crate::integration_control::{
    Controller, EXPLORE_GALLERY, Message, Phase, ProbeFixture, initialize_reporting, probe,
};
use crate::view_model::{ApplicationModel, ConnectionState};
use iced::Rectangle;
#[test]
fn numeric_seed_round_trip_reuses_one_distinct_exact_target_for_typing_and_paste() {
    for baseline in [0, 73, (1_u64 << 53) + 1, (1_u64 << 53) + 3, u64::MAX] {
        let mut fixture = ProbeFixture::new("square");
        let (mut model, settings) = fps_settings();
        model.connection = crate::view_model::ConnectionState::Connected;
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.ready = true;
        snapshot.busy = false;
        snapshot.revision = 17;
        snapshot.filter.shuffleseed = baseline;
        model
            .settings_snapshot
            .as_mut()
            .unwrap()
            .settingsstate
            .workflows
            .explore
            .shuffleseed = baseline;
        let driver = &mut fixture.controller;
        driver.driver.phase = Phase::ExploreNumericStart(2);
        let router = crate::view::router::Router::default();
        drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Explore, None));
        let target = driver.retained.explore_integer_target;
        assert_ne!(target, baseline);
        assert!(target > (1_u64 << 53));
        assert_eq!(target % 2, 1);
        assert_eq!(
            target,
            if baseline == (1_u64 << 53) + 1 {
                (1_u64 << 53) + 3
            } else {
                (1_u64 << 53) + 1
            }
        );
        assert_eq!(target.to_string().parse::<u64>(), Ok(target));
        driver.driver.phase = Phase::AwaitExploreNumeric { index: 2, step: 5 };
        model.explore.snapshot.as_mut().unwrap().revision += 1;
        drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Explore, None));
        assert_eq!(driver.driver.phase, Phase::AwaitExploreClipboard(18));
        assert_eq!(driver.retained.explore_integer_target, target);
        assert_eq!(driver.retained.explore_integer_baseline, baseline);
    }
}

#[test]
fn clipboard_preparation_and_reads_belong_to_the_active_numeric_round_trip() {
    let mut fixture = ProbeFixture::new("square");
    let driver = &mut fixture.controller;
    driver.driver.phase = Phase::AwaitExploreClipboard(17);
    let scoped = |generation, message| Message::Scoped {
        generation,
        receipt: None,
        message: Box::new(message),
    };
    driver.update(scoped(
        driver.driver.generation.wrapping_add(1),
        Message::NumberClipboardPrepared {
            revision: 17,
            result: Ok(()),
        },
    ));
    assert_eq!(driver.driver.phase, Phase::AwaitExploreClipboard(17));
    driver.update(scoped(
        driver.driver.generation,
        Message::NumberClipboardPrepared {
            revision: 16,
            result: Ok(()),
        },
    ));
    assert_eq!(driver.driver.phase, Phase::AwaitExploreClipboard(17));
    driver.update(scoped(
        driver.driver.generation,
        Message::NumberClipboardPrepared {
            revision: 17,
            result: Ok(()),
        },
    ));
    assert_eq!(
        driver.driver.phase,
        Phase::ExploreNumericControl { index: 2, step: 6 }
    );
    driver.driver.phase = Phase::AwaitExploreNumeric { index: 2, step: 6 };
    driver.update(scoped(
        driver.driver.generation.wrapping_add(1),
        Message::NumberPasteDelivered(false),
    ));
    driver.update(scoped(
        driver.driver.generation,
        Message::NumberPasteDelivered(true),
    ));
    assert_eq!(
        driver.driver.phase,
        Phase::AwaitExploreNumeric { index: 2, step: 6 }
    );
    driver.retained.explore_integer_target = (1_u64 << 53) + 1;
    let contents = std::sync::Arc::new(iced::clipboard::Content::Text(
        driver.retained.explore_integer_target.to_string(),
    ));
    driver.update(scoped(
        driver.driver.generation,
        Message::NumberPasteRead {
            target: explore_integer_id(0),
            result: Ok(contents.clone()),
        },
    ));
    assert!(!driver.retained.explore_paste_read);
    driver.update(scoped(
        driver.driver.generation,
        Message::NumberPasteRead {
            target: explore_integer_id(2),
            result: Ok(contents),
        },
    ));
    assert!(driver.retained.explore_paste_read);
    driver.update(scoped(
        driver.driver.generation,
        Message::NumberPasteDelivered(false),
    ));
    assert!(
        driver
            .driver
            .failure
            .contains("paste shortcut delivery failed")
    );
    driver.update(scoped(
        driver.driver.generation,
        Message::NumberClipboardPrepared {
            revision: 17,
            result: Ok(()),
        },
    ));
    assert_eq!(driver.driver.phase, Phase::Failed);
    assert!(!driver.retained.explore_paste_read);
}

#[test]
fn clipboard_rejection_preserves_the_underlying_error_and_fails_only_its_active_stage() {
    for writing in [true, false] {
        let mut fixture = ProbeFixture::new("square");
        let driver = &mut fixture.controller;
        driver.driver.phase = if writing {
            Phase::AwaitExploreClipboard(17)
        } else {
            Phase::AwaitExploreNumeric { index: 2, step: 6 }
        };
        let error = iced::clipboard::Error::Unknown {
            description: std::sync::Arc::new("clipboard permission rejected".into()),
        };
        let message = if writing {
            Message::NumberClipboardPrepared {
                revision: 17,
                result: Err(error),
            }
        } else {
            Message::NumberPasteRead {
                target: explore_integer_id(2),
                result: Err(error),
            }
        };
        driver.update(Message::Scoped {
            generation: driver.driver.generation,
            receipt: None,
            message: Box::new(message),
        });
        assert_eq!(driver.driver.phase, Phase::Failed);
        assert!(
            driver
                .driver
                .failure
                .contains("clipboard permission rejected")
        );
    }
}

#[test]
fn cold_gallery_walk_waits_for_new_complete_viewports_and_exact_held_receipts() {
    use crate::generated::{IntegrationControlKind as Kind, IntegrationControlReceipt};
    initialize_reporting(false, false);
    let mut driver = Controller::new(
        true,
        false,
        String::new(),
        String::new(),
        "512".into(),
        String::new(),
    );
    let (mut model, frame) = crate::view_model::test_support::explore_presentation();
    model.connection = ConnectionState::Connected;
    let settings = crate::view::settings::SettingsModel::default();
    let router = crate::view::router::Router::default();
    let drive = |driver: &mut Controller, model: &ApplicationModel| {
        drop(driver.advance(
            model,
            &settings,
            1.0,
            &router,
            FeatureId::Explore,
            Some(crate::view_model::test_support::physical_surface(frame)),
        ));
    };
    {
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.mode = crate::generated::ExploreMode::Gallery;
        snapshot.ready = true;
        snapshot.busy = false;
        snapshot.viewport.firstrow = 10;
        snapshot.viewport.rowcount = 2;
        snapshot.viewport.columns = 3;
        snapshot.gallery.layout.firstrow = 10;
        snapshot.gallery.layout.columns = 3;
        snapshot.gallery.layout.rowcount = 2;
        snapshot.gallery.generation = 4;
        snapshot.gallery.slots = vec![true; 6];
        snapshot.order.visibleindices = vec![17, 18, 19, 20, 21, 22];
        snapshot.order.matchingcount = 300;
    }
    driver.retained.atlas_row_extent = 160.0;
    driver.driver.phase = Phase::AwaitGalleryColdRead(10, 4);
    drive(&mut driver, &model);
    assert!(
        matches!(driver.driver.phase, Phase::AwaitGalleryColdRead(10, 4)),
        "unchanged demand cannot advance"
    );
    model.explore.snapshot.as_mut().unwrap().gallery.generation = 5;
    model.explore.snapshot.as_mut().unwrap().gallery.slots[0] = false;
    drive(&mut driver, &model);
    assert!(
        matches!(driver.driver.phase, Phase::AwaitGalleryColdRead(10, 4)),
        "pending read cannot advance"
    );
    model.explore.snapshot.as_mut().unwrap().gallery.slots[0] = true;
    drive(&mut driver, &model);
    assert!(matches!(driver.driver.phase, Phase::GalleryColdRead(17)));
    drive(&mut driver, &model);
    assert!(
        matches!(driver.driver.phase, Phase::GalleryColdRead(17)),
        "a sampleable publication must actually draw before a disjoint jump"
    );
    driver
        .probes
        .record_gallery_draw(frame.presentation_revision, frame.content_sequence - 1);
    drive(&mut driver, &model);
    assert!(matches!(driver.driver.phase, Phase::GalleryColdRead(17)));
    driver
        .probes
        .record_gallery_draw(frame.presentation_revision, frame.content_sequence);
    drive(&mut driver, &model);
    assert!(matches!(
        driver.driver.phase,
        Phase::AwaitGalleryColdRead(17, 5)
    ));
    let receipt = IntegrationControlReceipt {
        kind: Kind::GalleryReadCompletionHeld,
        sequence: 1,
        progress: 0,
        failureline: 0,
        failure: String::new(),
        readgeneration: 6,
        compiledindex: 47,
    };
    driver.receive_control(receipt.clone()).unwrap();
    drive(&mut driver, &model);
    assert!(
        matches!(driver.driver.phase, Phase::AwaitGalleryColdRead(17, 5)),
        "early receipt must retain its identity"
    );
    {
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.viewport.firstrow = 17;
        snapshot.gallery.layout.firstrow = 17;
        snapshot.gallery.generation = 6;
        snapshot.gallery.slots[0] = false;
        snapshot.order.visibleindices[0] = 47;
    }
    drive(&mut driver, &model);
    assert!(matches!(driver.driver.phase, Phase::GallerySweep));
    assert_eq!(driver.retained.gallery_completion_held, Some((6, 47)));
    assert!(driver.receive_control(receipt).is_err());
    assert!(matches!(driver.driver.phase, Phase::Failed));

    for held in [(5, 47), (6, 99)] {
        let mut mismatch = Controller::new(
            true,
            false,
            String::new(),
            String::new(),
            "512".into(),
            String::new(),
        );
        mismatch.driver.phase = Phase::AwaitGalleryColdRead(17, 5);
        mismatch.retained.gallery_completion_held = Some(held);
        drive(&mut mismatch, &model);
        assert!(
            matches!(mismatch.driver.phase, Phase::Failed),
            "stale generation or absent image cannot authorize the sweep"
        );
    }

    let viewport = &model.explore.snapshot.as_ref().unwrap().viewport;
    assert_eq!(
        cold_gallery_scroll_offset(17, viewport, 300, 160.0),
        Some(2760.0)
    );
    for extent in [0.0, -1.0, f32::NAN, f32::INFINITY] {
        assert!(cold_gallery_scroll_offset(17, viewport, 300, extent).is_none());
    }
    assert!(cold_gallery_scroll_offset(0, viewport, 300, 160.0).is_none());
    assert!(cold_gallery_scroll_offset(93, viewport, 300, 160.0).is_none());
    driver.driver.phase = Phase::GalleryColdRead(93);
    drive(&mut driver, &model);
    assert!(
        matches!(driver.driver.phase, Phase::Failed),
        "exhausted fixture is an explicit failure"
    );
}

#[test]
fn gallery_completion_controls_reject_stale_scenarios_and_reset_with_the_driver() {
    use crate::generated::{IntegrationControlKind as Kind, IntegrationControlReceipt};
    for sequence in [0, 2] {
        let mut driver = Controller::new(
            true,
            false,
            String::new(),
            String::new(),
            "512".into(),
            String::new(),
        );
        assert!(
            driver
                .receive_control(IntegrationControlReceipt {
                    kind: Kind::GalleryReadCompletionHeld,
                    sequence,
                    progress: 0,
                    failureline: 0,
                    failure: String::new(),
                    readgeneration: 7,
                    compiledindex: 47,
                })
                .is_err()
        );
    }
    let mut driver = Controller::new(
        true,
        false,
        String::new(),
        String::new(),
        "512".into(),
        String::new(),
    );
    driver.driver.phase = Phase::Complete;
    // A current physical read can settle after the UI's last step; it
    // belongs to this scenario until the native owner permits Advance.
    driver
        .receive_control(IntegrationControlReceipt {
            kind: Kind::GalleryReadCompletionHeld,
            sequence: 1,
            progress: 0,
            failureline: 0,
            failure: String::new(),
            readgeneration: 7,
            compiledindex: 47,
        })
        .unwrap();
    assert!(matches!(driver.driver.phase, Phase::Complete));
    driver
        .reset_scenario(String::new(), String::new(), "512".into(), String::new())
        .unwrap();
    assert!(driver.retained.gallery_completion_held.is_none());
}

#[test]
fn atlas_invalidation_cannot_retire_a_replacement_request_on_the_same_draw() {
    let mut fixture = ProbeFixture::new("atlas");
    let surface = fixture.surface;
    let bounds = fixture.bounds;
    let (controller, receiver) = (&mut fixture.controller, &mut fixture.receiver);
    let draw = AtlasDraw {
        surface,
        bounds,
        image: bounds,
        clip: bounds,
        snapshot: std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(
            &crate::view_model::test_support::explore_snapshot(),
        )),
    };
    record_probe_draw(EXPLORE_GALLERY, surface, bounds, bounds, bounds);
    report_atlas_draw(draw.clone(), false, 1.0);
    controller.update(receiver.try_recv().unwrap());
    let mut old_pixels = atlas_probe_output(false).unwrap();
    let mut old_composition = atlas_probe_output(true).unwrap();
    old_pixels
        .try_send(Message::AtlasPixels {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Invalidated,
        })
        .unwrap();
    controller.update(receiver.try_recv().unwrap());
    assert!(probe::tests::atlas_observation_matches(None));
    // A normal draw rearms the same physical frame after CSS/backing settles.
    report_atlas_draw(draw.clone(), false, 1.0);
    controller.update(receiver.try_recv().unwrap());
    let mut pixels = atlas_probe_output(false).unwrap();
    let mut composition = atlas_probe_output(true).unwrap();
    for (output, message) in [
        (
            &mut old_pixels,
            Message::AtlasPixels {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Invalidated,
            },
        ),
        (
            &mut old_composition,
            Message::AtlasComposition {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Invalidated,
            },
        ),
    ] {
        output.try_send(message).unwrap();
        let stale = receiver.try_recv().unwrap();
        assert!(!controller.probes.accepts_message(
            &controller.driver,
            &controller.pixel_checks,
            &stale
        ));
        controller.update(stale);
    }
    assert!(probe::tests::atlas_observation_matches(Some(&draw)));
    assert!(
        controller.retained.atlas_pixels.is_none()
            && controller.retained.atlas_composition.is_none()
    );
    pixels
        .try_send(Message::AtlasPixels {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(1, 1),
        })
        .unwrap();
    controller.update(receiver.try_recv().unwrap());
    composition
        .try_send(Message::AtlasComposition {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(13, 13),
        })
        .unwrap();
    controller.update(receiver.try_recv().unwrap());
    assert_eq!(controller.retained.atlas_pixels, Some(draw.clone()));
    assert_eq!(controller.retained.atlas_composition, Some(draw));
}

#[test]
fn retained_workflows_require_the_unique_settled_control_owner() {
    initialize_reporting(false, false);
    let mut driver = Controller::new(
        true,
        false,
        "mixed-source".into(),
        "mixed-output".into(),
        "512".into(),
        "retained".into(),
    );
    driver.configure_session("retained", "square-source".into(), "square-output".into());
    assert_eq!(driver.driver.viewer_scenario, "square");
    assert_eq!(driver.driver.resolution, "384");
    assert_eq!(driver.driver.dataset_source, "square-source");
    let generation = driver.driver.generation;
    let advance = advance_receipt(2);
    driver.driver.phase = Phase::Complete;
    driver.driver.control_phase = Some(Phase::Complete);
    driver.receive_control(advance.clone()).unwrap();
    assert_ne!(driver.driver.generation, generation);
    assert_eq!(driver.driver.control_sequence, 2);
    assert_eq!(driver.driver.dataset_source, "mixed-source");
    assert_eq!(driver.driver.compiled_directory, "mixed-output");
    assert_eq!(driver.driver.resolution, "512");
    assert!(driver.driver.viewer_scenario.is_empty());
    assert!(!driver.driver.reuse_compiled);
    // A retained paired Detail owns Close independently of logical delivery.
    crate::presentation_surface::reset_test_releases();
    let (mut model, frame) = crate::view_model::test_support::explore_presentation();
    model.connection = ConnectionState::Connected;
    model.window_width = 1200;
    model.window_height = 800;
    model
        .settings_snapshot
        .as_mut()
        .unwrap()
        .exploresource
        .available = true;
    let settings = crate::view::settings::SettingsModel::default();
    let mut router = crate::view::router::Router::default();
    router.explore_measure_gallery(
        896.0,
        896.0,
        crate::generated::VisualExtent {
            width: 1920,
            height: 1080,
        },
        3,
    );
    let drive = |driver: &mut Controller, model: &ApplicationModel| {
        drop(driver.advance(
            model,
            &settings,
            1.0,
            &router,
            FeatureId::Explore,
            Some(crate::view_model::test_support::physical_surface(frame)),
        ));
    };
    driver.driver.desired_dark = None;
    driver.driver.phase = Phase::AwaitExplore;
    assert!(model.explore_open_available());
    // Logical Detail may arrive before any actual composition.
    drive(&mut driver, &model);
    assert_eq!(driver.driver.phase, Phase::AwaitExplore);
    assert!(!driver.widgets.location_pending());
    for phase in [
        Phase::AwaitDetail(0),
        Phase::AwaitNext(99),
        Phase::AwaitPrevious(99),
        Phase::AwaitDetailAgain,
    ] {
        driver.driver.phase = phase.clone();
        drive(&mut driver, &model);
        assert_eq!(driver.driver.phase, phase);
        assert!(!driver.widgets.location_pending());
    }
    driver.driver.phase = Phase::AwaitExplore;
    assert!(crate::presentation_surface::accept_publication(frame));
    model.explore.snapshot.as_mut().unwrap().mode = crate::generated::ExploreMode::Gallery;
    drive(&mut driver, &model);
    assert_eq!(driver.driver.phase, Phase::AwaitExplore);
    assert!(!driver.widgets.location_pending());
    model.explore.snapshot.as_mut().unwrap().mode = crate::generated::ExploreMode::Detail;
    drive(&mut driver, &model);
    assert_eq!(driver.driver.phase, Phase::ExploreCloseDetail);
    assert!(driver.widgets.location_pending());
    // The packaged Wayland sequence exercises the browser click. This
    // native fixture starts at its receipt and checks native settlement.
    driver.driver.phase = Phase::AwaitExploreGallery;
    driver.widgets.location_completed();
    for (mode, busy, expected) in [
        (
            crate::generated::ExploreMode::Detail,
            false,
            Phase::AwaitExploreGallery,
        ),
        (
            crate::generated::ExploreMode::Gallery,
            true,
            Phase::AwaitExploreGallery,
        ),
        (
            crate::generated::ExploreMode::Gallery,
            false,
            Phase::AwaitExplore,
        ),
    ] {
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.mode = mode;
        snapshot.busy = busy;
        if mode == crate::generated::ExploreMode::Gallery && !busy {
            // Logical Close settlement cannot uncover the old graphics overlay.
            drive(&mut driver, &model);
            assert_eq!(driver.driver.phase, Phase::AwaitExploreGallery);
            let mut gallery = model.explore.snapshot.clone().unwrap();
            gallery.viewport.columns = 4;
            gallery.viewport.rowcount = 3;
            gallery.viewport.firstrow = 0;
            gallery.viewport.extent = gallery.frame.extent.clone();
            crate::view_model::test_support::gallery_layout(&mut gallery);
            crate::presentation_surface::metadata::retire(frame);
            crate::presentation_surface::metadata::install_explore(frame, &gallery);
            // A new Gallery composition can precede logical Close settlement too.
            model.explore.snapshot.as_mut().unwrap().mode = crate::generated::ExploreMode::Detail;
            drive(&mut driver, &model);
            assert_eq!(driver.driver.phase, Phase::AwaitExploreGallery);
            assert!(!driver.widgets.location_pending());
            model.explore.snapshot.as_mut().unwrap().mode = mode;
        }
        drive(&mut driver, &model);
        assert_eq!(driver.driver.phase, expected);
        assert!(!driver.widgets.location_pending());
        assert!(driver.driver.reporting.state_is_absent());
    }
    let revision = model.explore.snapshot.as_ref().unwrap().revision;
    model
        .explore
        .snapshot
        .as_mut()
        .unwrap()
        .detail
        .showoriginaldimensions = true;
    drive(&mut driver, &model);
    assert_eq!(
        driver.driver.phase,
        Phase::AwaitExplorePreparation(revision)
    );
    drive(&mut driver, &model);
    assert_eq!(
        driver.driver.phase,
        Phase::AwaitExplorePreparation(revision)
    );
    {
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.revision += 1;
        snapshot.detail.showoriginaldimensions = false;
        assert!(explore_scenario_preparation(snapshot).is_none());
    }
    drive(&mut driver, &model);
    assert_eq!(driver.driver.phase, Phase::AwaitExplore);

    // The shared padding donors have no annotations. Missing or unready
    // donor slots cannot advance even when the published frame is drawn.
    let snapshot = model.explore.snapshot.as_mut().unwrap();
    snapshot.labels.clear();
    let revision = snapshot.revision;
    let source = snapshot.frame.revision;
    driver
        .probes
        .record_gallery_draw(frame.presentation_revision, source);
    for (indices, slots, timeline, ready) in [
        (vec![0, 8], vec![true, true], 1, false),
        (vec![7, 8], vec![true, false], 1, false),
        (vec![7, 8], vec![true, true], 0, false),
        (vec![7, 8], vec![true, true], 1, true),
    ] {
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.order.visibleindices = indices;
        snapshot.gallery.slots = slots;
        crate::presentation_surface::metadata::retire(frame);
        if timeline != 0 {
            let mut product = snapshot.clone();
            product.mode = crate::generated::ExploreMode::Gallery;
            product.viewport.columns = 4;
            product.viewport.rowcount = 3;
            product.viewport.firstrow = 0;
            product.viewport.extent = product.frame.extent.clone();
            crate::view_model::test_support::gallery_layout(&mut product);
            crate::presentation_surface::metadata::install_explore(frame, &product);
        }
        driver.driver.phase = Phase::AwaitExploreInitialPatch {
            revision,
            frame_revision: source,
        };
        drive(&mut driver, &model);
        assert_eq!(
            driver.driver.phase,
            if ready {
                Phase::AwaitExploreExactGrid(revision)
            } else {
                Phase::AwaitExploreInitialPatch {
                    revision,
                    frame_revision: source,
                }
            }
        );
        assert!(driver.driver.reporting.state_is_absent());
    }
    // The empty message must reach a draw before restoring the filters.
    // Logical delivery, acquisition, and a preceding draw are insufficient.
    probe::tests::check_empty_atlas_draws(&mut driver, &mut model, frame, source, &drive);
    // The oversized measurement can resolve to the already committed
    // raster, so native viewport deduplication need not publish a revision.
    let snapshot = model.explore.snapshot.as_mut().unwrap();
    snapshot.viewport = crate::generated::ExploreViewport {
        extent: crate::generated::VisualExtent {
            width: 810,
            height: 810,
        },
        firstrow: 0,
        rowcount: 3,
        columns: 3,
    };
    snapshot.busy = false;
    driver.retained.oversized_gallery = Some((
        iced::Size::new(896.0, 896.0),
        crate::generated::VisualExtent {
            width: 1920,
            height: 1080,
        },
    ));
    driver.driver.phase = Phase::AwaitExploreExactGrid(revision);
    drive(&mut driver, &model);
    assert_eq!(
        driver.driver.phase,
        Phase::AwaitExploreExactGridPatch {
            revision,
            frame_revision: source,
        }
    );
    let annotation = model.annotation.snapshot.as_mut().unwrap();
    annotation.ready = true;
    annotation.ui.documentrevision = 1;
    annotation.frame = crate::view_model::test_support::visual_frame(
        crate::generated::PresentationSourceKind::Annotation,
        1,
    );
    annotation.inputdocumentepoch = 1;
    driver.observe_annotation_open(
        crate::generated::AnnotationOpen {
            crop: model
                .explore
                .snapshot
                .as_ref()
                .unwrap()
                .frame
                .content
                .clone(),
            target: model
                .explore
                .snapshot
                .as_ref()
                .unwrap()
                .frame
                .extent
                .clone(),
            source: model.explore.snapshot.as_ref().unwrap().frame.clone(),
        },
        0,
    );
    driver.driver.viewer_scenario = "terminal".into();
    driver.driver.phase = Phase::AwaitAnnotation;
    drive(&mut driver, &model);
    assert_eq!(driver.driver.phase, Phase::AwaitAnnotation);
    drop(driver.advance(
        &model,
        &settings,
        1.0,
        &router,
        FeatureId::Annotate,
        Some(crate::view_model::test_support::physical_surface(frame)),
    ));
    assert!(matches!(driver.driver.phase, Phase::AnnotationTool { .. }));
    assert!(driver.driver.reporting.state_is_absent());
    driver.driver.phase = Phase::Complete;
    assert!(driver.receive_control(advance).is_err());
    assert!(matches!(driver.driver.phase, Phase::Failed));

    let mut premature = Controller::new(
        true,
        false,
        String::new(),
        String::new(),
        "512".into(),
        "dpi".into(),
    );
    premature.configure_session("dpi", String::new(), String::new());
    premature.driver.phase = Phase::Complete;
    assert!(
        premature.receive_control(advance_receipt(2)).is_err(),
        "local completion is insufficient before the typed receipt was admitted"
    );
}

pub(in crate::integration_control) fn replace_scenario_with_retained_baseline(
    controller: &mut Controller,
) {
    controller.retained.atlas_baseline = Some((1, 5));
    assert!(
        controller
            .reset_scenario(
                "new-source".into(),
                "new-output".into(),
                "512".into(),
                "upscale".into()
            )
            .is_err()
    );
    controller.driver.phase = Phase::Complete;
    controller
        .reset_scenario(
            "new-source".into(),
            "new-output".into(),
            "512".into(),
            "upscale".into(),
        )
        .unwrap();
    assert!(controller.retained.atlas_baseline.is_none());
}

pub(in crate::integration_control) fn check_reset_atlas_callbacks(
    controller: &mut Controller,
    receiver: &mut iced::futures::channel::mpsc::Receiver<Message>,
    draw: AtlasDraw,
) {
    let (surface, bounds) = (draw.surface, draw.bounds);
    report_atlas_draw(draw.clone(), true, 1.0);
    assert!(receiver.try_recv().is_err());
    record_probe_draw(EXPLORE_GALLERY, surface, bounds, bounds, bounds);
    report_atlas_draw(draw.clone(), true, 1.0);
    let queued = receiver.try_recv().unwrap();
    let moved_draw = AtlasDraw {
        image: Rectangle { x: 17.0, ..bounds },
        ..draw
    };
    record_probe_draw(EXPLORE_GALLERY, surface, bounds, moved_draw.image, bounds);
    assert!(!controller.probes.accepts_message(
        &controller.driver,
        &controller.pixel_checks,
        &queued
    ));
    controller.update(queued);
    assert!(controller.retained.atlas_receipt.is_none());
    report_atlas_draw(moved_draw.clone(), true, 1.0);
    let queued = receiver.try_recv().unwrap();
    assert!(controller.probes.accepts_message(
        &controller.driver,
        &controller.pixel_checks,
        &queued
    ));
    controller.update(queued);
    assert_eq!(controller.retained.atlas_receipt, Some(moved_draw));
}

pub(in crate::integration_control) fn check_atlas_pixel_callbacks(
    controller: &mut Controller,
    receiver: &mut iced::futures::channel::mpsc::Receiver<Message>,
    draw: AtlasDraw,
) {
    record_probe_draw(
        EXPLORE_GALLERY,
        draw.surface,
        draw.bounds,
        draw.image,
        draw.clip,
    );
    atlas_probe_output(false)
        .unwrap()
        .try_send(Message::AtlasPixels {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(1, 0),
        })
        .unwrap();
    controller.update(receiver.try_recv().unwrap());
    assert!(controller.retained.atlas_pixels.is_none());
    atlas_probe_output(false)
        .unwrap()
        .try_send(Message::AtlasPixels {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(0, 0),
        })
        .unwrap();
    controller.update(receiver.try_recv().unwrap());
    assert!(controller.retained.atlas_pixels.is_none());
    atlas_probe_output(false)
        .unwrap()
        .try_send(Message::AtlasPixels {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(1, 1),
        })
        .unwrap();
    controller.update(receiver.try_recv().unwrap());
    assert_eq!(controller.retained.atlas_pixels, Some(draw.clone()));
    atlas_probe_output(true)
        .unwrap()
        .try_send(Message::AtlasComposition {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(1, 0),
        })
        .unwrap();
    controller.update(receiver.try_recv().unwrap());
    assert!(controller.retained.atlas_composition.is_none());
    atlas_probe_output(true)
        .unwrap()
        .try_send(Message::AtlasComposition {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(0, 0),
        })
        .unwrap();
    controller.update(receiver.try_recv().unwrap());
    assert!(controller.retained.atlas_composition.is_none());
    atlas_probe_output(true)
        .unwrap()
        .try_send(Message::AtlasComposition {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(1, 1),
        })
        .unwrap();
    controller.update(receiver.try_recv().unwrap());
    assert_eq!(controller.retained.atlas_composition, Some(draw));
}
