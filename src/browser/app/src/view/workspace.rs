use crate::app::{App, bool_at};
use crate::generated::{SettingsPath, Workflow, WorkspacePresent, WorkspaceSourceRegion};
use crate::message::{Action, Message, WorkspaceAspectRatio};
use iced::widget::{
    Column, Row, button, checkbox, column, container, responsive, row, shader, slider, space, text,
};
use iced::{Center, Element, Fill, Length, Padding};

pub fn view(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let mut content: Column<'_, Message> =
        column![preview_panel(app)].spacing(shell.section_spacing);

    if app.selected_workflow == Workflow::Annotate {
        content = content.push(annotation_timeline(app));
    }
    if app.selected_workflow == Workflow::Live {
        content = content.push(live_controls(app));
    }
    content = content.push(super::controls::advanced_card(app));

    container(content)
        .padding(Padding {
            right: shell.card_padding,
            bottom: shell.card_padding,
            left: shell.card_padding,
            ..Padding::ZERO
        })
        .width(Length::FillPortion(super::WORKSPACE_PORTION))
        .height(Length::Shrink)
        .style(iced::widget::container::secondary)
        .into()
}

fn preview_panel(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let selected = app.workspace_aspect_ratio;
    let mut ratio_buttons: Row<'_, Message> =
        row![text("Aspect ratio").size(shell.secondary_font_size)]
            .spacing(shell.field_spacing)
            .align_y(Center);

    for aspect_ratio in WorkspaceAspectRatio::ALL {
        ratio_buttons = ratio_buttons.push(
            button(text(aspect_ratio.label()).size(shell.primary_font_size))
                .on_press(Message::SetWorkspaceAspectRatio(aspect_ratio))
                .style(if aspect_ratio == selected {
                    iced::widget::button::primary
                } else {
                    iced::widget::button::secondary
                }),
        );
    }

    let preview_primary_font_size = shell.primary_font_size;
    let preview_secondary_font_size = shell.secondary_font_size;
    let preview_spacing = shell.field_spacing;
    let workspace_config = app.snapshot.workspace_surface.clone();
    let workspace_present = app.workspace_present.clone();
    let annotation = app.snapshot.annotation.clone();
    let crop = app
        .snapshot
        .source
        .has_crop
        .then(|| app.snapshot.source.crop.clone());
    let selected_tool = app.selected_tool;
    let workspace_ready = workspace_config.as_ref().is_some_and(|config| {
        config.ready
            && workspace_present.as_ref().is_some_and(|present| {
                present.generation == config.generation && present.slot < config.slot_count
            })
    });
    let preview = responsive(move |size| {
        let height = size.width * selected.height_factor();
        let content: Element<'_, Message> = match workspace_config.clone() {
            Some(config) => {
                let present = workspace_present
                    .clone()
                    .filter(|present| {
                        config.ready
                            && present.generation == config.generation
                            && present.slot < config.slot_count
                    })
                    .unwrap_or_else(|| WorkspacePresent {
                        generation: config.generation.clone(),
                        width: config.capacity_width,
                        height: config.capacity_height,
                        source_region: WorkspaceSourceRegion {
                            width: config.capacity_width,
                            height: config.capacity_height,
                            ..WorkspaceSourceRegion::default()
                        },
                        ..WorkspacePresent::default()
                    });
                shader(crate::workspace_surface::Program::new(
                    config,
                    present,
                    annotation.clone(),
                    crop.clone(),
                    selected_tool,
                ))
                .width(Fill)
                .height(Fill)
                .into()
            }
            _ => column![
                space::vertical(),
                text("Waiting for native workspace").size(28),
                text("Firefox is negotiating the Vulkan workspace surface pool.")
                    .size(preview_primary_font_size),
                text("The workspace appears after every DMA-BUF and timeline semaphore is ready.")
                    .size(preview_secondary_font_size),
                space::vertical(),
            ]
            .spacing(preview_spacing)
            .align_x(Center)
            .into(),
        };
        container(content)
            .width(Fill)
            .height(Length::Fixed(height))
            .align_x(Center)
            .align_y(Center)
            .style(iced::widget::container::dark)
    })
    .width(Fill)
    .height(Length::Shrink);

    column![
        container(ratio_buttons)
            .padding([shell.control_padding_y, shell.control_padding_x])
            .width(Fill)
            .style(iced::widget::container::primary),
        space::vertical().height(Length::Fixed(shell.section_spacing)),
        preview,
        container(
            column![
                text(if workspace_ready { "Native WebGPU workspace" } else { "Waiting for workspace" }).size(shell.primary_font_size),
                text("Prediction and annotation layers are flattened by the native compositor before this texture is sampled.").size(shell.secondary_font_size)
            ]
            .spacing(shell.field_spacing)
        )
        .padding([shell.control_padding_y, shell.control_padding_x])
        .width(Fill)
        .style(if workspace_ready {
            iced::widget::container::success
        } else {
            iced::widget::container::secondary
        })
    ]
    .spacing(0)
    .width(Fill)
    .into()
}

fn annotation_timeline(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let controls = &app.snapshot.workflow_state;
    let previous_enabled = bool_at(
        controls,
        &[
            "annotate_runtime_controls",
            "setup_frame_navigation",
            "prev",
            "enabled",
        ],
    );
    let reload_enabled = bool_at(
        controls,
        &[
            "annotate_runtime_controls",
            "setup_frame_navigation",
            "reload",
            "enabled",
        ],
    );
    let next_enabled = bool_at(
        controls,
        &[
            "annotate_runtime_controls",
            "setup_frame_navigation",
            "next",
            "enabled",
        ],
    );
    let hold_enabled = bool_at(
        controls,
        &["annotate_runtime_controls", "save", "hold_save", "enabled"],
    );
    column![
        row![
            button("Previous")
                .on_press_maybe(previous_enabled.then_some(Message::Run(Action::SetupPrevious))),
            button("Reload")
                .on_press_maybe(reload_enabled.then_some(Message::Run(Action::SetupReload))),
            button("Next").on_press_maybe(next_enabled.then_some(Message::Run(Action::SetupNext))),
            button(if app.hold_save {
                "Release save"
            } else {
                "Hold save"
            })
            .on_press_maybe(hold_enabled.then_some(Message::Run(Action::ToggleHoldSave))),
            text(format!(
                "instances: {}  document: {}",
                app.snapshot.annotation.instance_count, app.snapshot.annotation.document_generation
            ))
        ]
        .spacing(shell.field_spacing)
        .align_y(Center),
        row![
            text(format!("Brush radius: {}", app.brush_radius)),
            slider(1..=128, app.brush_radius, Message::SetBrushRadius)
                .on_release(Message::PersistUiSetting(
                    SettingsPath::UiAnnotationBrushRadius,
                ))
                .width(Fill)
        ]
        .spacing(shell.field_spacing)
        .align_y(Center)
    ]
    .spacing(shell.section_spacing)
    .into()
}

fn live_controls(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    row![
        checkbox(app.fit_to_capture)
            .label("Fit to capture")
            .size(shell.checkbox_size)
            .spacing(shell.field_spacing)
            .text_size(shell.primary_font_size)
            .on_toggle(Message::SetFitToCapture),
        checkbox(app.full_frame_display)
            .label("Full-frame overlay")
            .size(shell.checkbox_size)
            .spacing(shell.field_spacing)
            .text_size(shell.primary_font_size)
            .on_toggle(Message::SetFullFrameDisplay)
    ]
    .spacing(shell.section_spacing)
    .align_y(Center)
    .into()
}
