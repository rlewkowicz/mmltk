use crate::generated::FeatureId;
#[cfg(target_arch = "wasm32")]
use crate::integration_control::pixel_checks::atlas_resize_dimensions;
use crate::integration_control::pixel_checks::sample_upscale_pixels;
use crate::integration_control::pixel_checks::{
    AtlasDraw, ProbeOutcome, atlas_scroll_window, displayed_detail, fully_drawn_gallery,
    pixel_fixture_enabled, sampleable_presentation,
};
use crate::integration_control::probe::{
    ProbeReceipt, ScenarioOutput, complete_atlas_probe, current_receipt,
    invalidate_atlas_observation, probe_output, rearm_viewer_observation,
};
use crate::integration_control::widget_ops::{
    click, click_after_surface_draw, click_number_edge, paste_number_input, wheel_number_input,
};
use crate::integration_control::widget_ops::{
    gallery_slot_bounds, sidebar_control_visible, sidebar_reveal_offset,
};
use crate::integration_control::{
    COMPLETION_WITHOUT_INPUT, Driver, EXPLORE_AUGMENTATION_REROLL, EXPLORE_AUGMENTATION_TOGGLE,
    EXPLORE_DATASET_PANE, EXPLORE_DETAIL_CLOSE, EXPLORE_DETAIL_ORIGINAL, EXPLORE_DETAILS_PANE,
    EXPLORE_GALLERY, EXPLORE_RESHUFFLE, EXPLORE_UPSCALE_ACTIONS, Message, Phase, explore_message,
    pixel_checks, probe, reporting, route_edit_available, settled_settings_snapshot, widget_ops,
};
#[cfg(target_arch = "wasm32")]
use crate::integration_control::{
    canvas_size_js, canvas_size_settled_js, fullscreen_js, fullscreen_settled_js,
    hover_after_surface_draw_js, restore_canvas_size_js, sweep_js,
};
use crate::message::Message as RootMessage;
use crate::view::{explore, train};
use crate::view_model::ApplicationModel;
use iced::widget::operation::{AbsoluteOffset, RelativeOffset};
use iced::{Rectangle, Task};

/// Mutable observations owned by this scenario or mechanism.
pub(super) struct State {
    explore_paste_read: bool,
    gallery_completion_held: Option<(u64, u32)>,
    sweep_baseline: Option<(u64, crate::generated::ExploreViewport)>,
    explore_dataset_pane: Option<Rectangle>,
    explore_details_pane: Option<Rectangle>,
    upscale_button: Option<Rectangle>,
    upscale_pixels: Option<(u64, u64, u32, u32)>,
    upscale_repeat_revision: Option<u64>,
    upscale_repeat_observed: bool,
    upscale_cache_pass: bool,
    upscale_cached_frames: [Option<crate::generated::VisualFrame>; 3],
    viewer_continuity_request: Option<crate::generated::UpscaleRequest>,
    viewer_continuity_settings_revision: u64,
    viewer_continuity_performance: bool,
    viewer_route_persistence: bool,
    atlas_drawn: Option<(u64, u8)>,
    atlas_clip: (f32, f32),
    atlas_row_extent: f32,
    atlas_return_rows: u32,
    atlas_scroll_fraction: f32,
    atlas_receipt: Option<AtlasDraw>,
    atlas_pixels: Option<AtlasDraw>,
    atlas_composition: Option<AtlasDraw>,
    atlas_baseline: Option<(u64, u64)>,
    atlas_columns: u32,
    atlas_fixture_checked: bool,
    atlas_fixture_labels: bool,
    atlas_fixture_masks: bool,
    atlas_fixture_boxes: bool,
    explore_integer_baseline: u64,
    explore_integer_target: u64,
    explore_integer_revision: u64,
    resize_original_size: Option<iced::Size>,
    resize_previous_size: Option<iced::Size>,
    oversized_gallery: Option<(iced::Size, crate::generated::VisualExtent)>,
    selection_grid: Option<(u32, u32, u32, u64, u64)>,
}
impl Default for State {
    fn default() -> Self {
        Self {
            explore_paste_read: false,
            gallery_completion_held: None,
            sweep_baseline: None,
            explore_dataset_pane: None,
            explore_details_pane: None,
            upscale_button: None,
            upscale_pixels: None,
            upscale_repeat_revision: None,
            upscale_repeat_observed: false,
            upscale_cache_pass: false,
            upscale_cached_frames: std::array::from_fn(|_| None),
            viewer_continuity_request: None,
            viewer_continuity_settings_revision: 0,
            viewer_continuity_performance: false,
            viewer_route_persistence: false,
            atlas_drawn: None,
            atlas_clip: (0.0, 0.0),
            atlas_row_extent: 0.0,
            atlas_return_rows: 0,
            atlas_scroll_fraction: 0.0,
            atlas_receipt: None,
            atlas_pixels: None,
            atlas_composition: None,
            atlas_baseline: None,
            atlas_columns: 0,
            atlas_fixture_checked: false,
            atlas_fixture_labels: false,
            atlas_fixture_masks: false,
            atlas_fixture_boxes: false,
            explore_integer_baseline: 0,
            explore_integer_target: 0,
            explore_integer_revision: 0,
            resize_original_size: None,
            resize_previous_size: None,
            oversized_gallery: None,
            selection_grid: None,
        }
    }
}

