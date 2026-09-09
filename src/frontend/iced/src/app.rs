use iced::{Subscription, Task};

use crate::fluent_theme::{Element, Theme};
use crate::generated::{
    AnnotationOpen, AnnotationSave, ExploreNavigate, ExploreNavigation, ExploreOpen, ExploreSelect,
    ExploreViewportUpdate, FeatureId, FileDialogOpen, LiveStart, PresentationCapabilityCondition,
    PresentationSnapshot, SettingsResetRequest, Train, VisualExtent, VisualFrame,
};
use crate::message::Message;
use crate::presentation_surface::{FrameReady, Surface};
use crate::protocol::client_records::Intent;
use crate::protocol::{Bootstrap, IntentReply, RendererObservation, SystemEvent};
use crate::transport::{TransportConfig, TransportEvent};
use crate::transport_connection::Connection;
use crate::view::settings::{EditCadence, EditSchedule};
use crate::view_model::{ApplicationIntentEndpoint, ApplicationModel, UiError};

mod annotation;
mod explore;
pub mod presentation;
mod settings;
mod transport;
mod workflows;

pub(crate) const SETTINGS_PERSIST_DEBOUNCE_MS: u32 = 450;

pub struct App {
    config: TransportConfig,
    connection: Option<Connection>,
    peer_generation: u64,
    pub model: ApplicationModel,
    presentation: presentation::Controller,
    workspace: crate::view::router::Router,
    settings: crate::view::settings::Component,
    diagnostics: crate::view::diagnostics::Component,
    integration: Option<crate::integration_control::Controller>,
}

pub fn boot() -> (App, Task<Message>) {
    let config = TransportConfig::from_page();
    crate::presentation_surface::initialize_diagnostics(config.surface_trace, config.pixel_trace);
    crate::integration_control::initialize_reporting(config.integration, config.integration_pixel_fixture);
    let integration = config.integration.then(|| {
        crate::integration_control::Controller::new(
            true,
            config.integration_window_close,
            config.integration_dataset_source.clone(),
            config.integration_compiled_directory.clone(),
            config.integration_resolution.clone(),
            config.integration_viewer_scenario.clone(),
        )
    });
    (
        App {
            config,
            connection: None,
            peer_generation: 0,
            model: ApplicationModel::default(),
            presentation: presentation::Controller::default(),
            workspace: crate::view::router::Router::default(),
            settings: crate::view::settings::Component::default(),
            diagnostics: crate::view::diagnostics::Component::default(),
            integration,
        },
        Task::none(),
    )
}

pub fn theme(app: &App) -> Theme {
    crate::fluent_theme::app_theme(
        app.settings
            .draft()
            .as_ref()
            .is_some_and(|draft| draft.ui.darkmode),
    )
}

pub fn scale_factor(app: &App) -> f32 {
    app.settings.applied_scale()
}

pub fn subscription(app: &App) -> Subscription<Message> {
    Subscription::batch([
        crate::transport::subscription(app.config.clone()).map(Message::Transport),
        iced::window::events().map(|(_, event)| Message::Window(event)),
        crate::presentation_surface::subscription()
            .map(presentation::Message::Surface)
            .map(Message::Presentation),
        app.integration.as_ref().map_or_else(Subscription::none, |integration| {
            integration.subscription().map(Message::Integration)
        }),
        if app.workspace.active() == crate::generated::FeatureId::Annotate
            && !app.settings.state().open
        {
            crate::view::annotation::shortcuts().map(|message| {
                Message::Workspace(crate::view::router::Message::Annotation(message))
            })
        } else {
            Subscription::none()
        },
    ])
}

pub fn update(app: &mut App, message: Message) -> Task<Message> {
    let previous_surface = app.presentation.surface();
    let mut task = Task::none();
    match message {
        Message::Transport(event) => task = app.on_transport(event),
        Message::Window(event) => task = app.on_window(event),
        Message::Presentation(message) => task = app.on_presentation(message),
        Message::ExploreWritable {
            peer_generation,
            result,
        } => task = app.on_explore_writable(peer_generation, result),
        Message::Workspace(message) => {
            if let Some(integration) = app.integration.as_ref() {
                integration
                    .observe_workspace_message(&message, app.workspace.active());
            }
            task = app.on_workspace(message);
        }
        Message::FileDialog(message) => app.on_file_dialog(message),
        Message::Settings(message) => task = app.on_settings(message),
        Message::Error(message) => task = app.on_error(message),
        Message::Diagnostics(message) => app.diagnostics.update(message),
        Message::Integration(message) => {
            if let Some(integration) = app.integration.as_mut()
                && let Some(message) = integration.update(message)
            {
                task = app.on_workspace(crate::view::router::Message::Train(message));
            }
        }
    }
    app.reconcile_surface_frame();
    let frame = app.presentation.surface().and_then(|surface| surface.frame);
    let presentation_task = app
        .presentation
        .redraw(previous_surface)
        .map(Message::Presentation);
    Task::batch([
        task,
        presentation_task,
        if let Some(integration) = app.integration.as_mut() {
            integration.advance(
                &app.model,
                app.settings.state(),
                app.settings.applied_scale(),
                &app.workspace,
                app.workspace.active(),
                frame,
            )
        } else {
            Task::none()
        },
    ])
}
#[cfg(test)]
mod route_tests {
    use super::*;
    use crate::application_codec::IntoApplicationValue;

    fn installed_app() -> App {
        let (mut app, task) = boot();
        drop(task);
        let snapshots = crate::generated::application_snapshot_defaults()
            .expect("generated defaults")
            .into_iter()
            .map(|fact| fact.value)
            .collect();
        app.model
            .install_bootstrap(crate::generated::SCHEMA_FINGERPRINT, snapshots)
            .expect("bootstrap");
        let settings = app.model.settings_snapshot.as_ref().expect("Settings");
        app.settings.install(settings);
        app.workspace.bootstrap_components(&app.model);
        app.workspace
            .rebase(settings.settingsstate.currentview, &app.model);
        app
    }

    fn settings_event(app: &mut App, snapshot: crate::generated::SettingsUiState) {
        app.reduce_event(SystemEvent {
            state_revision: 0,
            delivery: crate::generated::EventDelivery::Critical,
            event: crate::generated::ApplicationEvent::SettingsSettingsChanged(
                crate::generated::SettingsChanged { snapshot },
            ),
        });
    }

    fn reset_ordering_fixture(app: &mut App) -> (u64, crate::generated::SettingsUiState) {
        let mut current = app.model.settings_snapshot.clone().expect("Settings");
        current.revision += 1;
        let constraint = crate::generated::constraint_uiuiscale();
        let minimum = constraint.minimum.unwrap() as f32;
        let maximum = constraint.maximum.unwrap() as f32;
        current.settingsstate.ui.uiscale = minimum;
        settings_event(app, current);
        assert_eq!(scale_factor(app), minimum);

        drop(app.on_settings(crate::view::settings::Message::UiScaleChanged(maximum)));
        assert_eq!(app.settings.draft().unwrap().ui.uiscale, maximum);
        assert_eq!(scale_factor(app), minimum);

        let correlation = app
            .model
            .begin_intent(ApplicationIntentEndpoint::SettingsReset)
            .unwrap();
        let mut reset = app.model.settings_snapshot.clone().expect("Settings");
        reset.revision += 1;
        reset.settingsstate.ui.uiscale = 1.0;
        (correlation, reset)
    }

