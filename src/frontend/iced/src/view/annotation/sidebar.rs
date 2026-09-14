use crate::fluent_theme::Element;
use crate::generated::{
    AnnotationColorRange, AnnotationEdit, AnnotationEditRequest, AnnotationHandleRole,
    AnnotationMaskColorsEdit, AnnotationObject, AnnotationPoint, AnnotationPointerTarget,
    AnnotationShape, AnnotationSplineHandleEdit, AnnotationSplineHandleMode,
};
use iced::Fill;
use iced::widget::{button, checkbox, column, container, row, text, text_input};

#[derive(Debug, Clone)]
pub enum Message {
    TextFocused,
    ToolSelected(crate::generated::AnnotationTool),
    Sidebar(crate::generated::AnnotationSidebarCommand),
    HoldChanged(bool),
    BrushRadiusChanged(i32),
    ObjectSelected(u16),
    ClassSelected(u16),
    CategoryDraftChanged(String),
    CategoryApplied,
    SelectedObjectEnabled(bool),
    SelectedObjectApplied(u16),
    SplineSelected(u16),
    HandleSelected(AnnotationHandleRole),
    HandleModeChanged(AnnotationSplineHandleMode),
    HandleXChanged(String),
    HandleYChanged(String),
    HandleApplied,
    SkeletonSelected(u16),
    MaskCleanup(crate::generated::AnnotationMaskCleanup),
    MaskSamplingChanged(bool),
    MaskChannelChanged {
        supported: bool,
        part: usize,
        channel: usize,
        value: String,
    },
    MaskColorsRequested,
    SceneResetRequested,
    UndoRequested,
    RedoRequested,
    StopRequested,
}

#[derive(Debug, Clone)]
pub(super) enum Outcome {
    EditRequested(AnnotationEditRequest),
    BrushRadiusChanged(i32),
    StopRequested,
}

pub(super) fn tool_id(tool: crate::generated::AnnotationTool) -> String {
    format!("annotation.tool.{tool:?}").to_ascii_lowercase()
}

#[derive(Debug, Clone)]
pub struct ColorRangeDraft {
    pub center: [String; 3],
    pub minus: [String; 3],
    pub plus: [String; 3],
    pub sampling: bool,
}

impl ColorRangeDraft {
    fn from_generated(value: &AnnotationColorRange) -> Self {
        let color = |value: &crate::generated::AnnotationColor| {
            [
                value.hue.to_string(),
                value.saturation.to_string(),
                value.value.to_string(),
            ]
        };
        Self {
            center: color(&value.center),
            minus: color(&value.minus),
            plus: color(&value.plus),
            sampling: value.sampling,
        }
    }

    fn generated(&self) -> Option<AnnotationColorRange> {
        let color = |value: &[String; 3]| {
            Some(crate::generated::AnnotationColor {
                hue: value[0].parse().ok()?,
                saturation: value[1].parse().ok()?,
                value: value[2].parse().ok()?,
            })
        };
        Some(AnnotationColorRange {
            center: color(&self.center)?,
            minus: color(&self.minus)?,
            plus: color(&self.plus)?,
            sampling: self.sampling,
        })
    }
}

#[derive(Debug)]
pub(super) struct Component {
    selected_object: Option<u16>,
    selected_element: Option<u16>,
    selected_role: Option<AnnotationHandleRole>,
    pub category_draft: String,
    pub selected_enabled: bool,
    pub handle_x_draft: String,
    pub handle_y_draft: String,
    pub handle_mode: AnnotationSplineHandleMode,
    pub mask_sup_draft: Option<ColorRangeDraft>,
    pub mask_nosup_draft: Option<ColorRangeDraft>,
    selected_enabled_dirty: bool,
    handle_dirty: bool,
    mask_dirty: bool,
}

impl Default for Component {
    fn default() -> Self {
        Self {
            selected_object: None,
            selected_element: None,
            selected_role: None,
            category_draft: String::new(),
            selected_enabled: true,
            handle_x_draft: String::new(),
            handle_y_draft: String::new(),
            handle_mode: AnnotationSplineHandleMode::Corner,
            mask_sup_draft: None,
            mask_nosup_draft: None,
            selected_enabled_dirty: false,
            handle_dirty: false,
            mask_dirty: false,
        }
    }
}

