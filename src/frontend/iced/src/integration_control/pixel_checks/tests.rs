use crate::generated::FeatureId;
use crate::integration_control::pixel_checks::{
    ATLAS_GRID_SAMPLES, AtlasDraw, FpsPixelOutcome, FpsPixels, ProbeOutcome,
    WORKSPACE_FPS_PIXEL_FAILURE, atlas_composition_samples, atlas_scroll_window,
    verify_workspace_fps_pixels,
};
use crate::integration_control::probe::{ScenarioOutput, record_probe_draw, same_probe};
use crate::integration_control::reporting::{Capture, FpsEvidence};
use crate::integration_control::{
    EXPLORE_GALLERY, Message, PIXEL_FIXTURE_ENABLED, Phase, ProbeFixture, initialize_reporting,
    pixel_checks, reporting,
};
use crate::message::Message as RootMessage;
use crate::view_model::ApplicationModel;
use iced::{Rectangle, Task};
pub(in crate::integration_control) fn fps_pixel_fixture(
    dark: bool,
    scale: f32,
) -> (FpsPixels, reporting::FpsEvidence) {
    let width = (74.0 * scale) as u32;
    let height = (22.0 * scale) as u32;
    let mut rgba = vec![if dark { 0 } else { 255 }; width as usize * height as usize * 4];
    for pixel in rgba.chunks_exact_mut(4) {
        pixel[3] = 255;
    }
    // Representative contrasting glyph stroke, safely inside the padded border.
    for y in (8.0 * scale) as u32..(18.0 * scale) as u32 {
        for x in (48.0 * scale) as u32..(51.0 * scale) as u32 {
            let offset = (y as usize * width as usize + x as usize) * 4;
            rgba[offset..offset + 3].fill(if dark { 255 } else { 0 });
        }
    }
    (
        FpsPixels {
            width,
            height,
            rgba,
        },
        reporting::FpsEvidence {
            bounds: Rectangle::new(iced::Point::new(20.0, 6.0), iced::Size::new(74.0, 22.0)),
            clip: Rectangle::new(iced::Point::ORIGIN, iced::Size::new(100.0, 60.0)),
            dark,
            frames: 30,
            seconds: 0.5,
        },
    )
}

#[test]
fn workspace_fps_pixel_acceptance_uses_the_visible_counter_at_each_scale_and_theme() {
    let capture = Capture::new(true);
    for scale in [1.0, 1.25, 1.5, 2.25] {
        for dark in [false, true] {
            let (pixels, evidence) = fps_pixel_fixture(dark, scale);
            assert!(verify_workspace_fps_pixels(&pixels, evidence));
        }
    }
    let records = capture.records();
    assert_eq!(records.len(), 8);
    assert!(records.iter().all(|(event, control, detail, values)| {
        event == "integration.workspace_fps_pixels"
            && control == EXPLORE_GALLERY
            && detail == "visible-counter"
            && values[0] == 30.0
            && values[1] == 0.5
    }));
}

#[test]
fn fps_pixels_require_opaque_text_background_complete_extent_and_unclipped_placement() {
    let _capture = Capture::new(true);
    for dark in [false, true] {
        let (pixels, evidence) = fps_pixel_fixture(dark, 1.0);
        let mut missing = pixels.clone();
        missing.rgba.clear();
        assert!(!verify_workspace_fps_pixels(&missing, evidence));
        let mut background = pixels.clone();
        for pixel in background.rgba.chunks_exact_mut(4) {
            pixel[..3].fill(if dark { 0 } else { 255 });
        }
        assert!(!verify_workspace_fps_pixels(&background, evidence));
        let mut transparent = pixels.clone();
        transparent.rgba[3] = 0;
        assert!(!verify_workspace_fps_pixels(&transparent, evidence));
        let mut incomplete = pixels.clone();
        incomplete.rgba.pop();
        assert!(!verify_workspace_fps_pixels(&incomplete, evidence));
        let mut border = pixels.clone();
        for pixel in border.rgba[..border.width as usize * 4].chunks_exact_mut(4) {
            pixel[..3].fill(if dark { 255 } else { 0 });
        }
        assert!(!verify_workspace_fps_pixels(&border, evidence));
        assert!(!verify_workspace_fps_pixels(
            &pixels,
            FpsEvidence {
                dark: !dark,
                ..evidence
            }
        ));
        for seconds in [0.0, 0.499, f64::NAN, f64::INFINITY] {
            assert!(!verify_workspace_fps_pixels(
                &pixels,
                FpsEvidence {
                    seconds,
                    ..evidence
                }
            ));
        }
        assert!(!verify_workspace_fps_pixels(
            &pixels,
            FpsEvidence {
                frames: 0,
                ..evidence
            }
        ));
        for x in [f32::NAN, -1.0, 21.0, 100.0] {
            assert!(!verify_workspace_fps_pixels(
                &pixels,
                FpsEvidence {
                    bounds: Rectangle {
                        x,
                        ..evidence.bounds
                    },
                    ..evidence
                }
            ));
        }
    }
}

