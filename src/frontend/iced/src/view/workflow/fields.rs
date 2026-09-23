use crate::fluent_theme::Element;
use iced::widget::{checkbox, column, row, space, text, text_input};
use iced::{Fill, Length};

const NUMERIC_INPUT_PORTION: u16 = 2;
const NUMERIC_TRAILING_PORTION: u16 = 3;

pub fn numeric_grid<'a, Message>() -> iced::widget::Grid<'a, Message, crate::fluent_theme::Theme> {
    iced::widget::Grid::new()
        .columns(4)
        .height(iced::Length::Shrink)
        .spacing(10)
}

fn labeled_field<'a, Message: 'a>(
    label: &'static str,
    control: Element<'a, Message>,
) -> Element<'a, Message> {
    column![text(label).size(12), control]
        .spacing(super::FIELD_SPACING)
        .into()
}

fn labeled_numeric_field<'a, Message: 'a>(
    label: &'static str,
    control: Element<'a, Message>,
) -> Element<'a, Message> {
    column![
        text(label).size(12),
        row![
            control,
            space::horizontal().width(Length::FillPortion(NUMERIC_TRAILING_PORTION)),
        ]
        .width(Fill),
    ]
    .spacing(super::FIELD_SPACING)
    .into()
}

pub fn text_field<'a, Message: Clone + 'a>(
    label: &'static str,
    stable_field_id: u64,
    value: &'a str,
    enabled: bool,
    on_input: impl Fn(String) -> Message + 'a,
) -> Element<'a, Message> {
    labeled_field(
        label,
        text_input(label, value)
            .id(stable_field_id.to_string())
            .on_input_maybe(enabled.then_some(on_input))
            .width(Fill)
            .into(),
    )
}

pub fn number_i32<'a, Message: Clone + 'a>(
    label: &'static str,
    value: i32,
    constraint: crate::generated::SettingsLeafConstraint,
    enabled: bool,
    on_input: impl Fn(i32) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    let stable_field_id = constraint.stable_field_id;
    let minimum = constraint.minimum.map_or(i32::MIN, |value| value as i32);
    let maximum = constraint.maximum.map_or(i32::MAX, |value| value as i32);
    // CLEANUP-IGNORE: Integer adapters retain their generated field type and callback without extra numeric traits.
    labeled_numeric_field(
        label,
        iced_aw::number_input(&value, minimum..=maximum, on_input)
            .id(stable_field_id.to_string())
            .ignore_scroll(true)
            .ignore_buttons(true)
            .on_input_maybe(enabled.then_some(on_input))
            .width(Length::FillPortion(NUMERIC_INPUT_PORTION))
            .into(),
    )
}

pub fn number_u64<'a, Message: Clone + 'a>(
    label: &'static str,
    value: u64,
    constraint: crate::generated::SettingsLeafConstraint,
    enabled: bool,
    on_input: impl Fn(u64) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    let stable_field_id = constraint.stable_field_id;
    let minimum = constraint.minimum.map_or(u64::MIN, |value| value as u64);
    let maximum = constraint.maximum.map_or(u64::MAX, |value| value as u64);
    labeled_numeric_field(
        label,
        iced_aw::number_input(&value, minimum..=maximum, on_input)
            .id(stable_field_id.to_string())
            .ignore_scroll(true)
            .ignore_buttons(true)
            .on_input_maybe(enabled.then_some(on_input))
            .width(Length::FillPortion(NUMERIC_INPUT_PORTION))
            .into(),
    )
}

pub fn number_f32<'a, Message: Clone + 'a>(
    label: &'static str,
    value: f32,
    constraint: crate::generated::SettingsLeafConstraint,
    enabled: bool,
    on_input: impl Fn(f32) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    number_f32_input(label, value, constraint, enabled, false, on_input)
}

pub fn decimal_f32<'a, Message: Clone + 'a>(
    label: &'static str, value: f32, constraint: crate::generated::SettingsLeafConstraint,
    enabled: bool, on_input: impl Fn(f32) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    number_f32_input(label, value, constraint, enabled, true, on_input)
}

fn number_f32_input<'a, Message: Clone + 'a>(
    label: &'static str, value: f32, constraint: crate::generated::SettingsLeafConstraint,
    enabled: bool, typed_only: bool, on_input: impl Fn(f32) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    let stable_field_id = constraint.stable_field_id;
    let minimum = constraint.minimum.map_or(f32::MIN, |value| value as f32);
    let maximum = constraint.maximum.map_or(f32::MAX, |value| value as f32);
    labeled_numeric_field(
        label,
        iced_aw::number_input(&value, minimum..=maximum, on_input)
            .id(stable_field_id.to_string())
            .step(0.01)
            .typed_only(typed_only)
            .ignore_scroll(true)
            .ignore_buttons(true)
            .on_input_maybe(enabled.then_some(on_input))
            .width(Length::FillPortion(NUMERIC_INPUT_PORTION))
            .into(),
    )
}

pub fn number_f64<'a, Message: Clone + 'a>(
    label: &'static str,
    value: f64,
    constraint: crate::generated::SettingsLeafConstraint,
    enabled: bool,
    on_input: impl Fn(f64) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    let stable_field_id = constraint.stable_field_id;
    let minimum = constraint.minimum.unwrap_or(f64::MIN);
    let maximum = constraint.maximum.unwrap_or(f64::MAX);
    labeled_numeric_field(
        label,
        iced_aw::number_input(&value, minimum..=maximum, on_input)
            .id(stable_field_id.to_string())
            .step(0.0001)
            .ignore_scroll(true)
            .ignore_buttons(true)
            .on_input_maybe(enabled.then_some(on_input))
            .width(Length::FillPortion(NUMERIC_INPUT_PORTION))
            .into(),
    )
}

pub fn toggle<'a, Message: Clone + 'a>(
    label: &'static str,
    value: bool,
    enabled: bool,
    on_toggle: impl Fn(bool) -> Message + 'a,
) -> Element<'a, Message> {
    let control = checkbox(value).label(label);
    if enabled {
        control.on_toggle(on_toggle).into()
    } else {
        control.into()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_numeric_adapter_uses_the_uniform_compact_layout_policy() {
        assert_eq!(NUMERIC_INPUT_PORTION, 2);
        assert_eq!(NUMERIC_TRAILING_PORTION, 3);
        assert_eq!(
            NUMERIC_INPUT_PORTION + NUMERIC_TRAILING_PORTION,
            5,
            "integer and floating adapters share one two-fifths input width"
        );
    }
}