impl Component {
    pub(super) fn update(
        &mut self,
        model: &crate::view_model::AnnotationModel,
        message: Message,
    ) -> Result<Option<Outcome>, String> {
        let edit = match message {
            Message::TextFocused => return Ok(None),
            Message::ClassSelected(category) => {
                AnnotationEdit::AnnotationClassEdit(crate::generated::AnnotationClassEdit {
                    category,
                })
            }
            Message::ToolSelected(tool) => {
                AnnotationEdit::AnnotationToolEdit(crate::generated::AnnotationToolEdit { tool })
            }
            Message::Sidebar(command) => {
                AnnotationEdit::AnnotationSidebarEdit(crate::generated::AnnotationSidebarEdit {
                    command,
                })
            }
            Message::HoldChanged(enabled) => {
                AnnotationEdit::AnnotationHoldEdit(crate::generated::AnnotationHoldEdit { enabled })
            }
            Message::BrushRadiusChanged(value) => {
                return Ok(Some(Outcome::BrushRadiusChanged(value)));
            }
            Message::ObjectSelected(object) => {
                AnnotationEdit::AnnotationObjectEdit(crate::generated::AnnotationObjectEdit {
                    object,
                })
            }
            Message::CategoryDraftChanged(value) => {
                self.category_draft = value;
                return Ok(None);
            }
            Message::CategoryApplied => {
                AnnotationEdit::AnnotationCategoryEdit(crate::generated::AnnotationCategoryEdit {
                    category: crate::generated::AnnotationText::try_from(
                        self.category_draft.as_str(),
                    )?,
                })
            }
            Message::SelectedObjectEnabled(enabled) => {
                self.selected_enabled = enabled;
                self.mark_selected_enabled_dirty();
                return Ok(None);
            }
            Message::SelectedObjectApplied(category) => {
                AnnotationEdit::AnnotationSelectedObjectEdit(
                    crate::generated::AnnotationSelectedObjectEdit {
                        category,
                        enabled: self.selected_enabled,
                    },
                )
            }
            Message::SplineSelected(segment) => {
                AnnotationEdit::AnnotationSplineEdit(crate::generated::AnnotationSplineEdit {
                    segment,
                })
            }
            Message::HandleSelected(role) => {
                self.select_handle(model, role);
                return Ok(None);
            }
            Message::HandleModeChanged(mode) => {
                self.handle_mode = mode;
                self.mark_handle_dirty();
                return Ok(None);
            }
            Message::HandleXChanged(value) => {
                self.handle_x_draft = value;
                self.mark_handle_dirty();
                return Ok(None);
            }
            Message::HandleYChanged(value) => {
                self.handle_y_draft = value;
                self.mark_handle_dirty();
                return Ok(None);
            }
            Message::HandleApplied => {
                return self
                    .handle_edit(model)
                    .map(|request| Some(Outcome::EditRequested(request)))
                    .ok_or_else(|| {
                        "Select a spline handle and enter valid coordinates before applying it."
                            .to_owned()
                    });
            }
            Message::SkeletonSelected(joint) => {
                AnnotationEdit::AnnotationSkeletonEdit(crate::generated::AnnotationSkeletonEdit {
                    joint,
                })
            }
            Message::MaskCleanup(operation) => AnnotationEdit::AnnotationMaskCleanupEdit(
                crate::generated::AnnotationMaskCleanupEdit {
                    operation,
                    radius: crate::generated::default_uimaskcleanupradius().unwrap() as u16,
                },
            ),
            Message::MaskSamplingChanged(value) => {
                if let (Some(sup), Some(nosup)) =
                    (&mut self.mask_sup_draft, &mut self.mask_nosup_draft)
                {
                    sup.sampling = value;
                    nosup.sampling = value;
                }
                self.mark_mask_dirty();
                return Ok(None);
            }
            Message::MaskChannelChanged {
                supported,
                part,
                channel,
                value,
            } => {
                let draft = if supported {
                    &mut self.mask_sup_draft
                } else {
                    &mut self.mask_nosup_draft
                };
                let Some(draft) = draft.as_mut() else {
                    return Err("Select a mask object before editing its colors.".to_owned());
                };
                let channels = match part {
                    0 => &mut draft.center,
                    1 => &mut draft.minus,
                    2 => &mut draft.plus,
                    _ => return Err("Mask color range is unavailable.".to_owned()),
                };
                let Some(channel) = channels.get_mut(channel) else {
                    return Err("Mask color channel is unavailable.".to_owned());
                };
                *channel = value;
                self.mark_mask_dirty();
                return Ok(None);
            }
            Message::MaskColorsRequested => {
                return self
                    .mask_colors_edit(model)
                    .map(|request| Some(Outcome::EditRequested(request)))
                    .ok_or_else(|| {
                        "Select a mask object before changing its color sampling.".to_owned()
                    });
            }
            Message::SceneResetRequested => {
                model.snapshot.as_ref().ok_or_else(|| {
                    "Open an annotation document before resetting its objects.".to_owned()
                })?;
                AnnotationEdit::AnnotationSceneEdit(crate::generated::AnnotationSceneEdit {})
            }
            Message::UndoRequested => {
                AnnotationEdit::AnnotationUndoEdit(crate::generated::AnnotationUndoEdit {})
            }
            Message::RedoRequested => {
                AnnotationEdit::AnnotationRedoEdit(crate::generated::AnnotationRedoEdit {})
            }
            Message::StopRequested => return Ok(Some(Outcome::StopRequested)),
        };
        Ok(Some(Outcome::EditRequested(AnnotationEditRequest { edit })))
    }

    pub(super) fn rebase(&mut self, model: &crate::view_model::AnnotationModel) {
        let Some(snapshot) = model.snapshot.as_ref() else {
            *self = Self::default();
            return;
        };
        let selection = snapshot
            .ui
            .editor
            .selectedobject
            .filter(|index| usize::from(*index) < snapshot.ui.scene.objects.len());
        let object = selection.and_then(|index| snapshot.ui.scene.objects.get(usize::from(index)));
        let segment = snapshot.ui.editor.selectedsplinesegment.filter(|index| {
            object.is_some_and(|object| {
                object.shape == AnnotationShape::Spline
                    && usize::from(*index) < object.splineknots.len()
            })
        });
        let object_changed = selection != self.selected_object;
        let segment_changed = segment != self.selected_element;
        if object_changed {
            self.selected_role = None;
            self.selected_enabled_dirty = false;
            self.mask_dirty = false;
            self.handle_dirty = false;
            self.category_draft.clear();
        } else if segment_changed {
            self.selected_role = None;
            self.handle_dirty = false;
        }
        self.selected_object = selection;
        self.selected_element = segment;
        let Some(object) = object else {
            self.selected_enabled = true;
            self.mask_sup_draft = None;
            self.mask_nosup_draft = None;
            self.clear_handle();
            return;
        };
        if !self.selected_enabled_dirty || object.enabled == self.selected_enabled {
            self.selected_enabled = object.enabled;
            self.selected_enabled_dirty = false;
        }
        if object.shape == AnnotationShape::Mask {
            let staged = (
                self.mask_sup_draft
                    .as_ref()
                    .and_then(ColorRangeDraft::generated),
                self.mask_nosup_draft
                    .as_ref()
                    .and_then(ColorRangeDraft::generated),
            );
            if !self.mask_dirty || staged == (Some(object.sup.clone()), Some(object.nosup.clone()))
            {
                self.mask_sup_draft = Some(ColorRangeDraft::from_generated(&object.sup));
                self.mask_nosup_draft = Some(ColorRangeDraft::from_generated(&object.nosup));
                self.mask_dirty = false;
            }
        } else {
            self.mask_sup_draft = None;
            self.mask_nosup_draft = None;
            self.mask_dirty = false;
        }
        self.reconcile_handle(object);
    }