impl State {
    pub(super) fn prepare_upscale_probe(
        &mut self,
        probes: &mut probe::Requests,
        image: Rectangle,
        source: u64,
        presentation: u64,
    ) -> Option<ScenarioOutput> {
        let output = probe_output(explore::DETAIL_WORKSPACE_ID)?;
        let receipt = output.receipt.as_ref()?;
        let frame = receipt.surface.frame?;
        if receipt.image != image
            || frame.content_sequence != source
            || frame.presentation_revision != presentation
        {
            return None;
        }
        probes.begin_upscale_probe(&output);
        self.upscale_pixels = None;
        Some(output)
    }
    pub(super) fn advance_retained(
        &mut self,
        driver: &mut Driver,
        pixel_checks: &mut pixel_checks::State,
        probes: &mut probe::Requests,
        widgets: &mut widget_ops::RevealState,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        router: &crate::view::router::Router,
        active: FeatureId,
        surface: Option<crate::presentation_surface::Surface>,
    ) -> Task<RootMessage> {
        let frame = surface.and_then(|surface| surface.frame);
        match driver.phase.clone() {
            Phase::AtlasPixelColumns(columns) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.viewport.columns != columns
                    || snapshot.busy
                    || model.has_explore_pending()
                    || snapshot.overlay.showlabels
                    || !snapshot.overlay.showmasks
                    || !snapshot.overlay.showboxes
                    || self.atlas_composition.as_ref().is_none_or(|draw| {
                        draw.snapshot.frame != snapshot.frame
                            || draw.snapshot.viewport.columns != columns
                    })
                    || self.atlas_pixels.as_ref().is_none_or(|draw| {
                        draw.snapshot.frame != snapshot.frame
                            || draw.snapshot.gallery.slots.iter().any(|ready| !*ready)
                    })
                {
                    return Task::none();
                }
                let next = if columns == 4 { 10 } else { self.atlas_columns };
                driver.phase = if columns == 4 {
                    Phase::AtlasPixelColumns(10)
                } else {
                    Phase::AtlasPixelRestore
                };
                let next_columns = explore_message(explore::Message::Gallery(
                    explore::gallery::Message::ColumnsChanged(next as i32),
                ));
                if columns == 10 {
                    next_columns
                        .chain(explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Overlay(
                                explore::overlay::Message::LabelsToggled(self.atlas_fixture_labels),
                            ),
                        )))
                        .chain(explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Overlay(
                                explore::overlay::Message::MasksToggled(self.atlas_fixture_masks),
                            ),
                        )))
                        .chain(explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Overlay(
                                explore::overlay::Message::BoxesToggled(self.atlas_fixture_boxes),
                            ),
                        )))
                } else {
                    next_columns
                }
            }
            Phase::AtlasPixelRestore => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.viewport.columns != self.atlas_columns
                    || snapshot.busy
                    || model.has_explore_pending()
                    || snapshot.overlay.showlabels != self.atlas_fixture_labels
                    || snapshot.overlay.showmasks != self.atlas_fixture_masks
                    || snapshot.overlay.showboxes != self.atlas_fixture_boxes
                    || self
                        .atlas_pixels
                        .as_ref()
                        .is_none_or(|draw| draw.snapshot.frame != snapshot.frame)
                {
                    return Task::none();
                }
                driver.advance_to(Phase::AwaitExploreReady)
            }
            Phase::ViewerConfirmSettings | Phase::ViewerRestoreSettings => {
                let restore = driver.phase == Phase::ViewerRestoreSettings;
                let Some(snapshot) = settled_settings_snapshot(
                    model,
                    settings,
                    self.viewer_continuity_settings_revision,
                ) else {
                    return Task::none();
                };
                let expected = if restore {
                    self.viewer_continuity_performance
                } else {
                    !self.viewer_continuity_performance
                };
                if snapshot.settingsstate.ui.showworkspaceperformance != expected {
                    return Task::none();
                }
                if model.explore.requested_upscale != self.viewer_continuity_request
                    || model.current_upscale().is_none_or(|upscale| {
                        self.upscale_cached_frames[2].as_ref() != Some(&upscale.frame)
                    })
                {
                    driver.fail("same-route Settings confirmation changed the resident viewer demand or result");
                    return Task::none();
                }
                self.viewer_continuity_settings_revision = snapshot.revision;
                if !restore {
                    driver.phase = Phase::ViewerRestoreSettings;
                    return Task::done(RootMessage::Settings(
                        crate::view::settings::Message::PerformanceChanged(
                            self.viewer_continuity_performance,
                        ),
                    ));
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_settings_preserved",
                        explore::DETAIL_WORKSPACE_ID,
                        "confirmed-native-settings",
                        [snapshot.revision as f64, 1.0, 0.0, 0.0],
                    )
                });
                self.viewer_route_persistence =
                    settings.draft.is_some() && model.settings_edit_available();
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_departure_started",
                        explore::DETAIL_WORKSPACE_ID,
                        "upscale-observation",
                        [
                            model
                                .upscale_snapshot
                                .as_ref()
                                .map_or(0, |state| state.revision)
                                as f64,
                            u8::from(self.viewer_route_persistence) as f64,
                            snapshot.revision as f64,
                            0.0,
                        ],
                    )
                });
                driver.phase = Phase::ViewerDepart;
                Task::done(RootMessage::Settings(crate::view::settings::Message::Close)).chain(
                    Task::done(RootMessage::Workspace(
                        crate::view::router::Message::Navigation(
                            crate::view::navigation::Message::PageSelected(FeatureId::Train),
                        ),
                    )),
                )
            }
            Phase::ViewerDepart => {
                if active != FeatureId::Train
                    || model.explore.requested_upscale.is_some()
                    || model.has_pending(crate::generated::ApplicationIntentEndpoint::UpscaleStop)
                    || !route_edit_available(model, settings)
                {
                    return Task::none();
                }
                if model.foreground_visual().is_some() {
                    driver.fail("mapped Train departure retained a visual foreground");
                    return Task::none();
                }
                if self.viewer_route_persistence
                    && !model.settings_snapshot.as_ref().is_some_and(|snapshot| {
                        snapshot.revision > self.viewer_continuity_settings_revision
                            && snapshot.settingsstate.currentview == FeatureId::Train
                    })
                {
                    return Task::none();
                }
                self.viewer_continuity_settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                if crate::presentation_surface::pixel_trace::enabled() {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.viewer_route_confirmed",
                            "navigation.train",
                            "None",
                            [
                                self.viewer_continuity_settings_revision as f64,
                                u8::from(self.viewer_route_persistence) as f64,
                                1.0,
                                0.0,
                            ],
                        )
                    });
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_abandoned",
                        explore::DETAIL_WORKSPACE_ID,
                        "mapped-route-departure",
                        [
                            model
                                .upscale_snapshot
                                .as_ref()
                                .map_or(0, |state| state.revision)
                                as f64,
                            0.0,
                            0.0,
                            0.0,
                        ],
                    )
                });
                driver.phase = Phase::ViewerReenter;
                probes.await_viewer_draw();
                rearm_viewer_observation();
                Task::done(RootMessage::Workspace(
                    crate::view::router::Message::Navigation(
                        crate::view::navigation::Message::PageSelected(FeatureId::Explore),
                    ),
                ))
            }
            Phase::ViewerReenter => {
                if active != FeatureId::Explore {
                    return Task::none();
                }
                if !route_edit_available(model, settings) {
                    return Task::none();
                }
                if self.viewer_route_persistence
                    && !model.settings_snapshot.as_ref().is_some_and(|snapshot| {
                        snapshot.revision > self.viewer_continuity_settings_revision
                            && snapshot.settingsstate.currentview == FeatureId::Explore
                    })
                {
                    return Task::none();
                }
                let Some(upscale) = model.current_upscale() else {
                    return Task::none();
                };
                let Some(request) = model.explore.requested_upscale.as_ref() else {
                    return Task::none();
                };
                if request.kernel != crate::generated::UpscaleKernel::Default
                    || self.viewer_continuity_request.as_ref().is_none_or(|prior| {
                        prior.source != request.source || prior.document != request.document
                    })
                    || model.displayed_upscale_kernel()
                        != Some(crate::generated::UpscaleKernel::Default)
                    || self.upscale_cached_frames[0].as_ref() != Some(&upscale.frame)
                    || probes
                        .draws()
                        .viewer
                        .is_none_or(|(_, source, _)| source != upscale.frame.revision)
                {
                    return Task::none();
                }
                if !matches!(
                    model.foreground_visual(),
                    Some(
                        crate::generated::PresentationSourceKind::Explore
                            | crate::generated::PresentationSourceKind::Upscale
                    )
                ) {
                    driver.fail("mapped Explore reentry has a mismatched visual foreground");
                    return Task::none();
                }
                let Some(sampleable) = sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Upscale,
                    upscale.frame.revision,
                ) else {
                    return Task::none();
                };
                if probes
                    .draws()
                    .viewer
                    .is_none_or(|(drawn, _, _)| drawn != sampleable.presentation_revision)
                {
                    return Task::none();
                }
                if crate::presentation_surface::pixel_trace::enabled() {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.viewer_route_confirmed",
                            "navigation.explore",
                            if model.foreground_visual()
                                == Some(crate::generated::PresentationSourceKind::Upscale)
                            {
                                "Upscale"
                            } else {
                                "Explore"
                            },
                            [
                                model
                                    .settings_snapshot
                                    .as_ref()
                                    .map_or(0, |snapshot| snapshot.revision)
                                    as f64,
                                u8::from(self.viewer_route_persistence) as f64,
                                1.0,
                                0.0,
                            ],
                        )
                    });
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_basic_reentry",
                        explore::DETAIL_WORKSPACE_ID,
                        "automatic-completed-draw",
                        [
                            upscale.frame.revision as f64,
                            sampleable.presentation_revision as f64,
                            upscale.frame.source.instance as f64,
                            upscale.frame.cleanrevision as f64,
                        ],
                    )
                });
                if !self.require_original_crop(driver, probes, &upscale.frame) {
                    return Task::none();
                }
                self.viewer_continuity_request = Some(request.clone());
                driver.phase = Phase::ViewerAwaitDisconnect;
                // Retiring the real outbound owner closes the worker's socket;
                // the ordinary transport subscription performs the reconnect.
                Task::done(RootMessage::Transport(
                    crate::transport::TransportEvent::Disconnected(
                        "rendered continuity acceptance".into(),
                    ),
                ))
            }
            Phase::ViewerAwaitDisconnect => {
                if model.connection == crate::view_model::ConnectionState::Connected {
                    return Task::none();
                }
                probes.await_viewer_draw();
                rearm_viewer_observation();
                driver.phase = Phase::ViewerReconnect;
                Task::none()
            }
            Phase::ViewerReconnect => {
                if model.connection != crate::view_model::ConnectionState::Connected
                    || model.error.is_some()
                    || active != FeatureId::Explore
                    || model.explore.requested_upscale != self.viewer_continuity_request
                {
                    return Task::none();
                }
                let Some(upscale) = model.current_upscale() else {
                    return Task::none();
                };
                let Some((drawn, source, _)) = probes.draws().viewer else {
                    return Task::none();
                };
                if source != upscale.frame.revision
                    || model.displayed_upscale_kernel()
                        != Some(crate::generated::UpscaleKernel::Default)
                    || self.upscale_cached_frames[0].as_ref() != Some(&upscale.frame)
                {
                    return Task::none();
                }
                let Some(sampleable) = sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Upscale,
                    source,
                ) else {
                    return Task::none();
                };
                if drawn != sampleable.presentation_revision {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_reconnected",
                        explore::DETAIL_WORKSPACE_ID,
                        "matching-completed-draw",
                        [
                            source as f64,
                            drawn as f64,
                            upscale.frame.source.instance as f64,
                            upscale.frame.cleanrevision as f64,
                        ],
                    )
                });
                if !self.require_original_crop(driver, probes, &upscale.frame) {
                    return Task::none();
                }
                probes.await_annotation_sample(drawn);
                let (next, control) = match driver.viewer_scenario.as_str() {
                    "copy" => (Phase::OpenAnnotation, EXPLORE_ANNOTATE),
                    "semantics" => (Phase::ViewerNoAspect, "explore.detail.aspect"),
                    _ => (
                        Phase::DetailNext(
                            model
                                .explore
                                .snapshot
                                .as_ref()
                                .and_then(|snapshot| snapshot.selectedimage)
                                .unwrap_or_default(),
                        ),
                        EXPLORE_NEXT,
                    ),
                };
                driver.phase = next;
                widgets.arm(driver, control)
            }
            Phase::Disabled | Phase::Complete | Phase::Failed => Task::none(),
            Phase::ExploreNavigation => widgets.arm(
                driver,
                crate::view::navigation::stable_id(FeatureId::Explore),
            ),
            Phase::AwaitExplore
                if active == FeatureId::Explore
                    && !settings.has_local_edits()
                    && model.explore_open_available() =>
            {
                use crate::presentation_surface::ExploreDisplay;
                let logical = model.explore.snapshot.as_ref();
                match crate::presentation_surface::explore_display(surface) {
                    Some(ExploreDisplay::Detail(_, _)) => {
                        if logical.is_some_and(|snapshot| {
                            snapshot.mode == crate::generated::ExploreMode::Detail
                        }) && model.explore_mutation_available()
                        {
                            driver.phase = Phase::ExploreCloseDetail;
                            return widgets.arm(driver, EXPLORE_DETAIL_CLOSE);
                        }
                        return Task::none();
                    }
                    Some(ExploreDisplay::Gallery(_, _)) => {
                        if logical.is_none_or(|snapshot| {
                            snapshot.mode != crate::generated::ExploreMode::Gallery
                        }) {
                            return Task::none();
                        }
                    }
                    None if logical.is_some_and(|snapshot| {
                        snapshot.ready || snapshot.mode == crate::generated::ExploreMode::Detail
                    }) =>
                    {
                        return Task::none();
                    }
                    None => {} // The initial empty page owns Open before any product exists.
                }
                if let Some(snapshot) = model
                    .explore
                    .snapshot
                    .as_ref()
                    .filter(|snapshot| snapshot.ready)
                    && let Some(message) = explore_scenario_preparation(snapshot)
                {
                    driver.phase = Phase::AwaitExplorePreparation(snapshot.revision);
                    return explore_message(message);
                }
                if router.explore_measured_viewport(3, 0, 0).is_none() {
                    return Task::none();
                }
                driver.phase = Phase::ExploreOpen;
                widgets.arm(driver, EXPLORE_OPEN)
            }
            Phase::ExploreCloseDetail => widgets.arm(driver, EXPLORE_DETAIL_CLOSE),
            Phase::AwaitExploreGallery
                if matches!(
                    crate::presentation_surface::explore_display(surface),
                    Some(crate::presentation_surface::ExploreDisplay::Gallery(_, _))
                ) && model.explore.snapshot.as_ref().is_some_and(|snapshot| {
                    snapshot.mode == crate::generated::ExploreMode::Gallery && !snapshot.busy
                }) && !model.explore.desired_close
                    && !model.has_explore_pending() =>
            {
                driver.advance_to(Phase::AwaitExplore)
            }
            Phase::AwaitExplorePreparation(revision)
                if model
                    .explore
                    .snapshot
                    .as_ref()
                    .is_some_and(|snapshot| snapshot.revision > revision && !snapshot.busy)
                    && !model.has_explore_pending()
                    && !settings.has_local_edits()
                    && !model.native_settings_unsettled() =>
            {
                driver.advance_to(Phase::AwaitExplore)
            }
            Phase::ExploreOpen => widgets.arm(driver, EXPLORE_OPEN),
            Phase::AwaitExploreReady => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.ready
                    && !snapshot.busy
                    && snapshot.frame.revision != 0
                    && !settings.has_local_edits()
                    && model.explore_viewport_available()
                {
                    if snapshot.dataset.classcatalogidentity == 0
                        || model.explore.presentation_state()
                            != crate::view_model::ExplorePresentationState::Populated
                    {
                        driver.fail(
                            "Explore typed snapshot did not reach the populated catalog state",
                        );
                        return Task::none();
                    }
                    let measured = router.explore_measured_viewport(
                        snapshot.viewport.columns,
                        snapshot.viewport.firstrow,
                        snapshot.order.matchingcount,
                    );
                    let Some(measured) = measured else {
                        return Task::none();
                    };
                    if measured.extent != snapshot.viewport.extent {
                        reporting::emit(|sink| {
                            sink.record(
                                "integration.explore_extent_pending",
                                EXPLORE_GALLERY,
                                "measured-native-convergence",
                                [
                                    measured.extent.width as f64,
                                    measured.extent.height as f64,
                                    snapshot.viewport.extent.width as f64,
                                    snapshot.viewport.extent.height as f64,
                                ],
                            )
                        });
                        return Task::none();
                    }
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_ready",
                            "",
                            "",
                            [
                                snapshot.dataset.imagecount as f64,
                                snapshot.dataset.imagewidth as f64,
                                snapshot.dataset.imageheight as f64,
                                snapshot.dataset.classnames.len() as f64,
                            ],
                        )
                    });
                    if driver.viewer_scenario == "quiet" {
                        self.selection_grid = Some((
                            snapshot.viewport.columns,
                            snapshot.viewport.rowcount,
                            0,
                            snapshot.revision,
                            snapshot.frame.revision,
                        ));
                        COMPLETION_WITHOUT_INPUT.with(|active| active.set(false));
                        driver.phase = Phase::ViewerSelect;
                        return widgets.arm(driver, EXPLORE_GALLERY);
                    }
                    let Some(draw) = self.atlas_pixels.as_ref().filter(|draw| {
                        draw.snapshot.dataset.identity == snapshot.dataset.identity
                            && draw.snapshot.gallery.generation == snapshot.gallery.generation
                            && draw.snapshot.frame == snapshot.frame
                            && draw.snapshot.viewport == snapshot.viewport
                            && draw.surface.frame == frame
                            && !snapshot.gallery.slots.is_empty()
                            && snapshot.gallery.slots.len() == snapshot.order.visibleindices.len()
                            && snapshot.gallery.slots.iter().all(|ready| *ready)
                            && self.atlas_receipt.as_ref() == Some(*draw)
                    }) else {
                        return Task::none();
                    };
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.initial_atlas_identity",
                            EXPLORE_GALLERY,
                            &format!(
                                "{:016x}:{:016x}{:016x}",
                                snapshot.dataset.identity, draw.surface.high, draw.surface.low
                            ),
                            [
                                snapshot.viewport.firstrow as f64,
                                snapshot.viewport.columns as f64,
                                snapshot.viewport.extent.width as f64,
                                snapshot.viewport.extent.height as f64,
                            ],
                        )
                    });
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.initial_atlas_complete",
                            EXPLORE_GALLERY,
                            "no-input-canvas-pixels",
                            [
                                snapshot.dataset.identity as f64,
                                snapshot.gallery.generation as f64,
                                snapshot.frame.revision as f64,
                                draw.surface
                                    .frame
                                    .map_or(0, |frame| frame.presentation_revision)
                                    as f64,
                            ],
                        )
                    });
                    COMPLETION_WITHOUT_INPUT.with(|active| active.set(false));
                    if pixel_fixture_enabled() && !self.atlas_fixture_checked {
                        self.atlas_fixture_checked = true;
                        self.atlas_columns = snapshot.viewport.columns;
                        self.atlas_fixture_labels = snapshot.overlay.showlabels;
                        self.atlas_fixture_masks = snapshot.overlay.showmasks;
                        self.atlas_fixture_boxes = snapshot.overlay.showboxes;
                        driver.phase = Phase::AtlasPixelColumns(4);
                        return explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Overlay(
                                explore::overlay::Message::LabelsToggled(false),
                            ),
                        ))
                        .chain(explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Overlay(
                                explore::overlay::Message::MasksToggled(true),
                            ),
                        )))
                        .chain(explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Overlay(
                                explore::overlay::Message::BoxesToggled(true),
                            ),
                        )))
                        .chain(explore_message(
                            explore::Message::Gallery(explore::gallery::Message::ColumnsChanged(4)),
                        ));
                    }
                    if let Some(task) = pixel_checks.begin_workspace_fps(driver, settings) {
                        return task;
                    }
                    if !driver.viewer_scenario.is_empty() {
                        self.selection_grid = Some((
                            snapshot.viewport.columns,
                            snapshot.viewport.rowcount,
                            0,
                            snapshot.revision,
                            snapshot.frame.revision,
                        ));
                        driver.phase = Phase::ViewerSelect;
                        if driver.viewer_scenario == "rapid" {
                            driver.phase = Phase::ViewerRapidGallery;
                            COMPLETION_WITHOUT_INPUT.with(|active| active.set(true));
                            return explore_message(explore::Message::Gallery(
                                explore::gallery::Message::AugmentationToggled(true),
                            ))
                            .chain(explore_message(explore::Message::Gallery(
                                explore::gallery::Message::AugmentationRerollRequested,
                            )))
                            .chain(explore_message(
                                explore::Message::Gallery(
                                    explore::gallery::Message::AugmentationRerollRequested,
                                ),
                            ));
                        }
                        return widgets.arm(driver, EXPLORE_GALLERY);
                    }
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_policy",
                            explore::ORDER_CONTROL_ID,
                            if snapshot.filter.order == crate::generated::ExploreOrder::Shuffled {
                                "shuffled"
                            } else {
                                "sequential"
                            },
                            [
                                snapshot.filter.minimumcompiledindex as f64,
                                snapshot.filter.maximumcompiledindex as f64,
                                snapshot.overlay.classselection.classes.len() as f64,
                                snapshot.order.shuffleseed as f64,
                            ],
                        )
                    });
                    self.sweep_baseline = Some((snapshot.revision, snapshot.viewport.clone()));
                    return driver.advance_to(Phase::AwaitExploreInitialPatch {
                        revision: snapshot.revision,
                        frame_revision: snapshot.frame.revision,
                    });
                }
                Task::none()
            }
            Phase::AwaitExploreInitialPatch {
                revision,
                frame_revision,
            }
            | Phase::AwaitExploreExactGridPatch {
                revision,
                frame_revision,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let initial_patch = matches!(driver.phase, Phase::AwaitExploreInitialPatch { .. });
                let revision_pending =
                    snapshot.revision < revision || snapshot.frame.revision < frame_revision;
                // Padding donors are background images. Their published tile
                // readiness, independent of annotations, must precede resizing.
                let initial_tiles_pending = initial_patch
                    && [7, 8].iter().any(|index| {
                        snapshot
                            .order
                            .visibleindices
                            .iter()
                            .position(|compiled| compiled == index)
                            .and_then(|slot| snapshot.gallery.slots.get(slot))
                            .copied()
                            != Some(true)
                    });
                let sampleable = sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                );
                if initial_patch {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_patch_wait",
                            EXPLORE_GALLERY,
                            "revision-tiles-busy-publication",
                            [
                                f64::from(u8::from(revision_pending)),
                                f64::from(u8::from(initial_tiles_pending)),
                                f64::from(u8::from(snapshot.busy)),
                                f64::from(u8::from(sampleable.is_none())),
                            ],
                        )
                    });
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_patch_draw",
                            EXPLORE_GALLERY,
                            "drawn-and-sampleable-publication",
                            [
                                probes
                                    .draws()
                                    .gallery
                                    .map_or(0.0, |(presentation, _)| presentation as f64),
                                probes
                                    .draws()
                                    .gallery
                                    .map_or(0.0, |(_, source)| source as f64),
                                sampleable
                                    .map_or(0.0, |sample| sample.presentation_revision as f64),
                                sampleable.map_or(0.0, |sample| sample.source_revision as f64),
                            ],
                        )
                    });
                }
                if revision_pending
                    || initial_tiles_pending
                    || snapshot.busy
                    || sampleable.is_none()
                    || sampleable.is_some_and(|sample| {
                        probes.draws().gallery
                            != Some((sample.presentation_revision, sample.source_revision))
                    })
                {
                    return Task::none();
                }
                if matches!(driver.phase, Phase::AwaitExploreExactGridPatch { .. }) {
                    let Some((size, maximum_extent)) = self.oversized_gallery.take() else {
                        driver.fail("oversized Explore measurement lacks its original geometry");
                        return Task::none();
                    };
                    driver.phase = Phase::ExploreDatasetPane;
                    // The capacity probe overrides the component's measurement,
                    // not the sensor's physical layout. No resize event is owed
                    // when the real bounds remain unchanged.
                    return explore_message(explore::Message::Gallery(
                        explore::gallery::Message::Measured {
                            size,
                            maximum_extent,
                            columns: snapshot.viewport.columns.max(1),
                        },
                    ))
                    .chain(widgets.arm(driver, EXPLORE_DATASET_PANE));
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_initial_patch",
                        EXPLORE_GALLERY,
                        "stable-gallery-generation",
                        [
                            revision as f64,
                            snapshot.revision as f64,
                            frame_revision as f64,
                            snapshot.frame.revision as f64,
                        ],
                    )
                });
                let Some(size) = router.explore_gallery_size() else {
                    return Task::none();
                };
                let columns = snapshot.viewport.columns.max(1);
                let capacity = snapshot.maximumatlasextent.clone();
                self.oversized_gallery = Some((size, capacity.clone()));
                driver.phase = Phase::AwaitExploreExactGrid(snapshot.revision);
                Task::done(RootMessage::Workspace(
                    crate::view::router::Message::Explore(explore::Message::Gallery(
                        explore::gallery::Message::Measured {
                            size: iced::Size::new(10_000.0, 10_000.0),
                            maximum_extent: capacity,
                            columns,
                        },
                    )),
                ))
            }
            Phase::AwaitExploreExactGrid(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let Some((_, capacity)) = self.oversized_gallery.as_ref() else {
                    driver.fail("oversized Explore capacity was not retained");
                    return Task::none();
                };
                let columns = snapshot.viewport.columns.max(1);
                let rows = snapshot.viewport.rowcount.max(1);
                // A different logical measurement may resolve to the same
                // bounded raster. That no-op has no new native revision.
                if snapshot.revision < revision
                    || snapshot.busy
                    || snapshot.viewport.extent.width > capacity.width
                    || snapshot.viewport.extent.height > capacity.height
                {
                    return Task::none();
                }
                if snapshot.viewport.extent.width % columns != 0
                    || snapshot.viewport.extent.height % rows != 0
                    || snapshot.viewport.extent.width / columns
                        != snapshot.viewport.extent.height / rows
                {
                    driver
                        .fail("oversized Explore measurement did not produce an exact square grid");
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_exact_grid",
                        EXPLORE_GALLERY,
                        "oversized-logical-fill",
                        [
                            snapshot.viewport.extent.width as f64,
                            snapshot.viewport.extent.height as f64,
                            columns as f64,
                            rows as f64,
                        ],
                    )
                });
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_exact_grid_capacity",
                        EXPLORE_GALLERY,
                        "measured-revision-capacity",
                        [
                            snapshot.revision as f64,
                            snapshot.frame.revision as f64,
                            capacity.width as f64,
                            capacity.height as f64,
                        ],
                    )
                });
                driver.advance_to(Phase::AwaitExploreExactGridPatch {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                })
            }
            Phase::ExploreDatasetPane => widgets.arm(driver, EXPLORE_DATASET_PANE),
            Phase::ExploreDetailsPane => widgets.arm(driver, EXPLORE_DETAILS_PANE),
            Phase::ExploreNumericStart(index) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if !model.explore_mutation_available()
                    || settings.has_local_edits()
                    || model.has_explore_pending()
                {
                    return Task::none();
                }
                if index == 5 {
                    driver.phase = Phase::ExplorePolicyOrderReady;
                    return iced::widget::operation::snap_to(
                        explore::DATASET_SCROLL_ID,
                        RelativeOffset::START,
                    );
                }
                let value = explore_integer_value(snapshot, index);
                self.explore_integer_baseline = value;
                self.explore_integer_target = match index {
                    0 => {
                        if value == 0 {
                            1
                        } else {
                            0
                        }
                    }
                    1 => {
                        if value == 10_000 {
                            9_999
                        } else {
                            10_000
                        }
                    }
                    2 => explore_seed_target(value),
                    3 => {
                        if value == 0 {
                            1
                        } else {
                            0
                        }
                    }
                    _ => value
                        .min(u64::from(snapshot.dataset.imagecount.saturating_sub(1)))
                        .saturating_sub(1),
                };
                self.explore_integer_revision = snapshot.revision;
                driver.phase = Phase::ExploreNumericControl { index, step: 0 };
                widgets.arm(driver, explore_integer_id(index))
            }
            Phase::ExploreNumericControl { index, .. } => {
                widgets.arm(driver, explore_integer_id(index))
            }
            Phase::ExploreNumericReveal { index, .. } => widgets.arm_revealed(
                driver,
                explore::DATASET_SCROLL_ID,
                explore_integer_id(index),
            ),
            Phase::AwaitExploreNumeric { index, step } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy || model.has_explore_pending() {
                    return Task::none();
                }
                let value = explore_integer_value(snapshot, index);
                if step < 3 {
                    if value != self.explore_integer_baseline {
                        driver.fail("Explore integer changed from an edge click or wheel");
                        return Task::none();
                    }
                    if step == 2 {
                        widgets.replace_numeric_value("x".to_owned());
                    }
                    driver.phase = Phase::ExploreNumericControl {
                        index,
                        step: step + 1,
                    };
                    return widgets.arm(driver, explore_integer_id(index));
                }
                if step == 3 {
                    if value != self.explore_integer_baseline {
                        driver.fail("Explore unsigned integer accepted invalid text");
                        return Task::none();
                    }
                    widgets.replace_numeric_value(self.explore_integer_target.to_string());
                    driver.phase = Phase::ExploreNumericControl { index, step: 4 };
                    return widgets.arm(driver, explore_integer_id(index));
                }
                let expected = if step == 4 || step == 6 {
                    self.explore_integer_target
                } else {
                    self.explore_integer_baseline
                };
                if value != expected
                    || snapshot.revision <= self.explore_integer_revision
                    || !explore_integer_persisted(model, index, expected)
                    || (step == 6 && !self.explore_paste_read)
                {
                    return Task::none();
                }
                if step == 4 {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_integer",
                            &explore_integer_id(index),
                            &value.to_string(),
                            [
                                self.explore_integer_revision as f64,
                                snapshot.revision as f64,
                                1.0,
                                1.0,
                            ],
                        )
                    });
                    self.explore_integer_revision = snapshot.revision;
                    widgets.replace_numeric_value(self.explore_integer_baseline.to_string());
                    if index == 4 && self.explore_integer_baseline == u64::MAX {
                        driver.phase = Phase::AwaitExploreNumeric { index, step: 5 };
                        return explore_message(explore::Message::Dataset(
                            explore::dataset::Message::UnlimitedCompiledIndex,
                        ));
                    }
                    driver.phase = Phase::ExploreNumericControl { index, step: 5 };
                    widgets.arm(driver, explore_integer_id(index))
                } else if index == 2 && step == 5 {
                    // The typed edit has already restored its baseline. Prepare
                    // clipboard contents separately; only the rendered widget
                    // may consume them through its ordinary paste shortcut.
                    self.explore_paste_read = false;
                    self.explore_integer_revision = snapshot.revision;
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_integer_paste_baseline",
                            &explore_integer_id(index),
                            &value.to_string(),
                            [snapshot.revision as f64, 0.0, 1.0, 1.0],
                        )
                    });
                    driver.phase = Phase::AwaitExploreClipboard(snapshot.revision);
                    let generation = driver.generation;
                    let revision = snapshot.revision;
                    iced::clipboard::write(self.explore_integer_target.to_string()).map(
                        move |result| {
                            RootMessage::Integration(Message::Scoped {
                                generation,
                                receipt: None,
                                message: Box::new(Message::NumberClipboardPrepared {
                                    revision,
                                    result,
                                }),
                            })
                        },
                    )
                } else if step == 6 {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_integer_paste",
                            &explore_integer_id(index),
                            &value.to_string(),
                            [
                                self.explore_integer_revision as f64,
                                snapshot.revision as f64,
                                1.0,
                                1.0,
                            ],
                        )
                    });
                    self.explore_integer_revision = snapshot.revision;
                    widgets.replace_numeric_value(self.explore_integer_baseline.to_string());
                    driver.phase = Phase::ExploreNumericControl { index, step: 7 };
                    widgets.arm(driver, explore_integer_id(index))
                } else {
                    if step == 7 {
                        self.explore_paste_read = false;
                        reporting::emit(|sink| {
                            sink.record(
                                "integration.explore_integer_paste_restored",
                                &explore_integer_id(index),
                                &value.to_string(),
                                [
                                    self.explore_integer_revision as f64,
                                    snapshot.revision as f64,
                                    1.0,
                                    1.0,
                                ],
                            )
                        });
                    }
                    driver.advance_to(Phase::ExploreNumericStart(index + 1))
                }
            }
            Phase::ExplorePolicyOrderReady => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_policy_order_arm",
                        explore::ORDER_SHUFFLED_ID,
                        "model-availability",
                        [
                            snapshot.revision as f64,
                            if snapshot.busy { 1.0 } else { 0.0 },
                            if settings.has_local_edits() { 1.0 } else { 0.0 },
                            if model.explore_mutation_available() {
                                1.0
                            } else {
                                0.0
                            },
                        ],
                    )
                });
                driver.phase = Phase::ExplorePolicyOrder(snapshot.revision);
                widgets.arm(driver, explore::ORDER_SHUFFLED_ID)
            }
            Phase::ExplorePolicyOrder(_) => widgets.arm(driver, explore::ORDER_SHUFFLED_ID),
            Phase::AwaitExplorePolicyOrder(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.filter.order != crate::generated::ExploreOrder::Shuffled
                {
                    return Task::none();
                }
                driver.phase = Phase::ExplorePolicyRangeReady;
                Task::none()
            }
            Phase::ExplorePolicyRangeReady => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                driver.phase = Phase::ExplorePolicyRange(snapshot.revision);
                widgets.arm(driver, explore::RANGE_START_ONE_ID)
            }
            Phase::ExplorePolicyRange(_) => widgets.arm(driver, explore::RANGE_START_ONE_ID),
            Phase::ExplorePolicyRangeVisible(_) => widgets.arm_revealed(
                driver,
                explore::DATASET_SCROLL_ID,
                explore::RANGE_START_ONE_ID,
            ),
            Phase::AwaitExplorePolicyRange(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.filter.minimumcompiledindex != 1
                {
                    return Task::none();
                }
                driver.phase = Phase::ExplorePolicyOverlayReady;
                Task::none()
            }
            Phase::ExplorePolicyOverlayReady => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                driver.phase = Phase::ExplorePolicyOverlay(snapshot.revision);
                widgets.arm(driver, explore::OVERLAY_NONE_ID)
            }
            Phase::ExplorePolicyOverlay(_) => widgets.arm(driver, explore::OVERLAY_NONE_ID),
            Phase::ExplorePolicyOverlayVisible(_) => {
                widgets.arm_revealed(driver, explore::DETAILS_SCROLL_ID, explore::OVERLAY_NONE_ID)
            }
            Phase::AwaitExplorePolicyOverlay(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.overlay.classselection.mode
                        != crate::generated::ExploreClassSelectionMode::None
                {
                    return Task::none();
                }
                driver.phase = Phase::AwaitExploreOverlayAll(snapshot.revision);
                Task::done(RootMessage::Workspace(
                    crate::view::router::Message::Explore(explore::Message::Details(
                        explore::details::Message::AllClasses,
                    )),
                ))
            }
            Phase::AwaitExploreOverlayAll(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.overlay.classselection.mode
                        != crate::generated::ExploreClassSelectionMode::All
                {
                    return Task::none();
                }
                driver.phase = Phase::AwaitExploreOverlaySubset(snapshot.revision);
                return Task::done(RootMessage::Workspace(
                    crate::view::router::Message::Explore(explore::Message::Details(
                        explore::details::Message::ClassToggled(0),
                    )),
                ));
            }
            Phase::AwaitExploreOverlaySubset(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.overlay.classselection.mode
                        != crate::generated::ExploreClassSelectionMode::Subset
                {
                    return Task::none();
                }
                driver.phase = Phase::AwaitExploreOverlayRestored(snapshot.revision);
                return Task::done(RootMessage::Workspace(
                    crate::view::router::Message::Explore(explore::Message::Details(
                        explore::details::Message::AllClasses,
                    )),
                ));
            }
            Phase::AwaitExploreOverlayRestored(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.overlay.classselection.mode
                        != crate::generated::ExploreClassSelectionMode::All
                {
                    return Task::none();
                }
                driver.phase = Phase::ExploreAugmentationToggle {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                };
                widgets.arm(driver, EXPLORE_AUGMENTATION_TOGGLE)
            }
            Phase::ExploreAugmentationToggle { .. } => {
                widgets.arm(driver, EXPLORE_AUGMENTATION_TOGGLE)
            }
            Phase::AwaitExploreAugmentationToggle {
                revision,
                frame_revision,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision || !snapshot.augmentation.enabled {
                    return Task::none();
                }
                if snapshot.augmentation.seed != 0 {
                    driver.fail("augmentation enable changed its seed or did not publish a new rendered frame");
                    return Task::none();
                }
                if snapshot.frame.revision <= frame_revision
                    || fully_drawn_gallery(frame, snapshot, probes.draws().gallery).is_none()
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_augmentation",
                        EXPLORE_AUGMENTATION_TOGGLE,
                        "enabled-rendered-seed-zero",
                        [
                            revision as f64,
                            snapshot.revision as f64,
                            frame_revision as f64,
                            snapshot.frame.revision as f64,
                        ],
                    )
                });
                driver.phase = Phase::ExploreAugmentationReroll {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                };
                widgets.arm(driver, EXPLORE_AUGMENTATION_REROLL)
            }
            Phase::ExploreAugmentationReroll { .. } => {
                widgets.arm(driver, EXPLORE_AUGMENTATION_REROLL)
            }
            Phase::AwaitExploreAugmentationReroll {
                revision,
                frame_revision,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision || snapshot.augmentation.seed != 1 {
                    return Task::none();
                }
                if snapshot.frame.revision <= frame_revision
                    || fully_drawn_gallery(frame, snapshot, probes.draws().gallery).is_none()
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_augmentation",
                        EXPLORE_AUGMENTATION_REROLL,
                        "rerolled-distinct-seed",
                        [
                            revision as f64,
                            snapshot.revision as f64,
                            frame_revision as f64,
                            snapshot.frame.revision as f64,
                        ],
                    )
                });
                driver.phase = Phase::ExploreReshuffle {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                    shuffle_seed: snapshot.order.shuffleseed,
                    augmentation_seed: snapshot.augmentation.seed,
                    order_signature: explore_order_signature(&snapshot.order.visibleindices),
                };
                widgets.arm(driver, EXPLORE_RESHUFFLE)
            }
            Phase::ExploreReshuffle { .. } => widgets.arm(driver, EXPLORE_RESHUFFLE),
            Phase::AwaitExploreReshuffle {
                revision,
                frame_revision,
                shuffle_seed,
                augmentation_seed,
                order_signature,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.order.shuffleseed <= shuffle_seed
                {
                    return Task::none();
                }
                if snapshot.augmentation.seed != augmentation_seed
                    || snapshot.frame.revision <= frame_revision
                {
                    driver.fail("Reshuffle changed augmentation state or did not publish the reordered gallery");
                    return Task::none();
                }
                let reshuffled_signature = explore_order_signature(&snapshot.order.visibleindices);
                if reshuffled_signature == order_signature {
                    driver.fail("Reshuffle did not change the visible gallery order");
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_reshuffle",
                        EXPLORE_RESHUFFLE,
                        "order-only",
                        [
                            shuffle_seed as f64,
                            snapshot.order.shuffleseed as f64,
                            augmentation_seed as f64,
                            snapshot.augmentation.seed as f64,
                        ],
                    )
                });
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_patch_baseline",
                        EXPLORE_CARD,
                        "reshuffle-placeholder",
                        [
                            snapshot.revision as f64,
                            snapshot.frame.revision as f64,
                            0.0,
                            0.0,
                        ],
                    )
                });
                driver.phase = Phase::ExploreCard {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                };
                Task::none()
            }
            Phase::ExploreCard { .. } => widgets.arm(driver, EXPLORE_CARD),
            Phase::AwaitGalleryPatch {
                revision,
                frame_revision,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision < revision
                    || snapshot.frame.revision < frame_revision
                    || snapshot.busy
                    || sampleable_presentation(
                        frame,
                        crate::generated::PresentationSourceKind::Explore,
                        snapshot.frame.revision,
                    )
                    .is_none()
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_patch_observed",
                        EXPLORE_CARD,
                        "post-placeholder-frame",
                        [
                            revision as f64,
                            snapshot.revision as f64,
                            frame_revision as f64,
                            snapshot.frame.revision as f64,
                        ],
                    )
                });
                self.sweep_baseline = Some((snapshot.revision, snapshot.viewport.clone()));
                if driver.viewer_scenario.is_empty() {
                    driver.phase = Phase::GalleryColdRead(
                        snapshot
                            .viewport
                            .firstrow
                            .saturating_add(snapshot.viewport.rowcount)
                            .saturating_add(COLD_GALLERY_ROW_GAP),
                    );
                    Task::none()
                } else {
                    driver.phase = Phase::GallerySweep;
                    widgets.arm(driver, EXPLORE_GALLERY)
                }
            }
            Phase::GalleryColdRead(row) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if model.has_explore_pending() || !model.explore_viewport_available() {
                    return Task::none();
                }
                // Leave space for the existing eight 96px wheels, the Later
                // row and the complete viewport. The five-row separation is
                // beyond the native four-neighbour prefetch window.
                let Some(offset) = cold_gallery_scroll_offset(
                    row,
                    &snapshot.viewport,
                    snapshot.order.matchingcount,
                    self.atlas_row_extent,
                ) else {
                    driver.fail("Explore fixture has no cold interior viewport with sweep room");
                    return Task::none();
                };
                // A disjoint jump hides the incumbent atlas. First draw its
                // completed publication so the retained fallback can advance
                // and release the previous physical slot.
                if fully_drawn_gallery(frame, snapshot, probes.draws().gallery).is_none() {
                    return Task::none();
                }
                driver.phase = Phase::AwaitGalleryColdRead(row, snapshot.gallery.generation);
                iced::widget::operation::scroll_to(
                    EXPLORE_GALLERY,
                    AbsoluteOffset { x: 0.0, y: offset },
                )
            }
            Phase::AwaitGalleryColdRead(row, prior_generation) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.mode != crate::generated::ExploreMode::Gallery
                    || snapshot.viewport.firstrow != row
                    || snapshot.gallery.generation <= prior_generation
                    || snapshot.gallery.layout.firstrow != row
                    || snapshot.gallery.layout.columns != snapshot.viewport.columns
                    || snapshot.gallery.layout.rowcount != snapshot.viewport.rowcount
                    || model.has_explore_pending()
                    || !model.explore_viewport_available()
                {
                    return Task::none();
                }
                if let Some((generation, index)) = self.gallery_completion_held {
                    if snapshot.gallery.generation < generation {
                        return Task::none();
                    }
                    if snapshot.gallery.generation != generation
                        || !snapshot.order.visibleindices.contains(&index)
                    {
                        driver.fail(
                            "held gallery completion does not match the accepted cold viewport",
                        );
                        return Task::none();
                    }
                    self.sweep_baseline = Some((snapshot.revision, snapshot.viewport.clone()));
                    driver.phase = Phase::GallerySweep;
                    return widgets.arm(driver, EXPLORE_GALLERY);
                }
                // Reshuffle retains pixel identities. A previously unseen
                // order range can therefore already be cached. Only a fully
                // ready accepted range permits moving to the next disjoint
                // range; unfinished reads wait for their actual completion.
                if !snapshot.gallery.slots.is_empty()
                    && snapshot.gallery.slots.len() == snapshot.order.visibleindices.len()
                    && snapshot.gallery.slots.iter().all(|ready| *ready)
                {
                    driver.phase = Phase::GalleryColdRead(
                        row.saturating_add(snapshot.viewport.rowcount)
                            .saturating_add(COLD_GALLERY_ROW_GAP),
                    );
                }
                Task::none()
            }
            Phase::GallerySweep => widgets.arm(driver, EXPLORE_GALLERY),
            Phase::AwaitGallerySweep => {
                let Some((revision, viewport)) = &self.sweep_baseline else {
                    driver.fail("Explore sweep baseline is unavailable");
                    return Task::none();
                };
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || snapshot.revision <= *revision
                    || &snapshot.viewport == viewport
                    || settings.has_local_edits()
                    || !model.explore_viewport_available()
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_sweep_observed",
                        EXPLORE_GALLERY,
                        "typed-viewport",
                        [
                            *revision as f64,
                            snapshot.revision as f64,
                            snapshot.viewport.extent.width as f64,
                            snapshot.viewport.extent.height as f64,
                        ],
                    )
                });
                driver.phase = Phase::GalleryLaterReady;
                Task::none()
            }
            Phase::GalleryLaterReady => {
                if settings.has_local_edits() || !model.explore_viewport_available() {
                    return Task::none();
                }
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                driver
                    .reporting
                    .observe(|reporting| reporting.reset_scroll());
                driver.phase = Phase::GalleryLater(snapshot.viewport.firstrow);
                widgets.arm(driver, EXPLORE_LATER)
            }
            Phase::GalleryLater(_) => widgets.arm(driver, EXPLORE_LATER),
            Phase::AwaitGalleryScroll(baseline) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.viewport.firstrow <= baseline
                    || snapshot.busy
                    || settings.has_local_edits()
                    || !model.explore_mutation_available()
                {
                    return Task::none();
                }
                driver
                    .reporting
                    .observe(|reporting| reporting.scroll_placeholder(snapshot));
                if sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                )
                .is_none()
                    || probes
                        .draws()
                        .gallery
                        .is_none_or(|(_, source)| source != snapshot.frame.revision)
                {
                    return Task::none();
                }
                let slot = (snapshot.viewport.rowcount / 2)
                    .saturating_mul(snapshot.viewport.columns)
                    .saturating_add(snapshot.viewport.columns / 2)
                    as usize;
                let Some(index) = snapshot.order.visibleindices.get(slot).copied() else {
                    driver.fail("Explore gallery has no visible image");
                    return Task::none();
                };
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    slot as u32,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_scrolled",
                        EXPLORE_GALLERY,
                        "",
                        [
                            baseline as f64,
                            snapshot.viewport.firstrow as f64,
                            index as f64,
                            0.0,
                        ],
                    )
                });
                driver.phase = Phase::GalleryImage(index);
                widgets.arm(driver, EXPLORE_GALLERY)
            }
            Phase::GalleryImage(_) => widgets.arm(driver, EXPLORE_GALLERY),
            Phase::AwaitDetail(expected) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.mode != crate::generated::ExploreMode::Detail
                    || snapshot.busy
                    || settings.has_local_edits()
                    || !model.explore_mutation_available()
                    || displayed_detail(surface, snapshot).is_none()
                {
                    return Task::none();
                }
                let Some(selected) = snapshot.selectedimage else {
                    return Task::none();
                };
                if selected != expected {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_pointer_mismatch",
                            EXPLORE_GALLERY,
                            "selected-image",
                            [
                                expected as f64,
                                selected as f64,
                                snapshot.revision as f64,
                                snapshot.frame.revision as f64,
                            ],
                        )
                    });
                    driver.fail("gallery pointer selection did not match the rendered grid slot");
                    return Task::none();
                }
                if let Some((_, _, slot, revision, _)) = self.selection_grid {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_pointer_selected",
                            EXPLORE_GALLERY,
                            "selected-from-dispatched-pointer",
                            [
                                revision as f64,
                                slot as f64,
                                f64::from(expected),
                                f64::from(selected),
                            ],
                        )
                    });
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_detail",
                        "",
                        "",
                        [selected as f64, snapshot.frame.revision as f64, 0.0, 0.0],
                    )
                });
                if snapshot.detail.showoriginaldimensions
                    || snapshot.frame.extent.width != snapshot.dataset.imagewidth
                    || snapshot.frame.extent.height != snapshot.dataset.imageheight
                {
                    driver.fail("initial Explore detail was not the native padded product");
                    return Task::none();
                }
                if sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                )
                .is_none()
                {
                    return Task::none();
                }
                if driver.viewer_scenario == "quiet" {
                    if crate::presentation_surface::viewer_annotation_request().is_none() {
                        return Task::none();
                    }
                    driver.phase = Phase::OpenAnnotation;
                    return widgets.arm(driver, EXPLORE_ANNOTATE);
                }
                driver.phase = Phase::DetailOriginal {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                    padded_width: snapshot.frame.extent.width,
                    padded_height: snapshot.frame.extent.height,
                };
                widgets.arm(driver, EXPLORE_DETAIL_ORIGINAL)
            }
            Phase::DetailOriginal { .. } => widgets.arm(driver, EXPLORE_DETAIL_ORIGINAL),
            Phase::AwaitDetailOriginal {
                revision,
                frame_revision,
                padded_width,
                padded_height,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || !snapshot.detail.showoriginaldimensions
                {
                    return Task::none();
                }
                if snapshot.frame.revision != frame_revision
                    || snapshot.frame.extent.width != padded_width
                    || snapshot.frame.extent.height != padded_height
                {
                    driver.fail("original crop changed the native product");
                    return Task::none();
                }
                // CLEANUP-IGNORE: Original-detail and dataset-reopen evidence call the shared presentation join for distinct completion contracts.
                if sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                )
                .is_none()
                {
                    return Task::none();
                }
                let Some((_, drawn_revision, drawn)) = probes.draws().viewer else {
                    return Task::none();
                };
                let content = &snapshot.frame.content;
                let Some(viewed) = model.viewed_explore_frame() else {
                    return Task::none();
                };
                let viewed_content = &viewed.content;
                if drawn_revision != viewed.revision
                    || drawn.crop
                        != [
                            viewed_content.x,
                            viewed_content.y,
                            viewed_content.width,
                            viewed_content.height,
                        ]
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_detail_source",
                        EXPLORE_DETAIL_ORIGINAL,
                        "padded-to-original-sampling",
                        [
                            padded_width as f64,
                            padded_height as f64,
                            content.width as f64,
                            content.height as f64,
                        ],
                    )
                });
                driver.phase = Phase::DetailFit;
                widgets.arm(driver, explore::DETAIL_FIT_ID)
            }
            Phase::DetailFit => widgets.arm(driver, explore::DETAIL_FIT_ID),
            Phase::ViewerSelect
            | Phase::AtlasReturnSelect
            | Phase::AtlasResizeSelect(_)
            | Phase::AtlasAwaySelect(_) => widgets.arm(driver, EXPLORE_GALLERY),
            Phase::AwaitAtlasCapacity
                if !settings.has_local_edits()
                    && model.settings_edit_available()
                    && model
                        .explore
                        .snapshot
                        .as_ref()
                        .and_then(|snapshot| snapshot.viewportresult.as_ref())
                        .is_some_and(|result| {
                            result.outcome
                                == crate::generated::ExploreViewportOutcome::VisibleCapacityExceeded
                        }) =>
            {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.atlas_capacity",
                        explore::GALLERY_CAPACITY_ID,
                        "native-visible-capacity-exceeded",
                        [1.0, 0.0, 0.0, 0.0],
                    )
                });
                driver.phase = Phase::AtlasCapacity;
                widgets.arm(driver, explore::GALLERY_CAPACITY_ID)
            }
            Phase::AtlasCapacity => widgets.arm(driver, explore::GALLERY_CAPACITY_ID),
            Phase::AtlasRestoreColumns => {
                driver.phase = Phase::AwaitAtlasColumns;
                explore_message(explore::Message::Gallery(
                    explore::gallery::Message::ColumnsChanged(self.atlas_columns as i32),
                ))
            }
            Phase::AwaitAtlasColumns
                if !settings.has_local_edits()
                    && model.explore_mutation_available()
                    && settings.draft.as_ref().is_some_and(|draft| {
                        draft.workflows.explore.gridwidth == self.atlas_columns as i32
                    }) =>
            {
                driver.phase = Phase::AwaitAtlasEmpty;
                explore_message(explore::Message::Dataset(
                    explore::dataset::Message::NoClasses,
                ))
            }
            Phase::AwaitAtlasEmpty
                if model.explore.snapshot.as_ref().is_some_and(|value| {
                    value.ready && !value.busy && value.order.matchingcount == 0
                }) && matches!(
                    crate::presentation_surface::explore_display(surface),
                    Some(crate::presentation_surface::ExploreDisplay::Gallery(shown, metadata))
                        if metadata.order.matchingcount == 0
                            && shown.frame.is_some_and(|frame| {
                                probes.draws().gallery
                                    == Some((frame.presentation_revision, metadata.frame.revision))
                            })
                ) =>
            {
                driver.phase = Phase::AtlasEmpty;
                widgets.arm(driver, explore::GALLERY_EMPTY_ID)
            }
            Phase::AtlasEmpty => widgets.arm(driver, explore::GALLERY_EMPTY_ID),
            Phase::AtlasRestoreFilter => {
                driver.phase = Phase::AwaitAtlasRestored;
                explore_message(explore::Message::Dataset(
                    explore::dataset::Message::AllClasses,
                ))
            }
            Phase::AwaitAtlasRestored => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if !snapshot.ready
                    || snapshot.busy
                    || snapshot.order.matchingcount == 0
                    || probes
                        .draws()
                        .gallery
                        .is_none_or(|(_, source)| source != snapshot.frame.revision)
                {
                    return Task::none();
                }
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    0,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                driver.phase = Phase::ViewerSelect;
                widgets.arm(driver, EXPLORE_GALLERY)
            }
            Phase::AwaitAtlasWindow(fullscreen) => {
                #[cfg(target_arch = "wasm32")]
                let settled = fullscreen_settled_js(fullscreen);
                #[cfg(not(target_arch = "wasm32"))]
                let settled = false;
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let gallery_matches = probes
                    .draws()
                    .gallery
                    .is_some_and(|(_, source)| source == snapshot.frame.revision);
                let viewport_matches = router
                    .explore_measured_viewport(
                        snapshot.viewport.columns,
                        snapshot.viewport.firstrow,
                        snapshot.order.matchingcount,
                    )
                    .as_ref()
                    == Some(&snapshot.viewport);
                if !settled
                    || snapshot.busy
                    || model.has_explore_pending()
                    || !gallery_matches
                    || !viewport_matches
                {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.atlas_window_wait",
                            EXPLORE_GALLERY,
                            if fullscreen { "fullscreen" } else { "restored" },
                            [
                                settled as u8 as f64,
                                (snapshot.busy || model.has_explore_pending()) as u8 as f64,
                                gallery_matches as u8 as f64,
                                viewport_matches as u8 as f64,
                            ],
                        )
                    });
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.atlas_window_draw",
                        EXPLORE_GALLERY,
                        if fullscreen { "fullscreen" } else { "restored" },
                        [
                            snapshot.frame.revision as f64,
                            model.window_width as f64,
                            model.window_height as f64,
                            snapshot.viewport.columns as f64,
                        ],
                    )
                });
                if fullscreen {
                    driver.phase = Phase::AwaitAtlasWindow(false);
                    #[cfg(target_arch = "wasm32")]
                    fullscreen_js(false);
                    Task::none()
                } else {
                    self.atlas_columns = snapshot.viewport.columns;
                    driver.phase = Phase::AwaitAtlasCapacity;
                    let maximum = crate::generated::constraint_workflowsexploregridwidth()
                        .maximum
                        .unwrap_or(self.atlas_columns as f64)
                        as i32;
                    explore_message(explore::Message::Gallery(
                        explore::gallery::Message::ColumnsChanged(maximum),
                    ))
                }
            }
            Phase::VisibleReadScroll(index) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let row = index / snapshot.viewport.columns.max(1);
                driver.phase = Phase::AwaitVisibleRead(index);
                iced::widget::operation::scroll_to(
                    EXPLORE_GALLERY,
                    AbsoluteOffset {
                        x: 0.0,
                        y: self.atlas_row_extent * (row as f32 + 0.5),
                    },
                )
            }
            Phase::AwaitVisibleReadHover(_, _) => Task::none(),
            Phase::AwaitVisibleReadPixels(index, generation)
            | Phase::VisibleReadSelect(index, generation) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let Some(draw) = self.confirmed_atlas(snapshot) else {
                    return Task::none();
                };
                let Some(slot) = draw.visible_slot(index) else {
                    driver.fail("held read is not an immediate visible image");
                    return Task::none();
                };
                if draw.snapshot.gallery.slots.get(slot) != Some(&false) {
                    driver.fail("held read lost its explicit placeholder");
                    return Task::none();
                }
                let columns = draw.snapshot.gallery.layout.columns;
                let scale = draw.image.width / (self.atlas_row_extent * columns as f32);
                let side = self.atlas_row_extent;
                let target = Rectangle {
                    x: draw.image.x / scale + (slot as u32 % columns) as f32 * side,
                    y: draw.image.y / scale + (slot as u32 / columns) as f32 * side,
                    width: side,
                    height: side,
                };
                let visible_clip = Rectangle {
                    x: draw.clip.x / scale,
                    y: draw.clip.y / scale,
                    width: draw.clip.width / scale,
                    height: draw.clip.height / scale,
                };
                let Some(target) = target.intersection(&visible_clip) else {
                    driver.fail("held read target does not intersect the actual gallery clip");
                    return Task::none();
                };
                if matches!(driver.phase, Phase::AwaitVisibleReadPixels(_, _)) {
                    crate::presentation_surface::trace_atlas_stage("held-visible", draw);
                    driver.phase = Phase::AwaitVisibleReadHover(index, generation);
                    #[cfg(target_arch = "wasm32")]
                    if hover_after_surface_draw_js(
                        f64::from(target.center_x()),
                        f64::from(target.center_y()),
                        EXPLORE_GALLERY,
                        snapshot.frame.revision as f64,
                    ) != 1
                    {
                        driver.fail("held-placeholder hover dispatch failed");
                    }
                } else {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.pending_hover",
                            EXPLORE_GALLERY,
                            "shared-mouse-placeholder-target",
                            [
                                index as f64,
                                generation as f64,
                                snapshot.frame.revision as f64,
                                draw.surface.frame.unwrap().presentation_revision as f64,
                            ],
                        )
                    });
                    driver.phase = Phase::AwaitVisibleReadSelection(index, generation);
                    if !click_after_surface_draw(
                        target,
                        EXPLORE_GALLERY,
                        snapshot.frame.revision,
                        false,
                    ) {
                        driver.fail("held-placeholder selection dispatch failed");
                    }
                }
                Task::none()
            }
            Phase::AwaitVisibleReadSelection(index, generation) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if !self.detail_drawn(probes, frame, snapshot)
                    || snapshot.selectedimage != Some(index)
                    || snapshot.busy
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.pending_selection",
                        EXPLORE_GALLERY,
                        "typed-image-before-thumbnail-read",
                        [
                            index as f64,
                            generation as f64,
                            snapshot.frame.revision as f64,
                            0.0,
                        ],
                    )
                });
                driver.phase = Phase::AwaitVisibleReadReturn(index, generation);
                explore_message(explore::Message::Detail(
                    explore::detail::Message::CloseRequested,
                ))
            }
            Phase::AwaitVisibleReadReturn(index, generation) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.mode != crate::generated::ExploreMode::Gallery {
                    return Task::none();
                }
                let Some(draw) = self.confirmed_atlas(snapshot) else {
                    return Task::none();
                };
                let Some(slot) = draw.visible_slot(index) else {
                    return Task::none();
                };
                if draw.snapshot.gallery.slots.get(slot) != Some(&false) {
                    driver.fail("pending gallery return completed before held read release");
                    return Task::none();
                }
                crate::presentation_surface::trace_atlas_stage("held-return", draw);
                driver.phase = Phase::AwaitVisibleReadOscillation(index, generation, 0);
                let row = index / snapshot.viewport.columns.max(1);
                iced::widget::operation::scroll_to(
                    EXPLORE_GALLERY,
                    AbsoluteOffset {
                        x: 0.0,
                        y: self.atlas_row_extent * row as f32,
                    },
                )
            }
            Phase::AwaitVisibleReadOscillation(index, generation, stage) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let row = index / snapshot.viewport.columns.max(1);
                if snapshot.mode != crate::generated::ExploreMode::Gallery
                    || snapshot.viewport.firstrow != row
                    || snapshot.viewport.rowcount != self.atlas_return_rows + u32::from(stage == 1)
                {
                    return Task::none();
                }
                let Some(draw) = self
                    .confirmed_atlas(snapshot)
                    .filter(|draw| draw.snapshot.viewport == snapshot.viewport)
                else {
                    return Task::none();
                };
                let Some(slot) = draw.visible_slot(index) else {
                    return Task::none();
                };
                if draw.snapshot.gallery.slots.get(slot) != Some(&false) {
                    driver.fail("held read escaped before pending-return oscillation completed");
                    return Task::none();
                }
                crate::presentation_surface::trace_atlas_stage(
                    ["held-aligned", "held-extra", "held-restored"][stage as usize],
                    draw,
                );
                if stage == 2 {
                    return driver.advance_to(Phase::VisibleReadRelease(index, generation));
                }
                driver.phase = Phase::AwaitVisibleReadOscillation(index, generation, stage + 1);
                iced::widget::operation::scroll_to(
                    EXPLORE_GALLERY,
                    AbsoluteOffset {
                        x: 0.0,
                        y: self.atlas_row_extent
                            * (row as f32
                                + if stage == 1 {
                                    0.0
                                } else {
                                    self.atlas_scroll_fraction
                                }),
                    },
                )
            }
            Phase::VisibleReadRelease(index, _) => {
                if driver.control_phase.as_ref() == Some(&driver.phase) {
                    driver.phase = Phase::AwaitVisibleReadComplete(index);
                }
                Task::none()
            }
            Phase::AwaitVisibleReadComplete(index) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy {
                    return Task::none();
                }
                let Some(draw) = self.confirmed_atlas(snapshot) else {
                    return Task::none();
                };
                let Some(slot) = draw.visible_slot(index) else {
                    return Task::none();
                };
                if draw.snapshot.gallery.slots.get(slot) != Some(&true) {
                    return Task::none();
                }
                crate::presentation_surface::trace_atlas_stage("held-complete", draw);
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    0,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                driver.phase = Phase::AtlasAwaySelect(0);
                widgets.arm(driver, EXPLORE_GALLERY)
            }
            Phase::AwaitAtlasAwayFilter(step, baseline) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.mode != crate::generated::ExploreMode::Gallery
                    || snapshot.busy
                    || settings.has_local_edits()
                    || model.has_explore_pending()
                    || snapshot.revision <= baseline
                    || self.confirmed_atlas(snapshot).is_none()
                {
                    return Task::none();
                }
                // Filter/order updates deliberately return to Gallery. Observe
                // that completed product before reopening detail for the next
                // change; the acceptance driver follows the ordinary API.
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    0,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                driver.phase = Phase::AtlasAwaySelect(step);
                widgets.arm(driver, EXPLORE_GALLERY)
            }
            Phase::AwaitAtlasAwayDetail(step, baseline) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if !self.detail_drawn(probes, frame, snapshot)
                    || snapshot.busy
                    || settings.has_local_edits()
                    || model.has_explore_pending()
                    || snapshot.revision <= baseline
                {
                    return Task::none();
                }
                driver.phase = if step < 3 {
                    Phase::AwaitAtlasAwayFilter(step + 1, snapshot.revision)
                } else {
                    Phase::AwaitAtlasAwayDetail(step + 1, snapshot.revision)
                };
                match step {
                    0 => explore_message(explore::Message::Dataset(
                        explore::dataset::Message::MinimumCompiledIndexChanged(1),
                    )),
                    1 => explore_message(explore::Message::Dataset(
                        explore::dataset::Message::OrderSelected(
                            crate::generated::ExploreOrder::Shuffled,
                        ),
                    )),
                    2 => explore_message(explore::Message::Dataset(
                        explore::dataset::Message::ShuffleSeedChanged(173),
                    )),
                    3 => explore_message(explore::Message::Gallery(
                        explore::gallery::Message::Overlay(
                            explore::overlay::Message::BoxesToggled(!snapshot.overlay.showboxes),
                        ),
                    )),
                    _ => {
                        driver.phase = Phase::AwaitAtlasAwayReturn;
                        explore_message(explore::Message::Detail(
                            explore::detail::Message::CloseRequested,
                        ))
                    }
                }
            }
            Phase::AwaitAtlasAwayReturn | Phase::AwaitAtlasAwayRestore => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.mode != crate::generated::ExploreMode::Gallery
                    || snapshot.busy
                    || settings.has_local_edits()
                    || model.has_explore_pending()
                {
                    return Task::none();
                }
                let Some(draw) = self.confirmed_atlas(snapshot) else {
                    return Task::none();
                };
                if matches!(driver.phase, Phase::AwaitAtlasAwayReturn) {
                    if snapshot.filter.minimumcompiledindex != 1
                        || snapshot.filter.order != crate::generated::ExploreOrder::Shuffled
                        || snapshot.order.shuffleseed != 173
                    {
                        driver.fail("gallery return lost filter, order, or seed changed in detail");
                        return Task::none();
                    }
                    crate::presentation_surface::trace_atlas_stage("away-return", draw);
                    driver.phase = Phase::AwaitAtlasAwayRestore;
                }
                if let Some(message) = explore_scenario_preparation(snapshot) {
                    return explore_message(message);
                }
                self.scroll_atlas(driver, 0)
            }
            Phase::AwaitCapacitySlots(baseline) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || model.has_explore_pending()
                    || snapshot.frame.revision == baseline
                    || crate::presentation_surface::capacity_acceptance_slots() != 2
                    || probes
                        .draws()
                        .gallery
                        .is_none_or(|(_, source)| source != snapshot.frame.revision)
                {
                    return Task::none();
                }
                driver.phase = Phase::AwaitCapacityArm;
                Task::none()
            }
            Phase::CapacityPublish => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if !crate::presentation_surface::release_capacity_sample() {
                    driver.fail("capacity acquisition lacks its held physical sample");
                    return Task::none();
                }
                driver.phase = Phase::AwaitCapacityCompletion;
                explore_message(explore::Message::Gallery(
                    explore::gallery::Message::Overlay(explore::overlay::Message::BoxesToggled(
                        !snapshot.overlay.showboxes,
                    )),
                ))
            }
            Phase::AwaitCapacityRetry => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy || model.has_explore_pending() {
                    return Task::none();
                }
                let Some(draw) = self.confirmed_atlas(snapshot) else {
                    return Task::none();
                };
                let ready = draw.surface.frame.expect("drawn capacity retry");
                reporting::emit(|sink| {
                    sink.record(
                        "integration.capacity_retry",
                        EXPLORE_GALLERY,
                        "completed-acquisition-after-physical-slot-release",
                        [
                            ready.content_sequence as f64,
                            ready.presentation_revision as f64,
                            snapshot.revision as f64,
                            0.0,
                        ],
                    )
                });
                crate::presentation_surface::end_capacity_acceptance();
                driver.phase = Phase::AwaitAtlasWindow(true);
                #[cfg(target_arch = "wasm32")]
                fullscreen_js(true);
                Task::none()
            }
            Phase::AtlasResizeStart => {
                self.resize_original_size = router.explore_gallery_size();
                self.resize_previous_size = self.resize_original_size;
                driver.phase = Phase::AwaitAtlasResizeGallery(0);
                #[cfg(target_arch = "wasm32")]
                if !canvas_size_js(1500.0, 600.0) {
                    driver.fail("cannot size the acceptance canvas");
                }
                Task::none()
            }
            Phase::AwaitAtlasResizeGallery(step) => {
                let (Some(snapshot), Some(size)) = (
                    model.explore.snapshot.as_ref(),
                    router.explore_gallery_size(),
                ) else {
                    return Task::none();
                };
                if snapshot.mode != crate::generated::ExploreMode::Gallery
                    || snapshot.busy
                    || model.has_explore_pending()
                    || (step == 3 && Some(size) != self.resize_original_size)
                {
                    return Task::none();
                }
                #[cfg(target_arch = "wasm32")]
                if step < 3 {
                    let (width, height) = if step == 1 {
                        (1000.0, 1020.0)
                    } else {
                        (1500.0, 600.0)
                    };
                    if !canvas_size_settled_js(width, height)
                        || model.window_width != width as u32
                        || model.window_height != height as u32
                    {
                        reporting::emit(|sink| {
                            sink.record(
                                "integration.atlas_resize_wait",
                                EXPLORE_GALLERY,
                                "window",
                                [
                                    model.window_width as f64,
                                    model.window_height as f64,
                                    width,
                                    height,
                                ],
                            )
                        });
                        return Task::none();
                    }
                }
                let Some(expected) = router.explore_measured_layout_request(
                    Some(snapshot),
                    snapshot.viewport.columns,
                    snapshot.order.matchingcount,
                ) else {
                    return Task::none();
                };
                if snapshot.viewport != expected.viewport {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.atlas_resize_wait",
                            EXPLORE_GALLERY,
                            "viewport",
                            [
                                snapshot.viewport.extent.width as f64,
                                snapshot.viewport.extent.height as f64,
                                expected.viewport.extent.width as f64,
                                expected.viewport.extent.height as f64,
                            ],
                        )
                    });
                    return Task::none();
                }
                let Some(draw) = self
                    .confirmed_atlas(snapshot)
                    .filter(|draw| draw.snapshot.viewport == snapshot.viewport)
                else {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.atlas_resize_wait",
                            EXPLORE_GALLERY,
                            "pixels",
                            [
                                snapshot.frame.revision as f64,
                                self.atlas_receipt
                                    .as_ref()
                                    .map_or(0, |draw| draw.snapshot.frame.revision)
                                    as f64,
                                self.atlas_pixels
                                    .as_ref()
                                    .map_or(0, |draw| draw.snapshot.frame.revision)
                                    as f64,
                                snapshot.viewport.rowcount as f64,
                            ],
                        )
                    });
                    return Task::none();
                };
                let required_rows = atlas_scroll_window(size, snapshot.viewport.columns).0;
                if snapshot.viewport.rowcount < required_rows
                    || snapshot.gallery.slots.iter().any(|ready| !*ready)
                {
                    return Task::none();
                }
                if step < 3 {
                    crate::presentation_surface::trace_atlas_stage(
                        [
                            "resize-landscape",
                            "resize-portrait-return",
                            "resize-landscape-return",
                        ][step as usize],
                        draw,
                    );
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.atlas_resize",
                            EXPLORE_GALLERY,
                            ["landscape", "portrait-return", "landscape-return"][step as usize],
                            [
                                f64::from(size.width),
                                f64::from(size.height),
                                f64::from(required_rows),
                                f64::from(snapshot.viewport.rowcount),
                            ],
                        )
                    });
                }
                if step == 2 {
                    driver.phase = Phase::AwaitAtlasResizeGallery(3);
                    #[cfg(target_arch = "wasm32")]
                    restore_canvas_size_js();
                    return Task::none();
                }
                if step == 3 {
                    self.resize_original_size = None;
                    self.resize_previous_size = None;
                    return driver.advance_to(Phase::AwaitAtlasReturnReady);
                }
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    snapshot.viewport.firstrow,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                self.resize_previous_size = Some(size);
                driver.phase = Phase::AtlasResizeSelect(step);
                widgets.arm(driver, EXPLORE_GALLERY)
            }
            Phase::AwaitAtlasResizeDetail(step) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy || !self.detail_drawn(probes, frame, snapshot) {
                    return Task::none();
                }
                driver.phase = Phase::AwaitAtlasResizeMeasurement(step * 2);
                #[cfg(target_arch = "wasm32")]
                {
                    let (width, height) = atlas_resize_dimensions(step * 2);
                    if !canvas_size_js(width, height) {
                        driver.fail("cannot resize canvas beneath Detail");
                    }
                }
                Task::none()
            }
            Phase::AwaitAtlasResizeMeasurement(step) => {
                let Some(size) = router.explore_gallery_size() else {
                    return Task::none();
                };
                if Some(size) == self.resize_previous_size {
                    return Task::none();
                }
                #[cfg(target_arch = "wasm32")]
                {
                    let (width, height) = atlas_resize_dimensions(step);
                    if !canvas_size_settled_js(width, height)
                        || model.window_width != width as u32
                        || model.window_height != height as u32
                    {
                        return Task::none();
                    }
                }
                self.resize_previous_size = Some(size);
                reporting::emit(|sink| {
                    sink.record(
                        "integration.atlas_resize_measured",
                        EXPLORE_GALLERY,
                        "detail-layout",
                        [
                            f64::from(step),
                            f64::from(size.width),
                            f64::from(size.height),
                            1.0,
                        ],
                    )
                });
                if step % 2 == 0 {
                    driver.phase = Phase::AwaitAtlasResizeMeasurement(step + 1);
                    #[cfg(target_arch = "wasm32")]
                    {
                        let (width, height) = atlas_resize_dimensions(step + 1);
                        if !canvas_size_js(width, height) {
                            driver.fail("cannot repeat canvas resize beneath Detail");
                        }
                    }
                    Task::none()
                } else {
                    driver.phase = Phase::AwaitAtlasResizeGallery(step / 2 + 1);
                    explore_message(explore::Message::Detail(
                        explore::detail::Message::CloseRequested,
                    ))
                }
            }
            Phase::AwaitAtlasReturnReady => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let Some(size) = router.explore_gallery_size() else {
                    return Task::none();
                };
                let (rows, fraction) = atlas_scroll_window(size, snapshot.viewport.columns);
                if snapshot.busy
                    || model.has_explore_pending()
                    || snapshot.viewport.rowcount != rows
                    || snapshot.viewport.firstrow != 0
                    || snapshot.gallery.slots.iter().any(|ready| !*ready)
                {
                    return Task::none();
                }
                let Some(draw) = self
                    .confirmed_atlas(snapshot)
                    .filter(|draw| draw.snapshot.viewport == snapshot.viewport)
                else {
                    return Task::none();
                };
                crate::presentation_surface::trace_atlas_stage("return-cached", draw);
                self.atlas_return_rows = rows;
                self.atlas_scroll_fraction = fraction;
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    rows,
                    0,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                driver.phase = Phase::AtlasReturnSelect;
                widgets.arm(driver, EXPLORE_GALLERY)
            }
            Phase::AwaitAtlasReturnDetail => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy || !self.detail_drawn(probes, frame, snapshot) {
                    return Task::none();
                }
                driver.phase = Phase::AwaitAtlasOscillation(0);
                explore_message(explore::Message::Detail(
                    explore::detail::Message::CloseRequested,
                ))
            }
            Phase::AwaitAtlasOscillation(stage) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let expected = self.atlas_return_rows + u32::from(stage == 1);
                if snapshot.mode != crate::generated::ExploreMode::Gallery
                    || snapshot.viewport.rowcount != expected
                    || snapshot.viewport.firstrow != 0
                    || model.has_explore_pending()
                {
                    return Task::none();
                }
                let Some(draw) = self
                    .confirmed_atlas(snapshot)
                    .filter(|draw| draw.snapshot.viewport == snapshot.viewport)
                else {
                    return Task::none();
                };
                crate::presentation_surface::trace_atlas_stage(
                    ["return-aligned", "return-extra", "return-restored"][stage as usize],
                    draw,
                );
                if stage < 2 {
                    driver.phase = Phase::AwaitAtlasOscillation(stage + 1);
                    iced::widget::operation::scroll_to(
                        EXPLORE_GALLERY,
                        AbsoluteOffset {
                            x: 0.0,
                            y: if stage == 0 {
                                self.atlas_row_extent * self.atlas_scroll_fraction
                            } else {
                                0.0
                            },
                        },
                    )
                } else {
                    if driver.session.profile == "retained" && driver.viewer_scenario == "rapid" {
                        driver.phase = Phase::AwaitVisibleReadArm(snapshot.viewport.columns * 10);
                        return Task::none();
                    }
                    self.scroll_atlas(driver, 0)
                }
            }
            Phase::AwaitAtlasScroll(stage) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || model.has_explore_pending()
                    || self
                        .atlas_drawn
                        .is_none_or(|(source, _)| source != snapshot.frame.revision)
                {
                    return Task::none();
                }
                let Some(receipt) = self.atlas_receipt.as_ref().filter(|draw| {
                    draw.snapshot.frame == snapshot.frame
                        && draw.snapshot.viewport == snapshot.viewport
                }) else {
                    return Task::none();
                };
                let total_rows = snapshot
                    .order
                    .matchingcount
                    .div_ceil(snapshot.viewport.columns.max(1));
                let settled = match stage {
                    0..=4 => {
                        let fractional = matches!(stage, 0 | 2);
                        snapshot.viewport.firstrow == [0, 1, 2, 10, 9][stage as usize]
                            && snapshot.viewport.rowcount
                                == self.atlas_return_rows + u32::from(fractional)
                            && if fractional {
                                self.atlas_clip.0 > 0.0
                            } else {
                                self.atlas_clip.0.abs() < 1.0
                            }
                    }
                    5 => {
                        snapshot.viewport.firstrow + snapshot.viewport.rowcount == total_rows
                            && self.atlas_clip.1.abs() < 1.0
                    }
                    _ => snapshot.viewport.firstrow == 0 && self.atlas_clip.0.abs() < 1.0,
                };
                if !settled {
                    return Task::none();
                }
                crate::presentation_surface::trace_atlas_stage(
                    [
                        "fractional",
                        "row1",
                        "row2",
                        "row10",
                        "row9",
                        "end",
                        "restored",
                    ][stage as usize],
                    receipt,
                );
                reporting::emit(|sink| {
                    sink.record(
                        "integration.atlas_scroll",
                        EXPLORE_GALLERY,
                        [
                            "fractional",
                            "row1",
                            "row2",
                            "row10",
                            "row9",
                            "end",
                            "restored",
                        ][stage as usize],
                        [
                            snapshot.frame.revision as f64,
                            snapshot.viewport.firstrow as f64,
                            self.atlas_clip.0 as f64,
                            self.atlas_clip.1 as f64,
                        ],
                    )
                });
                match stage {
                    0..=5 => self.scroll_atlas(driver, stage + 1),
                    _ => {
                        if driver.session.profile == "retained" && driver.viewer_scenario == "rapid"
                        {
                            if !crate::presentation_surface::begin_capacity_acceptance() {
                                driver.fail("capacity acceptance lacks retained fallback");
                                return Task::none();
                            }
                            driver.phase = Phase::AwaitCapacitySlots(snapshot.frame.revision);
                            explore_message(explore::Message::Gallery(
                                explore::gallery::Message::Overlay(
                                    explore::overlay::Message::BoxesToggled(
                                        !snapshot.overlay.showboxes,
                                    ),
                                ),
                            ))
                        } else {
                            driver.phase = Phase::AwaitAtlasWindow(true);
                            #[cfg(target_arch = "wasm32")]
                            fullscreen_js(true);
                            Task::none()
                        }
                    }
                }
            }
            Phase::AtlasOverlay(index) => widgets.arm(driver, overlay_control(index, false)),
            Phase::AwaitAtlasOverlay(index) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let expected = [6, 4, 5, 1, 0, 2, 3, 7][index];
                if snapshot.busy
                    || model.has_explore_pending()
                    || self.atlas_drawn != Some((snapshot.frame.revision, expected))
                {
                    return Task::none();
                }
                if let Some((revision, seed)) = self.atlas_baseline {
                    if snapshot.augmentation.seed != seed
                        || (index % 4 == 3 && snapshot.frame.revision != revision)
                    {
                        driver.fail(
                            "atlas visibility changed augmentation or labels changed the GPU frame",
                        );
                        return Task::none();
                    }
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.atlas_checkbox",
                        overlay_control(index, false),
                        "rendered-saved-visibility",
                        [
                            index as f64,
                            expected as f64,
                            snapshot.frame.revision as f64,
                            snapshot.augmentation.seed as f64,
                        ],
                    )
                });
                self.atlas_baseline = Some((snapshot.frame.revision, snapshot.augmentation.seed));
                if index == 7 {
                    let continuation = driver.advance_to(Phase::AtlasResizeStart);
                    iced::widget::operation::snap_to(EXPLORE_GALLERY, RelativeOffset::START)
                        .chain(continuation)
                } else {
                    driver.phase = Phase::AtlasOverlay(index + 1);
                    widgets.arm(driver, overlay_control(index + 1, false))
                }
            }
            Phase::ViewerOverlay(index) => widgets.arm(driver, overlay_control(index, true)),
            Phase::AwaitViewerOverlay(index) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let expected = [6, 4, 5, 1, 0, 2, 3, 7, 6][index];
                let actual = u8::from(snapshot.overlay.showboxes)
                    | (u8::from(snapshot.overlay.showmasks) << 1)
                    | (u8::from(snapshot.overlay.showlabels) << 2);
                if actual != expected
                    || snapshot.busy
                    || !model.explore_mutation_available()
                    || probes.draws().viewer.is_none_or(|(_, source, _)| {
                        model
                            .viewed_explore_frame()
                            .is_none_or(|viewed| source != viewed.revision)
                    })
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_overlay",
                        overlay_control(index, true),
                        "actual-draw",
                        [
                            index as f64,
                            actual as f64,
                            snapshot.frame.revision as f64,
                            snapshot.frame.cleanrevision as f64,
                        ],
                    )
                });
                if index == 8 {
                    self.begin_upscale_series(widgets, driver, model, &snapshot.frame)
                } else {
                    driver.phase = Phase::ViewerOverlay(index + 1);
                    widgets.arm(driver, overlay_control(index + 1, true))
                }
            }
            Phase::ViewerSquareBasic => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let Some(request) = model.explore.requested_upscale.as_ref() else {
                    return Task::none();
                };
                let Some(upscale) = model.current_upscale() else {
                    return Task::none();
                };
                if snapshot.frame.extent.width != 384
                    || snapshot.frame.extent.height != 384
                    || request.source != snapshot.frame
                    || request.kernel != crate::generated::UpscaleKernel::Default
                    || upscale.frame.extent.width != 1536
                    || upscale.frame.extent.height != 1536
                    || model.displayed_upscale_kernel()
                        != Some(crate::generated::UpscaleKernel::Default)
                {
                    return Task::none();
                }
                let Some(sampleable) = sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Upscale,
                    upscale.frame.revision,
                ) else {
                    return Task::none();
                };
                if sampleable.content_width != 1536
                    || sampleable.content_height != 1536
                    || probes.draws().viewer.is_none_or(|(drawn, source, _)| {
                        drawn != sampleable.presentation_revision
                            || source != upscale.frame.revision
                    })
                {
                    return Task::none();
                }
                driver.phase = Phase::ViewerNoAspect;
                widgets.arm(driver, "explore.detail.aspect")
            }
            Phase::ViewerNoAspect => widgets.arm(driver, "explore.detail.aspect"),
            Phase::ViewerRapidGallery => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || !snapshot.augmentation.enabled
                    || snapshot.augmentation.seed == 0
                    || model.has_explore_pending()
                    || model.explore.desired_augmentation_reroll
                    || snapshot.gallery.generation == 0
                    || snapshot.gallery.slots.len() != snapshot.order.visibleindices.len()
                    || snapshot.gallery.slots.iter().any(|ready| !*ready)
                    || self.atlas_pixels.as_ref().is_none_or(|draw| {
                        draw.snapshot.frame != snapshot.frame
                            || draw.snapshot.gallery.generation != snapshot.gallery.generation
                            || draw.snapshot.dataset.identity != snapshot.dataset.identity
                            || draw.surface.frame != frame
                    })
                    || probes
                        .draws()
                        .gallery
                        .is_none_or(|(_, source)| source != snapshot.frame.revision)
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.gallery_no_input_complete",
                        EXPLORE_GALLERY,
                        "matching-pixels-and-semantics",
                        [
                            snapshot.gallery.generation as f64,
                            snapshot.gallery.slots.len() as f64,
                            snapshot.frame.revision as f64,
                            snapshot.augmentation.seed as f64,
                        ],
                    )
                });
                COMPLETION_WITHOUT_INPUT.with(|active| active.set(false));
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    0,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                self.atlas_baseline = Some((snapshot.frame.revision, snapshot.augmentation.seed));
                driver.phase = Phase::AtlasOverlay(0);
                widgets.arm(driver, overlay_control(0, false))
            }
            Phase::ViewerRapidSelection(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || snapshot.selectedimage != Some(0)
                    || snapshot.revision <= revision
                    || model.has_explore_pending()
                    || model.explore.desired_navigation.is_some()
                {
                    return Task::none();
                }
                self.begin_upscale_series(widgets, driver, model, &snapshot.frame)
            }
            Phase::AwaitDetailFit => {
                let Some((_, _, drawn)) = probes.draws().viewer else {
                    return Task::none();
                };
                if drawn.fit_revision == 0 {
                    return Task::none();
                }
                let center = (drawn.image.center().x - drawn.container.center().x).abs() < 1.0
                    && (drawn.image.center().y - drawn.container.center().y).abs() < 1.0;
                let contain = drawn.image.width <= drawn.container.width + 1.0
                    && drawn.image.height <= drawn.container.height + 1.0
                    && ((drawn.image.width - drawn.container.width).abs() < 1.0
                        || (drawn.image.height - drawn.container.height).abs() < 1.0);
                if !center || !contain {
                    driver.fail(
                        "Fit did not center and contain the stored content in the actual viewer",
                    );
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_detail_fit",
                        explore::DETAIL_FIT_ID,
                        "centered-contained",
                        [
                            drawn.container.width as f64,
                            drawn.container.height as f64,
                            drawn.image.width as f64,
                            drawn.image.height as f64,
                        ],
                    )
                });
                if driver.viewer_scenario == "square" {
                    return driver.advance_to(Phase::ViewerSquareBasic);
                }
                if !driver.viewer_scenario.is_empty()
                    && !matches!(driver.viewer_scenario.as_str(), "copy" | "rapid")
                {
                    driver.phase = if driver.viewer_scenario == "semantics" {
                        Phase::ViewerOverlay(0)
                    } else {
                        Phase::ViewerNoAspect
                    };
                    return widgets.arm(
                        driver,
                        if driver.viewer_scenario == "semantics" {
                            overlay_control(0, true)
                        } else {
                            "explore.detail.aspect"
                        },
                    );
                }
                if driver.viewer_scenario == "rapid" {
                    driver.phase = Phase::ViewerRapidSelection(
                        model
                            .explore
                            .snapshot
                            .as_ref()
                            .map_or(0, |snapshot| snapshot.revision),
                    );
                    return explore_message(explore::Message::Detail(
                        explore::detail::Message::NextRequested,
                    ))
                    .chain(explore_message(explore::Message::Detail(
                        explore::detail::Message::PreviousRequested,
                    )));
                }
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                reporting::emit(|sink| {
                    sink.record(
                        "integration.upscale_action_arm",
                        EXPLORE_UPSCALE_ACTIONS[0],
                        "after-detail-fit",
                        [
                            0.0,
                            snapshot.frame.extent.width as f64,
                            snapshot.frame.extent.height as f64,
                            snapshot.frame.revision as f64,
                        ],
                    )
                });
                self.begin_upscale_series(widgets, driver, model, &snapshot.frame)
            }
            Phase::StartUpscale { kernel, .. } => {
                widgets.arm(driver, EXPLORE_UPSCALE_ACTIONS[kernel])
            }
            Phase::AwaitUpscale {
                kernel,
                source_width,
                source_height,
                upscale_revision: _,
                upscale_frame_revision,
                presentation_revision,
            } => {
                let Some(upscale) = model.current_upscale() else {
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "current_upscale_missing",
                    );
                    return Task::none();
                };
                if upscale.kernel != crate::generated::UPSCALE_KERNEL_VALUES[kernel] {
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "requested_kernel_mismatch",
                    );
                    return Task::none();
                }
                let Some(sampleable) = sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Upscale,
                    upscale.frame.revision,
                ) else {
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "sampleable_presentation_mismatch",
                    );
                    return Task::none();
                };
                if upscale.busy || !upscale.ready {
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "native_work_pending",
                    );
                    return Task::none();
                }
                if upscale.frame.revision != upscale_frame_revision
                    && sampleable.presentation_revision <= presentation_revision
                {
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "presentation_not_newer_than_baseline",
                    );
                    return Task::none();
                }
                let Some(expected_width) = source_width.checked_mul(4) else {
                    driver.fail("Upscale source width cannot be represented at four-times extent");
                    return Task::none();
                };
                let Some(expected_height) = source_height.checked_mul(4) else {
                    driver.fail("Upscale source height cannot be represented at four-times extent");
                    return Task::none();
                };
                if upscale.frame.extent.width != expected_width
                    || upscale.frame.extent.height != expected_height
                {
                    driver.fail("Upscale did not publish the exact four-times output extent");
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.upscale_growth",
                        EXPLORE_UPSCALE_ACTIONS[kernel],
                        upscale_acceptance_label(crate::generated::UPSCALE_KERNEL_VALUES[kernel]),
                        [
                            source_width as f64,
                            source_height as f64,
                            upscale.frame.extent.width as f64,
                            upscale.frame.extent.height as f64,
                        ],
                    )
                });
                if sampleable.content_width != expected_width
                    || sampleable.content_height != expected_height
                    || sampleable.capability_width < expected_width
                    || sampleable.capability_height < expected_height
                {
                    driver.fail("Presentation did not import the exact Upscale output capability");
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.upscale_presentation",
                        EXPLORE_UPSCALE_ACTIONS[kernel],
                        "complete-four-times-exported-frame",
                        [
                            sampleable.content_width as f64,
                            sampleable.content_height as f64,
                            sampleable.capability_width as f64,
                            sampleable.capability_height as f64,
                        ],
                    )
                });
                if model.displayed_upscale_kernel() != Some(upscale.kernel) {
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "displayed_kernel_mismatch",
                    );
                    return Task::none();
                }
                let Some((drawn, source, viewer)) =
                    probes.draws().viewer.filter(|(drawn, source, _)| {
                        *drawn == sampleable.presentation_revision
                            && *source == upscale.frame.revision
                    })
                else {
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "matching_viewer_draw_missing",
                    );
                    return Task::none();
                };
                let Some(button) = self.upscale_button else {
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "method_button_missing",
                    );
                    return Task::none();
                };
                let Some(receipt) = current_receipt(explore::DETAIL_WORKSPACE_ID) else {
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "draw_probe_receipt_missing",
                    );
                    return Task::none();
                };
                if (*probes.upscale_pending()).as_ref() != Some(&receipt) {
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "probe_receipt_not_current",
                    );
                    if let Some(output) =
                        self.prepare_upscale_probe(probes, viewer.image, source, drawn)
                    {
                        sample_upscale_pixels(output, viewer.image, button, source, drawn);
                    }
                    return Task::none();
                }
                let Some((pixel_source, pixel_presentation, checksum, blue)) = self.upscale_pixels
                else {
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "probe_pixels_pending",
                    );
                    return Task::none();
                };
                if pixel_source != source || pixel_presentation != drawn {
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "probe_pixels_frontier_mismatch",
                    );
                    if let Some(output) =
                        self.prepare_upscale_probe(probes, viewer.image, source, drawn)
                    {
                        sample_upscale_pixels(output, viewer.image, button, source, drawn);
                    }
                    return Task::none();
                }
                if checksum == 0 || blue < 32 {
                    driver.fail("Upscale actual canvas image or completed blue method did not match its displayed result");
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.upscale_completed_pixels",
                        EXPLORE_UPSCALE_ACTIONS[kernel],
                        "exact-completed-blue",
                        [source as f64, drawn as f64, checksum as f64, blue as f64],
                    )
                });
                if let Some(revision) = self.upscale_repeat_revision {
                    if !self.upscale_repeat_observed {
                        reporting::upscale_settlement(
                            driver,
                            self,
                            probes,
                            model,
                            frame,
                            "repeat_request_not_observed",
                        );
                        return Task::none();
                    }
                    if upscale.revision != revision {
                        driver.fail(
                            "Same completed Upscale method unnecessarily submitted native work",
                        );
                        return Task::none();
                    }
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.upscale_same_method",
                            EXPLORE_UPSCALE_ACTIONS[kernel],
                            "same-completed-result",
                            [source as f64, drawn as f64, revision as f64, 1.0],
                        )
                    });
                } else {
                    self.upscale_repeat_revision = Some(upscale.revision);
                    if !click(button) {
                        driver.fail("Upscale completed method re-click failed");
                    }
                    reporting::upscale_settlement(
                        driver,
                        self,
                        probes,
                        model,
                        frame,
                        "repeat_request_dispatched",
                    );
                    return Task::none();
                }
                if self.upscale_cache_pass {
                    if self.upscale_cached_frames[kernel].as_ref() != Some(&upscale.frame) {
                        driver.fail("cached method changed its completed physical product");
                        return Task::none();
                    }
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.upscale_cached",
                            EXPLORE_UPSCALE_ACTIONS[kernel],
                            "same-resident-product-drawn",
                            [
                                source as f64,
                                drawn as f64,
                                kernel as f64,
                                upscale.revision as f64,
                            ],
                        )
                    });
                    if kernel + 1 == EXPLORE_UPSCALE_ACTIONS.len()
                        && self.viewer_continuity_request.is_none()
                    {
                        let Some(snapshot) = model.settings_snapshot.as_ref() else {
                            reporting::upscale_settlement(
                                driver,
                                self,
                                probes,
                                model,
                                frame,
                                "continuity_settings_missing",
                            );
                            return Task::none();
                        };
                        if !route_edit_available(model, settings) {
                            reporting::upscale_settlement(
                                driver,
                                self,
                                probes,
                                model,
                                frame,
                                "continuity_route_unavailable",
                            );
                            return Task::none();
                        }
                        self.viewer_continuity_request = model.explore.requested_upscale.clone();
                        self.viewer_continuity_settings_revision = snapshot.revision;
                        self.viewer_continuity_performance =
                            snapshot.settingsstate.ui.showworkspaceperformance;
                        driver.phase = Phase::ViewerConfirmSettings;
                        return Task::done(RootMessage::Workspace(
                            crate::view::router::Message::Navigation(
                                crate::view::navigation::Message::SettingsRequested,
                            ),
                        ))
                        .chain(Task::done(RootMessage::Settings(
                            crate::view::settings::Message::PerformanceChanged(
                                !self.viewer_continuity_performance,
                            ),
                        )));
                    }
                } else {
                    self.upscale_cached_frames[kernel] = Some(upscale.frame.clone());
                    if kernel + 1 == EXPLORE_UPSCALE_ACTIONS.len()
                        && driver.viewer_scenario != "rapid"
                    {
                        self.upscale_cache_pass = true;
                        driver.phase = Phase::StartUpscale {
                            kernel: 0,
                            source_width,
                            source_height,
                            upscale_revision: upscale.revision,
                            upscale_frame_revision: upscale.frame.revision,
                            presentation_revision: sampleable.presentation_revision,
                        };
                        return widgets.arm(driver, EXPLORE_UPSCALE_ACTIONS[0]);
                    }
                }
                let pending_draw = match driver.viewer_scenario.as_str() {
                    "copy" if kernel + 1 == EXPLORE_UPSCALE_ACTIONS.len() => {
                        Some("annotation_handoff_draw_missing")
                    }
                    "semantics" if kernel + 1 == EXPLORE_UPSCALE_ACTIONS.len() => {
                        Some("semantic_handoff_draw_missing")
                    }
                    "rapid" => Some("rapid_completion_draw_missing"),
                    _ => None,
                };
                if let Some(reason) = pending_draw {
                    if probes.draws().viewer.is_none_or(|(drawn, source, _)| {
                        drawn != sampleable.presentation_revision
                            || source != upscale.frame.revision
                    }) {
                        reporting::upscale_settlement(driver, self, probes, model, frame, reason);
                        return Task::none();
                    }
                }
                if driver.viewer_scenario == "copy" && kernel + 1 == EXPLORE_UPSCALE_ACTIONS.len() {
                    driver.phase = Phase::OpenAnnotation;
                    return widgets.arm(driver, EXPLORE_ANNOTATE);
                }
                if driver.viewer_scenario == "semantics"
                    && kernel + 1 == EXPLORE_UPSCALE_ACTIONS.len()
                {
                    driver.phase = Phase::ViewerNoAspect;
                    return widgets.arm(driver, "explore.detail.aspect");
                }
                if driver.viewer_scenario == "rapid" {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.viewer_complete",
                            EXPLORE_UPSCALE_ACTIONS[kernel],
                            "rapid",
                            [
                                sampleable.presentation_revision as f64,
                                upscale.frame.revision as f64,
                                1.0,
                                kernel as f64,
                            ],
                        )
                    });
                    driver.phase = Phase::Complete;
                    return Task::none();
                }
                if kernel + 1 < EXPLORE_UPSCALE_ACTIONS.len() {
                    let next_kernel = kernel + 1;
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.upscale_action_arm",
                            EXPLORE_UPSCALE_ACTIONS[next_kernel],
                            "after-upscale-publication",
                            [
                                next_kernel as f64,
                                upscale.revision as f64,
                                upscale.frame.revision as f64,
                                sampleable.presentation_revision as f64,
                            ],
                        )
                    });
                    driver.phase = Phase::StartUpscale {
                        kernel: next_kernel,
                        source_width,
                        source_height,
                        upscale_revision: upscale.revision,
                        upscale_frame_revision: upscale.frame.revision,
                        presentation_revision: sampleable.presentation_revision,
                    };
                    widgets.arm(driver, EXPLORE_UPSCALE_ACTIONS[next_kernel])
                } else {
                    probes.await_annotation_sample(sampleable.presentation_revision);
                    driver.phase = Phase::DetailNext(
                        model
                            .explore
                            .snapshot
                            .as_ref()
                            .and_then(|snapshot| snapshot.selectedimage)
                            .unwrap_or_default(),
                    );
                    widgets.arm(driver, EXPLORE_NEXT)
                }
            }
            Phase::DetailNext(_) => widgets.arm(driver, EXPLORE_NEXT),
            Phase::AwaitNext(previous) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let Some(selected) = snapshot.selectedimage else {
                    return Task::none();
                };
                if snapshot.busy
                    || selected == previous
                    || settings.has_local_edits()
                    || !model.explore_mutation_available()
                    || displayed_detail(surface, snapshot).is_none()
                {
                    return Task::none();
                }
                let Some(sampleable) = sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                ) else {
                    return Task::none();
                };
                if sampleable.presentation_revision
                    <= probes.annotation_observation().sample_baseline
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.upscale_later_frame",
                        EXPLORE_GALLERY,
                        "distinct-imported-frame",
                        [
                            probes.annotation_observation().sample_baseline as f64,
                            sampleable.presentation_revision as f64,
                            sampleable.content_width as f64,
                            sampleable.content_height as f64,
                        ],
                    )
                });
                driver.phase = Phase::DetailPrevious(selected);
                widgets.arm(driver, EXPLORE_PREVIOUS)
            }
            Phase::DetailPrevious(_) => widgets.arm(driver, EXPLORE_PREVIOUS),
            Phase::AwaitPrevious(next) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || snapshot.selectedimage == Some(next)
                    || settings.has_local_edits()
                    || !model.annotation_open_available()
                    || displayed_detail(surface, snapshot).is_none()
                {
                    return Task::none();
                }
                probes.restart_annotation_sampling(
                    crate::presentation_surface::retained_surface()
                        .and_then(|surface| surface.frame)
                        .map_or(0, |frame| frame.presentation_revision),
                );
                driver.phase = Phase::DetailCloseEvidence;
                widgets.arm(driver, EXPLORE_DETAIL_CLOSE)
            }
            Phase::DetailCloseEvidence => widgets.arm(driver, EXPLORE_DETAIL_CLOSE),
            Phase::AwaitDetailClose => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy || snapshot.mode != crate::generated::ExploreMode::Gallery {
                    return Task::none();
                }
                driver.phase = Phase::ExploreDatasetReopen {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                };
                widgets.arm(driver, EXPLORE_OPEN)
            }
            Phase::ExploreDatasetReopen { .. } => widgets.arm(driver, EXPLORE_OPEN),
            Phase::AwaitExploreDatasetReopen {
                revision,
                frame_revision,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                driver.reporting.observe(|reporting| {
                    reporting.reopen_wait(
                        frame,
                        snapshot,
                        revision,
                        frame_revision,
                        self.selection_grid.is_some(),
                        probes.draws().gallery,
                    )
                });
                if !snapshot.ready
                    || snapshot.busy
                    || snapshot.revision <= revision
                    || snapshot.frame.revision <= frame_revision
                    || snapshot.gallery.generation == 0
                    || snapshot.gallery.slots.len() != snapshot.order.visibleindices.len()
                    || snapshot.gallery.slots.iter().any(|ready| !*ready)
                {
                    return Task::none();
                }
                let Some(_) = self.selection_grid else {
                    driver.fail("Explore selection grid is unavailable after reopen");
                    return Task::none();
                };
                if snapshot.viewport.columns == 0 || snapshot.viewport.rowcount == 0 {
                    return Task::none();
                }
                if sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                )
                .is_none()
                {
                    return Task::none();
                }
                if probes.draws().gallery.is_none_or(|(presentation, source)| {
                    source != snapshot.frame.revision
                        || frame.is_none_or(|frame| frame.presentation_revision != presentation)
                }) {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_reopened",
                        EXPLORE_OPEN,
                        "usable-after-reopen",
                        [
                            snapshot.revision as f64,
                            snapshot.frame.revision as f64,
                            snapshot.order.visibleindices.len() as f64,
                            snapshot.dataset.imagecount as f64,
                        ],
                    )
                });
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_final_cursor",
                        EXPLORE_GALLERY,
                        "coherent-placeholder",
                        [
                            snapshot.revision as f64,
                            snapshot.frame.revision as f64,
                            snapshot.gallery.generation as f64,
                            snapshot.order.visibleindices.len() as f64,
                        ],
                    )
                });
                let columns = snapshot.viewport.columns;
                let slot = (snapshot.viewport.rowcount / 2)
                    .saturating_mul(columns)
                    .saturating_add(columns / 2) as usize;
                if snapshot.order.visibleindices.get(slot).is_none() {
                    driver.fail("reopened Explore gallery has no usable image");
                    return Task::none();
                }
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    slot as u32,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                driver.phase = Phase::GalleryReselect;
                widgets.arm(driver, EXPLORE_GALLERY)
            }
            Phase::GalleryReselect => widgets.arm(driver, EXPLORE_GALLERY),
            Phase::AwaitDetailAgain => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || snapshot.mode != crate::generated::ExploreMode::Detail
                    || displayed_detail(surface, snapshot).is_none()
                {
                    return Task::none();
                }
                if probes
                    .draws()
                    .viewer
                    .is_none_or(|(presentation, source, _)| {
                        model
                            .viewed_explore_frame()
                            .is_none_or(|viewed| source != viewed.revision)
                            || frame.is_none_or(|frame| frame.presentation_revision != presentation)
                    })
                {
                    return Task::none();
                }
                driver.phase = Phase::OpenAnnotation;
                widgets.arm(driver, EXPLORE_ANNOTATE)
            }
            _ => Task::none(),
        }
    }
    pub(super) fn require_original_crop(
        &mut self,
        driver: &mut Driver,
        probes: &mut probe::Requests,
        frame: &crate::generated::VisualFrame,
    ) -> bool {
        let content = &frame.content;
        if probes.draws().viewer.is_none_or(|(_, _, draw)| {
            draw.crop != [content.x, content.y, content.width, content.height]
        }) {
            driver.fail("returning viewer lost the selected Original-content crop");
            return false;
        }
        true
    }
    pub(super) fn begin_upscale_series(
        &mut self,
        widgets: &mut widget_ops::RevealState,
        driver: &mut Driver,
        model: &ApplicationModel,
        source: &crate::generated::VisualFrame,
    ) -> Task<RootMessage> {
        driver.phase = Phase::StartUpscale {
            kernel: 0,
            source_width: source.extent.width,
            source_height: source.extent.height,
            upscale_revision: model
                .upscale_snapshot
                .as_ref()
                .map_or(0, |value| value.revision),
            upscale_frame_revision: model
                .upscale_snapshot
                .as_ref()
                .map_or(0, |value| value.frame.revision),
            presentation_revision: crate::presentation_surface::retained_surface()
                .and_then(|surface| surface.frame)
                .map_or(0, |frame| frame.presentation_revision),
        };
        widgets.arm(driver, EXPLORE_UPSCALE_ACTIONS[0])
    }
    pub(super) fn invalidate_atlas_draw(&mut self) {
        // The next ordinary physical draw can arm the same frame at settled geometry.
        self.atlas_receipt = None;
        self.atlas_pixels = None;
        self.atlas_composition = None;
        invalidate_atlas_observation();
    }
    pub(super) fn scroll_atlas(&mut self, driver: &mut Driver, stage: u8) -> Task<RootMessage> {
        driver.phase = Phase::AwaitAtlasScroll(stage);
        match stage {
            5 => iced::widget::operation::snap_to(EXPLORE_GALLERY, RelativeOffset::END),
            6 => iced::widget::operation::snap_to(EXPLORE_GALLERY, RelativeOffset::START),
            _ => iced::widget::operation::scroll_to(
                EXPLORE_GALLERY,
                AbsoluteOffset {
                    x: 0.0,
                    y: self.atlas_row_extent
                        * ([0.0, 1.0, 2.0, 10.0, 9.0][stage as usize]
                            + if matches!(stage, 0 | 2) {
                                self.atlas_scroll_fraction
                            } else {
                                0.0
                            }),
                },
            ),
        }
    }
    pub(super) fn confirmed_atlas(
        &self,
        snapshot: &crate::generated::ExploreSnapshot,
    ) -> Option<&AtlasDraw> {
        self.atlas_pixels.as_ref().filter(|draw| {
            draw.snapshot.frame == snapshot.frame && self.atlas_receipt.as_ref() == Some(*draw)
        })
    }
    pub(super) fn detail_drawn(
        &self,
        probes: &probe::Requests,
        frame: Option<crate::presentation_surface::FrameReady>,
        snapshot: &crate::generated::ExploreSnapshot,
    ) -> bool {
        snapshot.mode == crate::generated::ExploreMode::Detail
            && sampleable_presentation(
                frame,
                crate::generated::PresentationSourceKind::Explore,
                snapshot.frame.revision,
            )
            .is_some_and(|sampleable| {
                probes
                    .draws()
                    .viewer
                    .is_some_and(|(presentation, source, _)| {
                        source == sampleable.source_revision
                            && presentation == sampleable.presentation_revision
                    })
            })
    }
    pub(super) fn expected_retained(&self, driver: &Driver) -> Option<String> {
        Some(match driver.phase {
            Phase::ExploreNavigation => {
                crate::view::navigation::stable_id(FeatureId::Explore).to_owned()
            }
            Phase::ExploreOpen => EXPLORE_OPEN.to_owned(),
            Phase::ExploreCloseDetail => EXPLORE_DETAIL_CLOSE.to_owned(),
            Phase::ExploreDatasetPane => EXPLORE_DATASET_PANE.to_owned(),
            Phase::ExploreDetailsPane => EXPLORE_DETAILS_PANE.to_owned(),
            Phase::ExploreNumericControl { index, .. }
            | Phase::ExploreNumericReveal { index, .. } => explore_integer_id(index),
            Phase::ExplorePolicyOrder(_) => explore::ORDER_SHUFFLED_ID.to_owned(),
            Phase::ExplorePolicyRange(_) | Phase::ExplorePolicyRangeVisible(_) => {
                explore::RANGE_START_ONE_ID.to_owned()
            }
            Phase::ExplorePolicyOverlay(_) | Phase::ExplorePolicyOverlayVisible(_) => {
                explore::OVERLAY_NONE_ID.to_owned()
            }
            Phase::ExploreAugmentationToggle { .. } => EXPLORE_AUGMENTATION_TOGGLE.to_owned(),
            Phase::ExploreAugmentationReroll { .. } => EXPLORE_AUGMENTATION_REROLL.to_owned(),
            Phase::ExploreReshuffle { .. } => EXPLORE_RESHUFFLE.to_owned(),
            Phase::ExploreCard { .. } => EXPLORE_CARD.to_owned(),
            Phase::GallerySweep => EXPLORE_GALLERY.to_owned(),
            Phase::GalleryLater(_) => EXPLORE_LATER.to_owned(),
            Phase::GalleryImage(_) => EXPLORE_GALLERY.to_owned(),
            Phase::DetailOriginal { .. } => EXPLORE_DETAIL_ORIGINAL.to_owned(),
            Phase::DetailFit => explore::DETAIL_FIT_ID.to_owned(),
            Phase::ViewerSelect
            | Phase::AtlasReturnSelect
            | Phase::AtlasResizeSelect(_)
            | Phase::AtlasAwaySelect(_) => EXPLORE_GALLERY.to_owned(),
            Phase::AtlasCapacity => explore::GALLERY_CAPACITY_ID.to_owned(),
            Phase::AtlasEmpty => explore::GALLERY_EMPTY_ID.to_owned(),
            Phase::AtlasOverlay(index) => overlay_control(index, false).to_owned(),
            Phase::ViewerOverlay(index) => overlay_control(index, true).to_owned(),
            Phase::ViewerNoAspect => "explore.detail.aspect".to_owned(),
            Phase::StartUpscale { kernel, .. } => EXPLORE_UPSCALE_ACTIONS[kernel].to_owned(),
            Phase::DetailNext(_) => EXPLORE_NEXT.to_owned(),
            Phase::DetailPrevious(_) => EXPLORE_PREVIOUS.to_owned(),
            Phase::DetailCloseEvidence => EXPLORE_DETAIL_CLOSE.to_owned(),
            Phase::ExploreDatasetReopen { .. } => EXPLORE_OPEN.to_owned(),
            Phase::GalleryReselect => EXPLORE_GALLERY.to_owned(),
            _ => return None,
        })
    }
    pub(super) fn located_retained(
        &mut self,
        driver: &mut Driver,
        probes: &mut probe::Requests,
        widgets: &mut widget_ops::RevealState,
        control: String,
        bounds: Rectangle,
        input_bounds: Rectangle,
    ) -> Option<train::Message> {
        match driver.phase.clone() {
            Phase::ExploreCard {
                revision,
                frame_revision,
            } => {
                driver.phase = Phase::AwaitGalleryPatch {
                    revision,
                    frame_revision,
                };
                None
            }
            Phase::ExploreDatasetPane => {
                self.explore_dataset_pane = Some(bounds);
                driver.phase = Phase::ExploreDetailsPane;
                None
            }
            Phase::ExploreDetailsPane => {
                self.explore_details_pane = Some(bounds);
                driver.phase = Phase::ExploreNumericStart(0);
                None
            }
            Phase::ExploreNumericControl { index, step }
            | Phase::ExploreNumericReveal { index, step } => {
                let Some(pane) = self.explore_dataset_pane else {
                    driver.fail("Explore numeric input has no sidebar bounds");
                    return None;
                };
                if let Some(offset) = sidebar_reveal_offset(pane, bounds) {
                    widgets.measured_reveal(offset);
                    driver.phase = Phase::ExploreNumericReveal { index, step };
                    return None;
                }
                let delivered = match step {
                    0 | 1 => click_number_edge(input_bounds, step == 0),
                    2 => wheel_number_input(input_bounds),
                    6 => paste_number_input(input_bounds),
                    _ => widgets.replace_numeric_input(input_bounds, 20),
                };
                if !delivered {
                    driver.fail("Explore integer input dispatch failed");
                } else {
                    driver.phase = match step {
                        2 => Phase::AwaitExploreNumericWheel(index),
                        3 => Phase::AwaitExploreNumericInvalid(index),
                        _ => Phase::AwaitExploreNumeric { index, step },
                    };
                }
                None
            }
            Phase::ExplorePolicyOrder(revision) => {
                driver.phase = Phase::AwaitExplorePolicyOrder(revision);
                if !click(input_bounds) {
                    driver.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::ExplorePolicyRange(revision) => {
                let Some(pane) = self.explore_dataset_pane else {
                    driver.fail("Explore dataset pane bounds are unavailable");
                    return None;
                };
                if let Some(offset) = sidebar_reveal_offset(pane, bounds) {
                    widgets.measured_reveal(offset);
                    driver.phase = Phase::ExplorePolicyRangeVisible(revision);
                    return None;
                }
                driver.phase = Phase::AwaitExplorePolicyRange(revision);
                if !click(input_bounds) {
                    driver.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::ExplorePolicyRangeVisible(revision) => {
                let Some(pane) = self.explore_dataset_pane else {
                    driver.fail("Explore dataset pane bounds are unavailable");
                    return None;
                };
                if !sidebar_control_visible(pane, bounds) {
                    driver.fail("Explore range control was not revealed inside its sidebar");
                    return None;
                }
                driver.phase = Phase::AwaitExplorePolicyRange(revision);
                if !click(input_bounds) {
                    driver.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::ExplorePolicyOverlay(revision) => {
                let Some(pane) = self.explore_details_pane else {
                    driver.fail("Explore details pane bounds are unavailable");
                    return None;
                };
                if let Some(offset) = sidebar_reveal_offset(pane, bounds) {
                    widgets.measured_reveal(offset);
                    driver.phase = Phase::ExplorePolicyOverlayVisible(revision);
                    return None;
                }
                driver.phase = Phase::AwaitExplorePolicyOverlay(revision);
                if !click(input_bounds) {
                    driver.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::ExplorePolicyOverlayVisible(revision) => {
                let Some(pane) = self.explore_details_pane else {
                    driver.fail("Explore details pane bounds are unavailable");
                    return None;
                };
                if !sidebar_control_visible(pane, bounds) {
                    driver.fail("Explore overlay control was not revealed inside its sidebar");
                    return None;
                }
                driver.phase = Phase::AwaitExplorePolicyOverlay(revision);
                if !click(input_bounds) {
                    driver.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::ExploreAugmentationToggle {
                revision,
                frame_revision,
            } => {
                driver.phase = Phase::AwaitExploreAugmentationToggle {
                    revision,
                    frame_revision,
                };
                if !click(input_bounds) {
                    driver.fail("Firefox augmentation toggle click dispatch failed");
                }
                None
            }
            Phase::ExploreAugmentationReroll {
                revision,
                frame_revision,
            } => {
                driver.phase = Phase::AwaitExploreAugmentationReroll {
                    revision,
                    frame_revision,
                };
                if !click(input_bounds) {
                    driver.fail("Firefox augmentation reroll click dispatch failed");
                }
                None
            }
            Phase::ExploreReshuffle {
                revision,
                frame_revision,
                shuffle_seed,
                augmentation_seed,
                order_signature,
            } => {
                driver.phase = Phase::AwaitExploreReshuffle {
                    revision,
                    frame_revision,
                    shuffle_seed,
                    augmentation_seed,
                    order_signature,
                };
                if !click(input_bounds) {
                    driver.fail("Firefox Reshuffle click dispatch failed");
                }
                None
            }
            Phase::DetailOriginal {
                revision,
                frame_revision,
                padded_width,
                padded_height,
            } => {
                driver.phase = Phase::AwaitDetailOriginal {
                    revision,
                    frame_revision,
                    padded_width,
                    padded_height,
                };
                if !click(input_bounds) {
                    driver.fail("Firefox original-detail click dispatch failed");
                }
                None
            }
            Phase::ViewerSelect
            | Phase::AtlasReturnSelect
            | Phase::AtlasResizeSelect(_)
            | Phase::AtlasAwaySelect(_) => {
                let resizing = match driver.phase {
                    Phase::AtlasResizeSelect(step) => Some(step),
                    _ => None,
                };
                let returning = matches!(driver.phase, Phase::AtlasReturnSelect);
                let away = match driver.phase {
                    Phase::AtlasAwaySelect(step) => Some(step),
                    _ => None,
                };
                let Some((columns, _rows, _, _, revision)) = self.selection_grid else {
                    return None;
                };
                let index = if driver.viewer_scenario == "tall" {
                    7
                } else {
                    0
                };
                let side = input_bounds.width / columns as f32;
                let selected = Rectangle {
                    x: input_bounds.x + (index % columns) as f32 * side,
                    y: input_bounds.y + (index / columns) as f32 * side,
                    width: side,
                    height: side,
                };
                driver.phase = if let Some(step) = resizing {
                    Phase::AwaitAtlasResizeDetail(step)
                } else if returning {
                    Phase::AwaitAtlasReturnDetail
                } else if let Some(step) = away {
                    Phase::AwaitAtlasAwayDetail(step, 0)
                } else {
                    Phase::AwaitDetail(index)
                };
                if !click_after_surface_draw(selected, EXPLORE_GALLERY, revision, true) {
                    driver.fail("viewer first-image click failed");
                }
                None
            }
            Phase::AtlasCapacity | Phase::AtlasEmpty => {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.atlas_notice",
                        &control,
                        "rendered-local-outcome",
                        [
                            bounds.x as f64,
                            bounds.y as f64,
                            bounds.width as f64,
                            bounds.height as f64,
                        ],
                    )
                });
                driver.phase = if matches!(driver.phase, Phase::AtlasCapacity) {
                    Phase::AtlasRestoreColumns
                } else {
                    Phase::AtlasRestoreFilter
                };
                None
            }
            Phase::AtlasOverlay(index) => {
                driver.phase = Phase::AwaitAtlasOverlay(index);
                if !click(input_bounds) {
                    driver.fail("atlas visibility checkbox click failed");
                }
                None
            }
            Phase::ViewerOverlay(index) => {
                driver.phase = Phase::AwaitViewerOverlay(index);
                if !click(input_bounds) {
                    driver.fail("viewer semantic checkbox click failed");
                }
                None
            }
            Phase::DetailFit => {
                probes.await_viewer_draw();
                driver.phase = Phase::AwaitDetailFit;
                if !click(input_bounds) {
                    driver.fail("Firefox detail Fit click dispatch failed");
                }
                None
            }
            Phase::StartUpscale {
                kernel,
                source_width,
                source_height,
                upscale_revision,
                upscale_frame_revision,
                presentation_revision,
            } => {
                self.upscale_button = Some(input_bounds);
                probes.rearm_upscale_probe();
                self.upscale_pixels = None;
                self.upscale_repeat_revision = None;
                self.upscale_repeat_observed = false;
                driver.phase = Phase::AwaitUpscale {
                    kernel,
                    source_width,
                    source_height,
                    upscale_revision,
                    upscale_frame_revision,
                    presentation_revision,
                };
                if !click(input_bounds) {
                    driver.fail("Firefox Upscale action click dispatch failed");
                }
                if driver.viewer_scenario == "rapid" && kernel + 1 < EXPLORE_UPSCALE_ACTIONS.len() {
                    driver.phase = Phase::StartUpscale {
                        kernel: kernel + 1,
                        source_width,
                        source_height,
                        upscale_revision,
                        upscale_frame_revision,
                        presentation_revision,
                    };
                }
                None
            }
            Phase::DetailCloseEvidence => {
                driver.phase = Phase::AwaitDetailClose;
                if !click(input_bounds) {
                    driver.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::ExploreDatasetReopen {
                revision,
                frame_revision,
            } => {
                driver.phase = Phase::AwaitExploreDatasetReopen {
                    revision,
                    frame_revision,
                };
                if !click(input_bounds) {
                    driver.fail("Firefox Explore dataset-reopen click dispatch failed");
                }
                None
            }
            Phase::GalleryReselect => {
                let Some((columns, _, slot, _, revision)) = self.selection_grid else {
                    driver.fail("reopened Explore selection has no source revision");
                    return None;
                };
                let selected = gallery_slot_bounds(input_bounds, columns, slot, self.atlas_clip.0);
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_pointer_scheduled",
                        EXPLORE_GALLERY,
                        "reopened-grid-slot",
                        [
                            revision as f64,
                            slot as f64,
                            f64::from(selected.center_x()),
                            f64::from(selected.center_y()),
                        ],
                    )
                });
                driver.phase = Phase::AwaitDetailAgain;
                if !click_after_surface_draw(selected, EXPLORE_GALLERY, revision, false) {
                    driver.fail("Firefox reopened-gallery click dispatch failed");
                }
                None
            }
            Phase::GallerySweep => {
                #[cfg(target_arch = "wasm32")]
                let dispatched = sweep_js(
                    f64::from(input_bounds.x),
                    f64::from(input_bounds.y),
                    f64::from(input_bounds.width),
                    f64::from(input_bounds.height),
                ) == 1;
                #[cfg(not(target_arch = "wasm32"))]
                let dispatched = false;
                if !dispatched {
                    driver.fail("Firefox sweep dispatch failed");
                    return None;
                }
                driver.phase = Phase::AwaitGallerySweep;
                None
            }
            Phase::GalleryLater(baseline) => {
                driver.phase = Phase::AwaitGalleryScroll(baseline);
                if !click(input_bounds) {
                    driver.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::DetailNext(selected) => {
                driver.phase = Phase::AwaitNext(selected);
                if !click(input_bounds) {
                    driver.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::DetailPrevious(selected) => {
                driver.phase = Phase::AwaitPrevious(selected);
                if !click(input_bounds) {
                    driver.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::GalleryImage(index) => {
                let Some((columns, rows, expected_slot, snapshot_revision, frame_revision)) =
                    self.selection_grid
                else {
                    driver.fail("Explore selection grid is unavailable");
                    return None;
                };
                let selected =
                    gallery_slot_bounds(bounds, columns, expected_slot, self.atlas_clip.0);
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_pointer_inverse",
                        EXPLORE_GALLERY,
                        "rendered-grid-slot",
                        [
                            expected_slot as f64,
                            (rows / 2)
                                .saturating_mul(columns)
                                .saturating_add(columns / 2) as f64,
                            index as f64,
                            snapshot_revision as f64,
                        ],
                    )
                });
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_pointer_scheduled",
                        EXPLORE_GALLERY,
                        "real-canvas-pointer",
                        [
                            frame_revision as f64,
                            expected_slot as f64,
                            f64::from(selected.x + selected.width * 0.5),
                            f64::from(selected.y + selected.height * 0.5),
                        ],
                    )
                });
                driver.phase = Phase::AwaitDetail(index);
                if !click_after_surface_draw(selected, EXPLORE_GALLERY, frame_revision, false) {
                    driver.fail("Firefox click dispatch failed");
                }
                None
            }
            _ => {
                driver.phase = match driver.phase {
                    Phase::ExploreNavigation => Phase::AwaitExplore,
                    Phase::ExploreCloseDetail => Phase::AwaitExploreGallery,
                    Phase::ExploreOpen => {
                        COMPLETION_WITHOUT_INPUT.with(|active| active.set(true));
                        Phase::AwaitExploreReady
                    }
                    _ => driver.phase.clone(),
                };
                driver.click_located(input_bounds)
            }
        }
    }
}

impl State {
    pub(super) fn callback(
        &mut self,
        driver: &mut Driver,
        probes: &mut probe::Requests,
        message: Message,
        request_receipt: Option<ProbeReceipt>,
    ) {
        match message {
            Message::GalleryMouseDelivered => {
                if let Phase::AwaitVisibleReadHover(index, generation) = driver.phase {
                    driver.phase = Phase::VisibleReadSelect(index, generation);
                }
                return;
            }
            Message::NumberClipboardPrepared { revision, result } => {
                if driver.phase == Phase::AwaitExploreClipboard(revision) {
                    match result {
                        Ok(()) => driver.phase = Phase::ExploreNumericControl { index: 2, step: 6 },
                        Err(error) => driver.fail_detail(|| {
                            format!("Explore clipboard preparation failed: {error:?}").into()
                        }),
                    }
                }
                return;
            }
            Message::NumberPasteRead { target, result } => {
                if driver.phase == (Phase::AwaitExploreNumeric { index: 2, step: 6 })
                    && target == explore_integer_id(2)
                {
                    match result {
                        Ok(content)
                            if matches!(content.as_ref(), iced::clipboard::Content::Text(text)
                            if text == &self.explore_integer_target.to_string()) =>
                        {
                            self.explore_paste_read = true
                        }
                        Ok(_) => driver.fail("Explore paste read different clipboard contents"),
                        Err(error) => driver.fail_detail(|| {
                            format!("Explore clipboard read failed: {error:?}").into()
                        }),
                    }
                }
                return;
            }
            Message::NumberPasteDelivered(delivered) => {
                if !delivered && driver.phase == (Phase::AwaitExploreNumeric { index: 2, step: 6 })
                {
                    driver.fail("Explore clipboard paste shortcut delivery failed");
                }
                return;
            }
            Message::NumberInvalidDelivered => {
                if let Phase::AwaitExploreNumericInvalid(index) = driver.phase {
                    driver.phase = Phase::AwaitExploreNumeric { index, step: 3 };
                }
                return;
            }
            Message::UpscalePixels {
                source,
                presentation,
                outcome,
            } => {
                if !probes.complete_upscale_probe(&request_receipt, &outcome) {
                    return;
                }
                match outcome {
                    ProbeOutcome::Invalidated => {}
                    ProbeOutcome::Observed(checksum, blue) => {
                        self.upscale_pixels = Some((source, presentation, checksum, blue))
                    }
                    ProbeOutcome::Failed => driver.fail("Upscale canvas sampling failed"),
                }
                return;
            }
            Message::AtlasDrawn {
                source_revision,
                visibility,
                clipped_top,
                clipped_bottom,
                row_extent,
                receipt,
            } => {
                self.atlas_receipt = Some(receipt);
                self.atlas_row_extent = row_extent;
                self.atlas_clip = (clipped_top, clipped_bottom);
                self.atlas_drawn = Some((source_revision, visibility));
                return;
            }
            Message::AtlasComposition { receipt, outcome } => {
                complete_atlas_probe(true);
                match outcome {
                    ProbeOutcome::Invalidated => self.invalidate_atlas_draw(),
                    ProbeOutcome::Observed(expected, matched)
                        if expected != 0 && expected == matched =>
                    {
                        self.atlas_composition = Some(receipt)
                    }
                    ProbeOutcome::Failed => driver.fail("Atlas composition canvas sampling failed"),
                    _ => {}
                }
                return;
            }
            Message::AtlasPixels { receipt, outcome } => {
                complete_atlas_probe(false);
                match outcome {
                    ProbeOutcome::Invalidated => self.invalidate_atlas_draw(),
                    ProbeOutcome::Observed(visible, nonblack) => {
                        reporting::emit(|sink| {
                            sink.record(
                                "integration.atlas_canvas_pixels",
                                EXPLORE_GALLERY,
                                "visible-tile-interiors",
                                [
                                    receipt.snapshot.frame.revision as f64,
                                    receipt
                                        .surface
                                        .frame
                                        .map_or(0, |frame| frame.presentation_revision)
                                        as f64,
                                    visible as f64,
                                    nonblack as f64,
                                ],
                            )
                        });
                        if visible != 0 && visible == nonblack {
                            self.atlas_pixels = Some(receipt);
                        }
                    }
                    ProbeOutcome::Failed => driver.fail("Atlas canvas sampling failed"),
                }
                return;
            }
            Message::NumberWheelDelivered => {
                if let Phase::AwaitExploreNumericWheel(index) = driver.phase {
                    driver.phase = Phase::AwaitExploreNumeric { index, step: 2 };
                }
                return;
            }

            Message::GalleryDrawn {
                presentation_revision,
                source_revision,
            } => {
                if let Phase::AwaitExploreDatasetReopen {
                    revision,
                    frame_revision,
                } = &driver.phase
                {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_reopen_draw",
                            EXPLORE_GALLERY,
                            "physical-gallery-draw",
                            [
                                *revision as f64,
                                *frame_revision as f64,
                                source_revision as f64,
                                presentation_revision as f64,
                            ],
                        )
                    });
                }
                probes.record_gallery_draw(presentation_revision, source_revision);
                return;
            }
            Message::SurfaceDrawn {
                presentation_revision,
                source_revision,
                viewer,
            } => {
                if let Some(viewer) = viewer {
                    // Returning to a retained gallery still requires a draw
                    // after Detail; an earlier receipt cannot prove that return.
                    self.invalidate_atlas_draw();
                    probes.record_surface_draw(
                        presentation_revision,
                        source_revision,
                        Some(viewer),
                    );
                } else {
                    probes.record_surface_draw(presentation_revision, source_revision, None);
                }
                return;
            }
            _ => unreachable!("callback routed to the wrong scenario owner"),
        }
    }
}

impl State {
    pub(super) fn hold_gallery_completion(
        &mut self,
        driver: &mut Driver,
        receipt: &crate::generated::IntegrationControlReceipt,
    ) -> Result<(), &'static str> {
        if self.gallery_completion_held.is_some()
            || matches!(driver.phase, Phase::Disabled | Phase::Failed)
        {
            driver.fail("duplicate or inactive gallery completion hold");
            return Err("duplicate or inactive gallery completion hold");
        }
        self.gallery_completion_held = Some((receipt.readgeneration, receipt.compiledindex));
        return Ok(());
    }
}

impl State {
    pub(super) fn observe_upscale_request(
        &mut self,
        driver: &Driver,
        kernel: crate::generated::UpscaleKernel,
    ) {
        if let Phase::AwaitUpscale {
            kernel: selected, ..
        } = driver.phase
            && self.upscale_repeat_revision.is_some()
            && crate::generated::UPSCALE_KERNEL_VALUES[selected] == kernel
        {
            self.upscale_repeat_observed = true;
        }
    }
}

impl State {
    pub(super) fn cancel_input(&mut self) {
        self.explore_paste_read = false;
    }
}

impl State {
    pub(super) fn viewer_selector_located(
        &mut self,
        driver: &mut Driver,
        probes: &probe::Requests,
        bounds: Rectangle,
    ) {
        if bounds.width > 0.0 || bounds.height > 0.0 {
            driver.fail("Explore still renders an aspect-ratio selector");
        } else if let Some((presentation, source, _)) = probes.draws().viewer {
            reporting::emit(|sink| {
                sink.record(
                    "integration.viewer_complete",
                    "explore.detail.aspect",
                    &driver.viewer_scenario,
                    [presentation as f64, source as f64, 1.0, 0.0],
                )
            });
            driver.phase = if driver.viewer_scenario == "terminal" {
                Phase::OpenAnnotation
            } else {
                Phase::Complete
            };
        }
    }
}

pub(super) struct UpscaleObservation {
    pub(super) pixels: Option<(u64, u64, u32, u32)>,
    pub(super) button: Option<Rectangle>,
    pub(super) repeat: (Option<u64>, bool),
}
impl State {
    pub(super) fn upscale_observation(&self) -> UpscaleObservation {
        UpscaleObservation {
            pixels: self.upscale_pixels,
            button: self.upscale_button,
            repeat: (self.upscale_repeat_revision, self.upscale_repeat_observed),
        }
    }
}

pub(super) fn overlay_control(index: usize, detail: bool) -> &'static str {
    let controls = if detail {
        [
            explore::DETAIL_BOXES_ID,
            explore::DETAIL_MASKS_ID,
            explore::DETAIL_BOXES_ID,
            explore::DETAIL_LABELS_ID,
        ]
    } else {
        [
            explore::GALLERY_BOXES_ID,
            explore::GALLERY_MASKS_ID,
            explore::GALLERY_BOXES_ID,
            explore::GALLERY_LABELS_ID,
        ]
    };
    controls[index % 4]
}

