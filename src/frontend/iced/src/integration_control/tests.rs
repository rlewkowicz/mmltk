use crate::generated::FeatureId;
use crate::integration_control::{Controller, Message, Phase, initialize_reporting};
use crate::integration_control::probe::ViewerDraw;
use crate::integration_control::retained::EXPLORE_CARD;
use crate::view_model::ApplicationModel;
use iced::Rectangle;
pub(super) fn advance_receipt(sequence: u64) -> crate::generated::IntegrationControlReceipt {
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
        driver.probes.record_surface_draw(7, 3, Some(
            ViewerDraw {
                crop: [0, 0, 512, 512],
                container: Rectangle::default(),
                image: Rectangle::default(),
                fit_revision: 1,
            },
        ));
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
            assert_eq!(driver.widgets.location_pending(), imported);
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

        controller.widgets.begin_location();
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
            assert!(controller.widgets.location_pending());
            assert!(controller.probes.draws().annotation.is_none());
        }
    }
}
