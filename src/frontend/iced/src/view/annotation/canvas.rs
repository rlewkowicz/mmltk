use crate::generated::{
    AnnotationHandleRole as Role, AnnotationPoint, AnnotationPointerTarget as Target,
    AnnotationShape as Shape, AnnotationTool as Tool, AnnotationUiState,
};

fn empty() -> Target {
    Target {
        object: None,
        element: None,
        role: None,
    }
}

// Image coordinates arrive through the presentation viewport's inverse transform.
// Selection is resolved once at press; the gesture owner retains the target.
pub(super) fn target(state: &AnnotationUiState, x: f32, y: f32) -> Target {
    let selected = state.editor.selectedobject.and_then(|index| {
        state
            .scene
            .objects
            .get(index as usize)
            .map(|object| (index, object))
    });
    match state.editor.tool {
        Tool::Box | Tool::Point => return empty(),
        Tool::MaskPaint | Tool::MaskErase | Tool::MaskFill | Tool::ColorSample => {
            return selected
                .filter(|(_, object)| object.shape == Shape::Mask)
                .map_or_else(empty, |(index, _)| Target {
                    object: Some(index),
                    element: None,
                    role: None,
                });
        }
        Tool::Spline | Tool::Skeleton => {
            return selected
                .filter(|(_, object)| {
                    matches!(
                        (state.editor.tool, object.shape),
                        (Tool::Spline, Shape::Spline) | (Tool::Skeleton, Shape::Skeleton)
                    )
                })
                .map_or_else(empty, |(index, _)| Target {
                    object: Some(index),
                    element: None,
                    role: None,
                });
        }
        Tool::Select => {}
    }
    let near = |point: &AnnotationPoint| (point.x - x).hypot(point.y - y) <= 6.0;
    // Selected geometry gets handle precedence over overlapping object bodies.
    if let Some((index, object)) = selected {
        let handle = |element, role| Target {
            object: Some(index),
            element: Some(element),
            role: Some(role),
        };
        if matches!(object.shape, Shape::Box | Shape::Mask) {
            let b = &object.box_;
            for (element, point) in [
                (b.first.x - 5.0, b.first.y - 5.0),
                (b.second.x + 4.0, b.first.y - 5.0),
                (b.second.x + 4.0, b.second.y + 4.0),
                (b.first.x - 5.0, b.second.y + 4.0),
            ]
            .into_iter()
            .enumerate()
            {
                if near(&AnnotationPoint {
                    x: point.0,
                    y: point.1,
                }) {
                    return handle(element as u16, Role::BoxCorner);
                }
            }
        }
        if object.shape == Shape::Point && near(&object.point) {
            return handle(0, Role::Point);
        }
        for (element, knot) in object.splineknots.iter().enumerate() {
            if near(&knot.point) {
                return handle(element as u16, Role::SplineKnot);
            }
            if knot.in_.enabled && near(&knot.in_.point) {
                return handle(element as u16, Role::SplineInHandle);
            }
            if knot.out.enabled && near(&knot.out.point) {
                return handle(element as u16, Role::SplineOutHandle);
            }
        }
        for (element, node) in object.skeletonnodes.iter().enumerate() {
            if node.visible && near(&node.point) {
                return handle(element as u16, Role::SkeletonNode);
            }
        }
    }
    for (index, object) in state.scene.objects.iter().enumerate().rev() {
        if !object.enabled {
            continue;
        }
        let hit = match object.shape {
            Shape::Box => {
                x >= object.box_.first.x - 3.0
                    && x <= object.box_.second.x + 3.0
                    && y >= object.box_.first.y - 3.0
                    && y <= object.box_.second.y + 3.0
            }
            Shape::Mask => object.mask.runs.iter().any(|run| {
                run.row == y as u16 && x >= run.first as f32 && x < run.last as f32 + 1.0
            }),
            Shape::Point => near(&object.point),
            Shape::Spline => object.splineknots.iter().any(|knot| near(&knot.point)),
            Shape::Skeleton => object
                .skeletonnodes
                .iter()
                .any(|node| node.visible && near(&node.point)),
        };
        if hit {
            return Target {
                object: Some(index as u16),
                element: (object.shape == Shape::Point).then_some(0),
                role: (object.shape == Shape::Point).then_some(Role::Point),
            };
        }
    }
    empty()
}