pub(super) fn explore_scenario_preparation(
    snapshot: &crate::generated::ExploreSnapshot,
) -> Option<explore::Message> {
    use explore::{Message, dataset, detail, gallery, overlay};
    if snapshot.detail.showoriginaldimensions {
        return Some(Message::Detail(detail::Message::DetailSourceSelected(
            false,
        )));
    }
    if snapshot.augmentation.enabled {
        return Some(Message::Gallery(gallery::Message::AugmentationToggled(
            false,
        )));
    }
    if snapshot.filter.order != crate::generated::ExploreOrder::Sequential {
        return Some(Message::Dataset(dataset::Message::OrderSelected(
            crate::generated::ExploreOrder::Sequential,
        )));
    }
    if snapshot.filter.minimumcompiledindex != 0 {
        return Some(Message::Dataset(
            dataset::Message::MinimumCompiledIndexChanged(0),
        ));
    }
    if snapshot.filter.classselection.mode != crate::generated::ExploreClassSelectionMode::All {
        return Some(Message::Dataset(dataset::Message::AllClasses));
    }
    if snapshot.overlay.classselection.mode != crate::generated::ExploreClassSelectionMode::All {
        return Some(Message::Details(explore::details::Message::AllClasses));
    }
    let visibility = if !snapshot.overlay.showlabels {
        overlay::Message::LabelsToggled(true)
    } else if !snapshot.overlay.showmasks {
        overlay::Message::MasksToggled(true)
    } else if !snapshot.overlay.showboxes {
        overlay::Message::BoxesToggled(true)
    } else {
        return None;
    };
    Some(Message::Gallery(gallery::Message::Overlay(visibility)))
}

