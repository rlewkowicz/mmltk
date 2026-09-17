use super::lifecycle::same_numeric_value;
use super::retained::EXPLORE_ANNOTATE;
use crate::generated::FeatureId;
use crate::generated::{AnnotationMask, AnnotationShape, AnnotationUiState};
use crate::integration_control::pixel_checks::ProbeOutcome;
#[cfg(target_arch = "wasm32")]
use crate::integration_control::pixel_checks::pixel_result_callback;
use crate::integration_control::probe::{ProbeReceipt, current_receipt};
use crate::integration_control::widget_ops::click;
use crate::integration_control::widget_ops::{
    AnnotationReveal, locate, reveal_control, scroll_control_into_view,
};
use crate::integration_control::{
    ANNOTATION_SURFACE, CopyScaleStage, Driver, Message, Phase, annotation_checks,
    annotation_message, annotation_product, probe, reporting, settled_settings_snapshot,
    widget_ops,
};
#[cfg(target_arch = "wasm32")]
use crate::integration_control::{
    annotation_pixels_js, annotation_pointer_js, annotation_release_js,
};
use crate::message::Message as RootMessage;
use crate::view::{annotation, train};
use crate::view_model::ApplicationModel;
use iced::widget::operation::RelativeOffset;
use iced::{Rectangle, Task};

pub(super) fn mask_contains(mask: &AnnotationMask, [x, y]: [u16; 2]) -> bool {
    mask.runs
        .iter()
        .any(|run| run.row == y && run.first <= x && run.last >= x)
}

pub(super) fn sample_pixel(ui: &AnnotationUiState) -> [u16; 2] {
    // Keep the clean-image sample inside the image. A corner sample can land
    // on a filtered mask-cleanup boundary in the compact display.
    [ui.scene.framewidth / 2, ui.scene.frameheight / 2]
}

// Expected source-space sample positions follow public object geometry. The
// browser samples the completed canvas independently of the native rasterizer.
pub(super) fn probes(ui: &AnnotationUiState) -> Vec<f64> {
    let mut samples = Vec::new();
    let mut add = |x: f32, y: f32, color: iced::Color| {
        if x >= 0.0
            && y >= 0.0
            && x < f32::from(ui.scene.framewidth)
            && y < f32::from(ui.scene.frameheight)
        {
            samples.extend([
                f64::from(x),
                f64::from(y),
                f64::from(color.r) * 255.0,
                f64::from(color.g) * 255.0,
                f64::from(color.b) * 255.0,
                24.0,
                2.0,
            ]);
        }
    };
    for (index, object) in ui.scene.objects.iter().enumerate() {
        if !object.enabled {
            continue;
        }
        let selected = ui.editor.selectedobject == Some(index as u16);
        // Unselected singleton splines must remain observable too.
        if !selected && !(object.shape == AnnotationShape::Spline && object.splineknots.len() == 1)
        {
            continue;
        }
        let Some(color) = ui.scene.palette.get(object.category as usize) else {
            continue;
        };
        let color = crate::presentation_surface::labels::class_color(color);
        match object.shape {
            AnnotationShape::Box | AnnotationShape::Mask => {
                let b = &object.box_;
                add(b.first.x - 1.0, (b.first.y + b.second.y) / 2.0, color);
                add(b.second.x, (b.first.y + b.second.y) / 2.0, color);
                if selected {
                    add(b.first.x - 5.0, b.first.y - 5.0, iced::Color::WHITE);
                    add(b.second.x + 4.0, b.second.y + 4.0, iced::Color::WHITE);
                }
                if object.shape == AnnotationShape::Mask && object.sup.sampling {
                    let [x, y] = sample_pixel(ui);
                    let clean =
                        crate::presentation_surface::labels::class_color(&object.sup.center);
                    let alpha = if mask_contains(&object.mask, [x, y]) {
                        92.0 / 255.0
                    } else {
                        0.0
                    };
                    add(
                        f32::from(x) + 0.5,
                        f32::from(y) + 0.5,
                        iced::Color::from_rgb(
                            clean.r * (1.0 - alpha) + color.r * alpha,
                            clean.g * (1.0 - alpha) + color.g * alpha,
                            clean.b * (1.0 - alpha) + color.b * alpha,
                        ),
                    );
                }
            }
            AnnotationShape::Point => add(object.point.x, object.point.y, color),
            AnnotationShape::Spline => {
                for knot in &object.splineknots {
                    add(knot.point.x, knot.point.y, color);
                    if selected && knot.in_.enabled {
                        add(knot.in_.point.x, knot.in_.point.y, color);
                    }
                    if selected && knot.out.enabled {
                        add(knot.out.point.x, knot.out.point.y, color);
                    }
                }
                if object.splineknots.len() > 1 {
                    let a = &object.splineknots[0];
                    let b = &object.splineknots[1];
                    let c1 = if a.out.enabled {
                        &a.out.point
                    } else {
                        &a.point
                    };
                    let c2 = if b.in_.enabled {
                        &b.in_.point
                    } else {
                        &b.point
                    };
                    add(
                        (a.point.x + 3.0 * c1.x + 3.0 * c2.x + b.point.x) / 8.0,
                        (a.point.y + 3.0 * c1.y + 3.0 * c2.y + b.point.y) / 8.0,
                        color,
                    );
                }
            }
            AnnotationShape::Skeleton => {
                for node in &object.skeletonnodes {
                    if node.visible {
                        add(node.point.x, node.point.y, color);
                    }
                }
                for edge in &object.skeletonedges {
                    let a = &object.skeletonnodes[edge.source as usize];
                    let b = &object.skeletonnodes[edge.target as usize];
                    if a.visible && b.visible {
                        add(
                            (a.point.x + b.point.x) / 2.0,
                            (a.point.y + b.point.y) / 2.0,
                            color,
                        );
                    }
                }
            }
        }
    }
    samples
}

#[cfg(target_arch = "wasm32")]
pub(super) fn place_gesture(
    bounds: iced::Rectangle,
    extent: (f64, f64),
    points: [f64; 4],
) -> [f64; 4] {
    let (width, height) = extent;
    let scale = (f64::from(bounds.width) / width).min(f64::from(bounds.height) / height);
    let left = (f64::from(bounds.width) - width * scale) / 2.0;
    let top = (f64::from(bounds.height) - height * scale) / 2.0;
    [
        (left + points[0] * scale) / f64::from(bounds.width),
        (top + points[1] * scale) / f64::from(bounds.height),
        (left + points[2] * scale) / f64::from(bounds.width),
        (top + points[3] * scale) / f64::from(bounds.height),
    ]
}

/// Mutable observations owned by this scenario or mechanism.
pub(super) struct State {
    settings_revision: u64,
    annotation_open: Option<(crate::generated::AnnotationOpen, u64)>,
    bounded_document_revision: u64,
    bounded_object_count: usize,
    annotation_pixel_progress: ((u64, u64), usize, usize),
    copy_step: u8,
    copy_product: annotation_product::Pass,
    copy_product_gesture: Option<[f64; 4]>,
    copy_product_cancel: bool,
    copy_product_cancelled: bool,
    copy_product_frame: u64,
    copy_product_ui_revision: u64,
    copy_product_settlement: annotation_product::Settlement,
    copy_narrow: bool,
    copy_original_scale: f32,
    copy_list_revision: u64,
    copy_list_object_target: usize,
    copy_list_class_target: usize,
    copy_layout_objects: usize,
    copy_layout_classes: usize,
    copy_requested_scale: f32,
    copy_layout_bounds: Rectangle,
    copy_viewport_width: f32,
    copy_swatch_color: [f64; 3],
    copy_swatch_ready: bool,
    copy_capability_ready: bool,
    copy_capability_available: bool,
    copy_shape_points: usize,
    copy_before: Option<crate::generated::AnnotationObject>,
    copy_after: Option<crate::generated::AnnotationObject>,
    copy_objects: usize,
    copy_categories: Vec<crate::generated::ClassName>,
}
impl Default for State {
    fn default() -> Self {
        Self {
            settings_revision: 0,
            annotation_open: None,
            bounded_document_revision: 0,
            bounded_object_count: 0,
            annotation_pixel_progress: ((0, 0), 0, 0),
            copy_step: 0,
            copy_product: annotation_product::Pass::default(),
            copy_product_gesture: None,
            copy_product_cancel: false,
            copy_product_cancelled: false,
            copy_product_frame: 0,
            copy_product_ui_revision: 0,
            copy_product_settlement: annotation_product::Settlement::RenderedFrame,
            copy_narrow: false,
            copy_original_scale: 1.0,
            copy_list_revision: 0,
            copy_list_object_target: 0,
            copy_list_class_target: 0,
            copy_layout_objects: 0,
            copy_layout_classes: 0,
            copy_requested_scale: 1.0,
            copy_layout_bounds: Rectangle::default(),
            copy_viewport_width: 0.0,
            copy_swatch_color: [0.0; 3],
            copy_swatch_ready: false,
            copy_capability_ready: false,
            copy_capability_available: false,
            copy_shape_points: 0,
            copy_before: None,
            copy_after: None,
            copy_objects: 0,
            copy_categories: Vec::new(),
        }
    }
}

