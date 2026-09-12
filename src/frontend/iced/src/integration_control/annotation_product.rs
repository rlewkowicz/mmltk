use crate::generated::{
    self as native, AnnotationObject, AnnotationShape as Shape, AnnotationTool as Tool,
    AnnotationUiState,
};
use crate::view::annotation::{Message, sidebar};

#[derive(Clone, Copy)]
pub(super) enum Step {
    Select(u16),
    Tool(Tool),
    Sample,
    Fill,
    Cleanup(native::AnnotationMaskCleanup),
    Move(Shape),
    ResizeMask,
    Paint,
    Erase,
    Segment,
    Handle,
    Singleton,
    Create(Shape, u8),
    Reclass,
    Undo,
    Redo,
    Cancel,
}

#[derive(Clone, Copy, Debug)]
pub(super) enum Settlement {
    NativeUi,
    RenderedFrame,
}

pub(super) struct Action {
    pub messages: Vec<Message>,
    // Source-space coordinates; transformation belongs to the observed shader bounds.
    pub gesture: Option<[f64; 4]>,
    pub cancel: bool,
    pub tool: Option<Tool>,
    pub settlement: Settlement,
}

#[derive(Default)]
pub(super) struct Pass {
    steps: Vec<Step>,
    next: usize,
    before: Option<AnnotationObject>,
    objects: usize,
    gesture: Option<[f64; 4]>,
    original_category: u16,
}