    fn settings_reset_reply(
        app: &mut App,
        correlation: u64,
        snapshot: crate::generated::SettingsUiState,
    ) {
        app.reduce_reply(IntentReply {
            correlation,
            result: Ok(snapshot.into_application_value()),
        });
    }

    #[test]
    fn stale_settings_event_does_not_rollback_optimistic_navigation() {
        let mut app = installed_app();
        let mut installed = app.model.settings_snapshot.clone().expect("Settings");
        installed.revision += 2;
        installed.settingsstate.currentview = FeatureId::Train;
        settings_event(&mut app, installed.clone());
        app.workspace.select(FeatureId::Export);

        installed.revision -= 1;
        settings_event(&mut app, installed);
        assert_eq!(app.workspace.active(), FeatureId::Export);
    }

    #[test]
    fn newly_installed_settings_route_acknowledgement_rebases_router() {
        let mut app = installed_app();
        app.workspace.select(FeatureId::Export);
        let mut installed = app.model.settings_snapshot.clone().expect("Settings");
        installed.revision += 1;
        installed.settingsstate.currentview = FeatureId::Validate;
        settings_event(&mut app, installed);
        assert_eq!(app.workspace.active(), FeatureId::Validate);
    }

    #[test]
    fn failed_settings_save_rebases_route_to_authoritative_settings() {
        let mut app = installed_app();
        app.workspace.select(FeatureId::Export);
        app.settings
            .state_mut()
            .edit(EditCadence::Immediate, |draft| {
                crate::generated::edit_currentview(draft, FeatureId::Export)
            })
            .unwrap();
        assert!(app.settings.state_mut().take_request().is_some());
        let correlation = app
            .model
            .begin_intent(ApplicationIntentEndpoint::SettingsUpdate)
            .unwrap();
        app.reduce_reply(IntentReply {
            correlation,
            result: Err(crate::protocol::ApplicationError {
                category: crate::generated::ApplicationErrorCategory::Failed,
                detail: "settings rejected".into(),
            }),
        });
        assert_eq!(
            app.workspace.active(),
            app.model
                .settings_snapshot
                .as_ref()
                .unwrap()
                .settingsstate
                .currentview
        );
        assert!(!app.settings.has_local_edits());
    }

    #[test]
    fn reconnect_bootstrap_rebases_route_from_new_authoritative_settings() {
        let mut app = installed_app();
        app.workspace.select(FeatureId::Export);
        let snapshots = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .map(|fact| match fact.value {
                crate::generated::ApplicationSnapshot::Settings(mut settings) => {
                    settings.settingsstate.currentview = FeatureId::Validate;
                    crate::generated::ApplicationSnapshot::Settings(settings)
                }
                snapshot => snapshot,
            })
            .collect();
        app.install_bootstrap(Bootstrap {
            input_epoch: 1,
            schema_fingerprint: crate::generated::SCHEMA_FINGERPRINT,
            snapshots,
        });
        assert_eq!(app.workspace.active(), FeatureId::Validate);
        assert_eq!(
            app.model.connection,
            crate::view_model::ConnectionState::Connected
        );
    }

    #[test]
    fn root_scale_factor_uses_applied_scale_until_slider_release() {
        let mut app = installed_app();
        let original = scale_factor(&app);
        let constraint = crate::generated::constraint_uiuiscale();
        let changed = constraint
            .minimum
            .map(|value| value as f32)
            .filter(|value| (*value - original).abs() > f32::EPSILON)
            .unwrap_or(constraint.maximum.unwrap() as f32);

        drop(app.on_settings(crate::view::settings::Message::UiScaleChanged(changed)));
        assert_eq!(app.settings.draft().unwrap().ui.uiscale, changed);
        assert_eq!(scale_factor(&app), original);

        drop(app.on_settings(crate::view::settings::Message::UiScaleReleased));
        assert_eq!(scale_factor(&app), changed);
    }

    #[test]
    fn settings_reset_settles_deferred_scale_when_event_arrives_before_reply() {
        let mut app = installed_app();
        let (correlation, reset) = reset_ordering_fixture(&mut app);

        settings_event(&mut app, reset.clone());
        assert_ne!(
            app.settings.draft().unwrap().ui.uiscale,
            reset.settingsstate.ui.uiscale
        );
        settings_reset_reply(&mut app, correlation, reset.clone());

        assert_eq!(
            app.settings.draft().unwrap().ui.uiscale,
            reset.settingsstate.ui.uiscale
        );
        assert_eq!(scale_factor(&app), reset.settingsstate.ui.uiscale);
        assert!(!app.settings.has_local_edits());
    }

    #[test]
    fn settings_reset_settles_deferred_scale_when_reply_arrives_before_event() {
        let mut app = installed_app();
        let (correlation, reset) = reset_ordering_fixture(&mut app);

        settings_reset_reply(&mut app, correlation, reset.clone());
        settings_event(&mut app, reset.clone());

        assert_eq!(
            app.settings.draft().unwrap().ui.uiscale,
            reset.settingsstate.ui.uiscale
        );
        assert_eq!(scale_factor(&app), reset.settingsstate.ui.uiscale);
        assert!(!app.settings.has_local_edits());
    }
}