struct GestureState {
    pointer_active: bool,
    pointer_interaction: u64,
    pointer_sequence: u64,
    gesture_target: Option<Target>,
    last_pointer: AnnotationPoint,
}
impl Default for GestureState {
    fn default() -> Self {
        Self {
            pointer_active: false,
            pointer_interaction: 0,
            pointer_sequence: 0,
            gesture_target: None,
            last_pointer: AnnotationPoint { x: 0.0, y: 0.0 },
        }
    }
}
impl GestureState {
    pub(super) fn cancel_pointer(&mut self) -> Option<crate::generated::AnnotationPointer> {
        self.finish_pointer(
            crate::generated::AnnotationPointerPhase::Cancel,
            self.last_pointer.x,
            self.last_pointer.y,
            self.gesture_target.clone()?,
        )
    }
    pub(super) fn clear_pointer_lifecycle(&mut self) {
        self.pointer_active = false;
        self.pointer_sequence = 0;
    }

    fn finish_pointer(
        &mut self,
        phase: crate::generated::AnnotationPointerPhase,
        x: f32,
        y: f32,
        target: Target,
    ) -> Option<crate::generated::AnnotationPointer> {
        if !self.pointer_active {
            return None;
        }
        self.pointer_active = false;
        self.pointer_sequence = self.pointer_sequence.saturating_add(1);
        Some(crate::generated::AnnotationPointer {
            phase,
            interactionid: self.pointer_interaction,
            sequence: self.pointer_sequence,
            target,
            point: AnnotationPoint { x, y },
            brushradius: crate::generated::default_uiannotationbrushradius().unwrap() as u16,
        })
    }

    pub fn pointer(
        &mut self,
        pressed: bool,
        x: f32,
        y: f32,
        target: Target,
    ) -> Option<crate::generated::AnnotationPointer> {
        self.last_pointer = AnnotationPoint { x, y };
        use crate::generated::AnnotationPointerPhase;
        let phase = if pressed && !self.pointer_active {
            self.pointer_active = true;
            self.pointer_interaction = self.pointer_interaction.wrapping_add(1).max(1);
            self.pointer_sequence = 1;
            AnnotationPointerPhase::Begin
        } else if pressed {
            self.pointer_sequence = self.pointer_sequence.saturating_add(1);
            AnnotationPointerPhase::Update
        } else if self.pointer_active {
            return self.finish_pointer(AnnotationPointerPhase::End, x, y, target);
        } else {
            return None;
        };
        Some(crate::generated::AnnotationPointer {
            phase,
            interactionid: self.pointer_interaction,
            sequence: self.pointer_sequence,
            target,
            point: AnnotationPoint { x, y },
            brushradius: crate::generated::default_uiannotationbrushradius().unwrap() as u16,
        })
    }

    pub(super) fn pointer_from_gesture(
        &mut self,
        ui: &AnnotationUiState,
        gesture: crate::presentation_surface::SurfaceGesture,
    ) -> Option<crate::generated::AnnotationPointer> {
        let x = gesture.sample.content_x as f32;
        let y = gesture.sample.content_y as f32;
        let target = if self.pointer_active {
            self.gesture_target.clone()?
        } else {
            if gesture.kind != crate::presentation_surface::SurfaceGestureKind::Pointer
                || !gesture.sample.pressed
            {
                return None;
            }
            let target = target(ui, x, y);
            self.gesture_target = Some(target.clone());
            target
        };
        match gesture.kind {
            crate::presentation_surface::SurfaceGestureKind::Pointer => {
                self.pointer(gesture.sample.pressed, x, y, target)
            }
            crate::presentation_surface::SurfaceGestureKind::End => {
                self.finish_pointer(crate::generated::AnnotationPointerPhase::End, x, y, target)
            }
            crate::presentation_surface::SurfaceGestureKind::Cancel => self.finish_pointer(
                crate::generated::AnnotationPointerPhase::Cancel,
                x,
                y,
                target,
            ),
            crate::presentation_surface::SurfaceGestureKind::Viewport => None,
        }
    }
}

struct InputState {
    ui_revision: u64,
    input_document_epoch: u64,
    ui: AnnotationUiState,
}