impl State {
    pub(super) fn advance_annotation_checks(
        &mut self,
        widgets: &mut widget_ops::RevealState,
        driver: &mut Driver,
        probes: &mut probe::Requests,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        applied_scale: f32,
        active: FeatureId,
        surface: Option<crate::presentation_surface::Surface>,
    ) -> Task<RootMessage> {
        let frame = surface.and_then(|surface| surface.frame);
        match driver.phase.clone() {
            Phase::OpenAnnotation => self.annotation_arm(widgets, driver, probes, EXPLORE_ANNOTATE),
            Phase::CopyAwaitObject { index, mask } => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.ui.editor.selectedobject != Some(index)
                    || !model.annotation_edit_available()
                {
                    return Task::none();
                }
                self.begin_copy_object_edit(widgets, driver, probes, snapshot, index, mask)
            }
            Phase::CopyUndo { .. } => self.annotation_arm_scrolled(
                widgets,
                driver,
                probes,
                "annotation.undo",
                RelativeOffset::END,
            ),
            Phase::CopyRedo { .. } => self.annotation_arm_scrolled(
                widgets,
                driver,
                probes,
                "annotation.redo",
                RelativeOffset::END,
            ),
            Phase::CopyAwaitUndo { index, mask } => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !model.annotation_edit_available()
                    || snapshot.ui.scene.objects.get(index as usize) != self.copy_before.as_ref()
                {
                    return Task::none();
                }
                driver.phase = Phase::CopyRedo { index, mask };
                self.annotation_arm_scrolled(
                    widgets,
                    driver,
                    probes,
                    "annotation.redo",
                    RelativeOffset::END,
                )
            }
            Phase::CopyAwaitRedo { index, mask: _ } => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !model.annotation_edit_available()
                    || snapshot.ui.scene.objects.get(index as usize) != self.copy_after.as_ref()
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_import_edit",
                        ANNOTATION_SURFACE,
                        match self.copy_step {
                            0 => "box-move-undo-redo",
                            1 => "box-resize-undo-redo",
                            2 => "mask-paint-undo-redo",
                            3 => "mask-erase-undo-redo",
                            _ => "class-undo-redo",
                        },
                        [
                            index as f64,
                            snapshot.ui.scene.objects.len() as f64,
                            snapshot.ui.documentrevision as f64,
                            snapshot.frame.revision as f64,
                        ],
                    )
                });
                if self.copy_step == 3 && snapshot.ui.scene.categories.len() > 1 {
                    self.copy_step = 4;
                    self.copy_before = Some(snapshot.ui.scene.objects[index as usize].clone());
                    driver.phase = Phase::CopyAwaitClass { index };
                    return annotation_message(annotation::Message::Sidebar(
                        annotation::sidebar::Message::SelectedObjectApplied(
                            (snapshot.ui.scene.objects[index as usize].category + 1)
                                % snapshot.ui.scene.categories.len() as u16,
                        ),
                    ));
                }
                if self.copy_step >= 3 {
                    self.copy_step = 5;
                    self.copy_shape_points = 0;
                    self.copy_before = None;
                    driver.phase = Phase::AnnotationTool {
                        revision: snapshot.ui.interactionrevision,
                        tool: crate::generated::AnnotationTool::Point,
                    };
                    return self.annotation_arm_scrolled(
                        widgets,
                        driver,
                        probes,
                        annotation::tool_id(crate::generated::AnnotationTool::Point),
                        RelativeOffset::START,
                    );
                }
                {
                    self.copy_step += 1;
                    if self.copy_step == 1 {
                        self.copy_before = Some(snapshot.ui.scene.objects[index as usize].clone());
                        self.copy_after = None;
                        return self.begin_copy_object_edit(
                            widgets, driver, probes, snapshot, index, false,
                        );
                    }
                    let Some((index, object)) =
                        snapshot
                            .ui
                            .scene
                            .objects
                            .iter()
                            .enumerate()
                            .find(|(_, object)| {
                                object.shape == crate::generated::AnnotationShape::Mask
                            })
                    else {
                        driver.fail("copied document lost its mask object");
                        return Task::none();
                    };
                    self.copy_before = Some(object.clone());
                    self.copy_after = None;
                    self.begin_copy_object_edit(
                        widgets,
                        driver,
                        probes,
                        snapshot,
                        index as u16,
                        true,
                    )
                }
            }
            Phase::CopyAwaitClass { index } => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !model.annotation_edit_available()
                    || self.copy_before.as_ref().is_none_or(|before| {
                        before.category == snapshot.ui.scene.objects[index as usize].category
                    })
                {
                    return Task::none();
                }
                self.copy_after = Some(snapshot.ui.scene.objects[index as usize].clone());
                driver.phase = Phase::CopyUndo { index, mask: true };
                self.annotation_arm_scrolled(
                    widgets,
                    driver,
                    probes,
                    "annotation.undo",
                    RelativeOffset::START,
                )
            }
            Phase::CopyListSetup { stage, .. } => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !model.annotation_edit_available()
                    || model
                        .has_pending(crate::generated::ApplicationIntentEndpoint::AnnotationEdit)
                    || snapshot.busy
                    || snapshot.uirevision <= self.copy_list_revision
                {
                    return Task::none();
                }
                let ui = &snapshot.ui;
                let Some(selected) = ui
                    .editor
                    .selectedobject
                    .and_then(|index| ui.scene.objects.get(index as usize))
                else {
                    driver.fail("Annotation long-list setup requires a selected object");
                    return Task::none();
                };
                self.copy_list_revision = snapshot.uirevision;
                driver.phase = Phase::CopyListSetup {
                    stage,
                    revision: snapshot.uirevision,
                };
                use annotation::sidebar::Message as Sidebar;
                let command = match stage {
                    0 => {
                        driver.phase = Phase::CopyListSetup {
                            stage: 1,
                            revision: snapshot.uirevision,
                        };
                        Sidebar::Sidebar(crate::generated::AnnotationSidebarCommand::Duplicate)
                    }
                    1 if selected.enabled => {
                        driver.phase = Phase::CopyListSetup {
                            stage: 1,
                            revision: snapshot.uirevision,
                        };
                        return annotation_message(annotation::Message::Sidebar(
                            Sidebar::SelectedObjectEnabled(false),
                        ))
                        .chain(annotation_message(
                            annotation::Message::Sidebar(Sidebar::SelectedObjectApplied(
                                selected.category,
                            )),
                        ));
                    }
                    1 if ui.scene.objects.len() < self.copy_list_object_target => {
                        Sidebar::Sidebar(crate::generated::AnnotationSidebarCommand::Duplicate)
                    }
                    1 | 2 if ui.scene.categories.len() < self.copy_list_class_target => {
                        driver.phase = Phase::CopyListSetup {
                            stage: 2,
                            revision: snapshot.uirevision,
                        };
                        return annotation_message(annotation::Message::Sidebar(Sidebar::CategoryDraftChanged(
                            format!("Acceptance long category {} with a label that wraps inside its assigned column", ui.scene.categories.len()))))
                            .chain(annotation_message(annotation::Message::Sidebar(Sidebar::CategoryApplied)));
                    }
                    1 | 2 => {
                        driver.phase = Phase::CopyListSetup {
                            stage: 3,
                            revision: snapshot.uirevision,
                        };
                        Sidebar::ObjectSelected((self.copy_objects + 2) as u16)
                    }
                    _ => {
                        if ui.scene.objects.len() != self.copy_list_object_target
                            || ui.scene.categories.len() != self.copy_list_class_target
                        {
                            driver.fail("Annotation long-list setup did not preserve its exact bounded inventory");
                            return Task::none();
                        }
                        return self.copy_scale_transition(
                            driver,
                            model,
                            applied_scale,
                            CopyScaleStage::Wide,
                        );
                    }
                };
                annotation_message(annotation::Message::Sidebar(command))
            }
            Phase::CopyLayout(step) => {
                if let Some(snapshot) = &model.annotation.snapshot {
                    self.copy_layout_objects = snapshot.ui.scene.objects.len();
                    self.copy_layout_classes = snapshot.ui.scene.categories.len();
                }
                self.copy_viewport_width = model.window_width as f32;
                if let Some(ui) = model
                    .annotation
                    .snapshot
                    .as_ref()
                    .map(|snapshot| &snapshot.ui)
                {
                    if let Some(color) = ui
                        .editor
                        .selectedobject
                        .and_then(|index| ui.scene.objects.get(index as usize))
                        .and_then(|object| ui.scene.palette.get(object.category as usize))
                    {
                        let color = crate::presentation_surface::labels::class_color(color);
                        self.copy_swatch_color = [
                            f64::from(color.r) * 255.0,
                            f64::from(color.g) * 255.0,
                            f64::from(color.b) * 255.0,
                        ];
                    }
                }
                if step == 0 {
                    self.annotation_arm_scrolled(
                        widgets,
                        driver,
                        probes,
                        "workflow.workspace_and_advanced",
                        RelativeOffset::START,
                    )
                } else if step == 1 {
                    self.annotation_arm(widgets, driver, probes, "workflow.diagnostics")
                } else if step < 7 {
                    self.annotation_arm_scrolled(
                        widgets,
                        driver,
                        probes,
                        self.copy_layout_control(step),
                        RelativeOffset::START,
                    )
                } else {
                    if widgets.location_pending() {
                        return Task::none();
                    }
                    if !probes.prepare_control_probe(
                        widgets,
                        self.copy_swatch_color,
                        self.copy_capability_available,
                    ) {
                        return Task::none();
                    }
                    self.copy_swatch_ready = false;
                    self.annotation_arm_scrolled(
                        widgets,
                        driver,
                        probes,
                        "annotation.class.active.swatch",
                        RelativeOffset::START,
                    )
                }
            }
            Phase::CopySwatchWait => {
                if (*probes.annotation_observation().control_receipt)
                    != current_receipt("workflow.visual.workspace")
                {
                    self.copy_swatch_ready = false;
                    return advance_annotation(driver, Phase::CopyLayout(7));
                }
                if self.copy_swatch_ready {
                    advance_annotation(driver, Phase::CopyProductStart)
                } else {
                    Task::none()
                }
            }
            Phase::CopyCapability => {
                let Some(ui) = model
                    .annotation
                    .snapshot
                    .as_ref()
                    .map(|snapshot| &snapshot.ui)
                else {
                    return Task::none();
                };
                self.copy_capability_available = ui.toolcapabilities.iter().any(|fact| {
                    fact.tool == crate::generated::AnnotationTool::ColorSample && fact.available
                });
                let theme = crate::fluent_theme::app_theme(
                    settings
                        .draft
                        .as_ref()
                        .is_some_and(|draft| draft.ui.darkmode),
                );
                let style = iced_fluent_theme::button::rounded::secondary(
                    &theme,
                    if self.copy_capability_available {
                        iced::widget::button::Status::Active
                    } else {
                        iced::widget::button::Status::Disabled
                    },
                );
                let Some(iced::Background::Color(color)) = style.background else {
                    driver.fail("Annotation tool background is not sampleable");
                    return Task::none();
                };
                self.copy_swatch_color = [
                    f64::from(color.r) * 255.0,
                    f64::from(color.g) * 255.0,
                    f64::from(color.b) * 255.0,
                ];
                if !probes.prepare_control_probe(
                    widgets,
                    self.copy_swatch_color,
                    self.copy_capability_available,
                ) {
                    return Task::none();
                }
                self.copy_capability_ready = false;
                self.annotation_arm_scrolled(
                    widgets,
                    driver,
                    probes,
                    annotation::tool_id(crate::generated::AnnotationTool::ColorSample),
                    RelativeOffset::START,
                )
            }
            Phase::CopyCapabilityWait => {
                if (*probes.annotation_observation().control_receipt)
                    != current_receipt("workflow.visual.workspace")
                {
                    self.copy_capability_ready = false;
                    return advance_annotation(driver, Phase::CopyCapability);
                }
                if self.copy_capability_ready {
                    advance_annotation(driver, Phase::CopyProductStart)
                } else {
                    Task::none()
                }
            }
            Phase::CopyAwaitScale(stage) => {
                let scale = self.copy_requested_scale;
                let Some(settled) =
                    settled_settings_snapshot(model, settings, self.settings_revision)
                else {
                    return Task::none();
                };
                if !same_numeric_value(
                    f64::from(settled.settingsstate.ui.uiscale),
                    f64::from(scale),
                ) || !same_numeric_value(f64::from(applied_scale), f64::from(scale))
                {
                    driver.fail("Annotation scale did not settle at the requested valid value");
                    return Task::none();
                }
                let width = model.window_width as f32;
                if !width.is_finite() || width <= 0.0 {
                    driver.fail("Annotation settled viewport is invalid");
                    return Task::none();
                }
                self.copy_narrow = width < crate::view::PAGE_MIN_WIDTH;
                if stage != CopyScaleStage::Restore
                    && self.copy_narrow != (stage == CopyScaleStage::Narrow)
                {
                    driver.fail("Annotation scale did not establish the requested layout");
                    return Task::none();
                }
                if stage != CopyScaleStage::Restore {
                    let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                        return Task::none();
                    };
                    if let Err(detail) = self.copy_product.start(&snapshot.ui, self.copy_objects) {
                        driver.fail(detail);
                        return Task::none();
                    }
                    advance_annotation(driver, Phase::CopyLayout(0))
                } else {
                    driver.phase = Phase::CopyAwaitOutput;
                    annotation_message(annotation::Message::OutputDirectoryChanged(format!(
                        "{}/viewer-annotations.cbor",
                        driver.compiled_directory
                    )))
                }
            }
            Phase::CopyProductStart => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !model.annotation_edit_available() {
                    return Task::none();
                }
                let Some(action) = self.copy_product.action(&snapshot.ui) else {
                    if !self.copy_product.finished() {
                        driver.fail("Annotation product step lost its required object geometry");
                        return Task::none();
                    }
                    self.copy_product_gesture = None;
                    return self.copy_scale_transition(
                        driver,
                        model,
                        applied_scale,
                        if self.copy_narrow {
                            CopyScaleStage::Restore
                        } else {
                            CopyScaleStage::Narrow
                        },
                    );
                };
                self.copy_product_frame = snapshot.frame.revision;
                // Commands and completed reads can leave pixels unchanged.
                // Scene/editor matching and exact draw evidence remain required.
                self.copy_product_ui_revision = snapshot.uirevision;
                self.copy_product_settlement = action.settlement;
                self.copy_product_gesture = action.gesture;
                self.copy_product_cancel = action.cancel;
                self.copy_product_cancelled = false;
                if let Some(tool) = action.tool {
                    driver.phase = Phase::AnnotationTool {
                        revision: snapshot.ui.interactionrevision,
                        tool,
                    };
                    self.annotation_arm_scrolled(
                        widgets,
                        driver,
                        probes,
                        annotation::tool_id(tool),
                        RelativeOffset::START,
                    )
                } else if action.gesture.is_some() {
                    driver.phase = Phase::AnnotationSurface(snapshot.ui.interactionrevision);
                    self.annotation_arm_scrolled(
                        widgets,
                        driver,
                        probes,
                        ANNOTATION_SURFACE,
                        RelativeOffset::START,
                    )
                } else {
                    let completion = advance_annotation(driver, Phase::CopyProductWait);
                    action
                        .messages
                        .into_iter()
                        .fold(Task::none(), |tasks, message| {
                            tasks.chain(annotation_message(message))
                        })
                        .chain(completion)
                }
            }
            Phase::CopyAwaitOutput => {
                let destination = format!("{}/viewer-annotations.cbor", driver.compiled_directory);
                if settings.has_local_edits()
                    || !model.annotation_save_available()
                    || !model.settings_snapshot.as_ref().is_some_and(|snapshot| {
                        snapshot.settingsstate.workflows.annotate.outputdir == destination
                    })
                {
                    return Task::none();
                }
                driver.phase = Phase::CopySave;
                self.annotation_arm_scrolled(
                    widgets,
                    driver,
                    probes,
                    VIEWER_SAVE,
                    RelativeOffset::END,
                )
            }
            Phase::CopySave => self.annotation_arm_scrolled(
                widgets,
                driver,
                probes,
                VIEWER_SAVE,
                RelativeOffset::END,
            ),
            Phase::CopyAwaitSave => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || snapshot.ui.savestatus != crate::generated::AnnotationSaveStatus::Saved
                {
                    return Task::none();
                }
                let Some((presentation, source)) = probes.draws().annotation else {
                    return Task::none();
                };
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_complete",
                        VIEWER_SAVE,
                        "copy",
                        [presentation as f64, source as f64, 1.0, 0.0],
                    )
                });
                driver.phase = Phase::Complete;
                Task::none()
            }
            Phase::AwaitAnnotation => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if active != FeatureId::Annotate
                    || !snapshot.ready
                    || snapshot.busy
                    || snapshot.frame.revision == 0
                    || !model.annotation_edit_available()
                    || self
                        .annotation_open
                        .as_ref()
                        .is_none_or(|(_, before)| snapshot.inputdocumentepoch <= *before)
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.annotation_ready",
                        "",
                        "receiver-owned",
                        [
                            snapshot.frame.revision as f64,
                            snapshot.ui.documentrevision as f64,
                            snapshot.ui.interactionrevision as f64,
                            0.0,
                        ],
                    )
                });
                if matches!(driver.viewer_scenario.as_str(), "quiet" | "terminal") {
                    self.bounded_document_revision = snapshot.ui.documentrevision;
                    self.bounded_object_count = snapshot.ui.scene.objects.len();
                    driver.phase = Phase::AnnotationTool {
                        revision: snapshot.ui.interactionrevision,
                        tool: crate::generated::AnnotationTool::Box,
                    };
                    return self.annotation_arm_scrolled(
                        widgets,
                        driver,
                        probes,
                        annotation::tool_id(crate::generated::AnnotationTool::Box),
                        RelativeOffset::START,
                    );
                }
                if driver.viewer_scenario == "copy" {
                    let scene = &snapshot.ui.scene;
                    let Some((index, object)) =
                        scene.objects.iter().enumerate().find(|(_, object)| {
                            matches!(
                                object.shape,
                                crate::generated::AnnotationShape::Box
                                    | crate::generated::AnnotationShape::Mask
                            ) && object.box_.first.x < object.box_.second.x
                                && object.box_.first.y < object.box_.second.y
                        })
                    else {
                        driver.fail("viewed document imported no editable box geometry");
                        return Task::none();
                    };
                    if !scene.objects.iter().any(|object| {
                        object.shape == crate::generated::AnnotationShape::Mask
                            && !object.mask.runs.is_empty()
                    }) {
                        driver.fail("viewed document imported no editable mask");
                        return Task::none();
                    }
                    let Some((request, _)) = self.annotation_open.as_ref() else {
                        driver.fail("Annotation import has no dispatched source receipt");
                        return Task::none();
                    };
                    let expected = if request.originalcontent {
                        [request.source.content.width, request.source.content.height]
                    } else {
                        [request.source.extent.width, request.source.extent.height]
                    };
                    if !request.originalcontent
                        || snapshot.frame.extent.width != expected[0]
                        || snapshot.frame.extent.height != expected[1]
                        || u32::from(scene.framewidth) != expected[0]
                        || u32::from(scene.frameheight) != expected[1]
                    {
                        driver.fail("Annotation did not copy the viewed upscale crop");
                        return Task::none();
                    }
                    self.copy_before = Some(object.clone());
                    self.copy_objects = scene.objects.len();
                    self.copy_categories = scene.categories.clone();
                    return self.begin_copy_object_edit(
                        widgets,
                        driver,
                        probes,
                        snapshot,
                        index as u16,
                        false,
                    );
                }
                let Some(tool) = crate::generated::ANNOTATION_TOOL_VALUES
                    .iter()
                    .copied()
                    .find(|tool| *tool != snapshot.ui.editor.tool)
                else {
                    driver.fail("generated Annotation tool inventory has no selectable tool");
                    return Task::none();
                };
                driver.phase = Phase::AnnotationSidebar {
                    revision: snapshot.ui.interactionrevision,
                    tool,
                };
                self.annotation_arm(widgets, driver, probes, ANNOTATION_SIDEBAR)
            }
            Phase::AnnotationSidebar { revision, tool } => {
                driver.phase = Phase::AnnotationSidebar { revision, tool };
                self.annotation_arm(widgets, driver, probes, ANNOTATION_SIDEBAR)
            }
            Phase::AnnotationTimeline { revision, tool } => {
                driver.phase = Phase::AnnotationTimeline { revision, tool };
                self.annotation_arm(widgets, driver, probes, ANNOTATION_TIMELINE)
            }
            Phase::AnnotationOperation { revision, tool } => {
                driver.phase = Phase::AnnotationOperation { revision, tool };
                self.annotation_arm(widgets, driver, probes, ANNOTATION_OPERATION)
            }
            Phase::AnnotationStop { revision, tool } => {
                driver.phase = Phase::AnnotationStop { revision, tool };
                self.annotation_arm(widgets, driver, probes, ANNOTATION_STOP)
            }
            Phase::AnnotationBrush { revision, tool } => {
                driver.phase = Phase::AnnotationBrush { revision, tool };
                self.annotation_arm(widgets, driver, probes, ANNOTATION_BRUSH_RADIUS)
            }
            Phase::AnnotationTool { revision, tool } => {
                driver.phase = Phase::AnnotationTool { revision, tool };
                self.annotation_arm_scrolled(
                    widgets,
                    driver,
                    probes,
                    annotation::tool_id(tool),
                    RelativeOffset::START,
                )
            }
            Phase::AwaitTool { revision, tool } => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || (self.copy_step != 8 && snapshot.ui.interactionrevision <= revision)
                    || (self.copy_step == 8 && !self.copy_product_settled(snapshot))
                    || snapshot.ui.editor.tool != tool
                    || !model.annotation_edit_available()
                {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.annotation_tool_wait",
                            &annotation::tool_id(tool),
                            &format!(
                                "observed={:?}; busy={}; editable={}; copy_step={}; ui_revision={}; settlement={:?}; ui_baseline={}",
                                snapshot.ui.editor.tool,
                                snapshot.busy,
                                model.annotation_edit_available(),
                                self.copy_step,
                                snapshot.uirevision,
                                self.copy_product_settlement,
                                self.copy_product_ui_revision,
                            ),
                            [
                                revision as f64,
                                snapshot.ui.interactionrevision as f64,
                                self.copy_product_frame as f64,
                                snapshot.frame.revision as f64,
                            ],
                        )
                    });
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.annotation_tool_observed",
                        &annotation::tool_id(tool),
                        "typed-tool",
                        [
                            revision as f64,
                            snapshot.ui.interactionrevision as f64,
                            0.0,
                            0.0,
                        ],
                    )
                });
                if self.copy_step == 8 {
                    advance_annotation(driver, Phase::CopyProductWait)
                } else {
                    driver.phase = Phase::AnnotationSurface(snapshot.ui.interactionrevision);
                    self.annotation_arm_scrolled(
                        widgets,
                        driver,
                        probes,
                        ANNOTATION_SURFACE,
                        RelativeOffset::START,
                    )
                }
            }
            Phase::AnnotationSurface(revision) => {
                driver.phase = Phase::AnnotationSurface(revision);
                self.annotation_arm_scrolled(
                    widgets,
                    driver,
                    probes,
                    ANNOTATION_SURFACE,
                    RelativeOffset::START,
                )
            }
            Phase::AwaitAnnotationFrame(revision) => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !probes
                    .annotation_observation()
                    .frame_ready
                    .is_some_and(|sampleable| sampleable.source_revision == snapshot.frame.revision)
                {
                    return Task::none();
                }
                driver.phase = Phase::AnnotationPointer(revision);
                self.annotation_arm(widgets, driver, probes, ANNOTATION_SURFACE)
            }
            Phase::AnnotationPointer(revision) => {
                driver.phase = Phase::AnnotationPointer(revision);
                self.annotation_arm(widgets, driver, probes, ANNOTATION_SURFACE)
            }
            Phase::AwaitPointer(_) | Phase::CopyProductWait => {
                let revision = if let Phase::AwaitPointer(revision) = driver.phase {
                    revision
                } else {
                    0
                };
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if matches!(driver.viewer_scenario.as_str(), "quiet" | "terminal") {
                    let edited = model.annotation_edit_available()
                        && snapshot.ui.documentrevision > self.bounded_document_revision
                        && snapshot.ui.scene.objects.len() == self.bounded_object_count + 1
                        && snapshot.ui.canundo
                        && snapshot.ui.scene.objects.last().is_some_and(|object| {
                            object.shape == crate::generated::AnnotationShape::Box
                                && object.box_.second.x > object.box_.first.x
                                && object.box_.second.y > object.box_.first.y
                        });
                    if !edited {
                        return Task::none();
                    }
                    if driver.viewer_scenario == "quiet" {
                        driver.phase = Phase::Complete;
                        return Task::none();
                    }
                }
                let Some((_, image)) = crate::presentation_surface::retained_surface()
                    .and_then(crate::presentation_surface::drawable_annotation)
                else {
                    return Task::none();
                };
                let Some(rendered) = image.metadata.diagnostics.as_ref() else {
                    return Task::none();
                };

                reporting::emit(|sink| {
                    sink.record(
                        "integration.annotation_settlement",
                        ANNOTATION_SURFACE,
                        &format!(
                            "phase={:?}; editable={}; busy={}; ui_revision={}; interaction={}; settlement={:?}; ui_baseline={}; epoch={}/{}; scene={}/{}; editor_match={}; sampleable={:?}; presentation={:?}; drawn={:?}; location_pending={}; probe_pending={}",
                            driver.phase,
                            model.annotation_edit_available(),
                            snapshot.busy,
                            snapshot.uirevision,
                            snapshot.ui.interactionrevision,
                            self.copy_product_settlement,
                            self.copy_product_ui_revision,
                            snapshot.inputdocumentepoch,
                            rendered.documentepoch,
                            snapshot.ui.scenerevision,
                            rendered.scenerevision,
                            snapshot.ui.editor == rendered.editor,
                            probes.annotation_observation().frame_ready,
                            frame.and_then(|frame| crate::presentation_surface::metadata::product(frame)
                                .map(|product| (product.source.kind, product.revision, frame.presentation_revision))),
                            probes.draws().annotation,
                            widgets.location_pending(),
                            (*probes.annotation_observation().pending).is_some(),
                        ),
                        [
                            self.copy_product_frame as f64,
                            snapshot.frame.revision as f64,
                            revision as f64,
                            self.copy_step as f64,
                        ],
                    )
                });
                let Some(sampleable) = probes.annotation_observation().frame_ready else {
                    return Task::none();
                };
                let presentation_revision = sampleable.presentation_revision;
                if !model.annotation_edit_available()
                    || (self.copy_step != 8 && snapshot.ui.interactionrevision <= revision)
                    || (self.copy_step == 8 && !self.copy_product_settled(snapshot))
                    || sampleable.source_revision != snapshot.frame.revision
                    || frame
                        .and_then(crate::presentation_surface::metadata::product)
                        .is_none_or(|product| {
                            product.source.kind
                                != crate::generated::PresentationSourceKind::Annotation
                                || product.revision != snapshot.frame.revision
                        })
                    || frame
                        .is_none_or(|frame| frame.presentation_revision != presentation_revision)
                    || probes.draws().annotation
                        != Some((presentation_revision, snapshot.frame.revision))
                {
                    return Task::none();
                }
                if rendered.documentepoch != snapshot.inputdocumentepoch
                    || rendered.scenerevision != snapshot.ui.scenerevision
                    || rendered.editor != snapshot.ui.editor
                {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.annotation_render_wait",
                            ANNOTATION_SURFACE,
                            "logical-scene-and-rendered-scene",
                            [
                                snapshot.inputdocumentepoch as f64,
                                rendered.documentepoch as f64,
                                snapshot.ui.scenerevision as f64,
                                rendered.scenerevision as f64,
                            ],
                        )
                    });
                    return Task::none();
                }
                if self.copy_step == 8 && self.copy_product_cancel && !self.copy_product_cancelled {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.annotation_preview",
                            ANNOTATION_SURFACE,
                            "visible-before-cancel",
                            [
                                self.copy_product_frame as f64,
                                snapshot.frame.revision as f64,
                                presentation_revision as f64,
                                0.0,
                            ],
                        )
                    });
                    self.copy_product_cancelled = true;
                    self.copy_product_frame = snapshot.frame.revision;
                    return annotation_message(annotation::Message::CancelRequested);
                }
                let receipt = current_receipt("workflow.visual.workspace");
                if driver.viewer_scenario == "copy"
                    && (receipt.is_none() || (*probes.annotation_observation().receipt) != receipt)
                {
                    if widgets.location_pending()
                        || (*probes.annotation_observation().pending) == receipt
                    {
                        return Task::none();
                    }
                    let samples = annotation_checks::probes(&snapshot.ui);
                    let key = (snapshot.frame.revision, presentation_revision);
                    if self.annotation_pixel_progress.0 != key
                        || self.annotation_pixel_progress.1 >= samples.len() / 7
                    {
                        self.annotation_pixel_progress = (key, 0, samples.len() / 7);
                    }
                    let index = self.annotation_pixel_progress.1;
                    let Some(pixel) = samples.chunks_exact(7).nth(index) else {
                        driver.fail("Annotation pixel inventory lost its next expected sample");
                        return Task::none();
                    };
                    if !probes.prepare_annotation_probe(
                        widgets,
                        snapshot.frame.revision,
                        presentation_revision,
                        [snapshot.frame.extent.width, snapshot.frame.extent.height],
                        pixel.to_vec(),
                    ) {
                        return Task::none();
                    }
                    return self.annotation_arm(widgets, driver, probes, ANNOTATION_SURFACE);
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.annotation_pointer_observed",
                        ANNOTATION_SURFACE,
                        "typed-interaction",
                        [
                            revision as f64,
                            snapshot.ui.interactionrevision as f64,
                            presentation_revision as f64,
                            0.0,
                        ],
                    )
                });
                if driver.viewer_scenario == "copy" && self.copy_step == 8 {
                    #[cfg(target_arch = "wasm32")]
                    if self.copy_product_cancel {
                        annotation_release_js();
                    }
                    match self.copy_product.observe(&snapshot.ui) {
                        Ok(step) => {
                            reporting::emit(|sink| {
                                sink.record(
                                    "integration.annotation_product",
                                    ANNOTATION_SURFACE,
                                    &step.detail(),
                                    [
                                        snapshot.frame.revision as f64,
                                        presentation_revision as f64,
                                        snapshot.ui.documentrevision as f64,
                                        snapshot
                                            .ui
                                            .toolcapabilities
                                            .iter()
                                            .filter(|fact| fact.available)
                                            .count() as f64,
                                    ],
                                )
                            });
                            return advance_annotation(
                                driver,
                                match step {
                                    annotation_product::Step::Select(_) => Phase::CopyCapability,
                                    _ => Phase::CopyProductStart,
                                },
                            );
                        }
                        Err(detail) => {
                            driver.fail(detail);
                            return Task::none();
                        }
                    }
                }
                if driver.viewer_scenario == "copy" && (5..=7).contains(&self.copy_step) {
                    let Some(index) = snapshot.ui.editor.selectedobject else {
                        return Task::none();
                    };
                    let object = &snapshot.ui.scene.objects[index as usize];
                    let expected = match self.copy_step {
                        5 => crate::generated::AnnotationShape::Point,
                        6 => crate::generated::AnnotationShape::Spline,
                        _ => crate::generated::AnnotationShape::Skeleton,
                    };
                    if object.shape != expected
                        || snapshot.ui.scene.objects.len()
                            != self.copy_objects + usize::from(self.copy_step - 4)
                    {
                        driver
                            .fail("annotation shape creation did not publish its rendered object");
                        return Task::none();
                    }
                    self.copy_shape_points += 1;
                    if (self.copy_step == 6 && self.copy_shape_points < 3)
                        || (self.copy_step == 7 && self.copy_shape_points < 2)
                    {
                        driver.phase = Phase::AnnotationSurface(snapshot.ui.interactionrevision);
                        return self.annotation_arm_scrolled(
                            widgets,
                            driver,
                            probes,
                            ANNOTATION_SURFACE,
                            RelativeOffset::START,
                        );
                    }
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.annotation_shape",
                            ANNOTATION_SURFACE,
                            &format!("{:?}", object.shape),
                            [
                                index as f64,
                                snapshot.frame.revision as f64,
                                object.splineknots.len() as f64,
                                object.skeletonnodes.len() as f64,
                            ],
                        )
                    });
                    if self.copy_step == 7 {
                        if let Err(detail) =
                            self.copy_product.start(&snapshot.ui, self.copy_objects)
                        {
                            driver.fail(detail);
                            return Task::none();
                        }
                        self.copy_step = 8;
                        self.copy_original_scale = settings
                            .draft
                            .as_ref()
                            .map_or(1.0, |draft| draft.ui.uiscale);
                        self.copy_narrow =
                            (model.window_width as f32) < crate::view::PAGE_MIN_WIDTH;
                        self.copy_list_object_target = snapshot.ui.scene.objects.len() + 32;
                        self.copy_list_class_target = snapshot.ui.scene.categories.len() + 32;
                        self.copy_list_revision = snapshot.uirevision.saturating_sub(1);
                        return advance_annotation(
                            driver,
                            Phase::CopyListSetup {
                                stage: 0,
                                revision: snapshot.uirevision,
                            },
                        );
                    }
                    self.copy_step += 1;
                    self.copy_shape_points = 0;
                    let tool = if self.copy_step == 6 {
                        crate::generated::AnnotationTool::Spline
                    } else {
                        crate::generated::AnnotationTool::Skeleton
                    };
                    driver.phase = Phase::AnnotationTool {
                        revision: snapshot.ui.interactionrevision,
                        tool,
                    };
                    return self.annotation_arm_scrolled(
                        widgets,
                        driver,
                        probes,
                        annotation::tool_id(tool),
                        RelativeOffset::START,
                    );
                }
                if driver.viewer_scenario == "copy" {
                    let Some(index) = snapshot.ui.editor.selectedobject else {
                        return Task::none();
                    };
                    let object = &snapshot.ui.scene.objects[index as usize];
                    if self.copy_before.as_ref() == Some(object)
                        || snapshot.ui.scene.objects.len() != self.copy_objects
                        || snapshot.ui.scene.categories != self.copy_categories
                    {
                        driver.fail("pointer did not independently edit the imported object");
                        return Task::none();
                    }
                    let mask = matches!(
                        snapshot.ui.editor.tool,
                        crate::generated::AnnotationTool::MaskPaint
                            | crate::generated::AnnotationTool::MaskErase
                    );
                    if self.copy_before.as_ref().is_none_or(|before| {
                        if mask {
                            object.mask == before.mask
                        } else {
                            object.box_ == before.box_
                        }
                    }) {
                        driver
                            .fail("editing imported geometry changed its independent counterpart");
                        return Task::none();
                    }
                    self.copy_after = Some(object.clone());
                    driver.phase = Phase::CopyUndo { index, mask };
                    return self.annotation_arm_scrolled(
                        widgets,
                        driver,
                        probes,
                        "annotation.undo",
                        RelativeOffset::END,
                    );
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.complete",
                        "",
                        "typed-mvc-wayland",
                        [
                            snapshot.frame.revision as f64,
                            presentation_revision as f64,
                            presentation_revision as f64,
                            crate::presentation_surface::retained_surface()
                                .and_then(|surface| surface.frame)
                                .map_or(0, |frame| frame.presentation_revision)
                                as f64,
                        ],
                    )
                });
                driver.phase = Phase::Complete;
                driver.report_phase_progress();
                Task::none()
            }
            _ => Task::none(),
        }
    }
    pub(super) fn begin_copy_object_edit(
        &mut self,
        widgets: &mut widget_ops::RevealState,
        driver: &mut Driver,
        probes: &mut probe::Requests,
        snapshot: &crate::generated::AnnotationSnapshot,
        index: u16,
        mask: bool,
    ) -> Task<RootMessage> {
        if snapshot.ui.editor.selectedobject != Some(index) {
            driver.phase = Phase::CopyAwaitObject { index, mask };
            return annotation_message(annotation::Message::Sidebar(
                annotation::sidebar::Message::ObjectSelected(index),
            ));
        }
        let tool = if mask {
            if self.copy_step == 3 {
                crate::generated::AnnotationTool::MaskErase
            } else {
                crate::generated::AnnotationTool::MaskPaint
            }
        } else {
            crate::generated::AnnotationTool::Select
        };
        if snapshot.ui.editor.tool == tool {
            driver.phase = Phase::AnnotationSurface(snapshot.ui.interactionrevision);
            return self.annotation_arm_scrolled(
                widgets,
                driver,
                probes,
                ANNOTATION_SURFACE,
                RelativeOffset::START,
            );
        }
        driver.phase = Phase::AnnotationTool {
            revision: snapshot.ui.interactionrevision,
            tool,
        };
        self.annotation_arm_scrolled(
            widgets,
            driver,
            probes,
            annotation::tool_id(tool),
            RelativeOffset::START,
        )
    }
    pub(super) fn annotation_points(&self, driver: &Driver, width: f64, height: f64) -> [f64; 4] {
        self.copy_product_gesture.unwrap_or_else(|| {
            if let Some(object) = self
                .copy_before
                .as_ref()
                .filter(|_| driver.viewer_scenario == "copy")
            {
                let b = &object.box_;
                let mut start = [
                    f64::from((b.first.x + b.second.x) / 2.0),
                    f64::from((b.first.y + b.second.y) / 2.0),
                ];
                if self.copy_step == 0 || self.copy_step == 3 {
                    if let Some(run) = object.mask.runs.first() {
                        start = [
                            (f64::from(run.first) + f64::from(run.last)) / 2.0,
                            f64::from(run.row) + 0.5,
                        ];
                    }
                }
                if self.copy_step == 2 {
                    start = [
                        (f64::from(b.second.x) + 3.0).min(width - 1.0),
                        f64::from((b.first.y + b.second.y) / 2.0),
                    ];
                }
                if self.copy_step == 1 {
                    start = [f64::from(b.second.x), f64::from(b.second.y)];
                }
                [
                    start[0],
                    start[1],
                    (start[0] + 8.0).min(width - 1.0),
                    (start[1] + 8.0).min(height - 1.0),
                ]
            } else {
                let top = match self.copy_step {
                    5 => 0.15,
                    6 => 0.35,
                    7 => 0.7,
                    _ => 0.3,
                };
                let [x, y, ex, ey] = match self.copy_shape_points {
                    0 => [0.25, top, 0.3, top + 0.05],
                    1 => [0.6, top, 0.65, top + 0.05],
                    _ => [0.45, 0.65, 0.5, 0.7],
                };
                [x * width, y * height, ex * width, ey * height]
            }
        })
    }
    pub(super) fn annotation_reveal(
        &self,
        driver: &Driver,
        probes: &probe::Requests,
        control: &str,
    ) -> AnnotationReveal {
        if matches!(driver.phase, Phase::CopyLayout(2 | 3)) {
            return AnnotationReveal::Tail {
                count: if matches!(driver.phase, Phase::CopyLayout(2)) {
                    self.copy_layout_objects
                } else {
                    self.copy_layout_classes
                },
                narrow: self.copy_narrow,
            };
        }
        if control != ANNOTATION_SURFACE {
            return if control == ANNOTATION_SIDEBAR
                || matches!(driver.phase, Phase::PageRegion { .. })
            {
                AnnotationReveal::Geometry
            } else {
                AnnotationReveal::Control
            };
        }
        if let Some(probe) = &probes.annotation_observation().prepared {
            if let Some(pixel) = probe.pixels.chunks_exact(7).next() {
                let radius = pixel[6] as f32;
                return AnnotationReveal::Source {
                    extent: probe.extent.map(|value| value as f32),
                    region: Rectangle {
                        x: pixel[0] as f32 - radius,
                        y: pixel[1] as f32 - radius,
                        width: radius * 2.0,
                        height: radius * 2.0,
                    },
                    margin: 2.0 / driver.input_scale,
                };
            }
        }
        if matches!(driver.phase, Phase::AnnotationPointer(_)) {
            if let Some(frame) = probes.annotation_observation().frame_ready {
                let extent = [frame.content_width as f32, frame.content_height as f32];
                let [x, y, ex, ey] = self
                    .annotation_points(driver, f64::from(extent[0]), f64::from(extent[1]))
                    .map(|value| value as f32);
                return AnnotationReveal::Source {
                    extent,
                    region: Rectangle {
                        x: x.min(ex),
                        y: y.min(ey),
                        width: (ex - x).abs(),
                        height: (ey - y).abs(),
                    },
                    margin: 1.0 / driver.input_scale,
                };
            }
        }
        AnnotationReveal::Geometry
    }
    pub(super) fn copy_scale_transition(
        &mut self,
        driver: &mut Driver,
        model: &ApplicationModel,
        applied_scale: f32,
        stage: CopyScaleStage,
    ) -> Task<RootMessage> {
        let scale = if stage == CopyScaleStage::Restore {
            self.copy_original_scale
        } else {
            match annotation_layout_scale(
                applied_scale,
                model.window_width as f32,
                stage == CopyScaleStage::Narrow,
            ) {
                Ok(value) => value,
                Err(detail) => {
                    driver.fail(detail);
                    return Task::none();
                }
            }
        };
        let unchanged = same_numeric_value(f64::from(scale), f64::from(applied_scale));
        let revision = model
            .settings_snapshot
            .as_ref()
            .map_or(0, |snapshot| snapshot.revision);
        self.settings_revision = if unchanged {
            revision.saturating_sub(1)
        } else {
            revision
        };
        self.copy_requested_scale = scale;
        driver.phase = Phase::CopyAwaitScale(stage);
        if unchanged {
            let phase = driver.phase.clone();
            return advance_annotation(driver, phase);
        }
        Task::done(RootMessage::Settings(
            crate::view::settings::Message::UiScaleChanged(scale),
        ))
        .chain(Task::done(RootMessage::Settings(
            crate::view::settings::Message::UiScaleReleased,
        )))
    }
    pub(super) fn copy_layout_control(&self, step: u8) -> String {
        match step {
            0 => "workflow.workspace_and_advanced".into(),
            1 => "workflow.diagnostics".into(),
            2 => format!(
                "annotation.object.{}",
                self.copy_layout_objects.saturating_sub(1)
            ),
            3 => format!(
                "annotation.class.{}",
                self.copy_layout_classes.saturating_sub(1)
            ),
            4 => VIEWER_SAVE.into(),
            5 => ANNOTATION_TIMELINE.into(),
            6 => ANNOTATION_STOP.into(),
            _ => "annotation.class.active.swatch".into(),
        }
    }
    pub(super) fn copy_product_settled(
        &self,
        snapshot: &crate::generated::AnnotationSnapshot,
    ) -> bool {
        // Command admission advances the UI revision while the operation still
        // owns work. Check its result only after the native owner settles it.
        if snapshot.busy {
            return false;
        }
        match self.copy_product_settlement {
            annotation_product::Settlement::NativeUi => {
                snapshot.uirevision > self.copy_product_ui_revision
            }
            annotation_product::Settlement::RenderedFrame => {
                snapshot.frame.revision > self.copy_product_frame
            }
        }
    }
    pub(super) fn expected_annotation_checks(&self, driver: &Driver) -> Option<String> {
        Some(match driver.phase {
            Phase::CopyUndo { .. } => "annotation.undo".to_owned(),
            Phase::CopyRedo { .. } => "annotation.redo".to_owned(),
            Phase::CopySave => VIEWER_SAVE.to_owned(),
            Phase::CopyCapability => {
                annotation::tool_id(crate::generated::AnnotationTool::ColorSample)
            }
            Phase::CopyLayout(0) => "workflow.workspace_and_advanced".into(),
            Phase::CopyLayout(1) => "workflow.diagnostics".into(),
            Phase::CopyLayout(step) => self.copy_layout_control(step),
            Phase::OpenAnnotation => EXPLORE_ANNOTATE.to_owned(),
            Phase::AnnotationSidebar { .. } => ANNOTATION_SIDEBAR.to_owned(),
            Phase::AnnotationTimeline { .. } => ANNOTATION_TIMELINE.to_owned(),
            Phase::AnnotationOperation { .. } => ANNOTATION_OPERATION.to_owned(),
            Phase::AnnotationStop { .. } => ANNOTATION_STOP.to_owned(),
            Phase::AnnotationBrush { .. } => ANNOTATION_BRUSH_RADIUS.to_owned(),
            Phase::AnnotationTool { tool, .. } => annotation::tool_id(tool),
            Phase::AnnotationSurface(_)
            | Phase::AnnotationPointer(_)
            | Phase::AwaitPointer(_)
            | Phase::CopyProductWait => ANNOTATION_SURFACE.to_owned(),
            _ => return None,
        })
    }
    pub(super) fn located_annotation_checks(
        &mut self,
        driver: &mut Driver,
        probes: &mut probe::Requests,
        control: String,
        bounds: Rectangle,
        input_bounds: Rectangle,
    ) -> Option<train::Message> {
        match driver.phase.clone() {
            Phase::CopyCapability => {
                driver.phase = Phase::CopyCapabilityWait;
                if let Some(probe) = probes.take_control_probe() {
                    probe.sample(input_bounds, &control, true);
                }
                None
            }
            Phase::CopyLayout(0) => {
                self.copy_layout_bounds = bounds;
                driver.phase = Phase::CopyLayout(1);
                None
            }
            Phase::CopyLayout(1) => {
                let image = self.copy_layout_bounds;
                let page = crate::view::canvas_layout(self.copy_viewport_width);
                let valid = (image.width / page.page_width - 0.62).abs() < 0.002
                    && (bounds.width / page.page_width - 0.19).abs() < 0.002
                    && (bounds.x - image.x - image.width).abs() <= 1.0
                    && (bounds.y - image.y).abs() <= 1.0
                    && page.horizontal_overflow == self.copy_narrow;
                if !valid {
                    driver.fail(
                        "Annotation must retain shared columns with narrow horizontal overflow",
                    );
                    return None;
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.annotation_layout",
                        "workflow.diagnostics",
                        if self.copy_narrow { "narrow" } else { "wide" },
                        [
                            f64::from(image.width),
                            f64::from(bounds.width),
                            f64::from(page.page_width),
                            f64::from(self.copy_viewport_width),
                        ],
                    )
                });
                driver.phase = Phase::CopyLayout(2);
                None
            }
            Phase::CopyLayout(step @ 2..=6) => {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.annotation_reachable",
                        &control,
                        if self.copy_narrow { "narrow" } else { "wide" },
                        [
                            f64::from(bounds.x),
                            f64::from(bounds.y),
                            f64::from(bounds.width),
                            f64::from(bounds.height),
                        ],
                    )
                });
                driver.phase = Phase::CopyLayout(step + 1);
                None
            }
            Phase::CopyLayout(_) => {
                driver.phase = Phase::CopySwatchWait;
                if let Some(probe) = probes.take_control_probe() {
                    probe.sample(input_bounds, &control, false);
                }
                None
            }
            Phase::AwaitPointer(_) | Phase::CopyProductWait
                if probes.annotation_observation().prepared.is_some() =>
            {
                if let Some(probe) = probes.take_annotation_probe() {
                    #[cfg(not(target_arch = "wasm32"))]
                    let _ = probe;
                    #[cfg(target_arch = "wasm32")]
                    {
                        let mut output = probe.output;
                        let source = probe.source;
                        let canvas_probe = output.canvas_probe.clone();
                        let callback = pixel_result_callback(move |outcome| {
                            let _ = output.try_send(Message::AnnotationPixels {
                                revision: source,
                                outcome,
                            });
                        });
                        annotation_pixels_js(
                            &canvas_probe,
                            &[
                                f64::from(input_bounds.x),
                                f64::from(input_bounds.y),
                                f64::from(input_bounds.width),
                                f64::from(input_bounds.height),
                            ],
                            &probe.extent.map(f64::from),
                            &probe.pixels,
                            source as f64,
                            probe.presentation as f64,
                            &callback,
                        );
                    }
                }
                None
            }
            Phase::CopyUndo { index, mask } => {
                driver.phase = Phase::CopyAwaitUndo { index, mask };
                if !click(input_bounds) {
                    driver.fail("imported object undo click failed");
                }
                None
            }
            Phase::CopyRedo { index, mask } => {
                driver.phase = Phase::CopyAwaitRedo { index, mask };
                if !click(input_bounds) {
                    driver.fail("imported object redo click failed");
                }
                None
            }
            Phase::CopySave => {
                driver.phase = Phase::CopyAwaitSave;
                if !click(input_bounds) {
                    driver.fail("imported document save click failed");
                }
                None
            }
            Phase::AnnotationPointer(revision) => {
                #[cfg(target_arch = "wasm32")]
                let gesture = {
                    let frame = probes
                        .annotation_observation()
                        .frame_ready
                        .expect("sampleable annotation frame");
                    let (width, height) = (
                        f64::from(frame.content_width),
                        f64::from(frame.content_height),
                    );
                    let points = self.annotation_points(driver, width, height);
                    annotation_checks::place_gesture(input_bounds, (width, height), points)
                };
                #[cfg(target_arch = "wasm32")]
                let dispatched = annotation_pointer_js(
                    f64::from(input_bounds.x),
                    f64::from(input_bounds.y),
                    f64::from(input_bounds.width),
                    f64::from(input_bounds.height),
                    gesture[0],
                    gesture[1],
                    gesture[2],
                    gesture[3],
                    self.copy_product_cancel,
                    if driver.viewer_scenario == "quiet" {
                        160
                    } else {
                        1
                    },
                ) == 1;
                #[cfg(not(target_arch = "wasm32"))]
                let dispatched = false;
                if !dispatched {
                    driver.fail("Firefox annotation pointer dispatch failed");
                    return None;
                }
                driver.phase = if self.copy_step == 8 {
                    Phase::CopyProductWait
                } else {
                    Phase::AwaitPointer(revision)
                };
                None
            }
            Phase::AnnotationTool { revision, tool } => {
                driver.phase = Phase::AwaitTool { revision, tool };
                if !click(input_bounds) {
                    driver.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::AnnotationSidebar { revision, tool } => {
                driver.phase = Phase::AnnotationOperation { revision, tool };
                None
            }
            Phase::AnnotationOperation { revision, tool } => {
                driver.phase = Phase::AnnotationStop { revision, tool };
                None
            }
            Phase::AnnotationStop { revision, tool } => {
                driver.phase = Phase::AnnotationBrush { revision, tool };
                None
            }
            Phase::AnnotationBrush { revision, tool } => {
                driver.phase = Phase::AnnotationTimeline { revision, tool };
                None
            }
            Phase::AnnotationTimeline { revision, tool } => {
                driver.phase = Phase::AnnotationTool { revision, tool };
                None
            }
            Phase::AnnotationSurface(revision) => {
                driver.phase = Phase::AwaitAnnotationFrame(revision);
                None
            }
            _ => {
                driver.phase = match driver.phase {
                    Phase::OpenAnnotation => Phase::AwaitAnnotation,
                    _ => driver.phase.clone(),
                };
                driver.click_located(input_bounds)
            }
        }
    }
    pub(super) fn annotation_arm(
        &mut self,
        widgets: &mut widget_ops::RevealState,
        driver: &mut Driver,
        probes: &mut probe::Requests,
        control: impl Into<String>,
    ) -> Task<RootMessage> {
        if !widgets.begin_location() {
            return Task::none();
        }
        let control = control.into();
        if control.starts_with("annotation.") {
            reveal_control(
                control.clone(),
                driver.generation,
                self.annotation_reveal(driver, probes, &control),
            )
        } else {
            locate(control, driver.generation)
        }
    }
    pub(super) fn annotation_arm_scrolled(
        &mut self,
        widgets: &mut widget_ops::RevealState,
        driver: &mut Driver,
        probes: &mut probe::Requests,
        control: impl Into<String>,
        offset: RelativeOffset,
    ) -> Task<RootMessage> {
        if !widgets.begin_location() {
            return Task::none();
        }
        let control = control.into();
        if control.starts_with("annotation.") {
            let reveal = self.annotation_reveal(driver, probes, &control);
            let task = reveal_control(control.clone(), driver.generation, reveal);
            if matches!(reveal, AnnotationReveal::Tail { .. }) {
                iced::widget::operation::snap_to(crate::view::PAGE_SCROLL_ID, RelativeOffset::START)
                    .chain(iced::widget::operation::snap_to(
                        crate::view::HORIZONTAL_SCROLL_ID,
                        RelativeOffset::START,
                    ))
                    .chain(task)
            } else {
                task
            }
        } else {
            iced::widget::operation::snap_to(crate::view::PAGE_SCROLL_ID, offset)
                .chain(locate(control, driver.generation))
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
            Message::AnnotationControlPixels { outcome } => {
                if !probes.complete_capability_probe(&request_receipt, &outcome) {
                    return;
                }
                match outcome {
                    ProbeOutcome::Invalidated => {}
                    ProbeOutcome::Observed(1, 1) => self.copy_capability_ready = true,
                    _ => {
                        driver.fail("Rendered tool availability differs from the native capability")
                    }
                }
                return;
            }
            Message::AnnotationPixels { revision, outcome } => {
                if !probes.take_annotation_completion(revision, &request_receipt) {
                    return;
                }
                match outcome {
                    ProbeOutcome::Invalidated => {}
                    ProbeOutcome::Observed(expected, matched)
                        if expected != 0 && expected == matched =>
                    {
                        if revision == 0 {
                            probes.accept_annotation_pixels(0, request_receipt);
                            self.copy_swatch_ready = true;
                        } else {
                            self.annotation_pixel_progress.1 += 1;
                            if self.annotation_pixel_progress.1 >= self.annotation_pixel_progress.2
                            {
                                probes.accept_annotation_pixels(revision, request_receipt);
                            }
                        }
                    }
                    _ => driver
                        .fail("Annotation pixels do not match source geometry and native palette"),
                }
                return;
            }
            _ => unreachable!("callback routed to the wrong scenario owner"),
        }
    }
}