    fn clear_handle(&mut self) {
        self.handle_x_draft.clear();
        self.handle_y_draft.clear();
        self.handle_mode = AnnotationSplineHandleMode::Corner;
        self.handle_dirty = false;
    }

    fn reconcile_handle(&mut self, object: &AnnotationObject) {
        let Some((knot, role)) = self
            .selected_element
            .and_then(|index| object.splineknots.get(usize::from(index)))
            .zip(self.selected_role)
        else {
            if self.selected_role.is_none() {
                self.clear_handle();
            }
            return;
        };
        let handle = match role {
            AnnotationHandleRole::SplineInHandle => &knot.in_,
            AnnotationHandleRole::SplineOutHandle => &knot.out,
            _ => return,
        };
        let staged = (
            self.handle_x_draft.parse::<f32>().ok(),
            self.handle_y_draft.parse::<f32>().ok(),
            self.handle_mode,
        );
        if !self.handle_dirty || staged == (Some(handle.point.x), Some(handle.point.y), knot.mode) {
            self.handle_x_draft = handle.point.x.to_string();
            self.handle_y_draft = handle.point.y.to_string();
            self.handle_mode = knot.mode;
            self.handle_dirty = false;
        }
    }

    pub fn selected_object<'a>(
        &self,
        model: &'a crate::view_model::AnnotationModel,
    ) -> Option<(u16, &'a AnnotationObject)> {
        let index = self.selected_object?;
        model
            .snapshot
            .as_ref()?
            .ui
            .scene
            .objects
            .get(usize::from(index))
            .map(|object| (index, object))
    }

    pub fn selected_spline_segment(
        &self,
        model: &crate::view_model::AnnotationModel,
    ) -> Option<u16> {
        let (_, object) = self.selected_object(model)?;
        let segment = self.selected_element?;
        (object.shape == AnnotationShape::Spline && usize::from(segment) < object.splineknots.len())
            .then_some(segment)
    }

    pub fn selected_shape<'a>(
        &self,
        model: &'a crate::view_model::AnnotationModel,
        shape: AnnotationShape,
    ) -> Option<&'a AnnotationObject> {
        self.selected_object(model)
            .map(|(_, object)| object)
            .filter(|object| object.shape == shape)
    }

    pub fn mask_sampling(&self) -> bool {
        self.mask_sup_draft
            .as_ref()
            .is_some_and(|draft| draft.sampling)
    }

    pub fn select_handle(
        &mut self,
        model: &crate::view_model::AnnotationModel,
        role: AnnotationHandleRole,
    ) {
        self.selected_role = matches!(
            role,
            AnnotationHandleRole::SplineInHandle | AnnotationHandleRole::SplineOutHandle
        )
        .then_some(role);
        if let Some((_, object)) = self.selected_object(model) {
            self.reconcile_handle(&object.clone());
        }
    }

    pub fn pointer_target(
        &self,
        model: &crate::view_model::AnnotationModel,
    ) -> AnnotationPointerTarget {
        let Some((object, selected)) = self.selected_object(model) else {
            return AnnotationPointerTarget {
                object: None,
                element: None,
                role: None,
            };
        };
        let element = self.selected_spline_segment(model);
        if selected.shape == AnnotationShape::Spline
            && matches!(
                self.selected_role,
                Some(AnnotationHandleRole::SplineInHandle | AnnotationHandleRole::SplineOutHandle)
            )
        {
            return AnnotationPointerTarget {
                object: Some(object),
                element,
                role: self.selected_role,
            };
        }
        AnnotationPointerTarget {
            object: Some(object),
            element: None,
            role: None,
        }
    }

    pub fn handle_target_available(&self, model: &crate::view_model::AnnotationModel) -> bool {
        let target = self.pointer_target(model);
        target.object.is_some() && target.element.is_some() && target.role.is_some()
    }

    pub fn handle_edit(
        &self,
        model: &crate::view_model::AnnotationModel,
    ) -> Option<AnnotationEditRequest> {
        let target = self.pointer_target(model);
        Some(AnnotationEditRequest {
            edit: AnnotationEdit::AnnotationSplineHandleEdit(AnnotationSplineHandleEdit {
                handle: target.role?,
                mode: self.handle_mode,
                point: AnnotationPoint {
                    x: self.handle_x_draft.parse().ok()?,
                    y: self.handle_y_draft.parse().ok()?,
                },
            }),
        })
    }

    pub fn mask_colors_edit(
        &self,
        model: &crate::view_model::AnnotationModel,
    ) -> Option<AnnotationEditRequest> {
        let (_, object) = self.selected_object(model)?;
        (object.shape == AnnotationShape::Mask).then_some(())?;
        Some(AnnotationEditRequest {
            edit: AnnotationEdit::AnnotationMaskColorsEdit(AnnotationMaskColorsEdit {
                sup: self.mask_sup_draft.as_ref()?.generated()?,
                nosup: self.mask_nosup_draft.as_ref()?.generated()?,
            }),
        })
    }

    pub fn mark_selected_enabled_dirty(&mut self) {
        self.selected_enabled_dirty = true;
    }

    pub fn mark_handle_dirty(&mut self) {
        self.handle_dirty = true;
    }

    pub fn mark_mask_dirty(&mut self) {
        self.mask_dirty = true;
    }

    #[cfg(test)]
    pub(super) fn stage_component_reset_fixture(&mut self) {
        let range = ColorRangeDraft {
            center: ["901".into(), "902".into(), "903".into()],
            minus: ["904".into(), "905".into(), "906".into()],
            plus: ["907".into(), "908".into(), "909".into()],
            sampling: true,
        };
        self.selected_object = Some(u16::MAX);
        self.selected_element = Some(u16::MAX);
        self.selected_role = Some(AnnotationHandleRole::SplineInHandle);
        self.category_draft = "stale-category".into();
        self.selected_enabled = false;
        self.handle_x_draft = "stale-x".into();
        self.handle_y_draft = "stale-y".into();
        self.handle_mode = AnnotationSplineHandleMode::Mirrored;
        self.mask_sup_draft = Some(range.clone());
        self.mask_nosup_draft = Some(range);
        self.selected_enabled_dirty = true;
        self.handle_dirty = true;
        self.mask_dirty = true;
    }

    #[cfg(test)]
    pub(super) fn has_component_reset_fixture(&self) -> bool {
        self.selected_object == Some(u16::MAX)
            || self.selected_element == Some(u16::MAX)
            || self.category_draft == "stale-category"
            || self.handle_x_draft == "stale-x"
            || self.handle_y_draft == "stale-y"
    }

    #[cfg(test)]
    pub(super) fn is_default_projection(&self) -> bool {
        self.selected_object.is_none()
            && self.selected_element.is_none()
            && self.selected_role.is_none()
            && self.category_draft.is_empty()
            && self.selected_enabled
            && self.handle_x_draft.is_empty()
            && self.handle_y_draft.is_empty()
            && self.handle_mode == AnnotationSplineHandleMode::Corner
            && self.mask_sup_draft.is_none()
            && self.mask_nosup_draft.is_none()
            && !self.selected_enabled_dirty
            && !self.handle_dirty
            && !self.mask_dirty
    }

    pub(super) fn view<'a>(
        &'a self,
        model: &'a crate::view_model::AnnotationModel,
        settings: &'a crate::view::settings::SettingsModel,
        available: bool,
        settings_available: bool,
        stop_available: bool,
    ) -> Element<'a, Message> {
        let ui = model.snapshot.as_ref().map(|snapshot| &snapshot.ui);
        let brush_radius = settings.draft.as_ref().map_or(
            crate::generated::default_uiannotationbrushradius().unwrap_or(12),
            |draft| draft.ui.annotationbrushradius,
        );
        let brush_control = container(crate::view::workflow::fields::number_i32(
            "Brush radius",
            brush_radius,
            crate::generated::constraint_uiannotationbrushradius(),
            settings.draft.is_some() && settings_available,
            Message::BrushRadiusChanged,
        ))
        .id("annotation.brush_radius");
        let tools = crate::generated::ANNOTATION_TOOL_VALUES
            .iter()
            .copied()
            .fold(column![].spacing(5), |tools, tool| {
                tools.push(
                    container(
                        button(text(format!(
                            "{}{}",
                            if ui.is_some_and(|state| state.editor.tool == tool) {
                                "● "
                            } else {
                                ""
                            },
                            tool_label(tool)
                        )))
                        .width(Fill)
                        .on_press_maybe(
                            (available && ui.is_some_and(|state| tool_available(state, tool)))
                                .then_some(Message::ToolSelected(tool)),
                        ),
                    )
                    .id(tool_id(tool)),
                )
            });
        let active_class: Element<'a, Message> = ui
            .and_then(|state| {
                let index = state
                    .editor
                    .selectedobject
                    .and_then(|index| {
                        state
                            .scene
                            .objects
                            .get(index as usize)
                            .map(|object| object.category)
                    })
                    .or(state.editor.selectedcategory)? as usize;
                let name = &state.scene.categories.get(index)?.value;
                let color = crate::presentation_surface::labels::class_color(
                    state.scene.palette.get(index)?,
                );
                Some(
                    iced::widget::row![
                        container(iced::widget::space::horizontal())
                            .width(16)
                            .height(16)
                            .style(move |_| iced::widget::container::Style {
                                background: Some(color.into()),
                                ..Default::default()
                            })
                            .id("annotation.class.active.swatch"),
                        text(name).width(Fill)
                    ]
                    .spacing(8)
                    .into(),
                )
            })
            .unwrap_or_else(|| text("Select a class to draw").into());
        let sidebar = crate::generated::ANNOTATION_SIDEBAR_COMMAND_VALUES
            .iter()
            .copied()
            .filter(|command| {
                !matches!(
                    command,
                    crate::generated::AnnotationSidebarCommand::Undo
                        | crate::generated::AnnotationSidebarCommand::Redo
                )
            })
            .filter(|command| ui.is_some_and(|state| sidebar_available(state, *command)))
            .fold(column![].spacing(4), |row, command| {
                row.push(
                    button(text(command_label(command))).on_press_maybe(
                        ui.is_some_and(|state| available && sidebar_available(state, command))
                            .then_some(Message::Sidebar(command)),
                    ),
                )
            });
        let specialized: Element<'a, Message> = ui.map_or_else(
            || column![].into(),
            |state| {
                let selected_object = self.selected_object(model).map(|(_, object)| object);
                let selected_spline = self.selected_shape(model, AnnotationShape::Spline);
                let selected_mask = self.selected_shape(model, AnnotationShape::Mask);
                let selected_skeleton = self.selected_shape(model, AnnotationShape::Skeleton);
                let objects = state.scene.objects.iter().enumerate().fold(
                        column![text(format!("Objects ({})", state.scene.objects.len()))]
                            .spacing(4),
                        |list, (index, object)| {
                            let name = state
                                .scene
                                .categories
                                .get(object.category as usize)
                                .map_or("Unknown class", |name| name.value.as_str());
                            let swatch = state
                                .scene
                                .palette
                                .get(object.category as usize)
                                .map(crate::presentation_surface::labels::class_color)
                                .unwrap_or(iced::Color::WHITE);
                            list.push(
                                container(
                                    button(
                                        row![
                                            text("●").color(swatch),
                                            text(format!(
                                                "{}{} · {}{}",
                                                if state.editor.selectedobject == Some(index as u16)
                                                {
                                                    "✓ "
                                                } else {
                                                    ""
                                                },
                                                name,
                                                shape_label(object.shape),
                                                if object.enabled { "" } else { " (hidden)" }
                                            )).width(Fill)
                                        ]
                                        .spacing(6),
                                    )
                                    .width(Fill)
                                    .on_press_maybe(
                                        available.then_some(Message::ObjectSelected(index as u16)),
                                    ),
                                )
                                .id(format!("annotation.object.{index}")),
                            )
                        },
                    );
                let categories =
                    state.scene.categories.iter().enumerate().fold(
                        column![text("Class for new objects")].spacing(4),
                        |list, (index, name)| {
                            let swatch = state
                                .scene
                                .palette
                                .get(index)
                                .map(crate::presentation_surface::labels::class_color)
                                .unwrap_or(iced::Color::WHITE);
                            list.push(
                                container(
                                    button(
                                        row![
                                            text("●").color(swatch),
                                            text(format!(
                                                "{}{}",
                                                if state.editor.selectedcategory
                                                    == Some(index as u16)
                                                {
                                                    "✓ "
                                                } else {
                                                    ""
                                                },
                                                name.value
                                            )).width(Fill)
                                        ]
                                        .spacing(6),
                                    )
                                    .width(Fill)
                                    .on_press_maybe(
                                        available.then_some(Message::ClassSelected(index as u16)),
                                    ),
                                )
                                .id(format!("annotation.class.{index}")),
                            )
                        },
                    );
                let reclassify = button("Apply current class to selected object")
                    .width(Fill)
                    .on_press_maybe(
                        state
                            .editor
                            .selectedcategory
                            .filter(|_| available && selected_object.is_some())
                            .map(Message::SelectedObjectApplied),
                    );
                let spline_segments = selected_spline.map_or_else(
                    || column![].spacing(4),
                    |object| {
                        object.splineknots.iter().enumerate().fold(
                            column![].spacing(4),
                            |row, (index, _)| {
                                row.push(button(text(format!("Knot {index}"))).on_press_maybe(
                                    available.then_some(Message::SplineSelected(index as u16)),
                                ))
                            },
                        )
                    },
                );
                let handle_roles = crate::generated::ANNOTATION_HANDLE_ROLE_VALUES
                    .iter()
                    .copied()
                    .fold(column![].spacing(4), |row, role| {
                        let supported = matches!(
                            role,
                            AnnotationHandleRole::SplineInHandle
                                | AnnotationHandleRole::SplineOutHandle
                        );
                        row.push(
                            button(text(handle_label(role))).on_press_maybe(
                                (available && selected_spline.is_some() && supported)
                                    .then_some(Message::HandleSelected(role)),
                            ),
                        )
                    });
                let handle_modes = crate::generated::ANNOTATION_SPLINE_HANDLE_MODE_VALUES
                    .iter()
                    .copied()
                    .fold(column![].spacing(4), |row, mode| {
                        row.push(
                            button(text(mode_label(mode))).on_press_maybe(
                                (available
                                    && selected_spline.is_some()
                                    && self.selected_spline_segment(model).is_some()
                                    && self.handle_target_available(model))
                                .then_some(Message::HandleModeChanged(mode)),
                            ),
                        )
                    });
                let skeleton_nodes = selected_skeleton.map_or_else(
                    || column![].spacing(4),
                    |object| {
                        object.skeletonnodes.iter().enumerate().fold(
                            column![].spacing(4),
                            |row, (index, _)| {
                                row.push(button(text(format!("Joint {index}"))).on_press_maybe(
                                    available.then_some(Message::SkeletonSelected(index as u16)),
                                ))
                            },
                        )
                    },
                );
                let cleanup = crate::generated::ANNOTATION_MASK_CLEANUP_VALUES
                    .iter()
                    .copied()
                    .fold(column![].spacing(4), |row, operation| {
                        row.push(
                            button(text(cleanup_label(operation))).on_press_maybe(
                                (available && selected_mask.is_some())
                                    .then_some(Message::MaskCleanup(operation)),
                            ),
                        )
                    });
                column![
                    objects,
                    categories,
                    reclassify,
                    spline_segments,
                    handle_roles,
                    handle_modes,
                    column![
                        text_input("Handle x", &self.handle_x_draft)
                            .on_focus(Message::TextFocused)
                            .on_input(Message::HandleXChanged),
                        text_input("Handle y", &self.handle_y_draft)
                            .on_focus(Message::TextFocused)
                            .on_input(Message::HandleYChanged),
                        button("Apply handle").on_press_maybe(
                            (available && self.handle_target_available(model))
                                .then_some(Message::HandleApplied)
                        ),
                    ]
                    .spacing(4),
                    skeleton_nodes,
                    cleanup,
                    color_range_controls("Supported range", self.mask_sup_draft.as_ref(), true),
                    color_range_controls(
                        "Unsupported range",
                        self.mask_nosup_draft.as_ref(),
                        false
                    ),
                    column![
                        text_input("New category", &self.category_draft)
                            .on_focus(Message::TextFocused)
                            .on_input(Message::CategoryDraftChanged),
                        button("Add category").on_press_maybe(
                            (available && !self.category_draft.is_empty())
                                .then_some(Message::CategoryApplied)
                        ),
                        checkbox(self.selected_enabled)
                            .label("Selected object enabled")
                            .on_toggle(Message::SelectedObjectEnabled),
                    ]
                    .spacing(4),
                    column![
                        checkbox(self.mask_sampling())
                            .label("Sample mask colors")
                            .on_toggle(Message::MaskSamplingChanged),
                        button("Mask colors").on_press_maybe(
                            (available && selected_mask.is_some())
                                .then_some(Message::MaskColorsRequested)
                        ),
                        button("Clear scene objects").on_press_maybe(
                            (available && !state.scene.objects.is_empty())
                                .then_some(Message::SceneResetRequested)
                        ),
                    ]
                    .spacing(4),
                ]
                .spacing(5)
                .into()
            },
        );
        container(crate::view::shared::card(
            "Annotation tools",
            "Select an object on the image. Drag its box to move; drag a corner to resize.",
            column![
                active_class,
                tools,
                row![
                    container(
                        button("Undo").on_press_maybe(
                            (available && ui.is_some_and(|state| state.canundo))
                                .then_some(Message::UndoRequested)
                        )
                    )
                    .id("annotation.undo"),
                    container(
                        button("Redo").on_press_maybe(
                            (available && ui.is_some_and(|state| state.canredo))
                                .then_some(Message::RedoRequested)
                        )
                    )
                    .id("annotation.redo"),
                    button(ui.map_or("Hold", |state| if state.editor.holdsave {
                        "Release hold"
                    } else {
                        "Hold"
                    }))
                    .on_press_maybe(ui.and_then(|state| {
                        available.then_some(Message::HoldChanged(!state.editor.holdsave))
                    })),
                ]
                .spacing(6)
                .width(Fill)
                .wrap()
                .vertical_spacing(6),
                sidebar,
                brush_control,
                text(ui.map_or_else(
                    || "No document open".into(),
                    |state| format!(
                        "{} objects · {} classes · revision {}",
                        state.scene.objects.len(),
                        state.scene.categories.len(),
                        state.documentrevision
                    )
                )),
                text(ui.map_or("Open an image to begin", |state| tool_hint(
                    state.editor.tool
                )))
                .size(12),
                specialized,
                container(
                    text(model.snapshot.as_ref().map_or_else(
                        || "Operation unavailable".to_owned(),
                        |snapshot| format!(
                            "{} · {:?}",
                            if snapshot.cancellationrequested {
                                "stopping"
                            } else if snapshot.busy {
                                "active"
                            } else if snapshot.ui.documentrevision != snapshot.ui.savedrevision {
                                "Unsaved changes"
                            } else {
                                "All changes saved"
                            },
                            snapshot.ui.savestatus
                        )
                    ))
                    .size(12)
                )
                .id("annotation.operation"),
                container(
                    button("Stop")
                        .on_press_maybe(stop_available.then_some(Message::StopRequested))
                        .width(Fill)
                )
                .id("annotation.stop")
                .width(Fill),
            ]
            .spacing(8),
        ))
        .id(super::SIDEBAR_ID)
        .into()
    }
}