pub(super) fn upscale_acceptance_label(kernel: crate::generated::UpscaleKernel) -> &'static str {
    match kernel {
        crate::generated::UpscaleKernel::Default => "basic-four-times",
        crate::generated::UpscaleKernel::ShiftLut => "fast-four-times",
        crate::generated::UpscaleKernel::RealPlksr => "neural-four-times",
    }
}

pub(super) fn cold_gallery_scroll_offset(
    row: u32,
    viewport: &crate::generated::ExploreViewport,
    matching: u32,
    row_extent: f32,
) -> Option<f32> {
    if !row_extent.is_finite() || row_extent <= 0.0 || row == 0 || viewport.columns == 0 {
        return None;
    }
    let total_rows = matching.div_ceil(viewport.columns);
    let sweep_rows = (768.0 / row_extent).ceil() as u32;
    if row
        .saturating_add(viewport.rowcount)
        .saturating_add(sweep_rows)
        .saturating_add(2)
        >= total_rows
    {
        return None;
    }
    // A fractional row avoids floating rounding selecting the preceding row.
    let offset = row_extent * (row as f32 + 0.25);
    offset.is_finite().then_some(offset)
}

pub(super) fn explore_order_signature(indices: &[u32]) -> u64 {
    indices
        .iter()
        .fold(14_695_981_039_346_656_037_u64, |hash, index| {
            (hash ^ u64::from(*index)).wrapping_mul(1_099_511_628_211)
        })
}