#[derive(Default)]
struct Retained {
    gesture: GestureState,
    input: Option<InputState>,
    available: bool,
    radius: u16,
    connection: Option<crate::transport_connection::Connection>,
}
#[derive(Default)]
pub(super) struct Component {
    retained: std::sync::Arc<std::sync::Mutex<Retained>>,
}
impl Component {
    pub fn set_connection(&self, connection: Option<crate::transport_connection::Connection>) {
        let mut retained = self.retained.lock().expect("annotation canvas");
        retained.gesture.clear_pointer_lifecycle();
        retained.input = None;
        retained.connection = connection;
    }
    pub fn clear_pointer_lifecycle(&self) { self.retained.lock().expect("annotation canvas").gesture.clear_pointer_lifecycle(); }
    pub fn cancel_pointer(&self) -> Option<crate::generated::AnnotationPointer> { self.retained.lock().expect("annotation canvas").gesture.cancel_pointer() }
    pub fn pointer_from_gesture(&self, ui: &AnnotationUiState, gesture: crate::presentation_surface::SurfaceGesture) -> Option<crate::generated::AnnotationPointer> {
        self.retained.lock().expect("annotation canvas").gesture.pointer_from_gesture(ui, gesture)
    }
    pub fn dispatch(&self, model: &crate::view_model::ApplicationModel, radius: u16, keyboard: std::sync::Arc<std::sync::atomic::AtomicBool>)
        -> std::sync::Arc<dyn Fn(crate::presentation_surface::SurfaceGesture) -> Option<super::Message> + Send + Sync> {
        {
            let mut retained = self.retained.lock().expect("annotation canvas");
            if retained.input.as_ref().map(|input| {
                (input.ui_revision, input.input_document_epoch)
            }) != model.annotation.snapshot.as_ref().map(|snapshot| {
                (snapshot.uirevision, snapshot.inputdocumentepoch)
            })
            {
                retained.input = model.annotation.snapshot.as_ref().map(|snapshot| InputState {
                    ui_revision: snapshot.uirevision,
                    input_document_epoch: snapshot.inputdocumentepoch,
                    ui: snapshot.ui.clone(),
                });
            }
            retained.available = model.annotation_edit_available();
            retained.radius = radius;
        }
        let owner = self.retained.clone();
        std::sync::Arc::new(move |gesture| {
            let mut retained = owner.lock().expect("annotation canvas");
            if gesture.kind == crate::presentation_surface::SurfaceGestureKind::Pointer {
                keyboard.store(true, std::sync::atomic::Ordering::Relaxed);
            }
            let Retained { gesture: lifecycle, input, available, radius, connection } = &mut *retained;
            if !*available { lifecycle.clear_pointer_lifecycle(); return None; }
            let input = input.as_ref()?;
            let mut pointer = lifecycle.pointer_from_gesture(&input.ui, gesture)?;
            pointer.brushradius = *radius;
            let result = connection.as_mut().ok_or(crate::transport_connection::OutboundSendError::Closed)
                .and_then(|connection| connection.send_annotation_pointer(pointer, input.input_document_epoch));
            result.err().map(|error| super::Message::InputFailed(error))
        })
    }
}

pub(super) fn view(
    surface: Option<crate::presentation_surface::Surface>,
    aspect: crate::generated::WorkspaceAspectRatio,
    settings_available: bool,
    width: f32,
    local: std::sync::Arc<dyn Fn(crate::presentation_surface::SurfaceGesture) -> Option<super::Message> + Send + Sync>,
) -> crate::fluent_theme::Element<'static, super::Message> {
    use crate::view::workspace;
    use iced::widget::{column, container, shader, text};
    let (width, height) = workspace::surface_extent(width, aspect);
    let image: crate::fluent_theme::Element<'static, super::Message> = surface.map_or_else(
        || {
            container(text("Open an image from Explore to begin annotating"))
                .center(iced::Fill)
                .into()
        },
        |surface| {
            shader(crate::presentation_surface::Program {
                surface,
                publish: None,
                local: Some(local),
                placement: crate::presentation_surface::Placement::Contain,
                control_id: workspace::STABLE_ID,
            })
            .width(iced::Fill)
            .height(iced::Fill)
            .into()
        },
    );
    column![
        crate::view::aspect_ratio::selector(
            aspect,
            settings_available,
            crate::view::aspect_ratio::Scope::Workspace,
            |aspect| super::Message::Workspace(workspace::Message::AspectSelected(aspect))
        ),
        container(
            container(image)
                .id(workspace::STABLE_ID)
                .width(iced::Length::Fixed(width))
                .height(iced::Length::Fixed(height))
        )
        .id(super::WORKSPACE_ID)
        .style(crate::fluent_theme::container_workspace)
    ]
    .spacing(8)
    .width(iced::Fill)
    .into()
}