fn color_range_controls<'a>(
    title: &'static str,
    draft: Option<&'a ColorRangeDraft>,
    supported: bool,
) -> Element<'a, Message> {
    let Some(draft) = draft else {
        return column![].into();
    };
    [
        ("Center", &draft.center),
        ("Minus", &draft.minus),
        ("Plus", &draft.plus),
    ]
    .into_iter()
    .enumerate()
    .fold(
        column![text(title)].spacing(3),
        |column, (part, (label, values))| {
            column.push(["hue", "saturation", "value"].into_iter().enumerate().fold(
                row![text(label)].spacing(4),
                |row, (channel, label)| {
                    row.push(
                        text_input(label, &values[channel])
                            .on_focus(Message::TextFocused)
                            .on_input(move |value| Message::MaskChannelChanged {
                                supported,
                                part,
                                channel,
                                value,
                            }),
                    )
                },
            ))
        },
    )
    .into()
}

fn selected_shape(state: &crate::generated::AnnotationUiState, shape: AnnotationShape) -> bool {
    state
        .editor
        .selectedobject
        .and_then(|index| state.scene.objects.get(usize::from(index)))
        .is_some_and(|object| object.shape == shape)
}

fn sidebar_available(
    state: &crate::generated::AnnotationUiState,
    command: crate::generated::AnnotationSidebarCommand,
) -> bool {
    use crate::generated::AnnotationSidebarCommand;
    match command {
        AnnotationSidebarCommand::Assist => state.editor.assistavailable,
        AnnotationSidebarCommand::Delete | AnnotationSidebarCommand::Duplicate => state
            .editor
            .selectedobject
            .is_some_and(|index| state.scene.objects.get(usize::from(index)).is_some()),
        AnnotationSidebarCommand::Undo => state.canundo,
        AnnotationSidebarCommand::Redo => state.canredo,
        AnnotationSidebarCommand::RedrawBox => {
            selected_shape(state, AnnotationShape::Box)
                && state.editor.tool != crate::generated::AnnotationTool::Box
        }
        AnnotationSidebarCommand::SplineInsertKnot
        | AnnotationSidebarCommand::SplineClose
        | AnnotationSidebarCommand::SplineReopen => selected_shape(state, AnnotationShape::Spline),
        AnnotationSidebarCommand::SplineDeleteKnot => {
            selected_shape(state, AnnotationShape::Spline)
                && state.editor.selectedsplinesegment.is_some()
        }
        AnnotationSidebarCommand::SkeletonSkip
        | AnnotationSidebarCommand::SkeletonHide
        | AnnotationSidebarCommand::SkeletonShow
        | AnnotationSidebarCommand::SkeletonReseed => {
            selected_shape(state, AnnotationShape::Skeleton)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn color(hue: f32, saturation: f32, value: f32) -> crate::generated::AnnotationColor {
        crate::generated::AnnotationColor {
            hue,
            saturation,
            value,
        }
    }

    fn color_range() -> AnnotationColorRange {
        AnnotationColorRange {
            center: color(10.0, 0.1, 0.2),
            minus: color(20.0, 0.3, 0.4),
            plus: color(30.0, 0.5, 0.6),
            sampling: true,
        }
    }

    fn object(shape: AnnotationShape, enabled: bool) -> AnnotationObject {
        let point = AnnotationPoint { x: 1.0, y: 2.0 };
        AnnotationObject {
            name: crate::generated::AnnotationText::try_from("object").unwrap(),
            shape,
            box_: crate::generated::AnnotationBox {
                first: point.clone(),
                second: point.clone(),
            },
            point: point.clone(),
            mask: crate::generated::AnnotationMask {
                runs: Vec::new(),
                cleanupradius: 0,
                cleanup: crate::generated::AnnotationMaskCleanup::LargestComponent,
                present: shape == AnnotationShape::Mask,
            },
            sup: color_range(),
            nosup: color_range(),
            maskpoints: Vec::new(),
            splineknots: vec![crate::generated::AnnotationSplineKnot {
                point,
                in_: crate::generated::AnnotationSplineHandle {
                    point: AnnotationPoint { x: 3.0, y: 4.0 },
                    enabled: true,
                },
                out: crate::generated::AnnotationSplineHandle {
                    point: AnnotationPoint { x: 5.0, y: 6.0 },
                    enabled: true,
                },
                mode: AnnotationSplineHandleMode::Smooth,
            }],
            skeletonnodes: Vec::new(),
            skeletonedges: Vec::new(),
            category: 0,
            splineclosed: false,
            enabled,
        }
    }

    fn model_with_objects() -> crate::view_model::AnnotationModel {
        let mut snapshot = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .find_map(|fact| match fact.value {
                crate::generated::ApplicationSnapshot::Annotation(value) => Some(value),
                _ => None,
            })
            .unwrap();
        snapshot.revision = 1;
        snapshot.ui.scene.objects = vec![
            object(AnnotationShape::Mask, true),
            object(AnnotationShape::Spline, false),
        ];
        snapshot.ui.editor.selectedobject = Some(0);
        let mut model = crate::view_model::AnnotationModel::default();
        model.snapshot = Some(snapshot);
        model
    }

    #[test]
    fn color_range_draft_round_trips_and_rejects_invalid_channels() {
        let generated = color_range();
        let mut draft = ColorRangeDraft::from_generated(&generated);
        draft.center[0] = "77".into();
        assert_eq!(draft.generated().unwrap().center.hue, 77.0);
        draft.plus[2] = "invalid".into();
        assert!(draft.generated().is_none());
    }

    #[test]
    fn selection_change_rebases_mask_and_handle_drafts() {
        let mut model = model_with_objects();
        let mut inspector = Component::default();
        inspector.rebase(&model);
        assert_eq!(
            inspector.selected_object(&model).map(|(index, _)| index),
            Some(0)
        );
        inspector.mask_sup_draft.as_mut().unwrap().center[0] = "77".into();
        inspector.mark_mask_dirty();
        inspector.rebase(&model);
        assert_eq!(
            inspector
                .mask_sup_draft
                .as_ref()
                .unwrap()
                .generated()
                .unwrap()
                .center
                .hue,
            77.0
        );

        let snapshot = model.snapshot.as_mut().unwrap();
        snapshot.revision += 1;
        snapshot.ui.editor.selectedobject = Some(1);
        snapshot.ui.editor.selectedsplinesegment = Some(0);
        inspector.rebase(&model);
        assert!(inspector.mask_sup_draft.is_none());
        inspector.select_handle(&model, AnnotationHandleRole::SplineInHandle);
        inspector.handle_x_draft = "13".into();
        inspector.handle_y_draft = "17".into();
        inspector.handle_mode = AnnotationSplineHandleMode::Mirrored;
        inspector.mark_handle_dirty();
        let AnnotationEdit::AnnotationSplineHandleEdit(edit) =
            inspector.handle_edit(&model).unwrap().edit
        else {
            panic!("typed handle edit")
        };
        assert_eq!(edit.point, AnnotationPoint { x: 13.0, y: 17.0 });
    }

    #[test]
    fn matching_authoritative_values_clear_dirt_and_reconnect_clears_selection() {
        let model = model_with_objects();
        let mut inspector = Component::default();
        inspector.rebase(&model);
        inspector.selected_enabled = false;
        inspector.mark_selected_enabled_dirty();
        inspector.rebase(&model);
        assert!(!inspector.selected_enabled);

        let mut acknowledged = model.clone();
        acknowledged.snapshot.as_mut().unwrap().revision += 1;
        acknowledged.snapshot.as_mut().unwrap().ui.scene.objects[0].enabled = false;
        inspector.rebase(&acknowledged);
        assert!(!inspector.selected_enabled_dirty);

        inspector.rebase(&crate::view_model::AnnotationModel::default());
        assert!(inspector.selected_object.is_none());
        assert!(inspector.mask_sup_draft.is_none());
    }

    #[test]
    fn complete_generated_edit_vocabulary_emits_typed_outcomes() {
        let model = crate::view_model::AnnotationModel::default();
        let mut inspector = Component::default();
        for command in crate::generated::ANNOTATION_SIDEBAR_COMMAND_VALUES {
            assert!(matches!(
                inspector
                    .update(&model, Message::Sidebar(*command))
                    .unwrap(),
                Some(Outcome::EditRequested(_))
            ));
        }
        for tool in crate::generated::ANNOTATION_TOOL_VALUES {
            assert!(matches!(
                inspector
                    .update(&model, Message::ToolSelected(*tool))
                    .unwrap(),
                Some(Outcome::EditRequested(_))
            ));
        }
        for message in [
            Message::HoldChanged(true),
            Message::ObjectSelected(0),
            Message::SplineSelected(0),
            Message::SkeletonSelected(0),
            Message::UndoRequested,
            Message::RedoRequested,
        ] {
            assert!(matches!(
                inspector.update(&model, message).unwrap(),
                Some(Outcome::EditRequested(_))
            ));
        }
    }

    #[test]
    fn selected_object_and_category_use_generated_payload_policies() {
        let model = crate::view_model::AnnotationModel::default();
        let mut inspector = Component::default();
        inspector.selected_enabled = false;
        let Some(Outcome::EditRequested(request)) = inspector
            .update(&model, Message::SelectedObjectApplied(7))
            .unwrap()
        else {
            panic!("selected object edit");
        };
        let AnnotationEdit::AnnotationSelectedObjectEdit(edit) = request.edit else {
            panic!("typed selected object edit");
        };
        assert_eq!(edit.category, 7);
        assert!(!edit.enabled);

        assert!(
            inspector
                .update(&model, Message::CategoryDraftChanged("new category".into()),)
                .unwrap()
                .is_none()
        );
        let Some(Outcome::EditRequested(request)) =
            inspector.update(&model, Message::CategoryApplied).unwrap()
        else {
            panic!("category edit");
        };
        let AnnotationEdit::AnnotationCategoryEdit(edit) = request.edit else {
            panic!("typed category edit");
        };
        assert_eq!(edit.category.size, 12);
        inspector.category_draft = "\n".into();
        assert!(inspector.update(&model, Message::CategoryApplied).is_err());
    }
}

fn tool_label(tool: crate::generated::AnnotationTool) -> &'static str {
    use crate::generated::AnnotationTool;
    match tool {
        AnnotationTool::Select => "Select / move",
        AnnotationTool::Box => "Draw rectangle",
        AnnotationTool::MaskPaint => "Paint mask",
        AnnotationTool::MaskErase => "Erase mask",
        AnnotationTool::MaskFill => "Fill mask region",
        AnnotationTool::Spline => "Draw spline",
        AnnotationTool::Point => "Place point",
        AnnotationTool::Skeleton => "Add skeleton joint",
        AnnotationTool::ColorSample => "Sample image color",
    }
}
fn tool_available(
    state: &crate::generated::AnnotationUiState,
    tool: crate::generated::AnnotationTool,
) -> bool {
    state
        .toolcapabilities
        .iter()
        .any(|capability| capability.tool == tool && capability.available)
}

fn tool_hint(tool: crate::generated::AnnotationTool) -> &'static str {
    use crate::generated::AnnotationTool;
    match tool {
        AnnotationTool::Select => {
            "Click an object to select it. Drag its body or a visible handle. Click empty image space to deselect."
        }
        AnnotationTool::Box => "Drag from one corner to the opposite corner to create a rectangle.",
        AnnotationTool::MaskPaint => {
            "Drag to paint a continuous circular stroke. Select an existing mask to extend it."
        }
        AnnotationTool::MaskErase => "Drag to erase support from the selected mask.",
        AnnotationTool::MaskFill => {
            "Click an empty connected region to fill it in the selected mask."
        }
        AnnotationTool::Spline => {
            "Click to add knots. Select mode moves knots and handles. Deselect first to start another spline."
        }
        AnnotationTool::Point => "Click to create a point. Use Select to move it.",
        AnnotationTool::Skeleton => {
            "Click to append connected joints. Use Select to reposition them."
        }
        AnnotationTool::ColorSample => {
            "Click the clean image to sample the selected mask's supported HSV color."
        }
    }
}

