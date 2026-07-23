use crate::app::{App, string_at, value_at};
use crate::generated::{SettingsPath, Workflow};
use crate::message::{MaskColorField, MaskColorRange, Message, Tool};
use crate::model::ShellStyle;
use crate::view::controls::copyable_input;
use iced::widget::{
    Column, button, checkbox, column, container, row, rule, slider, text, text_input,
};
use iced::{Element, Fill, Font, Length, Padding};
use iced_aw::helpers::{card, number_input};
use serde_json::{Value, json};

pub fn view(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let mut content = Column::new()
        .spacing(shell.section_spacing)
        .padding(Padding {
            right: shell.card_padding,
            bottom: shell.card_padding,
            left: shell.card_padding,
            ..Padding::ZERO
        });
    if matches!(
        app.selected_workflow,
        Workflow::Predict | Workflow::Annotate | Workflow::Live
    ) {
        content = content.push(tools_card(app));
    }
    if app.selected_workflow == Workflow::Train {
        content = content.push(super::controls::train_controls(app));
    }
    content = content.push(workflow_card(app));
    if app.selected_workflow == Workflow::Annotate {
        content = content.push(annotation_sidebar(app));
    }
    content = content.push(job_card(app));

    container(content)
        .width(Length::FillPortion(super::SIDEBAR_PORTION))
        .height(Length::Shrink)
        .style(iced::widget::container::secondary)
        .into()
}

fn tools_card(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let tools: &[Tool] = if app.selected_workflow == Workflow::Annotate {
        &[
            Tool::Select,
            Tool::Direct,
            Tool::Box,
            Tool::Spline,
            Tool::Paint,
            Tool::Erase,
            Tool::Fill,
            Tool::Point,
            Tool::Skeleton,
            Tool::ColorSample,
        ]
    } else {
        &[Tool::Select, Tool::Box]
    };
    let mut body = Column::new().spacing(shell.field_spacing);
    for &tool in tools {
        body = body.push(
            button(tool.id())
                .on_press(Message::SelectTool(tool))
                .style(if tool == app.selected_tool {
                    iced::widget::button::primary
                } else {
                    iced::widget::button::secondary
                })
                .width(Fill),
        );
    }
    card(text("Tools"), body).width(Fill).into()
}

fn workflow_card(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let mut body = Column::new().spacing(5).push(text(format!(
        "Source: {}",
        if app.snapshot.source.locator.is_empty() {
            "not selected"
        } else {
            &app.snapshot.source.locator
        }
    )));
    match app.selected_workflow {
        Workflow::Train => {
            body = body
                .push(text(format!(
                    "Local GPUs: {}",
                    string_at(
                        &app.snapshot.workflow_state,
                        &["workflows", "train", "local_gpu", "selection_summary"],
                    )
                    .unwrap_or("inventory pending")
                )))
                .push(text(format!(
                    "Remote Session: {}",
                    string_at(
                        &app.snapshot.workflow_state,
                        &["workflows", "train", "remote_session", "current_state"],
                    )
                    .unwrap_or("idle")
                )))
                .push(text(format!(
                    "Dataset: {} split(s)",
                    app.snapshot.artifacts.dataset.splits.len()
                )));
            if !app.snapshot.artifacts.dataset.splits.is_empty() {
                body = body.push(super::controls::dataset_split_summary(app));
            }
        }
        Workflow::Validate => {
            body = body.push(text(format!(
                "Profile: {}",
                app.drafts
                    .validate
                    .boolean(crate::generated::SettingsPath::WorkflowsValidateValidationProfile)
            )));
        }
        Workflow::Predict | Workflow::Live => {
            body = body.push(text(format!(
                "Live Mode: {}",
                string_at(
                    &app.snapshot.workflow_state,
                    &["live_runtime", "active_mode"]
                )
                .unwrap_or("none")
            )));
        }
        Workflow::Annotate => {
            body = body.push(text(format!(
                "Document {} · {} instances",
                app.snapshot.annotation.document_generation, app.snapshot.annotation.instance_count
            )));
        }
        Workflow::Export => {
            body = body.push(text("ONNX and TensorRT artifact export"));
        }
    }
    card(text("Workflow Status"), body.spacing(shell.field_spacing))
        .width(Fill)
        .into()
}