pub(in crate::integration_control) fn fps_settings()
-> (ApplicationModel, crate::view::settings::SettingsModel) {
    let mut model = crate::view_model::test_support::bootstrapped();
    model
        .settings_snapshot
        .as_mut()
        .unwrap()
        .settingsstate
        .ui
        .showworkspaceperformance = true;
    let mut settings = crate::view::settings::SettingsModel::default();
    settings.install(model.settings_snapshot.as_ref().unwrap());
    (model, settings)
}

impl ProbeFixture {
    fn request_fps(
        &mut self,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
    ) -> ScenarioOutput {
        record_probe_draw(
            EXPLORE_GALLERY,
            self.surface,
            self.bounds,
            self.bounds,
            self.bounds,
        );
        let (_, evidence) = pixel_checks::tests::fps_pixel_fixture(false, 1.0);
        let mut output = self.fps_draw_output(evidence);
        self.controller.driver.phase = Phase::AwaitWorkspaceFps;
        output
            .try_send(Message::WorkspaceFpsDrawn(evidence))
            .unwrap();
        self.controller.update(self.receiver.try_recv().unwrap());
        drop(self.controller.advance(
            model,
            settings,
            1.0,
            &crate::view::router::Router::default(),
            FeatureId::Explore,
            None,
        ));
        assert_eq!(self.controller.driver.phase, Phase::AwaitWorkspaceFpsPixels);
        self.controller
            .pixel_checks
            .workspace_fps_probe
            .clone()
            .unwrap()
    }
    pub(crate) fn prepare_app_fps(
        &mut self,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        baseline: bool,
        capture: bool,
    ) -> Option<Message> {
        self.controller.pixel_checks.workspace_fps_baseline = baseline;
        if !capture {
            self.controller.driver.phase = Phase::AwaitWorkspaceFps;
            return None;
        }
        let mut output = self.request_fps(model, settings);
        let (pixels, _) = pixel_checks::tests::fps_pixel_fixture(false, 1.0);
        output.send(Message::WorkspaceFpsPixels(FpsPixelOutcome::Captured(
            pixels,
        )));
        Some(self.receiver.try_recv().unwrap())
    }

    fn complete_fps(
        &mut self,
        output: &mut ScenarioOutput,
        outcome: FpsPixelOutcome,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
    ) -> Task<RootMessage> {
        output
            .try_send(Message::WorkspaceFpsPixels(outcome))
            .unwrap();
        let message = self.receiver.try_recv().unwrap();
        if !self.controller.probes.accepts_message(
            &self.controller.driver,
            &self.controller.pixel_checks,
            &message,
        ) {
            return Task::none();
        }
        self.controller.update(message);
        self.controller.advance(
            model,
            settings,
            1.0,
            &crate::view::router::Router::default(),
            FeatureId::Explore,
            None,
        )
    }

    fn assert_pending_fps(&self, output: &ScenarioOutput) {
        assert_eq!(self.controller.driver.phase, Phase::AwaitWorkspaceFpsPixels);
        assert!(same_probe(
            &self
                .controller
                .pixel_checks
                .workspace_fps_probe
                .as_ref()
                .unwrap()
                .probe,
            output.probe.as_ref()
        ));
    }
}