impl Pass {
    pub fn start(&mut self, ui: &AnnotationUiState, imported: usize) -> Result<(), &'static str> {
        let mask = ui
            .scene
            .objects
            .iter()
            .position(|object| object.shape == Shape::Mask)
            .ok_or("product pass requires the imported mask")? as u16;
        let point = imported as u16;
        let spline = point + 1;
        let skeleton = point + 2;
        self.steps = vec![
            Step::Select(mask),
            Step::Tool(Tool::Select),
            Step::Move(Shape::Mask),
            Step::ResizeMask,
            Step::Tool(Tool::MaskPaint),
            Step::Paint,
            Step::Tool(Tool::MaskErase),
            Step::Erase,
            Step::Tool(Tool::ColorSample),
            Step::Sample,
            Step::Tool(Tool::MaskFill),
            Step::Fill,
        ];
        self.steps.extend(
            native::ANNOTATION_MASK_CLEANUP_VALUES
                .iter()
                .copied()
                .map(Step::Cleanup),
        );
        self.steps.extend([
            Step::Select(point),
            Step::Tool(Tool::Select),
            Step::Move(Shape::Point),
            Step::Reclass,
            Step::Undo,
            Step::Redo,
            Step::Cancel,
            Step::Select(spline),
            Step::Move(Shape::Spline),
            Step::Segment,
            Step::Handle,
            Step::Select(skeleton),
            Step::Move(Shape::Skeleton),
            Step::Select(point),
            Step::Tool(Tool::Spline),
            Step::Singleton,
            Step::Select(point),
            Step::Tool(Tool::Box),
            Step::Create(Shape::Box, 0),
            Step::Tool(Tool::Point),
            Step::Create(Shape::Point, 0),
            Step::Tool(Tool::Skeleton),
            Step::Create(Shape::Skeleton, 0),
            Step::Create(Shape::Skeleton, 1),
        ]);
        self.next = 0;
        Ok(())
    }

    pub fn finished(&self) -> bool {
        self.next == self.steps.len()
    }

    pub fn action(&mut self, ui: &AnnotationUiState) -> Option<Action> {
        let step = *self.steps.get(self.next)?;
        self.objects = ui.scene.objects.len();
        self.before = ui
            .editor
            .selectedobject
            .and_then(|index| ui.scene.objects.get(index as usize))
            .cloned();
        let mut action = Action {
            messages: Vec::new(),
            gesture: None,
            cancel: false,
            tool: None,
            settlement: step.settlement(),
        };
        let mut sidebar_message = |message| action.messages.push(Message::Sidebar(message));
        match step {
            Step::Select(index) => sidebar_message(sidebar::Message::ObjectSelected(index)),
            Step::Tool(tool) => action.tool = Some(tool),
            Step::Cleanup(operation) => sidebar_message(sidebar::Message::MaskCleanup(operation)),
            Step::Reclass => {
                self.original_category = self.before.as_ref()?.category;
                sidebar_message(sidebar::Message::SelectedObjectApplied(
                    (self.original_category + 1) % ui.scene.categories.len() as u16,
                ));
            }
            Step::Undo => sidebar_message(sidebar::Message::UndoRequested),
            Step::Redo => sidebar_message(sidebar::Message::RedoRequested),
            Step::Segment => sidebar_message(sidebar::Message::SplineSelected(0)),
            Step::Handle => {
                let knot = self.before.as_ref()?.splineknots.first()?;
                let point = &knot.point;
                // Repeat the editing pass after moving the spline. Choose the
                // opposite side so its existing translated handle also changes.
                let direction = if knot.out.enabled && knot.out.point.x > point.x {
                    -1.0
                } else {
                    1.0
                };
                sidebar_message(sidebar::Message::HandleSelected(
                    native::AnnotationHandleRole::SplineOutHandle,
                ));
                sidebar_message(sidebar::Message::HandleModeChanged(
                    native::AnnotationSplineHandleMode::Corner,
                ));
                sidebar_message(sidebar::Message::HandleXChanged(
                    (point.x + 12.0 * direction)
                        .clamp(0.0, f32::from(ui.scene.framewidth) - 1.0)
                        .to_string(),
                ));
                sidebar_message(sidebar::Message::HandleYChanged(
                    (point.y - 8.0 * direction)
                        .clamp(0.0, f32::from(ui.scene.frameheight) - 1.0)
                        .to_string(),
                ));
                sidebar_message(sidebar::Message::HandleApplied);
            }
            Step::Sample | Step::Fill => {
                let [x, y] =
                    super::annotation_checks::sample_pixel(ui).map(|value| f64::from(value) + 0.5);
                action.gesture = Some([x, y, x, y]);
            }
            Step::Paint | Step::Erase => {
                let object = self.before.as_ref()?;
                let center = super::annotation_checks::sample_pixel(ui);
                let [x, y] = if matches!(step, Step::Erase)
                    && super::annotation_checks::mask_contains(&object.mask, center)
                {
                    // Repeated passes must leave a real hole for the later Fill.
                    center.map(|value| f64::from(value) + 0.5)
                } else {
                    let b = &object.box_;
                    [
                        (f64::from(b.second.x) + 2.0).min(f64::from(ui.scene.framewidth) - 1.0),
                        f64::from((b.first.y + b.second.y) / 2.0),
                    ]
                };
                action.gesture = Some([x, y, x, y]);
            }
            Step::ResizeMask => {
                let b = &self.before.as_ref()?.box_;
                let x = f64::from((b.first.x - 5.0).max(0.0));
                let y = f64::from((b.first.y - 5.0).max(0.0));
                action.gesture = Some([x, y, x + 8.0, y + 8.0]);
            }
            Step::Create(shape, node) => {
                let (x, y, dx, dy) = match shape {
                    Shape::Box => (0.65, 0.1, 0.1, 0.1),
                    Shape::Point => (0.7, 0.85, 0.0, 0.0),
                    Shape::Skeleton => (
                        0.8 + 0.05 * f64::from(node),
                        0.65 + 0.05 * f64::from(node),
                        0.0,
                        0.0,
                    ),
                    _ => return None,
                };
                let w = f64::from(ui.scene.framewidth);
                let h = f64::from(ui.scene.frameheight);
                action.gesture = Some([x * w, y * h, (x + dx) * w, (y + dy) * h]);
            }
            Step::Singleton => {
                let x = f64::from(ui.scene.framewidth) * 0.15;
                let y = f64::from(ui.scene.frameheight) * 0.8;
                action.gesture = Some([x, y, x, y]);
            }
            Step::Move(shape) => {
                let object = self.before.as_ref()?;
                if shape == Shape::Mask {
                    let run = object.mask.runs.first()?;
                    let x = f64::from(run.first) + 0.5;
                    let y = f64::from(run.row) + 0.5;
                    let delta = if object.box_.first.x > 0.0 && object.box_.first.y > 0.0 {
                        -1.0
                    } else {
                        1.0
                    };
                    action.gesture = Some([x, y, x + delta, y + delta]);
                    self.gesture = action.gesture;
                    return Some(action);
                }
                let point = match shape {
                    Shape::Point => &object.point,
                    Shape::Spline => &object.splineknots.first()?.point,
                    Shape::Skeleton => &object.skeletonnodes.first()?.point,
                    _ => return None,
                };
                action.gesture = Some([
                    f64::from(point.x),
                    f64::from(point.y),
                    f64::from((point.x + 8.0).min(f32::from(ui.scene.framewidth) - 1.0)),
                    f64::from((point.y + 8.0).min(f32::from(ui.scene.frameheight) - 1.0)),
                ]);
            }
            Step::Cancel => {
                let point = &self.before.as_ref()?.point;
                action.gesture = Some([
                    f64::from(point.x),
                    f64::from(point.y),
                    f64::from(point.x + 4.0),
                    f64::from(point.y + 4.0),
                ]);
                action.cancel = true;
            }
        }
        self.gesture = action.gesture;
        Some(action)
    }

    fn at_endpoint(&self, point: &native::AnnotationPoint) -> bool {
        self.gesture.is_some_and(|gesture| {
            (gesture[2] - f64::from(point.x)).abs() <= 1.0
                && (gesture[3] - f64::from(point.y)).abs() <= 1.0
        })
    }

    pub fn observe(&mut self, ui: &AnnotationUiState) -> Result<Step, &'static str> {
        let step = self.steps[self.next];
        let selected = ui
            .editor
            .selectedobject
            .and_then(|index| ui.scene.objects.get(index as usize));
        let valid = match step {
            Step::Select(index) => ui.editor.selectedobject == Some(index),
            Step::Tool(tool) => {
                ui.editor.tool == tool
                    && ui
                        .toolcapabilities
                        .iter()
                        .any(|fact| fact.tool == tool && fact.available)
            }
            Step::Sample => {
                selected.is_some_and(|object| object.shape == Shape::Mask && object.sup.sampling)
            }
            Step::Fill => {
                let center = super::annotation_checks::sample_pixel(ui);
                selected
                    .zip(self.before.as_ref())
                    .is_some_and(|(after, before)| {
                        !super::annotation_checks::mask_contains(&before.mask, center)
                            && super::annotation_checks::mask_contains(&after.mask, center)
                            && after.mask.runs != before.mask.runs
                    })
            }
            Step::Cleanup(operation) => selected.is_some_and(|object| {
                object.mask.cleanup == operation
                    && object.mask.cleanupradius
                        == native::default_uimaskcleanupradius().unwrap() as u16
            }),
            Step::Move(shape) => {
                selected
                    .zip(self.before.as_ref())
                    .is_some_and(|(after, before)| {
                        after.shape == shape
                            && match shape {
                                Shape::Point => {
                                    after.point != before.point && self.at_endpoint(&after.point)
                                }
                                Shape::Spline => {
                                    after.splineknots[0].point != before.splineknots[0].point
                                        && self.at_endpoint(&after.splineknots[0].point)
                                }
                                Shape::Skeleton => {
                                    after.skeletonnodes[0].point != before.skeletonnodes[0].point
                                        && self.at_endpoint(&after.skeletonnodes[0].point)
                                }
                                Shape::Mask => {
                                    after.box_ != before.box_ && after.mask.runs != before.mask.runs
                                }
                                _ => false,
                            }
                    })
            }
            Step::ResizeMask => {
                selected
                    .zip(self.before.as_ref())
                    .is_some_and(|(after, before)| {
                        after.shape == Shape::Mask
                            && after.box_ != before.box_
                            && after.mask.runs != before.mask.runs
                    })
            }
            Step::Paint | Step::Erase => selected
                .zip(self.before.as_ref())
                .is_some_and(|(after, before)| after.mask.runs != before.mask.runs),
            Step::Create(shape, node) => {
                selected.is_some_and(|object| object.shape == shape)
                    && ui.scene.objects.len() == self.objects + usize::from(node == 0)
            }
            Step::Reclass | Step::Redo => selected.is_some_and(|object| {
                object.category == (self.original_category + 1) % ui.scene.categories.len() as u16
            }),
            Step::Undo => selected.is_some_and(|object| object.category == self.original_category),
            Step::Segment => ui.editor.selectedsplinesegment == Some(0),
            Step::Handle => selected
                .zip(self.before.as_ref())
                .is_some_and(|(after, before)| {
                    after.splineknots[0].out.enabled
                        && after.splineknots[0].out != before.splineknots[0].out
                }),
            Step::Singleton => {
                ui.scene.objects.len() == self.objects + 1
                    && selected.is_some_and(|object| {
                        object.shape == Shape::Spline && object.splineknots.len() == 1
                    })
            }
            Step::Cancel => {
                selected == self.before.as_ref() && ui.scene.objects.len() == self.objects
            }
        };
        if !valid {
            return Err(
                "annotation product operation did not produce its required geometry or capability",
            );
        }
        self.next += 1;
        Ok(step)
    }
}