fn annotation_sidebar(app: &App) -> Element<'_, Message> {
    const NEW_CATEGORY_INPUT_ID: &str = "annotate.new_category";
    let shell = app.drafts.shell_style();
    let sidebar =
        value_at(&app.snapshot.workflow_state, &["annotate_sidebar"]).and_then(Value::as_object);
    let Some(sidebar) = sidebar else {
        return card(text("Annotation Editor"), text("Sidebar state pending"))
            .width(Fill)
            .into();
    };
    let enabled = |key: &str| sidebar.get(key).and_then(Value::as_bool).unwrap_or(false);
    let category_input = text_input("New category", &app.new_annotation_category)
        .id(NEW_CATEGORY_INPUT_ID)
        .on_input(Message::EditAnnotationCategory)
        .size(shell.text_input_font_size)
        .padding([shell.control_padding_y, shell.control_padding_x])
        .width(Fill);
    let mut body = Column::new()
        .spacing(shell.field_spacing)
        .push(
            row![
                copyable_input(NEW_CATEGORY_INPUT_ID, category_input),
                button("Add").on_press_maybe(
                    (!app.new_annotation_category.trim().is_empty())
                        .then_some(Message::AddAnnotationCategory)
                )
            ]
            .spacing(shell.field_spacing),
        )
        .push(
            row![
                button("Undo").on_press_maybe(
                    enabled("can_undo").then_some(Message::AnnotationSidebar("undo", json!({})))
                ),
                button("Redo").on_press_maybe(
                    enabled("can_redo").then_some(Message::AnnotationSidebar("redo", json!({})))
                ),
                button("Delete").on_press_maybe(
                    enabled("can_delete_selected")
                        .then_some(Message::AnnotationSidebar("delete_selected", json!({})))
                )
            ]
            .spacing(shell.field_spacing),
        );

    if let Some(selected) = sidebar.get("selected_object").and_then(Value::as_object) {
        let object_enabled = selected
            .get("enabled")
            .and_then(Value::as_bool)
            .unwrap_or(true);
        let category = selected
            .get("category_index")
            .and_then(Value::as_u64)
            .unwrap_or_default();
        body = body.push(
            checkbox(object_enabled)
                .label("Selected object enabled")
                .size(shell.checkbox_size)
                .spacing(shell.field_spacing)
                .text_size(shell.primary_font_size)
                .on_toggle(move |enabled| {
                    Message::AnnotationSidebar(
                        "update_selected",
                        json!({"enabled": enabled, "category_index": category}),
                    )
                }),
        );
    }

    if let Some(objects) = sidebar.get("objects").and_then(Value::as_array) {
        body = body.push(text("Objects").size(14));
        for object in objects.iter().filter_map(Value::as_object) {
            let index = object
                .get("index")
                .and_then(Value::as_u64)
                .unwrap_or_default() as u32;
            let label = object
                .get("label")
                .and_then(Value::as_str)
                .unwrap_or("Object");
            let selected = object
                .get("selected")
                .and_then(Value::as_bool)
                .unwrap_or(false);
            body = body.push(
                button(text(if selected {
                    format!("Selected · {label}")
                } else {
                    label.to_owned()
                }))
                .on_press(Message::AnnotationSidebar(
                    "select_object",
                    json!({"object_index": index}),
                ))
                .width(Fill),
            );
        }
    }

    if let Some(categories) = sidebar.get("categories").and_then(Value::as_array) {
        body = body.push(text("Selected category").size(14));
        let selected_enabled = sidebar
            .get("selected_object")
            .and_then(Value::as_object)
            .and_then(|selected| selected.get("enabled"))
            .and_then(Value::as_bool)
            .unwrap_or(true);
        let mut category_list = Column::new().spacing(3);
        for (index, category) in categories.iter().filter_map(Value::as_object).enumerate() {
            let name = category
                .get("name")
                .and_then(Value::as_str)
                .unwrap_or("Class");
            category_list = category_list.push(
                button(text(name).size(shell.primary_font_size))
                    .width(Fill)
                    .on_press_maybe(enabled("has_selected_object").then_some(
                        Message::AnnotationSidebar(
                            "update_selected",
                            json!({"category_index": index, "enabled": selected_enabled}),
                        ),
                    )),
            );
        }
        body = body.push(category_list);
    }

    body = body
        .push(rule::horizontal(1))
        .push(
            button(
                if sidebar
                    .get("assist_running")
                    .and_then(Value::as_bool)
                    .unwrap_or(false)
                {
                    "Assist running…"
                } else {
                    "Run model assist"
                },
            )
            .on_press_maybe(
                (enabled("assist_available")
                    && !sidebar
                        .get("assist_running")
                        .and_then(Value::as_bool)
                        .unwrap_or(false))
                .then_some(Message::AnnotationSidebar("assist", json!({}))),
            )
            .width(Fill),
        )
        .push(
            button("Redraw selected box")
                .on_press_maybe(
                    enabled("can_redraw_selected_box")
                        .then_some(Message::AnnotationSidebar("redraw_box", json!({}))),
                )
                .width(Fill),
        );
    if enabled("can_edit_selected_mask") {
        body = body
            .push(text("Mask cleanup").size(14))
            .push(
                row![
                    text(format!("Radius: {}", app.mask_cleanup_radius)),
                    slider(
                        1..=32,
                        app.mask_cleanup_radius,
                        Message::SetMaskCleanupRadius
                    )
                    .on_release(Message::PersistUiSetting(SettingsPath::UiMaskCleanupRadius))
                    .width(Fill)
                ]
                .spacing(5),
            )
            .push(
                row![
                    cleanup_button("Open", "open", app.mask_cleanup_radius),
                    cleanup_button("Close", "close", app.mask_cleanup_radius),
                    cleanup_button("Erode", "erode", app.mask_cleanup_radius),
                    cleanup_button("Dilate", "dilate", app.mask_cleanup_radius)
                ]
                .spacing(4),
            )
            .push(
                row![
                    cleanup_button(
                        "Largest component",
                        "largest_component",
                        app.mask_cleanup_radius
                    ),
                    cleanup_button("Fill holes", "fill_holes", app.mask_cleanup_radius)
                ]
                .spacing(4),
            )
            .push(mask_color_controls(app, MaskColorRange::Sup, "SUP range"))
            .push(mask_color_controls(
                app,
                MaskColorRange::Nosup,
                "NOSUP range",
            ));
    }
    if let Some(selected) = sidebar.get("selected_object").and_then(Value::as_object) {
        if let Some(spline) = selected.get("spline").and_then(Value::as_object) {
            body = body.push(text("Spline editing").size(14));
            if let Some(segment_count) = spline.get("segment_count").and_then(Value::as_u64) {
                let mut segments = Column::new().spacing(3);
                for index in 0..segment_count.min(32) {
                    segments = segments.push(
                        button(text(format!("Segment {index}")).size(shell.primary_font_size))
                            .width(Fill)
                            .on_press(Message::AnnotationSidebar(
                                "spline.update_active_segment",
                                json!({"spline_segment_index": index}),
                            )),
                    );
                }
                body = body.push(segments);
            }
            body = body.push(
                row![
                    button("Insert knot").on_press_maybe(
                        spline
                            .get("can_insert_active_segment_knot")
                            .and_then(Value::as_bool)
                            .unwrap_or(false)
                            .then_some(Message::AnnotationSidebar(
                                "spline.insert_active_knot",
                                json!({}),
                            ))
                    ),
                    button("Delete knot").on_press_maybe(
                        spline
                            .get("can_delete_active_knot")
                            .and_then(Value::as_bool)
                            .unwrap_or(false)
                            .then_some(Message::AnnotationSidebar(
                                "spline.delete_active_knot",
                                json!({}),
                            ))
                    ),
                    button(
                        if spline
                            .get("closed")
                            .and_then(Value::as_bool)
                            .unwrap_or(false)
                        {
                            "Reopen"
                        } else {
                            "Close"
                        }
                    )
                    .on_press_maybe(
                        if spline
                            .get("closed")
                            .and_then(Value::as_bool)
                            .unwrap_or(false)
                        {
                            spline
                                .get("can_reopen")
                                .and_then(Value::as_bool)
                                .unwrap_or(false)
                                .then_some(Message::AnnotationSidebar("spline.reopen", json!({})))
                        } else {
                            spline
                                .get("can_close")
                                .and_then(Value::as_bool)
                                .unwrap_or(false)
                                .then_some(Message::AnnotationSidebar("spline.close", json!({})))
                        }
                    )
                ]
                .spacing(3),
            );
            if spline
                .get("can_edit_active_handle_mode")
                .and_then(Value::as_bool)
                .unwrap_or(false)
            {
                body = body.push(
                    row![
                        spline_mode_button("Corner", "corner"),
                        spline_mode_button("Smooth", "smooth"),
                        spline_mode_button("Mirrored", "mirrored")
                    ]
                    .spacing(3),
                );
            }
        }
        if let Some(skeleton) = selected.get("skeleton").and_then(Value::as_object) {
            body = body.push(text("Skeleton joints").size(14));
            if let Some(joints) = skeleton.get("joints").and_then(Value::as_array) {
                let mut joints_list = Column::new().spacing(3);
                for joint in joints.iter().filter_map(Value::as_object) {
                    let index = joint
                        .get("index")
                        .and_then(Value::as_u64)
                        .unwrap_or_default();
                    let label = joint
                        .get("label")
                        .and_then(Value::as_str)
                        .unwrap_or("Joint");
                    joints_list = joints_list.push(
                        button(text(label).size(shell.primary_font_size))
                            .width(Fill)
                            .on_press(Message::AnnotationSidebar(
                                "skeleton.update_active_joint",
                                json!({"skeleton_joint_index": index}),
                            )),
                    );
                }
                body = body.push(joints_list);
            }
            body = body.push(
                row![
                    skeleton_action(skeleton, "Skip", "can_skip_joint", "skeleton.skip_joint"),
                    skeleton_action(
                        skeleton,
                        "Hide",
                        "can_hide_active_joint",
                        "skeleton.hide_joint"
                    ),
                    skeleton_action(
                        skeleton,
                        "Show",
                        "can_reactivate_active_joint",
                        "skeleton.show_joint"
                    ),
                    skeleton_action(
                        skeleton,
                        "Reseed",
                        "can_reseed_active_joint",
                        "skeleton.reseed_joint"
                    )
                ]
                .spacing(3),
            );
        }
    }
    if let Some(summary) = sidebar.get("assist_summary").and_then(Value::as_str)
        && !summary.is_empty()
    {
        body = body.push(text(summary).size(12));
    }
    if let Some(error) = sidebar.get("assist_error").and_then(Value::as_str)
        && !error.is_empty()
    {
        body = body.push(
            container(text(error).size(12))
                .padding(6)
                .style(iced::widget::container::danger),
        );
    }
    card(text("Annotation Editor"), body).width(Fill).into()
}