#[test]
fn fps_capture_success_and_recoverable_failures_restore_both_canonical_baselines() {
    use iced::futures::StreamExt;
    for baseline in [false, true] {
        for result in 0..3 {
            let mut fixture = ProbeFixture::new("square");
            let (mut model, mut settings) = fps_settings();
            fixture.controller.pixel_checks.workspace_fps_baseline = baseline;
            let mut output = fixture.request_fps(&model, &settings);
            let (mut pixels, _) = pixel_checks::tests::fps_pixel_fixture(false, 1.0);
            if result == 1 {
                pixels.rgba.pop();
            }
            let task = fixture.complete_fps(
                &mut output,
                if result == 2 {
                    FpsPixelOutcome::Failed
                } else {
                    FpsPixelOutcome::Captured(pixels)
                },
                &model,
                &settings,
            );
            let driver = &mut fixture.controller;
            let router = crate::view::router::Router::default();
            assert_eq!(driver.driver.phase, Phase::AwaitWorkspaceFpsRestored);
            assert_eq!(
                driver.pixel_checks.workspace_fps_failure,
                (result != 0).then_some(WORKSPACE_FPS_PIXEL_FAILURE)
            );
            assert_eq!(driver.driver.failure_line, 0);
            let mut actions = iced_runtime::task::into_stream(task).unwrap();
            let action = iced::futures::executor::block_on(actions.next()).unwrap();
            let iced_runtime::Action::Output(RootMessage::Settings(message)) = action else {
                panic!("FPS restoration must use ordinary settings mutation");
            };
            assert!(
                matches!(message, crate::view::settings::Message::PerformanceChanged(value) if value == baseline)
            );
            let outcome = crate::view::settings::update(&mut settings, message).unwrap();
            assert!(matches!(
                outcome,
                Some(crate::view::settings::Outcome::SettingsEdited(_))
            ));
            drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Explore, None));
            assert_eq!(driver.driver.phase, Phase::AwaitWorkspaceFpsRestored);
            let restoration = settings
                .take_request()
                .expect("canonical restoration request for either baseline");
            assert_eq!(restoration.updates.len(), 1);
            assert_eq!(
                restoration.updates[0],
                crate::generated::update_uishowworkspaceperformance(baseline)
            );
            drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Explore, None));
            assert_eq!(driver.driver.phase, Phase::AwaitWorkspaceFpsRestored);
            let pending = model
                .begin_intent(crate::generated::ApplicationIntentEndpoint::SettingsUpdate)
                .unwrap();
            let authoritative = model.settings_snapshot.as_mut().unwrap();
            authoritative.revision += 1;
            authoritative.settingsstate.ui.showworkspaceperformance = baseline;
            settings.settle_success(authoritative);
            drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Explore, None));
            assert_eq!(driver.driver.phase, Phase::AwaitWorkspaceFpsRestored);
            assert_eq!(driver.driver.failure_line, 0);
            model.abandon_intent(pending);
            drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Explore, None));
            assert_eq!(crate::workspace_fps::enabled(&settings), baseline);
            assert_eq!(
                model
                    .settings_snapshot
                    .as_ref()
                    .unwrap()
                    .settingsstate
                    .ui
                    .showworkspaceperformance,
                baseline
            );
            assert_eq!(driver.pixel_checks.workspace_fps_verified, result == 0);
            assert_eq!(
                driver.driver.phase,
                if result == 0 {
                    Phase::AwaitExploreReady
                } else {
                    Phase::Failed
                }
            );
            assert_eq!(driver.driver.failure_line != 0, result != 0);
        }
    }
}

#[test]
fn fps_capture_invalidation_rearms_and_obsolete_callbacks_cannot_finish_replacements() {
    for change in 0..4 {
        let mut fixture = ProbeFixture::new("square");
        let (model, settings) = fps_settings();
        let mut old = fixture.request_fps(&model, &settings);
        let (pixels, _) = pixel_checks::tests::fps_pixel_fixture(false, 1.0);
        let outcome = match change {
            0 => FpsPixelOutcome::Invalidated,
            1 => {
                let moved = Rectangle {
                    x: 1.0,
                    ..fixture.bounds
                };
                record_probe_draw(EXPLORE_GALLERY, fixture.surface, moved, moved, moved);
                FpsPixelOutcome::Captured(pixels.clone())
            }
            2 => {
                fixture.invalidate_fps_draw(change);
                FpsPixelOutcome::Captured(pixels.clone())
            }
            _ => {
                fixture.invalidate_fps_draw(change);
                FpsPixelOutcome::Captured(pixels.clone())
            }
        };
        drop(fixture.complete_fps(&mut old, outcome, &model, &settings));
        assert_eq!(fixture.controller.driver.phase, Phase::AwaitWorkspaceFps);
        assert!(!fixture.controller.pixel_checks.workspace_fps_verified);
        assert!(
            fixture
                .controller
                .pixel_checks
                .workspace_fps_failure
                .is_none()
        );
        let mut replacement = fixture.request_fps(&model, &settings);
        drop(fixture.complete_fps(&mut old, FpsPixelOutcome::Cancelled, &model, &settings));
        fixture.assert_pending_fps(&replacement);
        drop(fixture.complete_fps(
            &mut replacement,
            FpsPixelOutcome::Captured(pixels),
            &model,
            &settings,
        ));
        assert_eq!(
            fixture.controller.driver.phase,
            Phase::AwaitWorkspaceFpsRestored
        );
        assert!(
            fixture
                .controller
                .pixel_checks
                .workspace_fps_failure
                .is_none()
        );
    }
}

