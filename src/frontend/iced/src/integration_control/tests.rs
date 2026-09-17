use super::pixel_checks::{WORKSPACE_FPS_PIXEL_FAILURE};
use super::annotation_checks::{annotation_layout_scale};
use super::retained::{explore_scenario_preparation, cold_gallery_scroll_offset, explore_integer_id, EXPLORE_CARD};

    use super::*;

    pub(crate) struct ProbeFixture {
        pub(crate) controller: Controller,
        pub(crate) receiver: iced::futures::channel::mpsc::Receiver<Message>,
        surface: crate::presentation_surface::Surface,
        bounds: Rectangle,
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
            probe::with_observer_fixture(|observer| {
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
            probe::with_observer_fixture(|observer| observer.borrow_mut().output = None);
            initialize_reporting(false, false);
        }
    }

    fn fps_settings() -> (ApplicationModel, crate::view::settings::SettingsModel) {
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
            let (_, evidence) = pixel_checks::fps_pixel_fixture(false, 1.0);
            let mut output = probe::with_observer_fixture(|observer| {
                let mut observer = observer.borrow_mut();
                observer.fps_draw = Some(evidence);
                observer.output_for(EXPLORE_GALLERY).unwrap().clone()
            });
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
            self.controller.pixel_checks.fixture().workspace_fps_probe.clone().unwrap()
        }
        pub(crate) fn prepare_app_fps(
            &mut self,
            model: &ApplicationModel,
            settings: &crate::view::settings::SettingsModel,
            baseline: bool,
            capture: bool,
        ) -> Option<Message> {
            self.controller.pixel_checks.configure_fixture(|fixture| fixture.workspace_fps_baseline = baseline);
            if !capture {
                self.controller.driver.phase = Phase::AwaitWorkspaceFps;
                return None;
            }
            let mut output = self.request_fps(model, settings);
            let (pixels, _) = pixel_checks::fps_pixel_fixture(false, 1.0);
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
            if !self.controller.probes.accepts_message(&self.controller.driver, &self.controller.pixel_checks, &message) {
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
                &self.controller.pixel_checks.fixture().workspace_fps_probe.as_ref().unwrap().probe,
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
                fixture.controller.pixel_checks.configure_fixture(|fixture| fixture.workspace_fps_baseline = baseline);
                let mut output = fixture.request_fps(&model, &settings);
                let (mut pixels, _) = pixel_checks::fps_pixel_fixture(false, 1.0);
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
                    driver.pixel_checks.fixture().workspace_fps_failure,
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
                assert_eq!(driver.pixel_checks.fixture().workspace_fps_verified, result == 0);
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
            let (pixels, _) = pixel_checks::fps_pixel_fixture(false, 1.0);
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
                    probe::with_observer_fixture(|observer| {
                        observer.borrow_mut().fps_draw.as_mut().unwrap().bounds.x += 1.0
                    });
                    FpsPixelOutcome::Captured(pixels.clone())
                }
                _ => {
                    probe::with_observer_fixture(|observer| {
                        observer.borrow_mut().fps_draw.as_mut().unwrap().frames += 1
                    });
                    FpsPixelOutcome::Captured(pixels.clone())
                }
            };
            drop(fixture.complete_fps(&mut old, outcome, &model, &settings));
            assert_eq!(fixture.controller.driver.phase, Phase::AwaitWorkspaceFps);
            assert!(!fixture.controller.pixel_checks.fixture().workspace_fps_verified);
            assert!(fixture.controller.pixel_checks.fixture().workspace_fps_failure.is_none());
            let mut replacement = fixture.request_fps(&model, &settings);
            drop(fixture.complete_fps(&mut old, FpsPixelOutcome::Cancelled, &model, &settings));
            fixture.assert_pending_fps(&replacement);
            drop(fixture.complete_fps(
                &mut replacement,
                FpsPixelOutcome::Captured(pixels),
                &model,
                &settings,
            ));
            assert_eq!(fixture.controller.driver.phase, Phase::AwaitWorkspaceFpsRestored);
            assert!(fixture.controller.pixel_checks.fixture().workspace_fps_failure.is_none());
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
        let (pixels, _) = pixel_checks::fps_pixel_fixture(false, 1.0);
        drop(fixture.complete_fps(
            &mut replacement,
            FpsPixelOutcome::Captured(pixels),
            &model,
            &settings,
        ));
        assert_eq!(fixture.controller.driver.phase, Phase::AwaitWorkspaceFpsRestored);
        assert!(fixture.controller.pixel_checks.fixture().workspace_fps_failure.is_none());
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
            let target = driver.retained.fixture().explore_integer_target;
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
            assert_eq!(driver.retained.fixture().explore_integer_target, target);
            assert_eq!(driver.retained.fixture().explore_integer_baseline, baseline);
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
        driver.retained.configure_fixture(|fixture| fixture.explore_integer_target = (1_u64 << 53) + 1);
        let contents = std::sync::Arc::new(iced::clipboard::Content::Text(
            driver.retained.fixture().explore_integer_target.to_string(),
        ));
        driver.update(scoped(
            driver.driver.generation,
            Message::NumberPasteRead {
                target: explore_integer_id(0),
                result: Ok(contents.clone()),
            },
        ));
        assert!(!driver.retained.fixture().explore_paste_read);
        driver.update(scoped(
            driver.driver.generation,
            Message::NumberPasteRead {
                target: explore_integer_id(2),
                result: Ok(contents),
            },
        ));
        assert!(driver.retained.fixture().explore_paste_read);
        driver.update(scoped(
            driver.driver.generation,
            Message::NumberPasteDelivered(false),
        ));
        assert!(driver.driver.failure.contains("paste shortcut delivery failed"));
        driver.update(scoped(
            driver.driver.generation,
            Message::NumberClipboardPrepared {
                revision: 17,
                result: Ok(()),
            },
        ));
        assert_eq!(driver.driver.phase, Phase::Failed);
        assert!(!driver.retained.fixture().explore_paste_read);
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
            assert!(driver.driver.failure.contains("clipboard permission rejected"));
        }
    }

    #[test]
    fn delivered_gallery_mouse_advances_placeholder_selection_for_the_current_driver() {
        let mut driver = Controller::new(
            true,
            false,
            String::new(),
            String::new(),
            "512".into(),
            String::new(),
        );
        driver.driver.phase = Phase::AwaitVisibleReadHover(5, 9);
        driver.update(Message::Scoped {
            generation: driver.driver.generation.wrapping_add(1),
            receipt: None,
            message: Box::new(Message::GalleryMouseDelivered),
        });
        assert_eq!(driver.driver.phase, Phase::AwaitVisibleReadHover(5, 9));
        driver.update(Message::Scoped {
            generation: driver.driver.generation,
            receipt: None,
            message: Box::new(Message::GalleryMouseDelivered),
        });
        assert_eq!(driver.driver.phase, Phase::VisibleReadSelect(5, 9));
    }

    #[test]
    fn typed_capacity_commands_reject_duplicates_and_wrong_scenario() {
        use crate::generated::{IntegrationControlKind as Kind, IntegrationControlReceipt};
        for sequence in [1, 2] {
            let mut driver = Controller::new(
                true,
                false,
                String::new(),
                String::new(),
                "512".into(),
                "rapid".into(),
            );
            driver.driver.phase = Phase::AwaitCapacityArm;
            let receipt = IntegrationControlReceipt {
                kind: Kind::CapacityArmed,
                sequence,
                progress: 0,
                failureline: 0,
                failure: String::new(),
                readgeneration: 0,
                compiledindex: 0,
            };
            let result = driver.receive_control(receipt.clone());
            assert_eq!(result.is_ok(), sequence == 1);
            if sequence == 1 {
                assert!(matches!(driver.driver.phase, Phase::CapacityPublish));
                assert!(driver.receive_control(receipt).is_err());
            }
            assert!(matches!(driver.driver.phase, Phase::Failed));
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
        driver.retained.configure_fixture(|fixture| fixture.atlas_row_extent = 160.0);
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
        driver.probes.configure_fixture(|fixture| fixture.gallery_drawn = Some((frame.presentation_revision, frame.content_sequence - 1)));
        drive(&mut driver, &model);
        assert!(matches!(driver.driver.phase, Phase::GalleryColdRead(17)));
        driver.probes.configure_fixture(|fixture| fixture.gallery_drawn = Some((frame.presentation_revision, frame.content_sequence)));
        drive(&mut driver, &model);
        assert!(matches!(driver.driver.phase, Phase::AwaitGalleryColdRead(17, 5)));
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
        assert_eq!(driver.retained.fixture().gallery_completion_held, Some((6, 47)));
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
            mismatch.retained.configure_fixture(|fixture| fixture.gallery_completion_held = Some(held));
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
        assert!(driver.retained.fixture().gallery_completion_held.is_none());
    }

    fn advance_receipt(sequence: u64) -> crate::generated::IntegrationControlReceipt {
        crate::generated::IntegrationControlReceipt {
            kind: crate::generated::IntegrationControlKind::Advance,
            sequence,
            progress: 0,
            failureline: 0,
            failure: String::new(),
            readgeneration: 0,
            compiledindex: 0,
        }
    }

    #[test]
    fn destructive_profile_continues_viewer_completion_into_annotation() {
        initialize_reporting(false, false);
        for window_close in [false, true] {
            let mut driver = Controller::new(
                true,
                window_close,
                "source".into(),
                "compiled".into(),
                "512".into(),
                "terminal".into(),
            );
            driver.configure_session("terminal", String::new(), String::new());
            assert_eq!(driver.driver.session.scenario(0), Some(("terminal", false)));
            assert_eq!(driver.driver.session.scenario(1), None);
            assert!(driver.driver.reuse_compiled);
            driver.driver.phase = Phase::ViewerNoAspect;
            driver.probes.configure_fixture(|fixture| fixture.viewer_drawn = Some((
                7,
                3,
                ViewerDraw {
                    crop: [0, 0, 512, 512],
                    container: Rectangle::default(),
                    image: Rectangle::default(),
                    fit_revision: 1,
                },
            )));
            driver.update(Message::Located {
                control: "explore.detail.aspect".into(),
                bounds: Rectangle::default(),
            });
            assert!(matches!(driver.driver.phase, Phase::OpenAnnotation));
            assert!(driver.driver.running());
            assert!(
                driver.receive_control(advance_receipt(2)).is_err(),
                "viewer evidence cannot settle a destructive Annotation workflow"
            );
            let (mut model, _) = crate::view_model::test_support::explore_presentation();
            let source = model.explore.snapshot.as_ref().unwrap().frame.clone();
            let snapshot = model.annotation.snapshot.as_mut().unwrap();
            snapshot.ready = true;
            snapshot.busy = false;
            snapshot.frame.revision = 23;
            snapshot.ui.documentrevision = 9;
            snapshot.inputdocumentepoch = 31;
            driver.driver.phase = Phase::AwaitAnnotation;
            let settings = crate::view::settings::SettingsModel::default();
            let router = crate::view::router::Router::default();
            drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Annotate, None));
            assert_eq!(driver.driver.phase, Phase::AwaitAnnotation);
            driver.observe_annotation_open(
                crate::generated::AnnotationOpen {
                    source,
                    originalcontent: true,
                },
                31,
            );
            for (epoch, busy, imported) in
                [(31, false, false), (32, true, false), (32, false, true)]
            {
                let snapshot = model.annotation.snapshot.as_mut().unwrap();
                snapshot.inputdocumentepoch = epoch;
                snapshot.busy = busy;
                drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Annotate, None));
                assert_eq!(driver.widgets.fixture().location_pending, imported);
                assert_eq!(
                    driver.driver.phase,
                    if imported {
                        Phase::AnnotationTool {
                            revision: model
                                .annotation
                                .snapshot
                                .as_ref()
                                .unwrap()
                                .ui
                                .interactionrevision,
                            tool: crate::generated::AnnotationTool::Box,
                        }
                    } else {
                        Phase::AwaitAnnotation
                    }
                );
            }
        }
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
        assert!(!driver.widgets.fixture().location_pending);
        for phase in [
            Phase::AwaitDetail(0),
            Phase::AwaitNext(99),
            Phase::AwaitPrevious(99),
            Phase::AwaitDetailAgain,
        ] {
            driver.driver.phase = phase.clone();
            drive(&mut driver, &model);
            assert_eq!(driver.driver.phase, phase);
            assert!(!driver.widgets.fixture().location_pending);
        }
        driver.driver.phase = Phase::AwaitExplore;
        assert!(crate::presentation_surface::accept_publication(frame));
        model.explore.snapshot.as_mut().unwrap().mode = crate::generated::ExploreMode::Gallery;
        drive(&mut driver, &model);
        assert_eq!(driver.driver.phase, Phase::AwaitExplore);
        assert!(!driver.widgets.fixture().location_pending);
        model.explore.snapshot.as_mut().unwrap().mode = crate::generated::ExploreMode::Detail;
        drive(&mut driver, &model);
        assert_eq!(driver.driver.phase, Phase::ExploreCloseDetail);
        assert!(driver.widgets.fixture().location_pending);
        // The packaged Wayland sequence exercises the browser click. This
        // native fixture starts at its receipt and checks native settlement.
        driver.driver.phase = Phase::AwaitExploreGallery;
        driver.widgets.configure_fixture(|fixture| fixture.location_pending = false);
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
                model.explore.snapshot.as_mut().unwrap().mode =
                    crate::generated::ExploreMode::Detail;
                drive(&mut driver, &model);
                assert_eq!(driver.driver.phase, Phase::AwaitExploreGallery);
                assert!(!driver.widgets.fixture().location_pending);
                model.explore.snapshot.as_mut().unwrap().mode = mode;
            }
            drive(&mut driver, &model);
            assert_eq!(driver.driver.phase, expected);
            assert!(!driver.widgets.fixture().location_pending);
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
        assert_eq!(driver.driver.phase, Phase::AwaitExplorePreparation(revision));
        drive(&mut driver, &model);
        assert_eq!(driver.driver.phase, Phase::AwaitExplorePreparation(revision));
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
        driver.probes.configure_fixture(|fixture| fixture.gallery_drawn = Some((frame.presentation_revision, source)));
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
            driver.probes.configure_fixture(|fixture| fixture.gallery_drawn = drawn);
            driver.driver.phase = Phase::AwaitAtlasEmpty;
            drive(&mut driver, &model);
            assert_eq!(
                driver.driver.phase,
                if ready {
                    Phase::AtlasEmpty
                } else {
                    Phase::AwaitAtlasEmpty
                }
            );
            assert_eq!(driver.widgets.fixture().location_pending, ready);
            driver.widgets.configure_fixture(|fixture| fixture.location_pending = false);
        }
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
        driver.retained.configure_fixture(|fixture| fixture.oversized_gallery = Some((
            iced::Size::new(896.0, 896.0),
            crate::generated::VisualExtent {
                width: 1920,
                height: 1080,
            },
        )));
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
                source: model.explore.snapshot.as_ref().unwrap().frame.clone(),
                originalcontent: true,
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

    #[test]
    fn first_native_gallery_draw_reports_placeholders_and_retains_pixels_in_enabled_and_quiet_modes()
     {
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
            let mut renderer = iced::futures::executor::block_on(
                <iced::Renderer as Headless>::new(Default::default(), Some("wgpu")),
            )
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
                assert!(fixture.controller.probes.accepts_message(&fixture.controller.driver, &fixture.controller.pixel_checks, &message));
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
            probe::with_observer_fixture(|observer| {
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
    fn quiet_failure_receipts_preserve_ui_error_kind_and_bounded_utf8() {
        use crate::application_codec::FromApplicationValue;
        for detail in [
            "inconsistent frame revision".to_owned(),
            "λ".repeat(crate::generated::INTEGRATION_FAILURE_MAX_BYTES),
        ] {
            initialize_reporting(false, false);
            let mut driver = Controller::new(
                true,
                false,
                String::new(),
                String::new(),
                String::new(),
                "quiet".into(),
            );
            let mut model = crate::view_model::test_support::bootstrapped();
            model.error = Some(crate::view_model::UiError::protocol(detail));
            drop(driver.advance(
                &model,
                &crate::view::settings::SettingsModel::default(),
                1.0,
                &crate::view::router::Router::default(),
                FeatureId::Explore,
                None,
            ));
            assert_eq!(driver.driver.phase, Phase::Failed);
            assert!(driver.driver.failure.starts_with("Protocol: "));
            assert!(driver.driver.failure.len() <= crate::generated::INTEGRATION_FAILURE_MAX_BYTES);
            assert!(driver.driver.failure.is_char_boundary(driver.driver.failure.len()));
            let (mut connection, _capture) =
                crate::transport_connection::Connection::test_channel();
            driver.publish_control(&mut connection);
            let mut wire = Vec::new();
            connection
                .flush(|bytes| {
                    wire.push(bytes.to_vec());
                    Ok(())
                })
                .unwrap();
            assert_eq!(wire.len(), 1);
            let envelope = crate::protocol::decode_envelope(&wire[0]).unwrap();
            let control =
                crate::generated::IntegrationControl::from_application_value(envelope.payload)
                    .unwrap();
            assert_eq!(control.receipt.failure, driver.driver.failure);
            assert_eq!(
                control.receipt.kind,
                crate::generated::IntegrationControlKind::Failed
            );
            assert!(crate::generated::integration_receipt_valid(
                &control.receipt
            ));
            let mut invalid = control.clone();
            invalid
                .receipt
                .failure
                .push_str(&"x".repeat(crate::generated::INTEGRATION_FAILURE_MAX_BYTES));
            assert!(invalid.encode().is_err());
            invalid = control;
            invalid.receipt.kind = crate::generated::IntegrationControlKind::Progress;
            invalid.receipt.failureline = 0;
            assert!(invalid.encode().is_err());
            invalid.receipt.kind = crate::generated::IntegrationControlKind::Advance;
            invalid.receipt.progress = 0;
            assert!(driver.receive_control(invalid.receipt).is_err());
            assert!(driver.driver.reporting.state_is_absent());
        }
        let mut disabled = Controller::new(
            false,
            false,
            String::new(),
            String::new(),
            String::new(),
            String::new(),
        );
        disabled.driver.fail_detail(|| panic!("disabled driver evaluated failure data"));
        assert!(disabled.driver.failure.is_empty());
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
        probe::with_observer_fixture(|observer| {
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
        assert!(
            displayed_detail(Some(surface), model.explore.snapshot.as_ref().unwrap()).is_none()
        );
        assert!(crate::presentation_surface::accept_publication(frame));
        assert!(
            displayed_detail(Some(surface), model.explore.snapshot.as_ref().unwrap()).is_some()
        );
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
            assert_eq!(driver.widgets.fixture().location_pending, expected_ready);
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
            probe::with_observer_fixture(|observer| assert!(observer.borrow().receipts.is_empty()));
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
        let generation = probe::with_observer_fixture(|observer| observer.borrow().generation);
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
            probe::with_observer_fixture(|observer| observer.borrow().generation),
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
        probe::with_observer_fixture(|observer| {
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
        controller.widgets.configure_fixture(|fixture| fixture.location_pending = true);
        controller.retained.configure_fixture(|fixture| fixture.atlas_baseline = Some((1, 5)));
        controller.driver.reporting
            .observe(|reporting| reporting.reported_style_bits = 7);
        controller.probes.configure_fixture(|fixture| fixture.annotation_pixels_receipt = Some(old_receipt.clone()));
        controller.probes.configure_fixture(|fixture| fixture.upscale_pixel_pending = Some(old_receipt));
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
        assert!(!controller.widgets.fixture().location_pending);
        assert!(controller.retained.fixture().atlas_baseline.is_none());
        assert!(controller.probes.fixture().upscale_pixel_pending.is_none());
        assert!(controller.probes.fixture().annotation_pixels_receipt.is_none());
        controller.driver.reporting
            .observe(|reporting| assert_eq!(reporting.reported_style_bits, 0));
        assert!(current_receipt(EXPLORE_GALLERY).is_none());
        assert_eq!(
            probe::with_observer_fixture(|observer| observer.borrow().identity),
            (0, 0)
        );
        controller.widgets.configure_fixture(|fixture| fixture.location_pending = true); // Same widget may already be armed in the replacement.
        while let Ok(message) = receiver.try_recv() {
            assert!(controller.update(message).is_none());
        }
        assert!(controller.widgets.fixture().location_pending);
        assert!(controller.probes.fixture().annotation_drawn.is_none());
        // A late result retains the original sender/generation even after reset.
        old_output
            .try_send(Message::SurfaceDrawn {
                presentation_revision: 5,
                source_revision: 1,
                viewer: None,
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.probes.fixture().annotation_drawn.is_none());
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
            assert!(!controller.probes.accepts_message(&controller.driver, &controller.pixel_checks, &queued));
            controller.update(queued);
            assert!(controller.probes.fixture().annotation_drawn.is_none());
            assert!(controller.probes.fixture().viewer_drawn.is_none());
            assert!(controller.probes.fixture().gallery_drawn.is_none());
            // Unchanged revisions and viewer fields must not suppress a new geometry receipt.
            report_surface_draw(control, 5, 1, true, 640, 480, 2, viewer);
            let queued = receiver.try_recv().unwrap();
            assert!(controller.probes.accepts_message(&controller.driver, &controller.pixel_checks, &queued));
            controller.update(queued);
            match control {
                EXPLORE_GALLERY => assert_eq!(controller.probes.configure_fixture(|fixture| fixture.gallery_drawn.take()), Some((5, 1))),
                explore::DETAIL_WORKSPACE_ID => {
                    assert_eq!(controller.probes.configure_fixture(|fixture| fixture.viewer_drawn.take()), Some((5, 1, viewer)))
                }
                _ => assert_eq!(controller.probes.configure_fixture(|fixture| fixture.annotation_drawn.take()), Some((5, 1))),
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
        probe::with_observer_fixture(|observer| observer.borrow_mut().receipts.remove(EXPLORE_GALLERY));
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
        assert!(!controller.probes.accepts_message(&controller.driver, &controller.pixel_checks, &queued));
        controller.update(queued);
        assert!(controller.retained.fixture().atlas_receipt.is_none());
        report_atlas_draw(moved_draw.clone(), true, 1.0);
        let queued = receiver.try_recv().unwrap();
        assert!(controller.probes.accepts_message(&controller.driver, &controller.pixel_checks, &queued));
        controller.update(queued);
        assert_eq!(controller.retained.fixture().atlas_receipt, Some(moved_draw));
        old_output.generation = controller.driver.generation;
        old_output
            .try_send(Message::SurfaceDrawn {
                presentation_revision: 5,
                source_revision: 1,
                viewer: None,
            })
            .unwrap();
        assert!(
            !controller.probes.accepts_message(&controller.driver, &controller.pixel_checks, &receiver.try_recv().unwrap()),
            "physical messages cannot use a generation-only output"
        );
        let replacement_subscription = std::sync::Arc::new(());
        probe::with_observer_fixture(|observer| {
            observer.borrow_mut().subscription = Some(replacement_subscription.clone())
        });
        drop(SurfaceDrawSubscription::for_test(old_subscription));
        assert!(probe::with_observer_fixture(|observer| observer.borrow().output.is_some()));
        assert!(current_receipt(EXPLORE_GALLERY).is_some());
        drop(SurfaceDrawSubscription::for_test(replacement_subscription));
        assert!(probe::with_observer_fixture(|observer| observer.borrow().output.is_none()));
        controller.driver.phase = Phase::Failed;
        assert!(
            controller
                .reset_scenario(String::new(), String::new(), String::new(), String::new())
                .is_err()
        );
        initialize_reporting(false, false);
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
        assert!(probe::with_observer_fixture(|observer| observer.borrow().atlas.is_none()));
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
            assert!(!controller.probes.accepts_message(&controller.driver, &controller.pixel_checks, &stale));
            controller.update(stale);
        }
        assert_eq!(
            probe::with_observer_fixture(|observer| observer.borrow().atlas.clone()),
            Some(draw.clone())
        );
        assert!(controller.retained.fixture().atlas_pixels.is_none() && controller.retained.fixture().atlas_composition.is_none());
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
        assert_eq!(controller.retained.fixture().atlas_pixels, Some(draw.clone()));
        assert_eq!(controller.retained.fixture().atlas_composition, Some(draw));
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
                    assert!(controller.probes.prepare_control_probe(&controller.widgets, controller.annotation_scenario.fixture().copy_swatch_color, controller.annotation_scenario.fixture().copy_capability_available));
                    controller.probes.configure_fixture(|fixture| fixture.control_probe.take()).unwrap().output
                }
                2 => {
                    controller.driver.phase = Phase::CopyProductWait;
                    assert!(controller.probes.prepare_annotation_probe(&controller.widgets, 
                        frame.content_sequence,
                        frame.presentation_revision,
                        [frame.content_width, frame.content_height],
                        vec![1.0; 7]
                    ));
                    controller.probes.configure_fixture(|fixture| fixture.annotation_probe.take()).unwrap().output
                }
                _ => {
                    controller.driver.phase = Phase::AwaitExploreReady;
                    controller.retained.prepare_upscale_probe(&mut controller.probes, 
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
            assert!(controller.probes.fixture().annotation_pixels_pending.is_none());
            assert!(controller.probes.fixture().control_probe_receipt.is_none());
            assert!(controller.probes.fixture().upscale_pixel_pending.is_none());
            assert!(!controller.annotation_scenario.fixture().copy_capability_ready && !controller.annotation_scenario.fixture().copy_swatch_ready);
            assert!(
                controller.probes.fixture().annotation_pixels_receipt.is_none()
                    && controller.retained.fixture().upscale_pixels.is_none()
            );

            let mut same_frame = arm(controller, bounds);
            old.try_send(message(ProbeOutcome::Invalidated)).unwrap();
            let stale = receiver.try_recv().unwrap();
            assert!(
                !controller.probes.accepts_message(&controller.driver, &controller.pixel_checks, &stale),
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
                assert!(!controller.probes.accepts_message(&controller.driver, &controller.pixel_checks, &stale));
                controller.update(stale);
            }
            assert_eq!(controller.driver.phase, phase);
            let observed = controller.probes.fixture();
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
                0 => assert!(controller.annotation_scenario.fixture().copy_capability_ready),
                1 => assert!(controller.annotation_scenario.fixture().copy_swatch_ready),
                2 => assert_eq!(controller.probes.fixture().annotation_pixels_receipt, replacement.receipt),
                _ => assert_eq!(
                    controller.retained.fixture().upscale_pixels,
                    Some((frame.content_sequence, frame.presentation_revision, 1, 1))
                ),
            }
            // A new current observation must retain both measured and adapter
            // failures. Upscale's wait phase owns checksum/color validation.
            if consumer == 2 {
                controller.probes.configure_fixture(|fixture| fixture.annotation_pixels_pending = None);
            }
            let mut current = arm(controller, moved);
            current
                .try_send(message(ProbeOutcome::Observed(1, 0)))
                .unwrap();
            controller.update(receiver.try_recv().unwrap());
            if consumer == 3 {
                assert_eq!(
                    controller.retained.fixture().upscale_pixels,
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
                controller.annotation_scenario.configure_fixture(|fixture| fixture.copy_swatch_color = [48.0, 80.0, 112.0]);
                controller.annotation_scenario.configure_fixture(|fixture| fixture.copy_capability_available = true);
                assert!(controller.probes.prepare_control_probe(&controller.widgets, controller.annotation_scenario.fixture().copy_swatch_color, controller.annotation_scenario.fixture().copy_capability_available));
                let prepared = controller.probes.fixture().control_probe.unwrap();
                assert_eq!(prepared.color, controller.annotation_scenario.fixture().copy_swatch_color);
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
                    assert!(!controller.probes.prepare_annotation_probe(&controller.widgets, 
                        source,
                        presentation,
                        extent,
                        vec![1.0; 7],
                    ));
                    assert!(controller.probes.fixture().annotation_probe.is_none());
                    assert!(controller.probes.fixture().annotation_pixels_pending.is_none());
                }
                assert!(controller.probes.prepare_annotation_probe(&controller.widgets, 
                    frame.content_sequence,
                    frame.presentation_revision,
                    [frame.content_width, frame.content_height],
                    vec![1.0; 7]
                ));
                let prepared = controller.probes.fixture().annotation_probe.unwrap();
                assert_eq!(prepared.source, frame.content_sequence);
                assert_eq!(prepared.presentation, frame.presentation_revision);
                assert_eq!(prepared.extent, [frame.content_width, frame.content_height]);
                assert_eq!(prepared.pixels, vec![1.0; 7]);
            }
            controller.widgets.configure_fixture(|fixture| fixture.location_pending = true);
            let moved = Rectangle { x: 17.0, ..bounds };
            record_probe_draw("workflow.visual.workspace", surface, bounds, moved, bounds);
            assert!(
                !controller.probes.prepare_annotation_probe(&controller.widgets, 999, 999, [1, 1], Vec::new()),
                "pending location cannot be overwritten"
            );
            assert!(!controller.probes.prepare_control_probe(&controller.widgets, controller.annotation_scenario.fixture().copy_swatch_color, controller.annotation_scenario.fixture().copy_capability_available));
            let phase = controller.driver.phase.clone();
            // The obsolete location cannot validate bounds or stamp a new
            // output onto the saved old frame. Normal advance can now rearm.
            controller.update(Message::Located {
                control: ANNOTATION_SURFACE.into(),
                bounds: Rectangle::default(),
            });
            assert_eq!(controller.driver.phase, phase);
            assert!(!controller.widgets.fixture().location_pending);
            assert!(controller.probes.fixture().annotation_probe.is_none() && controller.probes.fixture().control_probe.is_none());
            assert!(
                controller.probes.fixture().annotation_pixels_pending.is_none()
                    && controller.probes.fixture().control_probe_receipt.is_none()
            );
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

    #[test]
    fn integration_phases_assign_bounded_progress_deadline_classes() {
        assert_eq!(Phase::AwaitBootstrap.deadline_class(), "startup");
        assert_eq!(Phase::AwaitCompileCompletion.deadline_class(), "work");
        assert_eq!(
            Phase::AwaitExploreDatasetReopen {
                revision: 1,
                frame_revision: 1,
            }
            .deadline_class(),
            "work"
        );
        assert_eq!(
            Phase::AwaitUpscale {
                kernel: 0,
                source_width: 1,
                source_height: 1,
                upscale_revision: 1,
                upscale_frame_revision: 1,
                presentation_revision: 1,
            }
            .deadline_class(),
            "work"
        );
        assert_eq!(Phase::AwaitPointer(1).deadline_class(), "work");
        for stage in 0..=3 {
            let first = Phase::CopyListSetup {
                stage,
                revision: 10,
            };
            let settled = Phase::CopyListSetup {
                stage,
                revision: 11,
            };
            assert_eq!(first.deadline_class(), "work");
            assert_eq!(settled.deadline_class(), "work");
            assert_ne!(first, settled);
            assert_eq!(
                settled,
                Phase::CopyListSetup {
                    stage,
                    revision: 11
                }
            );
        }
        assert_eq!(Phase::Complete.deadline_class(), "work");
        assert_eq!(Phase::AwaitSettings.deadline_class(), "interaction");
    }

    #[test]
    fn measured_reveal_handles_both_edges_visible_and_oversized_controls() {
        assert_eq!(reveal_axis(120.0, 40.0, 100.0, 200.0), 0.0);
        assert_eq!(reveal_axis(80.0, 40.0, 100.0, 200.0), -21.0);
        assert_eq!(reveal_axis(280.0, 40.0, 100.0, 200.0), 21.0);
        assert_eq!(reveal_axis(120.0, 400.0, 100.0, 200.0), 20.0);
        assert_eq!(reveal_axis(100.0, 200.0, 100.0, 200.0), 0.0);
        assert_eq!(reveal_axis(500.0, 0.0, 100.0, 200.0), 0.0);
        assert_eq!(reveal_axis(500.0, 40.0, 100.0, 0.0), 0.0);
        for start in [30.2, 2598.2] {
            let offset = reveal_axis(start, 46.4, 52.0, 771.3).round();
            assert!(start - offset >= 52.0);
            assert!(start - offset + 46.4 <= 823.3);
        }
    }

    #[test]
    fn annotation_reveal_proves_subregions_without_rescaling_full_geometry() {
        let full = Rectangle {
            x: -100.0,
            y: -200.0,
            width: 1000.0,
            height: 1600.0,
        };
        let viewport = Rectangle {
            x: 0.0,
            y: 50.0,
            width: 700.0,
            height: 500.0,
        };
        let measured = ControlBounds {
            target: full,
            page: viewport,
            horizontal: viewport,
        };
        let visible = measured.visible().unwrap();
        assert_eq!(visible, viewport);
        assert!(!contains_rectangle(
            visible,
            measured.requested(AnnotationReveal::Control).unwrap()
        ));
        let request = AnnotationReveal::Source {
            extent: [1000.0, 1600.0],
            region: Rectangle {
                x: 250.0,
                y: 300.0,
                width: 10.0,
                height: 20.0,
            },
            margin: 2.0,
        };
        let requested = measured.requested(request).unwrap();
        assert_eq!(
            requested,
            Rectangle {
                x: 148.0,
                y: 98.0,
                width: 14.0,
                height: 24.0
            }
        );
        assert!(contains_rectangle(visible, requested));
        assert_eq!(measured.target, full);
        let trailing = ControlBounds {
            target: Rectangle { y: 400.0, ..full },
            ..measured
        };
        assert!(!contains_rectangle(
            viewport,
            trailing.requested(request).unwrap()
        ));
        let clamped = ControlBounds {
            target: Rectangle {
                x: 650.0,
                width: 100.0,
                ..full
            },
            ..measured
        };
        assert!(!contains_rectangle(
            clamped.visible().unwrap(),
            clamped.target
        ));
        let missing = ControlBounds {
            page: Rectangle::default(),
            ..measured
        };
        assert!(missing.visible().is_none());
        assert!(missing.requested(request).is_none());
        let absent = ControlBounds {
            target: Rectangle::default(),
            ..measured
        };
        assert!(absent.visible().is_none());
        assert!(absent.requested(request).is_none());
    }

    #[test]
    fn annotation_layout_sequence_supports_initially_wide_and_narrow_scales() {
        let constraint = crate::generated::constraint_uiuiscale();
        let minimum = constraint.minimum.unwrap() as f32;
        let maximum = constraint.maximum.unwrap() as f32;
        assert_eq!(maximum, 1.75);
        for (original, initially_narrow) in [(1.0, false), (maximum, true)] {
            assert!((minimum..=maximum).contains(&original));
            let physical_width = 1500.0;
            let original_width = physical_width / original;
            assert_eq!(
                original_width < crate::view::PAGE_MIN_WIDTH,
                initially_narrow
            );
            let wide = annotation_layout_scale(original, original_width, false).unwrap();
            assert!(physical_width / wide >= crate::view::PAGE_MIN_WIDTH);
            let narrow = annotation_layout_scale(wide, physical_width / wide, true).unwrap();
            assert!(physical_width / narrow < crate::view::PAGE_MIN_WIDTH);
            let mut controller = Controller::new(
                false,
                false,
                String::new(),
                String::new(),
                "512".into(),
                "copy".into(),
            );
            controller.annotation_scenario.configure_fixture(|fixture| fixture.copy_original_scale = original);
            let model = ApplicationModel::default();
            drop(controller.annotation_scenario.copy_scale_transition(&mut controller.driver, &model, narrow, CopyScaleStage::Restore));
            assert_eq!(controller.annotation_scenario.fixture().copy_requested_scale, original);
            assert_eq!(
                controller.driver.phase,
                Phase::CopyAwaitScale(CopyScaleStage::Restore)
            );
        }
    }

    #[test]
    fn annotation_narrow_scale_obeys_native_bounds_at_packaged_dpi_widths() {
        let constraint = crate::generated::constraint_uiuiscale();
        for (unscaled_width, current_scale) in
            [(1500.0, 1.0), (1280.0, 1.5), (1500.0, 1.75), (1000.0, 1.25)]
        {
            let logical_width = unscaled_width / current_scale;
            let scale = annotation_layout_scale(current_scale, logical_width, true)
                .expect("packaged width reaches narrow layout");
            assert!(f64::from(scale) >= constraint.minimum.unwrap());
            assert!(f64::from(scale) <= constraint.maximum.unwrap());
            assert!(unscaled_width / scale < crate::view::PAGE_MIN_WIDTH);
        }
        let maximum = constraint.maximum.unwrap() as f32;
        assert!(annotation_layout_scale(maximum, 1920.0 / maximum, true).is_err());
        assert!(annotation_layout_scale(1.0, 1920.0, true).is_err());
        assert!(annotation_layout_scale(1.0, f32::NAN, true).is_err());
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
        probe::with_observer_fixture(|observer| {
            observer.borrow_mut().output = Some(ScenarioOutput::new(controller.driver.generation, sender))
        });
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
        assert!(controller.retained.fixture().atlas_pixels.is_none());
        atlas_probe_output(false)
            .unwrap()
            .try_send(Message::AtlasPixels {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(0, 0),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.retained.fixture().atlas_pixels.is_none());
        atlas_probe_output(false)
            .unwrap()
            .try_send(Message::AtlasPixels {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(1, 1),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert_eq!(controller.retained.fixture().atlas_pixels, Some(draw.clone()));
        atlas_probe_output(true)
            .unwrap()
            .try_send(Message::AtlasComposition {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(1, 0),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.retained.fixture().atlas_composition.is_none());
        atlas_probe_output(true)
            .unwrap()
            .try_send(Message::AtlasComposition {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(0, 0),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.retained.fixture().atlas_composition.is_none());
        atlas_probe_output(true)
            .unwrap()
            .try_send(Message::AtlasComposition {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(1, 1),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert_eq!(controller.retained.fixture().atlas_composition, Some(draw));
        probe::with_observer_fixture(|observer| observer.borrow_mut().output = None);
        initialize_reporting(false, false);
    }

    #[test]
    fn inactive_controllers_do_not_observe_snapshots_or_consume_callbacks() {
        initialize_reporting(false, false);
        let mut model = ApplicationModel::default();
        model
            .install_bootstrap(
                crate::generated::SCHEMA_FINGERPRINT,
                crate::generated::application_snapshot_defaults()
                    .expect("generated snapshots")
                    .into_iter()
                    .map(|fact| fact.value)
                    .collect(),
            )
            .expect("bootstrap");
        let snapshot = model.explore.snapshot.as_mut().expect("Explore snapshot");
        snapshot.revision = 1;
        snapshot.order.visibleindices = vec![0, 1, 2];
        let settings = crate::view::settings::SettingsModel::default();
        let router = crate::view::router::Router::default();

        for phase in [Phase::Disabled, Phase::Complete, Phase::Failed] {
            let mut controller = Controller::new(
                false,
                false,
                String::new(),
                String::new(),
                String::new(),
                String::new(),
            );
            controller.driver.phase = phase;
            assert_eq!(
                controller
                    .advance(&model, &settings, 2.0, &router, FeatureId::Explore, None)
                    .units(),
                0
            );
            assert_eq!(controller.driver.input_scale, 1.0);
            assert!(controller.driver.reporting.state_is_absent());

            controller.widgets.configure_fixture(|fixture| fixture.location_pending = true);
            let phase = std::mem::discriminant(&controller.driver.phase);
            for message in [
                Message::Advance,
                Message::SurfaceDrawn {
                    presentation_revision: 1,
                    source_revision: 1,
                    viewer: None,
                },
                Message::Located {
                    control: EXPLORE_CARD.to_string(),
                    bounds: Rectangle::default(),
                },
            ] {
                assert!(controller.update(message).is_none());
                assert_eq!(std::mem::discriminant(&controller.driver.phase), phase);
                assert!(controller.widgets.fixture().location_pending);
                assert!(controller.probes.fixture().annotation_drawn.is_none());
            }
        }
    }