fn cleanup_button(
    label: &'static str,
    operation: &'static str,
    radius: u16,
) -> Element<'static, Message> {
    button(label)
        .on_press(Message::AnnotationSidebar(
            "mask.cleanup",
            json!({"cleanup_op": operation, "cleanup_radius": radius}),
        ))
        .into()
}

fn mask_color_controls<'a>(
    app: &'a App,
    range: MaskColorRange,
    label: &'static str,
) -> Element<'a, Message> {
    let shell = app.drafts.shell_style();
    let value = match range {
        MaskColorRange::Sup => &app.mask_sup,
        MaskColorRange::Nosup => &app.mask_nosup,
    };
    column![
        text(label).size(13),
        mask_color_number(
            &shell,
            value,
            range,
            MaskColorField::HueDegrees,
            "Hue",
            "center",
            "hue_degrees",
            360.0,
            1.0
        ),
        mask_color_number(
            &shell,
            value,
            range,
            MaskColorField::Saturation,
            "Saturation",
            "center",
            "saturation",
            1.0,
            0.01
        ),
        mask_color_number(
            &shell,
            value,
            range,
            MaskColorField::Value,
            "Value",
            "center",
            "value",
            1.0,
            0.01
        ),
        mask_color_number(
            &shell,
            value,
            range,
            MaskColorField::HueMinus,
            "Hue -%",
            "tolerance",
            "hue_minus_pct",
            100.0,
            1.0
        ),
        mask_color_number(
            &shell,
            value,
            range,
            MaskColorField::HuePlus,
            "Hue +%",
            "tolerance",
            "hue_plus_pct",
            100.0,
            1.0
        ),
        mask_color_number(
            &shell,
            value,
            range,
            MaskColorField::SaturationMinus,
            "Saturation -%",
            "tolerance",
            "saturation_minus_pct",
            100.0,
            1.0
        ),
        mask_color_number(
            &shell,
            value,
            range,
            MaskColorField::SaturationPlus,
            "Saturation +%",
            "tolerance",
            "saturation_plus_pct",
            100.0,
            1.0
        ),
        mask_color_number(
            &shell,
            value,
            range,
            MaskColorField::ValueMinus,
            "Value -%",
            "tolerance",
            "value_minus_pct",
            100.0,
            1.0
        ),
        mask_color_number(
            &shell,
            value,
            range,
            MaskColorField::ValuePlus,
            "Value +%",
            "tolerance",
            "value_plus_pct",
            100.0,
            1.0
        ),
        checkbox(
            value
                .get("sampling")
                .and_then(Value::as_bool)
                .unwrap_or(false)
        )
        .label("Sample next workspace color")
        .size(shell.checkbox_size)
        .spacing(shell.field_spacing)
        .text_size(shell.primary_font_size)
        .on_toggle(move |enabled| Message::SetMaskColorSampling(range, enabled)),
        button("Reset this range")
            .on_press(Message::ResetMaskColorRange(range))
            .width(Fill)
    ]
    .spacing(3)
    .into()
}

