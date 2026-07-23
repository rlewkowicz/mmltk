use crate::app::App;
use crate::generated::{CustomModelArtifactKind, SettingsPath};
use crate::message::{Action, Message};
use crate::model::ShellStyle;
use crate::view::controls::copyable_input;
use iced::widget::{button, center, checkbox, column, container, opaque, row, slider, text};
use iced::{Center, Color, Element, Fill, Length};
use iced_aw::helpers::{card, color_picker, number_input};

pub fn view(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let density = shell_density(app);
    let accent_color = shell.accent_color;
    let accent_label = format!("Accent color  {accent_color}");
    let accent_button = button(text(accent_label))
        .on_press(Message::OpenAccentColorPicker)
        .style(move |_theme, status| accent_button_style(accent_color, status));
    let accent_picker = color_picker(
        app.accent_picker_open,
        accent_color,
        accent_button,
        Message::CancelAccentColorPicker,
        Message::SetAccentColor,
    );

    center(opaque(
        container(
            column![
                row![
                    text("Settings").size(24),
                    iced::widget::space::horizontal(),
                    button("Close").on_press(Message::ToggleSettings)
                ]
                .align_y(Center),
                styled_checkbox(&shell, shell.dark_mode, "Dark mode")
                    .on_toggle(|value| Message::SetBoolean(SettingsPath::UiDarkMode, value)),
                row![
                    setting_label(&shell, "UI scale"),
                    iced::widget::space::horizontal(),
                    copyable_input(SettingsPath::UiUiScale.as_str(), number_input(&shell.ui_scale, 0.85..=1.75, |value| {
                        Message::SetNumber(SettingsPath::UiUiScale, value.into())
                    })
                        .id(SettingsPath::UiUiScale.as_str())
                        .step(0.05)
                        .ignore_scroll(true)
                        .ignore_buttons(true)
                        .set_size(shell.text_input_font_size)
                        .padding([shell.control_padding_y, shell.control_padding_x])
                        .width(Length::Fixed(104.0))
                    )
                ]
                .spacing(shell.field_spacing)
                .align_y(Center),
                slider(0.85..=1.75, shell.ui_scale, |value| {
                    Message::SetNumber(SettingsPath::UiUiScale, value.into())
                })
                .step(0.05),
                numeric_setting(app, "Primary font size", SettingsPath::UiFontSize, 10.0, 32.0),
                numeric_setting(app, "Secondary font size", SettingsPath::UiSecondaryFontSize, 9.0, 28.0),
                numeric_setting(app, "Monospace font size", SettingsPath::UiMonoFontSize, 9.0, 28.0),
                numeric_setting(app, "Textbox font size", SettingsPath::UiTextInputFontSize, 9.0, 31.0),
                row![
                    setting_label(&shell, "Density"),
                    iced::widget::space::horizontal(),
                    density_button("Compact", density == 0, 0.0),
                    density_button("Balanced", density == 1, 1.0),
                    density_button("Comfortable", density == 2, 2.0)
                ]
                .spacing(shell.field_spacing)
                .align_y(Center),
                accent_picker,
                container(
                    column![
                        text("Frame interaction geometry").size(16),
                        text(format!(
                            "Edge hit width: {:.1} · corner hit size: {:.1} · handle radius: {:.1}",
                            app.drafts.ui.number(SettingsPath::UiCropEdgeHitHalfWidth),
                            app.drafts.ui.number(SettingsPath::UiCropCornerHitSize),
                            app.drafts.ui.number(SettingsPath::UiCropHandleRadius)
                        )),
                        text("Unavailable until capture-frame viewport and crop commits are restored.").size(12)
                    ]
                    .spacing(shell.field_spacing)
                )
                .padding(shell.card_padding)
                .style(iced::widget::container::danger),
                text("Browser: Firefox").size(shell.secondary_font_size),
                text("Renderer: WebGPU (hardware only)").size(shell.secondary_font_size),
                text("Capture surface: deferred; no fallback renderer")
                    .size(shell.secondary_font_size),
                button(if app.drafts.ui.is_dirty() {
                    "Apply settings"
                } else {
                    "Settings applied"
                })
                    .on_press_maybe(
                        app.drafts
                            .ui
                            .is_dirty()
                            .then_some(Message::Run(Action::ApplyShellSettings)),
                    )
                    .style(iced::widget::button::success),
                button("Reset all settings")
                    .on_press(Message::RequestSettingsReset)
            ]
            .spacing(shell.section_spacing),
        )
        .padding(shell.card_padding * 2.0)
        .width(520)
        .style(iced::widget::container::primary),
    ))
    .width(Fill)
    .height(Fill)
    .style(modal_backdrop_style)
    .into()
}

pub fn custom_model_confirmation(app: &App) -> Element<'_, Message> {
    let Some(confirmation) = app.custom_model_confirmation.as_ref() else {
        return iced::widget::Space::new().into();
    };
    let kind = match confirmation.artifact_kind {
        CustomModelArtifactKind::Weights => "weights",
        CustomModelArtifactKind::Onnx => "ONNX model",
        CustomModelArtifactKind::TensorRt => "TensorRT engine",
    };
    let body = column![
        text(format!(
            "Use this {kind} file as {} at {} x {}?",
            confirmation.preset_name, confirmation.resolution, confirmation.resolution
        )),
        text(&confirmation.path).size(12),
        text("A one-image GPU preflight will run before the workflow can start.").size(12),
    ]
    .spacing(10);
    let actions = row![
        button("Cancel").on_press(Message::CancelCustomWeights),
        iced::widget::space::horizontal(),
        button("Use selected artifact")
            .on_press(Message::ConfirmCustomWeights)
            .style(iced::widget::button::success),
    ]
    .spacing(8)
    .align_y(Center);
    modal_backdrop(
        card(text("Confirm Custom RF-DETR Artifact"), body)
            .foot(actions)
            .width(Length::Fixed(520.0))
            .into(),
    )
}