impl Step {
    fn settlement(self) -> Settlement {
        match self {
            Self::Select(_)
            | Self::Tool(_)
            | Self::Sample
            | Self::Cleanup(_)
            | Self::Reclass
            | Self::Undo
            | Self::Redo
            | Self::Segment
            | Self::Handle => Settlement::NativeUi,
            Self::Fill
            | Self::Move(_)
            | Self::ResizeMask
            | Self::Paint
            | Self::Erase
            | Self::Singleton
            | Self::Create(_, _)
            | Self::Cancel => Settlement::RenderedFrame,
        }
    }

    pub(super) fn detail(self) -> String {
        match self {
            Step::Select(_) => "selection".into(),
            Step::Tool(tool) => format!("tool-{tool:?}"),
            Step::Sample => "color-sample".into(),
            Step::Fill => "mask-fill".into(),
            Step::Cleanup(operation) => format!("cleanup-{operation:?}"),
            Step::Reclass => "reclassify".into(),
            Step::Undo => "undo-class".into(),
            Step::Redo => "redo-class".into(),
            Step::ResizeMask => "resize-mask".into(),
            Step::Paint => "paint-mask".into(),
            Step::Erase => "erase-mask".into(),
            Step::Create(shape, _) => format!("create-{shape:?}"),
            Step::Move(shape) => format!("move-{shape:?}"),
            Step::Segment => "select-knot".into(),
            Step::Handle => "spline-handle".into(),
            Step::Singleton => "singleton-spline".into(),
            Step::Cancel => "cancel-preview".into(),
        }
    }
}