#[allow(clippy::too_many_arguments)]
fn mask_color_number<'a>(
    shell: &ShellStyle,
    value: &'a Value,
    range: MaskColorRange,
    field: MaskColorField,
    label: &'static str,
    section: &'static str,
    key: &'static str,
    maximum: f64,
    step: f64,
) -> Element<'a, Message> {
    let current = value
        .get(section)
        .and_then(|section| section.get(key))
        .and_then(Value::as_f64)
        .unwrap_or_default();
    let id = mask_color_input_id(range, field);
    let input = number_input(&current, 0.0..=maximum, move |value| {
        Message::SetMaskColorNumber(range, field, value)
    })
    .id(id)
    .step(step)
    .ignore_scroll(true)
    .ignore_buttons(true)
    .set_size(shell.text_input_font_size)
    .padding([shell.control_padding_y, shell.control_padding_x])
    .width(Length::Fixed(112.0));
    row![
        text(label).size(shell.secondary_font_size).width(Fill),
        copyable_input(id, input)
    ]
    .spacing(shell.field_spacing)
    .into()
}

const fn mask_color_input_id(range: MaskColorRange, field: MaskColorField) -> &'static str {
    match (range, field) {
        (MaskColorRange::Sup, MaskColorField::HueDegrees) => "annotate.mask.sup.hue_degrees",
        (MaskColorRange::Sup, MaskColorField::Saturation) => "annotate.mask.sup.saturation",
        (MaskColorRange::Sup, MaskColorField::Value) => "annotate.mask.sup.value",
        (MaskColorRange::Sup, MaskColorField::HueMinus) => "annotate.mask.sup.hue_minus",
        (MaskColorRange::Sup, MaskColorField::HuePlus) => "annotate.mask.sup.hue_plus",
        (MaskColorRange::Sup, MaskColorField::SaturationMinus) => {
            "annotate.mask.sup.saturation_minus"
        }
        (MaskColorRange::Sup, MaskColorField::SaturationPlus) => {
            "annotate.mask.sup.saturation_plus"
        }
        (MaskColorRange::Sup, MaskColorField::ValueMinus) => "annotate.mask.sup.value_minus",
        (MaskColorRange::Sup, MaskColorField::ValuePlus) => "annotate.mask.sup.value_plus",
        (MaskColorRange::Nosup, MaskColorField::HueDegrees) => "annotate.mask.nosup.hue_degrees",
        (MaskColorRange::Nosup, MaskColorField::Saturation) => "annotate.mask.nosup.saturation",
        (MaskColorRange::Nosup, MaskColorField::Value) => "annotate.mask.nosup.value",
        (MaskColorRange::Nosup, MaskColorField::HueMinus) => "annotate.mask.nosup.hue_minus",
        (MaskColorRange::Nosup, MaskColorField::HuePlus) => "annotate.mask.nosup.hue_plus",
        (MaskColorRange::Nosup, MaskColorField::SaturationMinus) => {
            "annotate.mask.nosup.saturation_minus"
        }
        (MaskColorRange::Nosup, MaskColorField::SaturationPlus) => {
            "annotate.mask.nosup.saturation_plus"
        }
        (MaskColorRange::Nosup, MaskColorField::ValueMinus) => "annotate.mask.nosup.value_minus",
        (MaskColorRange::Nosup, MaskColorField::ValuePlus) => "annotate.mask.nosup.value_plus",
    }
}