fn command_label(command: crate::generated::AnnotationSidebarCommand) -> &'static str {
    use crate::generated::AnnotationSidebarCommand::*;
    match command {
        Assist => "Run assistance",
        Delete => "Delete selected object",
        Duplicate => "Duplicate selected object",
        Undo => "Undo",
        Redo => "Redo",
        RedrawBox => "Redraw rectangle",
        SplineInsertKnot => "Insert spline knot",
        SplineClose => "Close spline",
        SplineReopen => "Open spline",
        SplineDeleteKnot => "Delete selected knot",
        SkeletonSkip => "Select next joint",
        SkeletonHide => "Hide joint",
        SkeletonShow => "Show joint",
        SkeletonReseed => "Reset selected joint",
    }
}
fn shape_label(shape: AnnotationShape) -> &'static str {
    match shape {
        AnnotationShape::Box => "Rectangle",
        AnnotationShape::Mask => "Mask",
        AnnotationShape::Spline => "Spline",
        AnnotationShape::Point => "Point",
        AnnotationShape::Skeleton => "Skeleton",
    }
}
fn handle_label(role: AnnotationHandleRole) -> &'static str {
    match role {
        AnnotationHandleRole::Point => "Point",
        AnnotationHandleRole::SplineKnot => "Knot",
        AnnotationHandleRole::SplineInHandle => "Incoming handle",
        AnnotationHandleRole::SplineOutHandle => "Outgoing handle",
        AnnotationHandleRole::SkeletonNode => "Joint",
        AnnotationHandleRole::BoxCorner => "Rectangle corner",
    }
}
fn mode_label(mode: AnnotationSplineHandleMode) -> &'static str {
    match mode {
        AnnotationSplineHandleMode::Corner => "Independent handles",
        AnnotationSplineHandleMode::Smooth => "Smooth tangent",
        AnnotationSplineHandleMode::Mirrored => "Mirrored handles",
    }
}
fn cleanup_label(operation: crate::generated::AnnotationMaskCleanup) -> &'static str {
    use crate::generated::AnnotationMaskCleanup::*;
    match operation {
        LargestComponent => "Keep largest region",
        FillHoles => "Fill enclosed holes",
        Dilate => "Expand mask",
        Erode => "Shrink mask",
        Open => "Remove small protrusions",
        Close => "Close small gaps",
    }
}
