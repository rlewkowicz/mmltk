use crate::fluent_theme::Element;
use crate::presentation_surface::Surface;
use crate::view::{shared, workspace};
use crate::view_model::ApplicationModel;
use iced::widget::{button, column, container, text};

mod canvas;
pub(crate) mod sidebar;
mod timeline;

pub const WORKSPACE_ID: &str = "annotation.workspace.surface";
pub const SIDEBAR_ID: &str = "annotation.sidebar";
pub const TIMELINE_ID: &str = "annotation.timeline";

pub(crate) fn tool_id(tool: crate::generated::AnnotationTool) -> String {
    sidebar::tool_id(tool)
}

#[derive(Debug, Clone)]
pub enum Shortcut {
    Tool(crate::generated::AnnotationTool),
    Undo,
    Redo,
    Save,
    Delete,
    Cancel,
    Fit,
}

#[derive(Debug, Clone)]
pub enum Message {
    Shortcut(Shortcut),
    ShortcutResolved { shortcut: Shortcut, focused: bool },
    TextFocused,
    OpenRequested,
    SaveRequested,
    CancelRequested,
    FitRequested,
    DialogRequested(u64),
    OutputDirectoryChanged(String),
    Sidebar(sidebar::Message),
    Timeline(timeline::Message),
    Workspace(workspace::Message),
}

#[derive(Debug, Clone)]
pub enum Outcome {
    ShortcutRequested(Shortcut),
    OpenRequested,
    SaveRequested,
    StopRequested,
    DialogRequested(u64),
    EditRequested(crate::generated::AnnotationEditRequest),
    SettingsEdited(crate::view::settings::EditSchedule),
}

#[derive(Default)]
pub struct Component {
    sidebar: sidebar::Component,
    canvas: canvas::Component,
    timeline: timeline::Component,
    fit_revision: u64,
    keyboard_canvas: std::sync::Arc<std::sync::atomic::AtomicBool>,
}

impl Component {
    pub fn set_connection(&self, connection: Option<crate::transport_connection::Connection>) {
        self.canvas.set_connection(connection);
    }
    pub fn rebase(&mut self, model: &ApplicationModel) {
        self.sidebar.rebase(&model.annotation);
    }