fn skeleton_action<'a>(
    skeleton: &'a serde_json::Map<String, Value>,
    label: &'static str,
    capability: &'static str,
    action: &'static str,
) -> Element<'a, Message> {
    button(label)
        .on_press_maybe(
            skeleton
                .get(capability)
                .and_then(Value::as_bool)
                .unwrap_or(false)
                .then_some(Message::AnnotationSidebar(action, json!({}))),
        )
        .into()
}

fn spline_mode_button(label: &'static str, mode: &'static str) -> Element<'static, Message> {
    button(label)
        .on_press(Message::AnnotationSidebar(
            "spline.set_handle_mode",
            json!({"spline_handle_mode": mode}),
        ))
        .into()
}

fn job_card(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let mut body = column![
        text(format!(
            "job: {}",
            if app.snapshot.job.running {
                "running"
            } else {
                "idle"
            }
        )),
        text(format!("label: {}", app.snapshot.job.label)),
        text(&app.snapshot.job.summary),
    ]
    .spacing(5);
    if !app.snapshot.job.error.is_empty() {
        body = body.push(
            container(text(&app.snapshot.job.error).size(12))
                .padding(7)
                .width(Fill)
                .style(iced::widget::container::danger),
        );
    }
    if !app.snapshot.job.output_tail.is_empty() {
        body = body
            .push(rule::horizontal(1))
            .push(text("Output tail").size(14))
            .push(
                text(&app.snapshot.job.output_tail)
                    .size(shell.mono_font_size)
                    .font(Font::MONOSPACE),
            );
    }
    let retained_start = app.snapshot.job.recent_logs.len().saturating_sub(24);
    for entry in app.snapshot.job.recent_logs.iter().skip(retained_start) {
        body = body.push(
            text(format!(
                "{} [{}] {}",
                entry.sequence, entry.level, entry.message
            ))
            .size(shell.mono_font_size)
            .font(Font::MONOSPACE),
        );
    }
    card(text("Job and Logs"), body).width(Fill).into()
}