impl State {
    pub(super) fn observe_open(
        &mut self,
        request: crate::generated::AnnotationOpen,
        document_epoch: u64,
    ) {
        self.annotation_open = Some((request, document_epoch));
    }
    #[cfg(test)]
    pub(super) fn open_for_test(&self) -> Option<(&crate::generated::AnnotationOpen, u64)> {
        self.annotation_open
            .as_ref()
            .map(|(request, epoch)| (request, *epoch))
    }
}

pub(super) fn annotation_layout_scale(
    current_scale: f32,
    logical_width: f32,
    narrow: bool,
) -> Result<f32, &'static str> {
    let constraint = crate::generated::constraint_uiuiscale();
    let (minimum, maximum) = constraint
        .minimum
        .zip(constraint.maximum)
        .ok_or("Annotation narrow scale requires native bounds")?;
    let (minimum, maximum) = (minimum as f32, maximum as f32);
    if !constraint.finite
        || !minimum.is_finite()
        || !maximum.is_finite()
        || minimum <= 0.0
        || maximum < minimum
        || !current_scale.is_finite()
        || current_scale <= 0.0
        || !logical_width.is_finite()
        || logical_width <= 0.0
    {
        return Err("Annotation narrow scale has invalid bounds or viewport dimensions");
    }
    let unscaled_width = current_scale * logical_width;
    let target_width = crate::view::PAGE_MIN_WIDTH * if narrow { 0.98 } else { 1.1 };
    let scale = (unscaled_width / target_width).clamp(minimum, maximum);
    if !unscaled_width.is_finite()
        || (unscaled_width / scale < crate::view::PAGE_MIN_WIDTH) != narrow
    {
        return Err("Native UI-scale bounds cannot reach the requested Annotation layout");
    }
    Ok(scale)
}

pub(super) const VIEWER_SAVE: &str =
    crate::view::workflow::Composition::new(FeatureId::Annotate, 0.0)
        .stable_id(crate::view::workflow::Region::PrimaryAction);

pub(super) const ANNOTATION_SIDEBAR: &str = annotation::SIDEBAR_ID;

pub(super) const ANNOTATION_TIMELINE: &str = annotation::TIMELINE_ID;

pub(super) const ANNOTATION_OPERATION: &str = "annotation.operation";

pub(super) const ANNOTATION_STOP: &str = "annotation.stop";

pub(super) const ANNOTATION_BRUSH_RADIUS: &str = "annotation.brush_radius";

fn advance_annotation(driver: &mut Driver, phase: Phase) -> Task<RootMessage> {
    let reveal = matches!(phase, Phase::CopyProductWait);
    let continuation = driver.advance_to(phase);
    if reveal {
        // Restore both axes before waiting for a completed canvas draw.
        scroll_control_into_view(ANNOTATION_SURFACE.into(), AnnotationReveal::Geometry)
            .chain(continuation)
    } else {
        continuation
    }
}

#[cfg(test)]
pub(in crate::integration_control) mod tests;