pub(super) fn explore_integer_id(index: u8) -> String {
    match index {
        0 => crate::generated::constraint_workflowsexploremininstances()
            .stable_field_id
            .to_string(),
        1 => crate::generated::constraint_workflowsexploremaxinstances()
            .stable_field_id
            .to_string(),
        2 => crate::generated::constraint_workflowsexploreshuffleseed()
            .stable_field_id
            .to_string(),
        3 => crate::generated::constraint_workflowsexploremincompiledindex()
            .stable_field_id
            .to_string(),
        4 => crate::generated::constraint_workflowsexploremaxcompiledindex()
            .stable_field_id
            .to_string(),
        _ => unreachable!("five Explore integer fields"),
    }
}

pub(super) fn explore_seed_target(baseline: u64) -> u64 {
    const PRIMARY: u64 = (1_u64 << 53) + 1;
    const ALTERNATE: u64 = (1_u64 << 53) + 3;
    if baseline == PRIMARY {
        ALTERNATE
    } else {
        PRIMARY
    }
}

pub(super) fn explore_integer_value(
    snapshot: &crate::generated::ExploreSnapshot,
    index: u8,
) -> u64 {
    match index {
        0 => u64::from(snapshot.filter.minimuminstances),
        1 => u64::from(snapshot.filter.maximuminstances),
        2 => snapshot.filter.shuffleseed,
        3 => snapshot.filter.minimumcompiledindex,
        4 => snapshot.filter.maximumcompiledindex,
        _ => unreachable!("five Explore integer fields"),
    }
}