    pub fn update(
        &mut self,
        application: &mut ApplicationModel,
        settings: &mut crate::view::settings::SettingsModel,
        message: Message,
    ) -> Result<Option<Outcome>, String> {
        if matches!(
            &message,
            Message::OutputDirectoryChanged(_) | Message::Sidebar(_) | Message::DialogRequested(_)
        ) {
            self.keyboard_canvas
                .store(false, std::sync::atomic::Ordering::Relaxed);
        }
        let outcome = match message {
            Message::TextFocused => {
                self.keyboard_canvas
                    .store(false, std::sync::atomic::Ordering::Relaxed);
                return Ok(None);
            }
            Message::Shortcut(shortcut) => {
                return Ok(self
                    .keyboard_canvas
                    .load(std::sync::atomic::Ordering::Relaxed)
                    .then_some(Outcome::ShortcutRequested(shortcut)));
            }
            Message::ShortcutResolved { shortcut, focused } => {
                if focused {
                    return Ok(None);
                }
                if !self
                    .keyboard_canvas
                    .load(std::sync::atomic::Ordering::Relaxed)
                {
                    return Ok(None);
                }
                let command = match shortcut {
                    Shortcut::Tool(tool) => Message::Sidebar(sidebar::Message::ToolSelected(tool)),
                    Shortcut::Undo => Message::Sidebar(sidebar::Message::UndoRequested),
                    Shortcut::Redo => Message::Sidebar(sidebar::Message::RedoRequested),
                    Shortcut::Save => Message::SaveRequested,
                    Shortcut::Delete => Message::Sidebar(sidebar::Message::Sidebar(
                        crate::generated::AnnotationSidebarCommand::Delete,
                    )),
                    Shortcut::Cancel => Message::CancelRequested,
                    Shortcut::Fit => Message::FitRequested,
                };
                let result = self.update(application, settings, command);
                self.keyboard_canvas
                    .store(true, std::sync::atomic::Ordering::Relaxed);
                return result;
            }
            Message::FitRequested => {
                self.fit_revision = self.fit_revision.wrapping_add(1);
                return Ok(None);
            }
            Message::CancelRequested => {
                self.canvas.cancel(application);
                return Ok(None);
            }
            Message::OpenRequested => Outcome::OpenRequested,
            Message::SaveRequested => Outcome::SaveRequested,
            Message::DialogRequested(id) => Outcome::DialogRequested(id),
            Message::OutputDirectoryChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    crate::generated::edit_workflowsannotateoutputdir(draft, value)
                })?,
            ),
            Message::Sidebar(message) => {
                let Some(outcome) = self.sidebar.update(&application.annotation, message)? else {
                    return Ok(None);
                };
                match outcome {
                    sidebar::Outcome::EditRequested(mut request) => {
                        if let crate::generated::AnnotationEdit::AnnotationMaskCleanupEdit(edit) =
                            &mut request.edit
                        {
                            edit.radius = settings.draft.as_ref().map_or_else(
                                || crate::generated::default_uimaskcleanupradius().unwrap(),
                                |draft| draft.ui.maskcleanupradius,
                            ) as u16;
                        }
                        Outcome::EditRequested(request)
                    }
                    sidebar::Outcome::BrushRadiusChanged(value) => Outcome::SettingsEdited(
                        settings.edit(crate::view::settings::EditCadence::Immediate, |draft| {
                            crate::generated::edit_uiannotationbrushradius(draft, value)
                        })?,
                    ),
                    sidebar::Outcome::StopRequested => Outcome::StopRequested,
                }
            }
            Message::Timeline(message) => match self.timeline.update(message) {
                timeline::Outcome::EditRequested(request) => Outcome::EditRequested(request),
            },
            Message::Workspace(message) => match workspace::update(message) {
                workspace::Outcome::Gesture(gesture) => {
                    if gesture.kind == crate::presentation_surface::SurfaceGestureKind::Pointer {
                        self.keyboard_canvas
                            .store(true, std::sync::atomic::Ordering::Relaxed);
                    }
                    return Ok(None);
                }
                workspace::Outcome::AspectSelected(aspect) => {
                    Outcome::SettingsEdited(workspace::edit_aspect(settings, aspect)?)
                }
            },
        };
        Ok(Some(outcome))
    }

    // CLEANUP-IGNORE: Annotation owns a pointer-aware component entry point.
    pub fn view<'a>(
        &'a self,
        model: &'a ApplicationModel,
        settings: &'a crate::view::settings::SettingsModel,
        surface: Option<Surface>,
        width: f32,
    ) -> Element<'a, Message> {
        let surface = surface.map(|mut surface| {
            surface.fit_revision = surface.fit_revision.wrapping_add(self.fit_revision);
            surface
        });
        let composition =
            crate::view::workflow::Composition::new(crate::generated::FeatureId::Annotate, width);
        let settings_edit_available = settings.draft.is_some() && model.settings_edit_available();
        let settings_settled = !settings.has_local_edits();
        let draft = settings
            .draft
            .as_ref()
            .map(|settings| &settings.workflows.annotate);
        let dialogs = crate::generated::FILE_DIALOGS
            .iter()
            .filter(|fact| {
                fact.workflows
                    .contains(&crate::generated::FeatureId::Annotate)
                    && fact.stable_field_id
                        == crate::generated::constraint_workflowsannotateoutputdir().stable_field_id
            })
            .fold(column![].spacing(7), |column, fact| {
                column.push(
                    container(
                        button(fact.title)
                            .on_press_maybe(
                                model
                                    .file_dialog_open_available(
                                        fact,
                                        crate::generated::FeatureId::Annotate,
                                    )
                                    .then_some(Message::DialogRequested(fact.stable_field_id)),
                            )
                            .style(crate::fluent_theme::button_secondary),
                    )
                    .id(format!("dialog.{}", fact.stable_field_id)),
                )
            });
        let setup: Element<'a, Message> = column![
            shared::card(
                "Annotation source",
                "Open an image from Explore, or continue editing the transferred image.",
                text(
                    model
                        .explore
                        .snapshot
                        .as_ref()
                        .filter(|snapshot| {
                            snapshot.mode == crate::generated::ExploreMode::Detail
                                && snapshot.selectedimage.is_some()
                        })
                        .map_or_else(
                            || "Explore detail source unavailable".to_owned(),
                            |snapshot| format!(
                                "{:?} {} · frame {} · {} × {}",
                                snapshot.frame.source.kind,
                                snapshot.frame.source.instance,
                                snapshot.frame.revision,
                                snapshot.frame.extent.width,
                                snapshot.frame.extent.height
                            ),
                        )
                ),
            ),
            column![
                text("Annotation output directory or .cbor file").size(12),
                iced::widget::text_input(
                    "Annotation output",
                    draft.map_or("", |value| value.outputdir.as_str())
                )
                .id(crate::generated::constraint_workflowsannotateoutputdir()
                    .stable_field_id
                    .to_string())
                .on_focus(Message::TextFocused)
                .on_input_maybe(settings_edit_available.then_some(Message::OutputDirectoryChanged))
            ],
            dialogs,
            button("Open")
                .on_press_maybe(
                    (settings_settled && model.annotation_open_available())
                        .then_some(Message::OpenRequested)
                )
                .style(crate::fluent_theme::button_secondary),
            crate::view::workflow::primary_action(
                crate::generated::FeatureId::Annotate,
                model.primary_action_active(crate::generated::FeatureId::Annotate),
                (settings_settled && model.annotation_save_available())
                    .then_some(Message::SaveRequested),
                None,
                column![].into(),
            ),
        ]
        .spacing(crate::view::workflow::SECTION_SPACING)
        .into();
        let aspect = settings.draft.as_ref().map_or(
            crate::generated::WorkspaceAspectRatio::Widescreen,
            |draft| draft.ui.workspaceaspectratio,
        );
        let workspace: Element<'a, Message> = container(canvas::view(
            surface,
            aspect,
            settings_edit_available,
            composition.center_width(),
            self.canvas.binding(
                model,
                settings.draft.as_ref().map_or_else(
                    || crate::generated::default_uiannotationbrushradius().unwrap(),
                    |draft| draft.ui.annotationbrushradius,
                ) as u16,
            ),
            self.keyboard_canvas.clone(),
            crate::workspace_fps::enabled(settings),
        ))
        .into();
        let advanced = self
            .timeline
            .view(&model.annotation, model.annotation_edit_available())
            .map(Message::Timeline);
        let diagnostics = self
            .sidebar
            .view(
                &model.annotation,
                settings,
                model.annotation_edit_available(),
                settings_edit_available,
                model.annotation_stop_available(),
            )
            .map(Message::Sidebar);
        let image = column![
            iced::widget::row![button("Fit image (F)").on_press(Message::FitRequested),
                button("Cancel gesture (Esc)").on_press(Message::CancelRequested)]
                .spacing(8)
                .width(iced::Fill)
                .wrap()
                .vertical_spacing(8),
            text("V select · B rectangle · P paint · E erase · G point · S spline · K skeleton · Ctrl+Z undo · Ctrl+Shift+Z redo · Ctrl+S save · Wheel zoom / middle-drag pan").size(12),
            workspace
        ].spacing(8);
        crate::view::workflow::Regions::new(
            crate::generated::FeatureId::Annotate,
            setup,
            image.into(),
            column![text("Advanced"), advanced].spacing(8).into(),
            diagnostics,
        )
        .render(width)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ready_model() -> ApplicationModel {
        let mut model = ApplicationModel::default();
        let snapshots = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .map(|fact| fact.value)
            .collect();
        model
            .install_bootstrap(crate::generated::SCHEMA_FINGERPRINT, snapshots)
            .unwrap();
        let annotation = model.annotation.snapshot.as_mut().unwrap();
        annotation.ready = true;
        annotation.busy = false;
        annotation.revision = 1;
        annotation.uirevision = 1;
        annotation.inputdocumentepoch = 1;
        annotation.ui.documentrevision = 1;
        annotation.frame = crate::generated::VisualFrame {
            source: crate::generated::PresentationSourceIdentity {
                kind: crate::generated::PresentationSourceKind::Annotation,
                instance: 1,
            },
            extent: crate::generated::VisualExtent {
                width: 640,
                height: 480,
            },
            revision: 1,
            cleanrevision: 1,
            content: crate::generated::VisualRegion {
                x: 0,
                y: 0,
                width: 640,
                height: 480,
            },
        };
        model
    }

    fn gesture(
        kind: crate::presentation_surface::SurfaceGestureKind,
    ) -> crate::presentation_surface::SurfaceGesture {
        crate::presentation_surface::SurfaceGesture {
            kind,
            sample: crate::presentation_surface::SurfaceSample {
                width: 640,
                height: 480,
                x: 20,
                y: 30,
                content_x: 20.0,
                content_y: 30.0,
                pressed: true,
            },
        }
    }

    fn assert_gesture_suppressed(
        component: &mut Component,
        model: &mut ApplicationModel,
        settings: &mut crate::view::settings::SettingsModel,
        kind: crate::presentation_surface::SurfaceGestureKind,
    ) {
        assert!(
            component
                .update(
                    model,
                    settings,
                    Message::Workspace(workspace::Message::Gesture(gesture(kind))),
                )
                .unwrap()
                .is_none()
        );
    }

    #[test]
    fn page_maps_sidebar_domain_outcomes_without_owning_raw_messages() {
        let mut model = ApplicationModel::default();
        let mut component = Component::default();
        let mut settings = crate::view::settings::SettingsModel::default();
        assert!(matches!(
            component
                .update(
                    &mut model,
                    &mut settings,
                    Message::Sidebar(sidebar::Message::UndoRequested),
                )
                .unwrap(),
            Some(Outcome::EditRequested(_))
        ));
    }

    #[test]
    fn unavailable_and_non_pointer_workspace_gestures_remain_local() {
        let mut unavailable = ApplicationModel::default();
        let mut component = Component::default();
        let mut settings = crate::view::settings::SettingsModel::default();
        assert_gesture_suppressed(
            &mut component,
            &mut unavailable,
            &mut settings,
            crate::presentation_surface::SurfaceGestureKind::Pointer,
        );

        let unavailable_configurations: [fn(&mut ApplicationModel); 3] = [
            |model: &mut ApplicationModel| {
                model
                    .annotation
                    .snapshot
                    .as_mut()
                    .unwrap()
                    .ui
                    .documentrevision = 0;
            },
            |model: &mut ApplicationModel| {
                model
                    .annotation
                    .snapshot
                    .as_mut()
                    .unwrap()
                    .cancellationrequested = true;
            },
            |model: &mut ApplicationModel| {
                model
                    .begin_intent(crate::view_model::ApplicationIntentEndpoint::AnnotationEdit)
                    .unwrap();
            },
        ];
        for configure in unavailable_configurations {
            let mut unavailable = ready_model();
            configure(&mut unavailable);
            let mut component = Component::default();
            assert_gesture_suppressed(
                &mut component,
                &mut unavailable,
                &mut settings,
                crate::presentation_surface::SurfaceGestureKind::Pointer,
            );
        }

        let mut ready = ready_model();
        assert_gesture_suppressed(
            &mut component,
            &mut ready,
            &mut settings,
            crate::presentation_surface::SurfaceGestureKind::Viewport,
        );
    }

    #[test]
    fn absent_snapshot_rebase_resets_the_complete_inspector_projection() {
        let mut component = Component::default();
        component.sidebar.stage_component_reset_fixture();
        assert!(component.sidebar.has_component_reset_fixture());

        component.rebase(&ApplicationModel::default());
        assert!(component.sidebar.is_default_projection());

        let model = ready_model();
        component.rebase(&model);
        assert!(!component.sidebar.has_component_reset_fixture());
    }
}

