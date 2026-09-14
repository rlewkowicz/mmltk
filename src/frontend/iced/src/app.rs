use iced::{Subscription, Task};

use crate::fluent_theme::{Element, Theme};
use crate::generated::{
    AnnotationOpen, AnnotationSave, ExploreNavigate, ExploreNavigation, ExploreOpen, ExploreSelect,
    ExploreViewportUpdate, FeatureId, FileDialogOpen, LiveStart, SettingsResetRequest, Train,
    VisualExtent, VisualFrame,
};
use crate::message::Message;
use crate::presentation_surface::{FrameReady, Surface};
use crate::protocol::client_records::Intent;
use crate::protocol::{Bootstrap, IntentReply, SystemEvent};
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
    #[cfg(target_arch = "wasm32")]
    if config.surface_trace {
        std::panic::set_hook(Box::new(|panic| {
            if !crate::presentation_surface::surface_trace_enabled() {
                return;
            }
            if let Ok(message) =
                js_sys::JSON::stringify(&wasm_bindgen::JsValue::from_str(&panic.to_string()))
                && let Some(message) = message.as_string()
            {
                crate::presentation_surface::emit_surface_trace(&format!(
                    "{{\"event\":\"iced.panic\",\"message\":{message}}}"
                ));
            }
        }));
    }
    crate::integration_control::initialize_reporting(
        config.integration && config.surface_trace,
        config.integration_pixel_fixture,
    );
    let integration = config.integration.then(|| {
        let mut controller = crate::integration_control::Controller::new(
            true,
            config.integration_window_close,
            config.integration_dataset_source.clone(),
            config.integration_compiled_directory.clone(),
            config.integration_resolution.clone(),
            config.integration_viewer_scenario.clone(),
        );
        controller.configure_session(
            &config.integration_viewer_scenario,
            config.integration_square_source.clone(),
            config.integration_square_compiled.clone(),
        );
        controller
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
        iced::window::events().filter_map(|(_, event)| {
            // Retained workspace cadence stays in the widget/window loop.
            // A browser redraw does not coordinate with native snapshots.
            (!matches!(event, iced::window::Event::RedrawRequested(_)))
                .then_some(Message::Window(event))
        }),
        crate::presentation_surface::subscription()
            .map(presentation::Message::Surface)
            .map(Message::Presentation),
        app.integration
            .as_ref()
            .map_or_else(Subscription::none, |integration| {
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
                integration.observe_workspace_message(|| (&message, app.workspace.active()));
            }
            task = app.on_workspace(message);
        }
        Message::FileDialog(message) => app.on_file_dialog(message),
        Message::Settings(message) => task = app.on_settings(message),
        Message::Error(message) => task = app.on_error(message),
        Message::Diagnostics(message) => app.diagnostics.update(message),
        Message::Integration(message) => {
            if app
                .integration
                .as_ref()
                .is_none_or(|integration| !integration.accepts_message(&message))
            {
                return Task::none();
            }
            if let Some(integration) = app.integration.as_mut()
                && let Some(message) = integration.update(message)
            {
                task = app.on_workspace(crate::view::router::Message::Train(message));
            }
        }
    }
    app.reconcile_surface_frame();
    let surface = app.presentation.surface();
    let presentation_task = app
        .presentation
        .redraw(previous_surface)
        .map(Message::Presentation);
    let integration_task = if let Some(integration) = app.integration.as_mut() {
        let task = integration.advance(
            &app.model,
            app.settings.state(),
            app.settings.applied_scale(),
            &app.workspace,
            app.workspace.active(),
            surface,
        );
        if let Some(connection) = app.connection.as_mut() {
            integration.publish_control(connection);
        }
        task
    } else {
        Task::none()
    };
    Task::batch([task, presentation_task, integration_task])
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
            result: Ok(snapshot.into_application_transport_value()),
        });
    }

    #[test]
    fn obsolete_integration_message_cannot_advance_the_current_scenario() {
        let mut app = installed_app();
        app.model.window_width = 640;
        app.model.window_height = 480;
        app.integration = Some(crate::integration_control::Controller::new(
            true,
            false,
            "source".into(),
            "compiled".into(),
            "512".into(),
            "atlas".into(),
        ));
        let task = update(
            &mut app,
            Message::Integration(crate::integration_control::Message::Scoped {
                generation: 0,
                receipt: None,
                message: Box::new(crate::integration_control::Message::Advance),
            }),
        );
        assert_eq!(task.units(), 0);
        // A current continuation does have work: the obsolete one must not run it.
        let task = update(
            &mut app,
            Message::Integration(crate::integration_control::Message::Advance),
        );
        assert!(task.units() > 0);
    }

    fn fps_app() -> App {
        let mut app = installed_app();
        let mut settings = app.model.settings_snapshot.clone().unwrap();
        settings.revision += 1;
        settings.settingsstate.ui.showworkspaceperformance = true;
        settings_event(&mut app, settings);
        app.integration = Some(crate::integration_control::Controller::new(
            false,
            false,
            String::new(),
            String::new(),
            String::new(),
            String::new(),
        ));
        app
    }

    fn fps_outcome(
        message: &mut crate::integration_control::Message,
        outcome: crate::integration_control::FpsPixelOutcome,
    ) {
        use crate::integration_control::Message as Integration;
        let Integration::Scoped { message, .. } = message else {
            panic!("scenario completion");
        };
        let Integration::ProbeCompleted { message, .. } = message.as_mut() else {
            panic!("owned completion");
        };
        **message = Integration::WorkspaceFpsPixels(outcome);
    }

    fn apply_fps_restoration(app: &mut App, task: Task<Message>, baseline: bool) {
        use iced::futures::StreamExt;
        let mut actions =
            iced_runtime::task::into_stream(task).expect("completion must schedule restoration");
        let action = iced::futures::executor::block_on(actions.next()).unwrap();
        let iced_runtime::Action::Output(Message::Settings(message)) = action else {
            panic!("completion must directly return the ordinary settings operation");
        };
        assert!(
            matches!(message, crate::view::settings::Message::PerformanceChanged(value) if value == baseline)
        );
        // Process the returned application task, without another redraw or input.
        drop(update(app, Message::Settings(message)));
        assert_eq!(
            app.settings.draft().unwrap().ui.showworkspaceperformance,
            baseline
        );
        assert!(app.settings.state().has_local_edits());
    }

    #[test]
    fn fps_canvas_completion_drives_restoration_through_the_application_guard() {
        use crate::integration_control::{FpsPixelOutcome, tests::ProbeFixture};
        for baseline in [false, true] {
            for failed in [false, true] {
                let mut app = fps_app();
                let mut fixture = ProbeFixture::new("square");
                let mut completion = fixture
                    .prepare_app_fps(&app.model, app.settings.state(), baseline, true)
                    .unwrap();
                if failed {
                    fps_outcome(&mut completion, FpsPixelOutcome::Failed);
                }
                std::mem::swap(app.integration.as_mut().unwrap(), &mut fixture.controller);
                let task = update(&mut app, Message::Integration(completion));
                apply_fps_restoration(&mut app, task, baseline);
            }
        }
    }

    #[test]
    fn disabling_fps_diagnostics_wakes_restoration_with_or_without_a_capture() {
        use crate::integration_control::{self, FpsPixelOutcome, tests::ProbeFixture};
        for baseline in [false, true] {
            // Deliver either cancellation first to prove exactly one restoration.
            for callback_first in [None, Some(false), Some(true)] {
                let mut app = fps_app();
                let mut fixture = ProbeFixture::new("square");
                let mut callback = fixture.prepare_app_fps(
                    &app.model,
                    app.settings.state(),
                    baseline,
                    callback_first.is_some(),
                );
                let stale_pixels = callback.clone();
                if let Some(message) = &mut callback {
                    fps_outcome(message, FpsPixelOutcome::Cancelled);
                }
                std::mem::swap(app.integration.as_mut().unwrap(), &mut fixture.controller);
                integration_control::initialize_reporting(false, false);
                // This is the real disable notification, captured before diagnostic
                // receipt retirement. No synthetic Advance rescues the driver.
                let disabled = fixture
                    .receiver
                    .try_recv()
                    .expect("disable must wake the scenario");
                if let Some(stale_pixels) = stale_pixels {
                    assert_eq!(
                        update(&mut app, Message::Integration(stale_pixels)).units(),
                        0
                    );
                }
                let (first, second) = if callback_first == Some(true) {
                    (callback.take().unwrap(), Some(disabled))
                } else {
                    (disabled, callback)
                };
                let task = update(&mut app, Message::Integration(first));
                apply_fps_restoration(&mut app, task, baseline);
                if let Some(second) = second {
                    assert_eq!(update(&mut app, Message::Integration(second)).units(), 0);
                }
            }
        }
    }

    #[test]
    fn fps_cancellation_preserves_terminal_transport_failure() {
        use crate::integration_control::{self, tests::ProbeFixture};
        let mut app = fps_app();
        let mut fixture = ProbeFixture::new("square");
        assert!(
            fixture
                .prepare_app_fps(&app.model, app.settings.state(), false, false)
                .is_none()
        );
        std::mem::swap(app.integration.as_mut().unwrap(), &mut fixture.controller);
        app.model.error = Some(UiError::protocol("terminal transport closed"));
        integration_control::initialize_reporting(false, false);
        let cancelled = fixture.receiver.try_recv().unwrap();
        assert_eq!(update(&mut app, Message::Integration(cancelled)).units(), 0);
        assert!(
            !app.integration
                .as_ref()
                .unwrap()
                .accepts_message(&integration_control::Message::Advance)
        );
        assert_eq!(
            app.model.error.as_ref().unwrap().detail,
            "terminal transport closed"
        );
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
    fn accepted_detail_preference_survives_route_and_peer_restoration() {
        use crate::generated::{ApplicationIntentEndpoint, ApplicationSnapshot};
        use crate::presentation_surface::{self, ExploreDisplay};
        for delivery in 0..4 {
            let lost_reply = delivery == 2;
            presentation_surface::reset_test_releases();
            let (model, input_frame) = crate::view_model::test_support::explore_presentation();
            let mut app = installed_app();
            app.model = model;
            app.workspace.select(FeatureId::Explore);
            let original = app.model.explore.snapshot.clone().unwrap();
            let mut product = original.frame.clone();
            product.source.kind = crate::generated::PresentationSourceKind::Upscale;
            product.revision = 2;
            product.extent = crate::generated::VisualExtent {
                width: 1280,
                height: 1280,
            };
            product.content = crate::generated::VisualRegion {
                x: 0,
                y: 320,
                width: 1280,
                height: 640,
            };
            let frame = crate::view_model::test_support::physical_frame(
                crate::generated::presentation_source_session(product.source.kind),
                2,
                6,
                1280,
                1280,
            );
            let bytes = presentation_surface::metadata::encode(
                product.clone(),
                presentation_surface::metadata::encode_product(
                    crate::generated::ApplicationSystem::Upscale,
                    crate::generated::UpscaleImageMetadata {
                        frame: product,
                        input: original.frame.clone(),
                        scene: original.scene.clone(),
                    },
                ),
                Some(presentation_surface::metadata::encode_product(
                    crate::generated::ApplicationSystem::Explore,
                    crate::generated::ExploreImageMetadata::from(&original),
                )),
            );
            presentation_surface::metadata::retire(input_frame);
            presentation_surface::metadata::install(frame, 1280, 1280, 6, &bytes).unwrap();
            assert!(presentation_surface::accept_publication(frame));
            let surface = presentation_surface::metadata::surface(frame).unwrap();
            let Some(ExploreDisplay::Detail(_, content)) =
                presentation_surface::explore_display(Some(surface))
            else {
                panic!("paired detail fixture");
            };
            assert!(
                !app.workspace
                    .explore_state_for_test()
                    .detail_original(&content)
            );
            let (connection, mut capture) = crate::transport_connection::Connection::test_channel();
            app.connection = Some(connection);
            app.workspace
                .explore_state_for_test()
                .choose_detail_original(true);
            drop(app.on_explore(crate::view::explore::Outcome::DetailUpdated(
                crate::generated::ExploreDetailUpdate {
                    showoriginaldimensions: true,
                },
            )));
            let mut detail_correlation = None;
            while let Ok(record) = capture.try_recv() {
                if let crate::transport_connection::CapturedRecord::Intent(intent) = record
                    && app.model.pending_intent(intent.correlation)
                        == Some(ApplicationIntentEndpoint::ExploreUpdateDetail)
                {
                    detail_correlation = Some(intent.correlation);
                }
            }
            let correlation = detail_correlation.expect("ordinary detail submission");
            let mut accepted = original.clone();
            accepted.revision += 1;
            accepted.detail.showoriginaldimensions = true;
            if !lost_reply {
                if matches!(delivery, 1 | 3) {
                    drop(app.transition_page(FeatureId::Train));
                }
                if delivery == 3 {
                    drop(app.transition_page(FeatureId::Explore));
                    assert_eq!(
                        app.model.pending_intent(correlation),
                        Some(ApplicationIntentEndpoint::ExploreUpdateDetail)
                    );
                    let original = app
                        .workspace
                        .explore_state_for_test()
                        .detail_original(&content);
                    assert!(original);
                    assert_eq!(
                        content.configure_surface(surface, original, 0).crop,
                        Some([0, 320, 1280, 640])
                    );
                }
                app.reduce_reply(IntentReply {
                    correlation,
                    result: Ok(accepted.clone().into_application_transport_value()),
                });
                if delivery == 0 {
                    drop(app.transition_page(FeatureId::Train));
                }
                drop(app.transition_page(FeatureId::Explore));
                assert!(
                    app.workspace
                        .explore_state_for_test()
                        .detail_original(&content)
                );
            }
            app.retire_peer(UiError::transport("replacement peer"));
            let snapshots = crate::generated::application_snapshot_defaults()
                .unwrap()
                .into_iter()
                .map(|fact| match fact.value {
                    ApplicationSnapshot::Explore(_) => {
                        ApplicationSnapshot::Explore(accepted.clone())
                    }
                    ApplicationSnapshot::Settings(mut settings) => {
                        settings.settingsstate.currentview = FeatureId::Explore;
                        ApplicationSnapshot::Settings(settings)
                    }
                    value => value,
                })
                .collect();
            app.install_bootstrap(Bootstrap {
                input_epoch: 2,
                schema_fingerprint: crate::generated::SCHEMA_FINGERPRINT,
                snapshots,
            });
            assert_eq!(app.workspace.active(), FeatureId::Explore);
            assert!(
                app.workspace
                    .explore_state_for_test()
                    .detail_original(&content)
            );
            assert!(!content.original_dimensions()); // Cached metadata remains immutable.
            let shown = content.configure_surface(
                surface,
                app.workspace
                    .explore_state_for_test()
                    .detail_original(&content),
                0,
            );
            assert_eq!(shown.crop, Some([0, 320, 1280, 640]));
            presentation_surface::retire_publication(frame);
        }
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

    fn saturated_connection() -> (Connection, crate::transport_connection::Capture) {
        let (mut connection, capture) = Connection::test_channel();
        for _ in 0..64 {
            assert_eq!(
                connection
                    .send_interaction(crate::protocol::client_records::Interaction {
                        replaceable: false,
                        endpoint_id: crate::generated::ENDPOINT_Explore_UpdateViewport,
                        value: Vec::new()
                    })
                    .unwrap(),
                crate::transport_connection::SendDisposition::Queued
            );
        }
        (connection, capture)
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
    fn intent_and_protocol_terminal_paths_retire_the_peer() {
        let closed_connection = || {
            let (sender, receiver) = Connection::test_channel();
            drop(receiver);
            sender
        };

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

        for (event, detail) in [
            (
                TransportEvent::ProtocolError("invalid record".into()),
                "invalid record",
            ),
            (
                TransportEvent::IntegrationInputSettled,
                "integration input settlement without a driver",
            ),
        ] {
            let (mut protocol, task) = boot();
            drop(task);
            install_default_bootstrap(&mut protocol);
            protocol.workspace.select(FeatureId::Export);
            drop(protocol.on_transport(event));
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
            drop(protocol.on_transport(TransportEvent::IntegrationInputSettled));
            drop(protocol.on_transport(TransportEvent::Disconnected("worker stopped".into())));
            assert_eq!(
                protocol.model.error.as_ref().unwrap().kind,
                crate::view_model::UiErrorKind::Protocol
            );
            assert_eq!(protocol.model.error.as_ref().unwrap().detail, detail);
        }
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
                for _ in 0..64 {
                    connection
                        .send_interaction(crate::protocol::client_records::Interaction {
                            replaceable: false,
                            endpoint_id: crate::generated::ENDPOINT_Explore_UpdateViewport,
                            value: Vec::new(),
                        })
                        .unwrap();
                }
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
        let (connection, receiver) = saturated_connection();
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
    fn gallery_scroll_admits_each_viewport_while_native_work_is_pending() {
        for pending_overlay in [false, true] {
            let (mut app, task) = boot();
            drop(task);
            install_default_bootstrap(&mut app);
            app.workspace.select(FeatureId::Explore);
            let snapshot = app.model.explore.snapshot.as_mut().unwrap();
            snapshot.ready = true;
            snapshot.busy = !pending_overlay;
            snapshot.renderpending = pending_overlay;
            snapshot.mode = crate::generated::ExploreMode::Gallery;
            snapshot.order.matchingcount = 180_000;
            if pending_overlay {
                app.model
                    .begin_intent(ApplicationIntentEndpoint::ExploreUpdateOverlay)
                    .unwrap();
            }
            let (connection, mut capture) = Connection::test_channel();
            app.connection = Some(connection);
            for firstrow in [76, 77, 76, 79] {
                let request = ExploreViewportUpdate {
                    viewport: crate::generated::ExploreViewport {
                        extent: crate::generated::VisualExtent {
                            width: 896,
                            height: 1120,
                        },
                        firstrow,
                        rowcount: 5,
                        columns: 4,
                    },
                };
                drop(app.request_explore_viewport(request));
                assert!(
                    app.workspace.explore_dispatchable_viewport().is_none(),
                    "native work must not hold the current visible-row request"
                );
                assert!(matches!(
                    capture.try_recv().unwrap(),
                    crate::transport_connection::CapturedRecord::Other
                ));
            }
        }
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
            viewport: crate::generated::ExploreViewport {
                extent: crate::generated::VisualExtent {
                    width: 64,
                    height: 64,
                },
                firstrow: 0,
                rowcount: 1,
                columns: 1,
            },
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
        };
        drop(rejected.request_explore_viewport(draft_request));
        let authoritative = rejected.model.settings_snapshot.clone().unwrap();
        rejected.settings.settle_failure(Some(&authoritative));
        rejected.reconcile_explore_viewport();
        let rolled_back = rejected.workspace.explore_dispatchable_viewport().unwrap();
        assert_eq!(rolled_back.viewport.columns, authoritative_columns);
        assert_eq!(rolled_back.viewport.firstrow, 9);
    }

    #[test]
    fn explore_measured_messages_reconcile_on_gallery_return_without_more_input() {
        use crate::view::explore::{self, gallery};
        for sizes in [
            [(900.5, 320.25), (510.5, 710.25), (470.25, 810.5)],
            [(470.25, 810.5), (820.5, 440.25), (940.5, 310.25)],
        ] {
            let (mut app, task) = boot();
            drop(task);
            install_ready_explore(&mut app, 127);
            let columns = authoritative_explore_columns(&app);
            app.model.explore.snapshot.as_mut().unwrap().mode =
                crate::generated::ExploreMode::Detail;
            for (width, height) in sizes {
                drop(app.on_workspace(crate::view::router::Message::Explore(
                    explore::Message::Gallery(gallery::Message::Measured {
                        size: iced::Size::new(width, height),
                        maximum_extent: gallery_capacity(2048, 2048),
                        columns,
                    }),
                )));
                assert_eq!(
                    app.workspace.explore_gallery_size(),
                    Some(iced::Size::new(width, height))
                );
                assert_eq!(app.model.pending_count(), 0);
            }
            drop(app.on_workspace(crate::view::router::Message::Explore(
                explore::Message::Gallery(gallery::Message::Scrolled {
                    first_row: 2,
                    row_fraction: 0.375,
                    request: None,
                }),
            )));
            let mut returned = app.model.explore.snapshot.clone().unwrap();
            returned.mode = crate::generated::ExploreMode::Gallery;
            returned.revision += 1;
            reduce_explore_changed(&mut app, returned);
            let expected = app
                .workspace
                .explore_measured_layout_request(app.model.explore.snapshot.as_ref(), columns, 127)
                .unwrap();
            assert_eq!(expected.viewport.firstrow, 2);
            assert_eq!(
                app.workspace.explore_dispatchable_viewport(),
                Some(expected)
            );
        }
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
        drop(resize.request_explore_viewport(resized.clone()));
        let retained = resize.workspace.explore_dispatchable_viewport().unwrap();
        assert_eq!(retained, resized);

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
                }
                _ => {}
            }
        }
        let measured = reconnect
            .workspace
            .explore_measured_viewport(3, 11, 100)
            .unwrap();
        reconnect.install_bootstrap(Bootstrap {
            input_epoch: 1,
            schema_fingerprint: crate::generated::SCHEMA_FINGERPRINT,
            snapshots,
        });
        let rebased = reconnect.workspace.explore_dispatchable_viewport().unwrap();
        assert_eq!(rebased.viewport, measured);
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
        };
        app.workspace
            .explore_request_viewport(app.model.explore.snapshot.as_ref(), newer);

        let mut filtered = app.model.explore.snapshot.clone().unwrap();
        filtered.revision += 1;
        filtered.viewport = sent.viewport;
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

        app.workspace.explore_viewport_queued(rebased.clone());
        let mut committed = app.model.explore.snapshot.clone().unwrap();
        committed.revision += 1;
        committed.viewport = rebased.viewport;
        reduce_explore_changed(&mut app, committed);

        assert!(app.workspace.explore_dispatchable_viewport().is_none());
    }
}