pub(super) fn explore_integer_persisted(model: &ApplicationModel, index: u8, value: u64) -> bool {
    model.settings_snapshot.as_ref().is_some_and(|snapshot| {
        let settings = &snapshot.settingsstate.workflows.explore;
        let persisted = match index {
            0 => u64::from(settings.mininstances),
            1 => u64::from(settings.maxinstances),
            2 => settings.shuffleseed,
            3 => settings.mincompiledindex,
            4 => settings.maxcompiledindex,
            _ => unreachable!("five Explore integer fields"),
        };
        persisted == value
    })
}

pub(super) const COLD_GALLERY_ROW_GAP: u32 = 5;

pub(super) const EXPLORE_OPEN: &str = explore::OPEN_ID;

pub(super) const EXPLORE_CARD: &str = explore::STATUS_CARD_ID;

pub(super) const EXPLORE_LATER: &str = explore::GALLERY_LATER_ID;

pub(super) const EXPLORE_NEXT: &str = explore::DETAIL_NEXT_ID;

pub(super) const EXPLORE_PREVIOUS: &str = explore::DETAIL_PREVIOUS_ID;

pub(super) const EXPLORE_ANNOTATE: &str = explore::DETAIL_ANNOTATE_ID;

#[cfg(test)]
pub(in crate::integration_control) mod tests;