#[test]
fn fps_capture_from_a_prior_scenario_cannot_settle_the_replacement() {
    let mut fixture = ProbeFixture::new("square");
    let (model, settings) = fps_settings();
    let mut old = fixture.request_fps(&model, &settings);
    fixture.controller.driver.phase = Phase::Complete;
    fixture
        .controller
        .reset_scenario(String::new(), String::new(), "512".into(), "square".into())
        .unwrap();
    let mut replacement = fixture.request_fps(&model, &settings);
    drop(fixture.complete_fps(&mut old, FpsPixelOutcome::Cancelled, &model, &settings));
    fixture.assert_pending_fps(&replacement);
    let (pixels, _) = pixel_checks::tests::fps_pixel_fixture(false, 1.0);
    drop(fixture.complete_fps(
        &mut replacement,
        FpsPixelOutcome::Captured(pixels),
        &model,
        &settings,
    ));
    assert_eq!(
        fixture.controller.driver.phase,
        Phase::AwaitWorkspaceFpsRestored
    );
    assert!(
        fixture
            .controller
            .pixel_checks
            .workspace_fps_failure
            .is_none()
    );
}

#[test]
fn fps_capture_scale_change_rearms_without_accepting_the_old_result() {
    let mut fixture = ProbeFixture::new("square");
    let (model, settings) = fps_settings();
    let mut old = fixture.request_fps(&model, &settings);
    let router = crate::view::router::Router::default();
    drop(
        fixture
            .controller
            .advance(&model, &settings, 1.5, &router, FeatureId::Explore, None),
    );
    assert_eq!(fixture.controller.driver.phase, Phase::AwaitWorkspaceFps);
    drop(fixture.complete_fps(&mut old, FpsPixelOutcome::Failed, &model, &settings));
    assert_eq!(fixture.controller.driver.phase, Phase::AwaitWorkspaceFps);
}

#[test]
fn atlas_scroll_uses_real_geometry_to_add_one_row_without_resizing_pixels() {
    for size in [
        iced::Size::new(896.0, 896.0),
        iced::Size::new(896.0, 896.6667),
        iced::Size::new(896.0, 895.3333),
        iced::Size::new(600.0, 712.5),
    ] {
        for columns in [3, 4, 5, 10] {
            let (rows, fraction) = atlas_scroll_window(size, columns);
            assert!(fraction > 0.0 && fraction < 1.0);
            let geometry = |fraction| {
                crate::view::explore::state::gallery_geometry_at_fraction(
                    size.width,
                    size.height,
                    crate::generated::VisualExtent {
                        width: 1920,
                        height: 1080,
                    },
                    columns,
                    1_000,
                    0,
                    fraction,
                )
                .unwrap()
            };
            let aligned = geometry(0.0);
            let fractional = geometry(fraction);
            assert_eq!(aligned.viewport().rowcount, rows);
            assert_eq!(fractional.viewport().rowcount, rows + 1);
            assert_eq!(
                aligned.viewport().extent.width,
                fractional.viewport().extent.width
            );
        }
    }
}