pub(crate) fn shortcuts() -> iced::Subscription<Message> {
    iced::event::listen_with(|event, status, _| {
        if status == iced::event::Status::Captured {
            return None;
        }
        let iced::Event::Keyboard(iced::keyboard::Event::KeyPressed {
            key,
            modifiers,
            repeat: false,
            ..
        }) = event
        else {
            return None;
        };
        use iced::keyboard::{Key, key::Named};
        if key == Key::Named(Named::Escape) {
            return Some(Message::Shortcut(Shortcut::Cancel));
        }
        if key == Key::Named(Named::Delete) {
            return Some(Message::Shortcut(Shortcut::Delete));
        }
        let Key::Character(key) = key else {
            return None;
        };
        let key = key.to_lowercase();
        if modifiers.control() {
            return match key.as_str() {
                "z" if modifiers.shift() => Some(Message::Shortcut(Shortcut::Redo)),
                "z" => Some(Message::Shortcut(Shortcut::Undo)),
                "s" => Some(Message::Shortcut(Shortcut::Save)),
                _ => None,
            };
        }
        use crate::generated::AnnotationTool as Tool;
        let tool = match key.as_str() {
            "f" => return Some(Message::Shortcut(Shortcut::Fit)),
            "v" => Tool::Select,
            "b" => Tool::Box,
            "p" => Tool::MaskPaint,
            "e" => Tool::MaskErase,
            "g" => Tool::Point,
            "s" => Tool::Spline,
            "k" => Tool::Skeleton,
            _ => return None,
        };
        Some(Message::Shortcut(Shortcut::Tool(tool)))
    })
}