pub fn reset_confirmation() -> Element<'static, Message> {
    let body = column![
        text("Reset every persisted GUI, workflow, and model setting?"),
        text("Cached weights, compiled datasets, and workflow outputs will not be deleted.")
            .size(12),
    ]
    .spacing(10);
    let actions = row![
        button("Cancel").on_press(Message::CancelSettingsReset),
        iced::widget::space::horizontal(),
        button("Reset all settings")
            .on_press(Message::ConfirmSettingsReset)
            .style(iced::widget::button::danger),
    ]
    .spacing(8)
    .align_y(Center);
    modal_backdrop(
        card(text("Reset Settings"), body)
            .foot(actions)
            .width(Length::Fixed(500.0))
            .into(),
    )
}

pub fn error_modal(app: &App) -> Element<'_, Message> {
    let Some(error) = app.active_error.as_ref() else {
        return iced::widget::Space::new().into();
    };
    let shell = app.drafts.shell_style();
    let body = column![
        text(&error.context).size(shell.secondary_font_size),
        container(text(&error.message).size(shell.secondary_font_size))
            .padding(shell.card_padding)
            .width(Fill)
            .style(iced::widget::container::danger),
    ]
    .spacing(shell.field_spacing);
    let actions = row![
        button("Copy error").on_press(Message::CopyError),
        iced::widget::space::horizontal(),
        button("Dismiss")
            .on_press(Message::DismissError)
            .style(iced::widget::button::primary),
    ]
    .spacing(shell.field_spacing)
    .align_y(Center);
    modal_backdrop(
        card(text(&error.title), body)
            .foot(actions)
            .width(Length::Fixed(560.0))
            .into(),
    )
}

fn modal_backdrop<'a>(content: Element<'a, Message>) -> Element<'a, Message> {
    center(opaque(content))
        .width(Fill)
        .height(Fill)
        .style(modal_backdrop_style)
        .into()
}

fn modal_backdrop_style(_theme: &iced::Theme) -> iced::widget::container::Style {
    iced::widget::container::Style {
        background: Some(
            iced::Color {
                a: 0.82,
                ..iced::Color::BLACK
            }
            .into(),
        ),
        ..Default::default()
    }
}

fn numeric_setting<'a>(
    app: &'a App,
    label: &'a str,
    path: SettingsPath,
    minimum: f64,
    maximum: f64,
) -> Element<'a, Message> {
    let shell = app.drafts.shell_style();
    let value = app.drafts.ui.number(path);
    let input = number_input(&value, minimum..=maximum, move |value| {
        Message::SetNumber(path, value)
    })
    .id(path.as_str())
    .step(1.0)
    .ignore_scroll(true)
    .ignore_buttons(true)
    .set_size(shell.text_input_font_size)
    .padding([shell.control_padding_y, shell.control_padding_x])
    .width(Length::Fixed(120.0));
    row![
        setting_label(&shell, label),
        iced::widget::space::horizontal(),
        copyable_input(path.as_str(), input)
    ]
    .spacing(shell.field_spacing)
    .align_y(Center)
    .into()
}

fn setting_label<'a>(shell: &ShellStyle, label: &'a str) -> iced::widget::Text<'a> {
    text(label).size(shell.secondary_font_size)
}

fn styled_checkbox<'a>(
    shell: &ShellStyle,
    checked: bool,
    label: &'a str,
) -> iced::widget::Checkbox<'a, Message> {
    checkbox(checked)
        .label(label)
        .size(shell.checkbox_size)
        .spacing(shell.field_spacing)
        .text_size(shell.primary_font_size)
}

fn shell_density(app: &App) -> i32 {
    app.drafts.ui.integer(SettingsPath::UiDensity).clamp(0, 2)
}

fn density_button(
    label: &'static str,
    selected: bool,
    value: f64,
) -> iced::widget::Button<'static, Message> {
    button(label)
        .on_press(Message::SetNumber(SettingsPath::UiDensity, value))
        .style(if selected {
            iced::widget::button::primary
        } else {
            iced::widget::button::secondary
        })
}

fn accent_button_style(
    color: Color,
    status: iced::widget::button::Status,
) -> iced::widget::button::Style {
    let background = match status {
        iced::widget::button::Status::Hovered => color.mix(Color::WHITE, 0.12),
        iced::widget::button::Status::Pressed => color.mix(Color::BLACK, 0.12),
        iced::widget::button::Status::Active | iced::widget::button::Status::Disabled => color,
    };
    let luminance = 0.2126 * background.r + 0.7152 * background.g + 0.0722 * background.b;

    iced::widget::button::Style {
        background: Some(background.into()),
        text_color: if luminance > 0.55 {
            Color::BLACK
        } else {
            Color::WHITE
        },
        ..Default::default()
    }
}