#[test]
fn pixel_outcome_adapter_preserves_invalidated_observed_and_failed() {
    assert_eq!(
        ProbeOutcome::decode(Some("invalidated"), [Some(0.0); 2]),
        ProbeOutcome::Invalidated
    );
    assert_eq!(
        ProbeOutcome::decode(Some("observed"), [Some(1.0), Some(0.0)]),
        ProbeOutcome::Observed(1, 0)
    );
    for (status, values) in [
        (None, [Some(0.0); 2]),
        (Some("invalidated"), [Some(1.0), Some(0.0)]),
        (Some("observed"), [None, Some(0.0)]),
        (Some("observed"), [Some(f64::NAN), Some(0.0)]),
        (Some("observed"), [Some(f64::INFINITY), Some(0.0)]),
        (Some("observed"), [Some(-1.0), Some(0.0)]),
        (Some("observed"), [Some(0.5), Some(0.0)]),
        (
            Some("observed"),
            [Some(f64::from(u32::MAX) + 1.0), Some(0.0)],
        ),
        (Some("failed"), [Some(0.0); 2]),
    ] {
        assert_eq!(ProbeOutcome::decode(status, values), ProbeOutcome::Failed);
    }
}

#[test]
fn atlas_composition_samples_bound_all_three_grid_lines_at_required_columns_and_dpi() {
    initialize_reporting(true, true);
    PIXEL_FIXTURE_ENABLED.with(|enabled| enabled.set(true));
    for columns in [4, 10] {
        for dpi in [1.0, 1.5] {
            let mut snapshot = crate::view_model::test_support::explore_snapshot();
            snapshot.viewport.columns = columns;
            snapshot.viewport.rowcount = columns;
            snapshot.gallery.layout.columns = columns;
            snapshot.gallery.layout.rowcount = columns;
            snapshot.gallery.layout.rowcapacity = columns;
            snapshot.gallery.layout.roworigin = 0;
            snapshot.gallery.layout.cardextent = 100;
            snapshot.augmentation.enabled = false;
            snapshot.overlay.showlabels = false;
            snapshot.overlay.showmasks = true;
            snapshot.overlay.showboxes = true;
            snapshot.labels.clear(); // Background row, no annotation or source padding.
            snapshot.gallery.slots = vec![true; (columns * columns) as usize];
            let (_, mut frame) = crate::view_model::test_support::explore_presentation();
            frame.content_width = columns * 100;
            frame.content_height = columns * 100;
            let mut surface = crate::view_model::test_support::physical_surface(frame);
            surface.width = frame.content_width;
            surface.height = frame.content_height;
            let width = 800.0 * dpi;
            let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(width, width));
            let draw = AtlasDraw {
                surface,
                snapshot: std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(
                    &snapshot,
                )),
                bounds,
                image: bounds,
                clip: bounds,
            };
            let samples = atlas_composition_samples(&draw).unwrap();
            assert_eq!(samples.count, ATLAS_GRID_SAMPLES * 10);
            assert_eq!(samples.card_count, 0);
            assert_eq!(samples.cards.len(), 256);
            let cell = width / columns as f32;
            // Independent raster strips at known physical positions, not
            // the shader's rounding/distance algorithm.
            let positions = [
                0.5,
                1.5,
                2.5,
                3.5,
                cell - 1.5,
                cell - 0.5,
                cell + 0.5,
                cell + 1.5,
                cell + 2.5,
                0.5,
                1.5,
                2.5,
                3.5,
            ];
            let clean_indices = [3, 4, 8, 12];
            let white_indices = [1, 6, 10];
            for (index, point) in samples.points[..samples.count].chunks_exact(10).enumerate() {
                let expected_position = if index < 9 {
                    [positions[index], cell / 2.0]
                } else {
                    [cell * 1.5, positions[index]]
                };
                assert_eq!(point[2..4], expected_position);
                let expected = if clean_indices.contains(&index) {
                    [48.0, 80.0, 112.0]
                } else if white_indices.contains(&index) {
                    [255.0; 3]
                } else {
                    [0.0; 3]
                };
                assert_eq!(point[4..7], expected);
                assert_eq!(point[7], 255.0);
                assert_eq!(point[8], 4.0);
            }
            // The real scrollbar can cover the last five logical pixels.
            // None of the thirteen required samples enters that overlay.
            assert!(
                samples.points[..samples.count]
                    .chunks_exact(10)
                    .all(|point| point[2] < width - 5.0 * dpi)
            );
            let mut clipped = draw.clone();
            clipped.clip.y = cell;
            clipped.clip.height -= cell;
            assert!(atlas_composition_samples(&clipped).is_none());
        }
    }
    initialize_reporting(false, false);
}