pub fn view(app: &App) -> Element<'_, Message> {
    crate::view::view(
        &app.model,
        app.presentation.surface(),
        &app.diagnostics,
        &app.workspace,
        &app.settings,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::generated::PresentationSourceKind;

    fn install_default_bootstrap(app: &mut App) {
        let snapshots = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .map(|snapshot| snapshot.value)
            .collect();
        app.install_bootstrap(Bootstrap {
            input_epoch: 1,
            schema_fingerprint: crate::generated::SCHEMA_FINGERPRINT,
            snapshots,
        });
    }

    fn install_ready_annotation(app: &mut App) {
        install_default_bootstrap(app);
        let annotation = app.model.annotation.snapshot.as_mut().unwrap();
        annotation.ready = true;
        annotation.busy = false;
        annotation.cancellationrequested = false;
        annotation.ui.documentrevision = 1;
        annotation.frame =
            crate::view_model::test_support::visual_frame(PresentationSourceKind::Annotation, 1);
        app.workspace.rebase_annotation(&app.model);
    }

    fn local_annotation_press(app: &mut App) -> crate::generated::AnnotationPointer {
        let message = crate::view::router::Message::Annotation(
            crate::view::annotation::Message::Workspace(crate::view::workspace::Message::Gesture(
                crate::presentation_surface::SurfaceGesture {
                    kind: crate::presentation_surface::SurfaceGestureKind::Pointer,
                    sample: crate::presentation_surface::SurfaceSample {
                        width: 640,
                        height: 480,
                        x: 20,
                        y: 30,
                        content_x: 20,
                        content_y: 30,
                        pressed: true,
                    },
                },
            )),
        );
        let Some(crate::view::router::Outcome::Annotation(
            crate::view::annotation::Outcome::Pointer(pointer),
        )) = app
            .workspace
            .update(&mut app.model, &mut app.settings, message)
            .unwrap()
        else {
            panic!("local typed annotation pointer")
        };
        pointer
    }

    fn stage_annotation_retirement(app: &mut App) {
        install_default_bootstrap(app);
        app.workspace.select(FeatureId::Annotate);
        app.settings
            .state_mut()
            .edit(EditCadence::Debounced, |draft| {
                let darkmode = !draft.ui.darkmode;
                crate::generated::edit_uidarkmode(draft, darkmode)
            })
            .unwrap();
    }

    fn annotation_pointer(
        phase: crate::generated::AnnotationPointerPhase,
        sequence: u64,
    ) -> crate::generated::AnnotationPointer {
        crate::generated::AnnotationPointer {
            phase,
            interactionid: 1,
            sequence,
            target: crate::generated::AnnotationPointerTarget {
                object: None,
                element: None,
                role: None,
            },
            point: crate::generated::AnnotationPoint { x: 20.0, y: 20.0 },
            brushradius: crate::generated::default_uiannotationbrushradius().unwrap() as u16,
        }
    }

    fn assert_persistent_owners_retired(app: &App) {
        assert!(app.connection.is_none());
        assert!(app.settings.draft().is_none());
        assert_eq!(app.workspace.active(), FeatureId::Train);
        assert_eq!(
            app.model.connection,
            crate::view_model::ConnectionState::Reconnecting
        );
    }

    fn install_current_model_selection(app: &mut App) {
        let mut settings = app.model.settings_snapshot.clone().unwrap();
        settings.revision += 1;
        settings.settingsstate.workflows.train.modelsource =
            crate::generated::ModelSelectionSource::Canonical;
        settings.settingsstate.workflows.train.modelinput =
            crate::generated::ModelArtifactInputKind::Weights;
        app.reduce_event(SystemEvent {
            state_revision: 0,
            delivery: crate::generated::EventDelivery::Critical,
            event: crate::generated::ApplicationEvent::SettingsSettingsChanged(
                crate::generated::SettingsChanged { snapshot: settings },
            ),
        });
        let receipt = app
            .model
            .workflow
            .model_selection_receipt(crate::generated::FeatureId::Train)
            .unwrap();
        let correlation = app.model.begin_model_select_intent(receipt).unwrap();
        app.model.reduce_reply(
            correlation,
            Ok(crate::generated::ApplicationReply::ModelSelect({
                let mut snapshot = app.model.model_snapshot.clone().unwrap();
                let train = &app.settings.draft().unwrap().workflows.train;
                snapshot.generation = snapshot.generation.checked_add(1).unwrap();
                snapshot.active = false;
                snapshot.selection.key.workflow = crate::generated::FeatureId::Train;
                snapshot.selection.key.source = crate::generated::ModelSelectionSource::Canonical;
                snapshot.selection.key.input = crate::generated::ModelArtifactInputKind::Weights;
                snapshot.selection.key.preset = train.request.presetname.clone();
                snapshot.selection.key.resolution = train.request.resolution as u32;
                snapshot.selection.artifact = crate::generated::RFDETR_PRESET_CATALOG
                    .first()
                    .unwrap()
                    .canonicalweightfilename
                    .to_string();
                snapshot.terminal.outcome = crate::generated::ModelSelectionOutcome::Accepted;
                snapshot.terminal.detail.clear();
                snapshot
            })),
        );
        assert!(
            app.settings.draft().is_some_and(|draft| {
                app.model.model_selection_matches(draft, FeatureId::Train)
            })
        );
    }

    fn authoritative_explore_columns(app: &App) -> u32 {
        app.model
            .settings_snapshot
            .as_ref()
            .unwrap()
            .settingsstate
            .workflows
            .explore
            .gridwidth
            .clamp(1, 99) as u32
    }

    fn gallery_capacity(width: u32, height: u32) -> crate::generated::VisualExtent {
        crate::generated::VisualExtent { width, height }
    }

    fn install_ready_explore(app: &mut App, matching_count: u32) {
        install_default_bootstrap(app);
        let columns = authoritative_explore_columns(app);
        assert!(app.workspace.explore_measure_gallery(
            601.0,
            420.0,
            gallery_capacity(601, 420),
            columns
        ));
        let viewport = app
            .workspace
            .explore_measured_viewport(columns, 0, matching_count)
            .unwrap();
        let visible_count = matching_count.min(viewport.rowcount.saturating_mul(viewport.columns));
        let snapshot = app.model.explore.snapshot.as_mut().unwrap();
        snapshot.ready = true;
        snapshot.busy = false;
        snapshot.viewport = viewport;
        snapshot.order.matchingcount = matching_count;
        snapshot.order.visibleindices = (0..visible_count).collect();
    }

    fn reduce_explore_changed(app: &mut App, snapshot: crate::generated::ExploreSnapshot) {
        app.reduce_event(SystemEvent {
            state_revision: snapshot.revision,
            delivery: crate::generated::EventDelivery::LatestState,
            event: crate::generated::ApplicationEvent::ExploreExploreChanged(
                crate::generated::ExploreChanged { snapshot },
            ),
        });
    }

    #[test]
    fn application_boot_keeps_the_root_shell_small_and_disconnected() {
        let (app, task) = boot();
        drop(task);
        assert!(app.connection.is_none());
        assert_eq!(app.model.pending_count(), 0);
    }

    #[test]
    fn root_transport_connection_and_peer_loss_reset_the_application_boundary() {
        let (mut app, task) = boot();
        drop(task);
        let (sender, _receiver) = Connection::test_channel();

        drop(app.on_transport(TransportEvent::Connected(sender)));

        assert!(app.connection.is_some());
        assert_eq!(
            app.model.connection,
            crate::view_model::ConnectionState::AwaitingBootstrap
        );

        drop(app.on_transport(TransportEvent::Disconnected("peer lost".into())));

        assert!(app.connection.is_none());
        assert_eq!(
            app.model.connection,
            crate::view_model::ConnectionState::Reconnecting
        );
        assert_eq!(
            app.model.error.as_ref().map(|error| error.detail.as_str()),
            Some("peer lost")
        );
        assert_eq!(app.workspace.active(), FeatureId::Train);
    }

    #[test]
    fn ready_surface_presented_intent_and_protocol_terminal_paths_retire_the_peer() {
        let closed_connection = || {
            let (sender, receiver) = Connection::test_channel();
            drop(receiver);
            sender
        };

        let (mut ready, task) = boot();
        drop(task);
        drop(ready.on_transport(TransportEvent::Connected(closed_connection())));
        assert!(ready.connection.is_none());
        assert_eq!(
            ready.model.connection,
            crate::view_model::ConnectionState::Reconnecting
        );

        let (mut surface, task) = boot();
        drop(task);
        install_default_bootstrap(&mut surface);
        surface.connection = Some(closed_connection());
        surface.model.window_width = 640;
        surface.model.window_height = 480;
        surface.send_surface_observation();
        assert!(surface.connection.is_none());
        assert!(surface.settings.draft().is_none());

        let (mut presented, task) = boot();
        drop(task);
        install_default_bootstrap(&mut presented);
        presented.connection = Some(closed_connection());
        presented.presentation.set_test_surface(Surface {
            high: 1,
            low: 2,
            generation: 3,
            width: 640,
            height: 480,
            timeline_ready: 0,
            frame: None,
            integration: false,
            crop: None,
            viewer_identity: None,
            fit_revision: 0,
        });
        let retained = FrameReady {
            high: 1,
            low: 2,
            layer: 0,
            slot: 0,
            content_session: 1,
            content_sequence: 1,
            presentation_revision: 4,
            content_width: 640,
            content_height: 480,
        };
        presented.present_native_frame(retained);
        assert!(presented.connection.is_none());
        assert_eq!(
            presented
                .presentation
                .surface()
                .and_then(|surface| surface.frame),
            Some(retained)
        );

        let (mut intent, task) = boot();
        drop(task);
        install_default_bootstrap(&mut intent);
        intent.connection = Some(closed_connection());
        assert!(
            !intent.submit_intent(ApplicationIntentEndpoint::DatasetStop, |correlation| {
                crate::generated::encode_dataset_Stop(correlation)
            })
        );
        assert!(intent.connection.is_none());
        assert_eq!(intent.model.pending_count(), 0);

        let (mut protocol, task) = boot();
        drop(task);
        install_default_bootstrap(&mut protocol);
        protocol.workspace.select(FeatureId::Export);
        drop(protocol.on_transport(TransportEvent::ProtocolError("invalid record".into())));
        assert!(protocol.connection.is_none());
        assert_eq!(protocol.workspace.active(), FeatureId::Train);
        assert_eq!(
            protocol.model.connection,
            crate::view_model::ConnectionState::Reconnecting
        );
        assert_eq!(
            protocol.model.error.as_ref().unwrap().kind,
            crate::view_model::UiErrorKind::Protocol
        );
        drop(protocol.on_transport(TransportEvent::Disconnected("worker stopped".into())));
        assert_eq!(
            protocol.model.error.as_ref().unwrap().kind,
            crate::view_model::UiErrorKind::Protocol
        );
    }

    #[test]
    fn overlay_submission_failures_restore_authoritative_preferences() {
        for failure in ["registration", "disconnected", "capacity"] {
            let (mut app, task) = boot();
            drop(task);
            install_ready_explore(&mut app, 20);
            let saved = app.model.explore.snapshot.as_ref().unwrap().overlay.clone();
            let mut requested = saved.clone();
            requested.showlabels = !saved.showlabels;
            app.workspace
                .explore_record_submission(crate::generated::ExploreFilterUpdate {
                    filter: app.model.explore.snapshot.as_ref().unwrap().filter.clone(),
                    overlay: requested.clone(),
                });
            app.model.explore.desired_overlay = Some(requested.clone());
            let (sender, receiver) = Connection::test_channel();
            let mut connection = sender;
            if failure == "capacity" {
                for _ in 0..64 { connection.send_renderer_observation(RendererObservation::Ready).unwrap(); }
            }
            if failure != "disconnected" {
                app.connection = Some(connection);
            }
            let submitted = if failure == "registration" {
                app.submit_intent(
                    ApplicationIntentEndpoint::ExploreUpdateOverlay,
                    |correlation| crate::generated::encode_dataset_Stop(correlation),
                )
            } else {
                app.submit_intent(
                    ApplicationIntentEndpoint::ExploreUpdateOverlay,
                    |correlation| {
                        crate::generated::encode_explore_UpdateOverlay(correlation, requested)
                    },
                )
            };
            assert!(!submitted, "{failure}");
            assert!(app.model.explore.desired_overlay.is_none(), "{failure}");
            assert!(app.model.explore.desired_filter.is_none(), "{failure}");
            assert_eq!(app.model.pending_count(), 0, "{failure}");
            let next = app
                .workspace
                .update(
                    &mut app.model,
                    &mut app.settings,
                    crate::view::router::Message::Explore(crate::view::explore::Message::Gallery(
                        crate::view::explore::gallery::Message::Overlay(
                            crate::view::explore::overlay::Message::MasksToggled(!saved.showmasks),
                        ),
                    )),
                )
                .unwrap()
                .unwrap();
            let crate::view::router::Outcome::Explore(
                crate::view::explore::Outcome::OverlayUpdated(next),
            ) = next
            else {
                panic!("expected local overlay outcome");
            };
            assert_eq!(next.showlabels, saved.showlabels, "{failure}");
            assert_eq!(next.showmasks, !saved.showmasks, "{failure}");
            drop(receiver);
        }
    }

    #[test]
    fn intent_capacity_is_recoverable_and_abandons_only_the_unsent_record() {
        let (mut app, task) = boot();
        drop(task);
        install_default_bootstrap(&mut app);
        let (sender, receiver) = Connection::test_channel();
        let mut connection = sender;
        assert_eq!(
            connection
                .send_renderer_observation(RendererObservation::Ready)
                .unwrap(),
            crate::transport_connection::SendDisposition::Queued
        );
        for _ in 1..64 { connection.send_renderer_observation(RendererObservation::Ready).unwrap(); }
        app.connection = Some(connection);
        assert!(
            !app.submit_intent(ApplicationIntentEndpoint::DatasetStop, |correlation| {
                crate::generated::encode_dataset_Stop(correlation)
            })
        );
        assert!(app.connection.is_some());
        assert_eq!(app.model.pending_count(), 0);
        assert_eq!(
            app.model.connection,
            crate::view_model::ConnectionState::Connected
        );
        drop(receiver);
    }

    #[test]
    fn accepted_annotation_edit_rebase_starts_the_next_pointer_interaction_fresh() {
        let (mut app, task) = boot();
        drop(task);
        install_ready_annotation(&mut app);
        let (sender, receiver) = Connection::test_channel();
        app.connection = Some(sender);

        let first = local_annotation_press(&mut app);
        drop(app.on_workspace(crate::view::router::Message::Annotation(
            crate::view::annotation::undo_requested(),
        )));
        assert_eq!(
            app.model.pending_intent(1),
            Some(ApplicationIntentEndpoint::AnnotationEdit)
        );
        app.reduce_reply(IntentReply {
            correlation: 1,
            result: Err(crate::protocol::ApplicationError {
                category: crate::generated::ApplicationErrorCategory::Failed,
                detail: "edit rejected without replacing the document".into(),
            }),
        });
        assert_eq!(app.model.pending_count(), 0);

        let next = local_annotation_press(&mut app);
        assert_eq!(first.phase, crate::generated::AnnotationPointerPhase::Begin);
        assert_eq!(next.phase, crate::generated::AnnotationPointerPhase::Begin);
        assert_eq!(next.sequence, 1);
        assert_ne!(next.interactionid, first.interactionid);
        drop(receiver);
    }

    #[test]
    fn capacity_failed_annotation_edit_keeps_the_active_local_pointer() {
        let (mut app, task) = boot();
        drop(task);
        install_ready_annotation(&mut app);
        let (sender, receiver) = Connection::test_channel();
        let mut connection = sender;
        assert_eq!(
            connection
                .send_renderer_observation(RendererObservation::Ready)
                .unwrap(),
            crate::transport_connection::SendDisposition::Queued
        );
        for _ in 1..64 { connection.send_renderer_observation(RendererObservation::Ready).unwrap(); }
        app.connection = Some(connection);

        let first = local_annotation_press(&mut app);
        drop(app.on_workspace(crate::view::router::Message::Annotation(
            crate::view::annotation::undo_requested(),
        )));
        assert_eq!(app.model.pending_count(), 0);
        assert_eq!(
            app.model.error.as_ref().unwrap().kind,
            crate::view_model::UiErrorKind::Busy
        );

        let next = local_annotation_press(&mut app);
        assert_eq!(first.phase, crate::generated::AnnotationPointerPhase::Begin);
        assert_eq!(next.phase, crate::generated::AnnotationPointerPhase::Update);
        assert_eq!(next.interactionid, first.interactionid);
        assert_eq!(next.sequence, first.sequence + 1);
        drop(receiver);
    }

    #[test]
    fn annotation_direct_send_failure_resets_every_persistent_owner() {
        let (mut app, task) = boot();
        drop(task);
        stage_annotation_retirement(&mut app);
        let annotation = app.model.annotation.snapshot.as_mut().unwrap();
        annotation.ready = true;
        annotation.busy = false;
        annotation.frame =
            crate::view_model::test_support::visual_frame(PresentationSourceKind::Annotation, 1);
        let (sender, receiver) = Connection::test_channel();
        drop(receiver);
        app.connection = Some(sender);
        app.submit_annotation_pointer(annotation_pointer(
            crate::generated::AnnotationPointerPhase::Begin,
            1,
        ));
        assert_persistent_owners_retired(&app);
        let (mut local, task) = boot();
        drop(task);
        stage_annotation_retirement(&mut local);
        let (connection, _capture) = Connection::test_channel();
        local.connection = Some(connection);
        drop(local.on_workspace(crate::view::router::Message::Annotation(
            crate::view::annotation::Message::Workspace(crate::view::workspace::Message::InputFailed(
                crate::transport_connection::OutboundSendError::Closed,
            )),
        )));
        assert_persistent_owners_retired(&local);
    }

    #[test]
    fn annotation_pointer_without_a_physical_peer_uses_the_same_retirement_handoff() {
        let (mut app, task) = boot();
        drop(task);
        stage_annotation_retirement(&mut app);
        assert!(app.connection.is_none());
        app.submit_annotation_pointer(annotation_pointer(
            crate::generated::AnnotationPointerPhase::Begin,
            1,
        ));
        assert_persistent_owners_retired(&app);
        assert_eq!(
            app.model.error.as_ref().unwrap().kind,
            crate::view_model::UiErrorKind::Transport
        );
    }

    #[test]
    fn ordered_annotation_capacity_retains_samples_without_retiring_the_peer() {
        let (mut app, task) = boot();
        drop(task);
        stage_annotation_retirement(&mut app);
        let (sender, receiver) = Connection::test_channel();
        app.connection = Some(sender);
        app.submit_annotation_pointer(annotation_pointer(
            crate::generated::AnnotationPointerPhase::Begin,
            1,
        ));
        assert!(app.connection.is_some());
        app.submit_annotation_pointer(annotation_pointer(
            crate::generated::AnnotationPointerPhase::Update,
            2,
        ));
        for sequence in 3..=130 { app.submit_annotation_pointer(annotation_pointer(if sequence == 130 { crate::generated::AnnotationPointerPhase::End } else { crate::generated::AnnotationPointerPhase::Update }, sequence)); }
        assert!(app.connection.is_some());
        let mut receiver = receiver;
        let mut batches = 0;
        while let Ok(record) = receiver.try_recv() { if let crate::transport_connection::CapturedRecord::Other(envelope) = record { assert_eq!(envelope.kind, "Interaction"); batches += 1; } }
        assert_eq!(batches, 2, "all remaining samples await consumption credits");
        drop(receiver);
    }

    #[test]
    fn explore_direct_send_and_writable_failures_reset_component_admission() {
        let (mut send, task) = boot();
        drop(task);
        install_default_bootstrap(&mut send);
        send.workspace.select(FeatureId::Explore);
        send.model.window_width = 640;
        send.model.window_height = 480;
        let explore = send.model.explore.snapshot.as_mut().unwrap();
        explore.ready = true;
        explore.busy = false;
        let request = ExploreViewportUpdate {
            viewport: crate::generated::default_request_exploreUpdateViewportviewport().unwrap(),
            focusedcompiledindex: Some(3),
        };
        send.workspace
            .explore_request_viewport(send.model.explore.snapshot.as_ref(), request);
        let (sender, receiver) = Connection::test_channel();
        drop(receiver);
        send.connection = Some(sender);
        drop(send.dispatch_explore_viewport());
        assert!(send.connection.is_none());
        assert!(send.workspace.explore_dispatchable_viewport().is_none());
        assert_eq!(send.workspace.active(), FeatureId::Train);

        let (mut writable, task) = boot();
        drop(task);
        install_default_bootstrap(&mut writable);
        writable.workspace.select(FeatureId::Explore);
        writable
            .settings
            .state_mut()
            .edit(EditCadence::Debounced, |draft| {
                let darkmode = !draft.ui.darkmode;
                crate::generated::edit_uidarkmode(draft, darkmode)
            })
            .unwrap();
        drop(writable.on_explore_writable(
            0,
            Err(crate::transport_connection::OutboundSendError::Closed),
        ));
        assert!(writable.settings.draft().is_none());
        assert_eq!(writable.workspace.active(), FeatureId::Train);
        assert_eq!(
            writable.model.connection,
            crate::view_model::ConnectionState::Reconnecting
        );
    }

    #[test]
    fn stale_explore_writable_completion_cannot_touch_a_replacement_peer() {
        let (mut app, task) = boot();
        drop(task);
        let (first_sender, first_receiver) = Connection::test_channel();
        drop(app.on_transport(TransportEvent::Connected(first_sender)));
        let first_generation = app.peer_generation;
        drop(first_receiver);

        let (second_sender, second_receiver) = Connection::test_channel();
        drop(app.on_transport(TransportEvent::Connected(second_sender)));
        assert_ne!(app.peer_generation, first_generation);
        assert!(app.workspace.explore_arm_writable_wait());
        drop(app.on_explore_writable(
            first_generation,
            Err(crate::transport_connection::OutboundSendError::Closed),
        ));
        assert!(app.connection.is_some());
        assert_eq!(
            app.model.connection,
            crate::view_model::ConnectionState::AwaitingBootstrap
        );
        assert!(!app.workspace.explore_arm_writable_wait());
        drop(second_receiver);
    }

    #[test]
    fn navigation_remains_local_when_optional_persistence_is_unavailable() {
        for condition in 0..3 {
            let (mut app, task) = boot();
            drop(task);
            install_default_bootstrap(&mut app);
            let pending = match condition {
                0 => {
                    app.settings = crate::view::settings::Component::default();
                    app.model.settings_snapshot = None;
                    None
                }
                1 => Some(
                    app.model
                        .begin_intent(ApplicationIntentEndpoint::SettingsUpdate)
                        .unwrap(),
                ),
                _ => Some(
                    app.model
                        .begin_intent(ApplicationIntentEndpoint::SettingsReset)
                        .unwrap(),
                ),
            };
            let source =
                crate::view_model::test_support::visual_frame(PresentationSourceKind::Live, 1);
            app.model.live_snapshot.as_mut().unwrap().frame = source.clone();
            let (sender, mut receiver) = Connection::test_channel();
            app.connection = Some(sender);
            drop(update(
                &mut app,
                Message::Workspace(crate::view::router::Message::Navigation(
                    crate::view::navigation::Message::PageSelected(FeatureId::Live),
                )),
            ));
            assert_eq!(app.workspace.active(), FeatureId::Live);
            assert!(app.model.error.is_none());
            let crate::transport_connection::CapturedRecord::Intent(intent) =
                receiver.try_recv().unwrap()
            else {
                panic!("expected Live foreground selection");
            };
            assert_eq!(
                intent,
                crate::generated::encode_presentation_Select(intent.correlation, source.source)
                    .record
            );
            assert!(receiver.try_recv().is_err());
            assert_eq!(
                app.model.pending_count(),
                usize::from(pending.is_some()) + 1
            );
            if let Some(pending) = pending {
                app.model.abandon_intent(pending);
            }
        }
    }

    #[test]
    fn dialog_open_rejects_generated_fact_outside_active_feature() {
        let (mut app, task) = boot();
        drop(task);
        app.workspace.select(FeatureId::Train);
        let dialog = crate::generated::FILE_DIALOGS
            .iter()
            .find(|fact| !fact.workflows.contains(&crate::generated::FeatureId::Train))
            .expect("generated dialog outside Train");

        app.open_dialog(dialog.stable_field_id);

        assert_eq!(
            app.model.error.as_ref().unwrap().kind,
            crate::view_model::UiErrorKind::InvalidIntent
        );
        assert_eq!(app.model.pending_count(), 0);
    }

    #[test]
    fn root_rejects_compute_without_a_current_selection_and_while_active() {
        let (mut app, task) = boot();
        drop(task);
        install_default_bootstrap(&mut app);

        drop(app.on_workspace(crate::view::router::Message::Validate(
            crate::view::validate::Message::StartRequested,
        )));
        assert_eq!(
            app.model.error.as_ref().unwrap().kind,
            crate::view_model::UiErrorKind::InvalidIntent
        );
        assert_eq!(app.model.pending_count(), 0);

        install_current_model_selection(&mut app);
        let correlation = app
            .model
            .begin_intent(ApplicationIntentEndpoint::ValidationStart)
            .unwrap();
        let mut running = app.model.workflow.validation.clone().unwrap();
        running.generationfrontier += 1;
        running.active = true;
        running.terminal.generation = running.generationfrontier;
        running.terminal.outcome = crate::generated::ComputeOperationOutcome::Running;
        app.model.reduce_reply(
            correlation,
            Ok(crate::generated::ApplicationReply::ValidationStart(running)),
        );
        drop(app.on_workspace(crate::view::router::Message::Validate(
            crate::view::validate::Message::StartRequested,
        )));
        assert_eq!(
            app.model.error.as_ref().unwrap().kind,
            crate::view_model::UiErrorKind::Busy
        );
        assert_eq!(app.model.pending_count(), 0);
    }

    #[test]
    fn root_rejects_compute_while_configuration_or_model_selection_is_pending() {
        let (mut app, task) = boot();
        drop(task);
        install_default_bootstrap(&mut app);
        install_current_model_selection(&mut app);

        let settings = app
            .model
            .begin_intent(ApplicationIntentEndpoint::SettingsUpdate)
            .unwrap();
        drop(app.on_workspace(crate::view::router::Message::Validate(
            crate::view::validate::Message::StartRequested,
        )));
        assert_eq!(
            app.model.error.as_ref().unwrap().kind,
            crate::view_model::UiErrorKind::Busy
        );
        app.model.abandon_intent(settings);

        let receipt = app
            .model
            .workflow
            .model_selection_receipt(crate::generated::FeatureId::Export)
            .unwrap();
        let selection = app.model.begin_model_select_intent(receipt).unwrap();
        drop(app.on_workspace(crate::view::router::Message::Export(
            crate::view::export::Message::StartRequested,
        )));
        assert_eq!(
            app.model.error.as_ref().unwrap().kind,
            crate::view_model::UiErrorKind::Busy
        );
        app.model.abandon_intent(selection);
    }

    #[test]
    fn debounced_train_configuration_is_local_and_blocks_settings_consumers() {
        let (mut app, task) = boot();
        drop(task);
        install_default_bootstrap(&mut app);
        install_current_model_selection(&mut app);
        let installed = &app
            .model
            .settings_snapshot
            .as_ref()
            .unwrap()
            .settingsstate
            .workflows
            .train
            .request
            .presetname;
        let index = crate::generated::RFDETR_PRESET_CATALOG
            .iter()
            .position(|preset| preset.presetname.as_ref() != installed)
            .expect("alternate generated preset");

        drop(app.on_workspace(crate::view::router::Message::Train(
            crate::view::train::Message::Model(
                crate::view::workflow::model_card::Message::PresetSelected(index),
            ),
        )));

        assert!(
            !app.settings.draft().is_some_and(|draft| {
                app.model.model_selection_matches(draft, FeatureId::Train)
            })
        );
        assert_eq!(app.model.pending_count(), 0);
        assert!(app.settings_unsettled());
        assert!(
            !app.settings.draft().is_some_and(|draft| {
                app.model.compute_start_available(draft, FeatureId::Train)
            })
        );
        assert!(app.model.error.is_none());
    }

    #[test]
    fn annotation_save_constructor_rechecks_settings_settlement() {
        let (mut app, task) = boot();
        drop(task);
        install_default_bootstrap(&mut app);
        let annotation = app.model.annotation.snapshot.as_mut().unwrap();
        annotation.ready = true;
        annotation.frame =
            crate::view_model::test_support::visual_frame(PresentationSourceKind::Annotation, 1);
        app.settings
            .state_mut()
            .edit(EditCadence::Debounced, |draft| {
                crate::generated::edit_workflowsannotateoutputdir(draft, "./new-output".into())
            })
            .unwrap();

        drop(app.on_workspace(crate::view::router::Message::Annotation(
            crate::view::annotation::Message::SaveRequested,
        )));

        assert_eq!(app.model.pending_count(), 0);
        assert_eq!(
            app.model.error.as_ref().unwrap().kind,
            crate::view_model::UiErrorKind::Busy
        );
    }

    #[test]
    fn every_direct_intent_producer_registers_the_encoded_endpoint() {
        let ready = || {
            let (mut app, task) = boot();
            drop(task);
            install_default_bootstrap(&mut app);
            let (sender, receiver) = Connection::test_channel();
            app.connection = Some(sender);
            (app, receiver)
        };

        let (mut generic, _generic_receiver) = ready();
        assert!(
            generic.submit_intent(ApplicationIntentEndpoint::DatasetStop, |correlation| {
                crate::generated::encode_dataset_Stop(correlation)
            })
        );
        assert_eq!(
            generic.model.pending_endpoint(1),
            Some(crate::generated::ENDPOINT_Dataset_Stop)
        );
        assert_eq!(generic.model.pending_count(), 1);

        let (mut optional_settings, _settings_receiver) = ready();
        optional_settings
            .settings
            .state_mut()
            .edit(EditCadence::Immediate, |draft| {
                crate::generated::edit_currentview(draft, crate::generated::FeatureId::Train)
            })
            .unwrap();
        optional_settings.flush_settings_edits();
        assert_eq!(
            optional_settings.model.pending_endpoint(1),
            Some(crate::generated::ENDPOINT_Settings_Update)
        );
        assert_eq!(optional_settings.model.pending_count(), 1);
        assert!(optional_settings.model.error.is_none());

        let (mut explore_filter, _explore_receiver) = ready();
        let explore = explore_filter.model.explore.snapshot.as_mut().unwrap();
        explore.ready = true;
        explore.busy = false;
        explore.frame =
            crate::view_model::test_support::visual_frame(PresentationSourceKind::Explore, 1);
        drop(
            explore_filter.on_workspace(crate::view::router::Message::Explore(
                crate::view::explore::Message::Dataset(
                    crate::view::explore::dataset::Message::MinimumInstancesChanged(3),
                ),
            )),
        );
        assert_eq!(
            explore_filter.model.pending_endpoint(1),
            Some(crate::generated::ENDPOINT_Explore_UpdateFilter)
        );
        assert_eq!(explore_filter.model.pending_count(), 1);
        assert_eq!(explore_filter.settings.state().queued_len(), 0);
        assert!(!explore_filter.settings.state().update_in_flight());

        let (mut presentation, _presentation_receiver) = ready();
        presentation.select_presentation(crate::view_model::invalid_visual_frame());
        assert_eq!(
            presentation.model.pending_endpoint(1),
            Some(crate::generated::ENDPOINT_Presentation_Select)
        );
        assert_eq!(presentation.model.pending_count(), 1);
        assert!(presentation.model.error.is_none());

        let (mut interaction, _interaction_receiver) = ready();
        let _ = interaction.request_explore_viewport(ExploreViewportUpdate {
            viewport: crate::generated::default_request_exploreUpdateViewportviewport().unwrap(),
            focusedcompiledindex: None,
        });
        assert_eq!(interaction.model.pending_count(), 0);
    }

    #[test]
    fn no_dataset_augmentation_toggle_coalesces_without_speculative_state() {
        let (mut app, task) = boot();
        drop(task);
        install_default_bootstrap(&mut app);
        let (sender, _receiver) = Connection::test_channel();
        app.connection = Some(sender);
        let before = app.model.explore.snapshot.clone().unwrap();
        assert!(!before.ready);
        assert!(app.model.explore_augmentation_update_available());

        drop(
            app.on_explore(crate::view::explore::Outcome::AugmentationUpdated(
                crate::generated::ExploreAugmentationUpdate { enabled: true },
            )),
        );
        assert_eq!(
            app.model.pending_endpoint(1),
            Some(crate::generated::ENDPOINT_Explore_UpdateAugmentation)
        );
        let pending = app.model.explore.snapshot.as_ref().unwrap();
        assert_eq!(pending.frame, before.frame);
        assert_eq!(pending.augmentation, before.augmentation);

        drop(
            app.on_explore(crate::view::explore::Outcome::AugmentationUpdated(
                crate::generated::ExploreAugmentationUpdate { enabled: false },
            )),
        );
        assert_eq!(app.model.pending_count(), 1);
        assert!(app.model.error.is_none());
        assert_eq!(
            app.model.explore.desired_augmentation,
            Some(crate::generated::ExploreAugmentationUpdate { enabled: false })
        );
        let still_pending = app.model.explore.snapshot.as_ref().unwrap();
        assert_eq!(still_pending.frame, before.frame);
        assert_eq!(still_pending.augmentation, before.augmentation);
    }

    #[test]
    fn explore_geometry_reconciles_successful_and_rejected_column_saves() {
        let (mut success, task) = boot();
        drop(task);
        install_ready_explore(&mut success, 100);
        success
            .settings
            .state_mut()
            .edit(EditCadence::Debounced, |draft| {
                crate::generated::edit_workflowsexploregridwidth(draft, 5)
            })
            .unwrap();
        assert!(success.settings.state_mut().take_request().is_some());
        let draft_request = ExploreViewportUpdate {
            viewport: success
                .workspace
                .explore_measured_viewport(5, 7, 100)
                .unwrap(),
            focusedcompiledindex: Some(37),
        };
        drop(success.request_explore_viewport(draft_request));
        let mut authoritative = success.model.settings_snapshot.clone().unwrap();
        authoritative.settingsstate.workflows.explore.gridwidth = 5;
        success.model.settings_snapshot = Some(authoritative.clone());
        success.settings.settle_success(&authoritative);
        success.reconcile_explore_viewport();
        let settled = success.workspace.explore_dispatchable_viewport().unwrap();
        assert_eq!(settled.viewport.columns, 5);
        assert_eq!(settled.viewport.firstrow, 7);
        assert_eq!(settled.focusedcompiledindex, Some(37));

        let (mut rejected, task) = boot();
        drop(task);
        install_ready_explore(&mut rejected, 100);
        let authoritative_columns = authoritative_explore_columns(&rejected);
        rejected
            .settings
            .state_mut()
            .edit(EditCadence::Debounced, |draft| {
                crate::generated::edit_workflowsexploregridwidth(draft, 5)
            })
            .unwrap();
        assert!(rejected.settings.state_mut().take_request().is_some());
        let draft_request = ExploreViewportUpdate {
            viewport: rejected
                .workspace
                .explore_measured_viewport(5, 9, 100)
                .unwrap(),
            focusedcompiledindex: Some(49),
        };
        drop(rejected.request_explore_viewport(draft_request));
        let authoritative = rejected.model.settings_snapshot.clone().unwrap();
        rejected.settings.settle_failure(Some(&authoritative));
        rejected.reconcile_explore_viewport();
        let rolled_back = rejected.workspace.explore_dispatchable_viewport().unwrap();
        assert_eq!(rolled_back.viewport.columns, authoritative_columns);
        assert_eq!(rolled_back.viewport.firstrow, 9);
        assert_eq!(rolled_back.focusedcompiledindex, Some(49));
    }

    #[test]
    fn explore_geometry_survives_unavailable_resize_and_bootstrap_reconnect() {
        let (mut resize, task) = boot();
        drop(task);
        install_ready_explore(&mut resize, 100);
        resize.model.explore.snapshot.as_mut().unwrap().busy = true;
        resize
            .model
            .explore
            .snapshot
            .as_mut()
            .unwrap()
            .cancellationrequested = true;
        assert!(resize.workspace.explore_measure_gallery(
            777.0,
            333.0,
            gallery_capacity(777, 333),
            4
        ));
        let resized = resize
            .workspace
            .explore_measured_layout_request(resize.model.explore.snapshot.as_ref(), 4, 100)
            .unwrap();
        drop(resize.request_explore_viewport(resized));
        let retained = resize.workspace.explore_dispatchable_viewport().unwrap();
        assert_eq!(retained.viewport.extent.width, 664);
        assert_eq!(retained.viewport.extent.height, 332);

        let (mut reconnect, task) = boot();
        drop(task);
        assert!(reconnect.workspace.explore_measure_gallery(
            733.0,
            377.0,
            gallery_capacity(733, 377),
            3
        ));
        let mut snapshots: Vec<_> = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .map(|snapshot| snapshot.value)
            .collect();
        for snapshot in &mut snapshots {
            match snapshot {
                crate::generated::ApplicationSnapshot::Settings(settings) => {
                    settings.settingsstate.workflows.explore.gridwidth = 3;
                }
                crate::generated::ApplicationSnapshot::Explore(explore) => {
                    explore.ready = true;
                    explore.order.matchingcount = 100;
                    explore.viewport.firstrow = 11;
                    explore.focusedimage = Some(34);
                }
                _ => {}
            }
        }
        reconnect.install_bootstrap(Bootstrap {
            input_epoch: 1,
            schema_fingerprint: crate::generated::SCHEMA_FINGERPRINT,
            snapshots,
        });
        let rebased = reconnect.workspace.explore_dispatchable_viewport().unwrap();
        assert_eq!(rebased.viewport.columns, 3);
        assert_eq!(rebased.viewport.extent.width, 564);
        assert_eq!(rebased.viewport.extent.height, 376);
        assert_eq!(rebased.viewport.firstrow, 11);
        assert_eq!(rebased.focusedcompiledindex, Some(34));
    }

    #[test]
    fn explore_changed_rebases_initial_open_from_zero_candidates() {
        let (mut app, task) = boot();
        drop(task);
        install_default_bootstrap(&mut app);
        let columns = app
            .model
            .settings_snapshot
            .as_ref()
            .unwrap()
            .settingsstate
            .workflows
            .explore
            .gridwidth
            .clamp(1, 99) as u32;
        assert!(app.workspace.explore_measure_gallery(
            601.0,
            420.0,
            gallery_capacity(601, 420),
            columns
        ));
        let open_viewport = app
            .workspace
            .explore_measured_viewport(columns, 0, 0)
            .unwrap();
        assert_eq!(open_viewport.rowcount, 1);

        let mut opened = app.model.explore.snapshot.clone().unwrap();
        opened.revision += 1;
        opened.ready = true;
        opened.busy = false;
        opened.viewport = open_viewport;
        opened.order.matchingcount = 100;
        opened.order.visibleindices = (0..columns).collect();
        reduce_explore_changed(&mut app, opened);

        let expected = ExploreViewportUpdate {
            viewport: app
                .workspace
                .explore_measured_viewport(columns, 0, 100)
                .unwrap(),
            focusedcompiledindex: None,
        };
        assert!(expected.viewport.rowcount > 1);
        assert_eq!(
            app.workspace.explore_dispatchable_viewport(),
            Some(expected)
        );
    }

    #[test]
    fn explore_open_waits_for_gallery_measurement_then_submits_once() {
        let (mut app, task) = boot();
        drop(task);
        install_default_bootstrap(&mut app);
        app.model.window_width = 640;
        app.model.window_height = 480;
        let settings = app.model.settings_snapshot.as_mut().unwrap();
        settings.exploresource.compiledsource = "./compiled/train.bin".into();
        settings.exploresource.available = true;
        app.settings.install(settings);
        let (sender, _receiver) = Connection::test_channel();
        app.connection = Some(sender);
        assert!(app.model.explore_open_available());

        drop(app.on_explore(crate::view::explore::Outcome::OpenRequested));

        assert!(app.model.explore.desired_open);
        assert_eq!(app.model.pending_count(), 0);
        assert!(app.model.error.is_none());

        let columns = authoritative_explore_columns(&app);
        assert!(app.workspace.explore_measure_gallery(
            601.0,
            420.0,
            gallery_capacity(601, 420),
            columns,
        ));
        let measured = ExploreViewportUpdate {
            viewport: app
                .workspace
                .explore_measured_viewport(columns, 0, 0)
                .unwrap(),
            focusedcompiledindex: None,
        };
        drop(app.request_explore_viewport(measured.clone()));

        assert!(!app.model.explore.desired_open);
        assert_eq!(
            app.model.pending_endpoint(1),
            Some(crate::generated::ENDPOINT_Explore_Open)
        );
        assert_eq!(app.model.pending_count(), 1);
        assert!(app.model.error.is_none());

        drop(app.request_explore_viewport(measured));
        assert_eq!(app.model.pending_count(), 1);
    }

    #[test]
    fn explore_changed_rebases_filter_count_and_exact_commit_coalesces() {
        let (mut app, task) = boot();
        drop(task);
        install_ready_explore(&mut app, 100);
        let columns = authoritative_explore_columns(&app);
        let sent = ExploreViewportUpdate {
            viewport: app
                .workspace
                .explore_measured_viewport(columns, 1, 100)
                .unwrap(),
            focusedcompiledindex: Some(5),
        };
        app.workspace
            .explore_request_viewport(app.model.explore.snapshot.as_ref(), sent.clone());
        app.workspace.explore_viewport_queued(sent.clone());
        let filtered_count = 14_u32;
        let last_filtered_row = filtered_count.div_ceil(columns).saturating_sub(1);
        let newer = ExploreViewportUpdate {
            viewport: app
                .workspace
                .explore_measured_viewport(columns, last_filtered_row, 100)
                .unwrap(),
            focusedcompiledindex: Some(13),
        };
        app.workspace
            .explore_request_viewport(app.model.explore.snapshot.as_ref(), newer);

        let mut filtered = app.model.explore.snapshot.clone().unwrap();
        filtered.revision += 1;
        filtered.viewport = sent.viewport;
        filtered.focusedimage = sent.focusedcompiledindex;
        filtered.order.matchingcount = filtered_count;
        let visible_begin = filtered
            .viewport
            .firstrow
            .saturating_mul(filtered.viewport.columns);
        let visible_end = visible_begin
            .saturating_add(
                filtered
                    .viewport
                    .rowcount
                    .saturating_mul(filtered.viewport.columns),
            )
            .min(filtered_count);
        filtered.order.visibleindices = (visible_begin..visible_end).collect();
        reduce_explore_changed(&mut app, filtered);

        let rebased = app.workspace.explore_dispatchable_viewport().unwrap();
        assert_eq!(rebased.viewport.firstrow, last_filtered_row);
        assert_eq!(rebased.viewport.rowcount, 1);
        assert_eq!(rebased.focusedcompiledindex, Some(13));

        app.workspace.explore_viewport_queued(rebased.clone());
        let mut committed = app.model.explore.snapshot.clone().unwrap();
        committed.revision += 1;
        committed.viewport = rebased.viewport;
        committed.focusedimage = rebased.focusedcompiledindex;
        reduce_explore_changed(&mut app, committed);

        assert!(app.workspace.explore_dispatchable_viewport().is_none());
    }
}
