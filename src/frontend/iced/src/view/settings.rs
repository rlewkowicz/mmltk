use crate::fluent_theme::Element;
use crate::view_model::ApplicationModel;
use iced::widget::{button, checkbox, column, container, row, slider, space, text};
use iced::{Center, Fill, Length};

pub mod editor;
pub use editor::{EditCadence, EditSchedule, SettingsModel};

#[cfg(test)]
pub(crate) fn installed_settings_model() -> SettingsModel {
    let snapshot = crate::generated::application_snapshot_defaults()
        .unwrap()
        .into_iter()
        .find_map(|fact| match fact.value {
            crate::generated::ApplicationSnapshot::Settings(value) => Some(value),
            _ => None,
        })
        .unwrap();
    let mut settings = SettingsModel::default();
    settings.install(&snapshot);
    settings
}

pub struct Component {
    state: SettingsModel,
    applied_scale: f32,
    ui_scale_drag_active: bool,
}

impl Default for Component {
    fn default() -> Self {
        Self {
            state: SettingsModel::default(),
            applied_scale: 1.0,
            ui_scale_drag_active: false,
        }
    }
}

impl Component {
    pub fn update(&mut self, message: Message) -> Result<Option<Outcome>, String> {
        match message {
            Message::UiScaleChanged(value) => {
                let outcome = update(&mut self.state, Message::UiScaleChanged(value))?;
                self.ui_scale_drag_active = true;
                Ok(outcome)
            }
            Message::UiScaleReleased => {
                self.apply_draft_scale();
                Ok(None)
            }
            Message::Close => {
                self.apply_draft_scale();
                update(&mut self.state, Message::Close)
            }
            message => update(&mut self.state, message),
        }
    }

    pub fn view<'a>(&'a self, model: &'a ApplicationModel) -> Element<'a, Message> {
        view(model, &self.state)
    }

    pub fn state(&self) -> &SettingsModel {
        &self.state
    }

    pub fn state_mut(&mut self) -> &mut SettingsModel {
        &mut self.state
    }

    pub fn draft(&self) -> Option<&crate::generated::GuiSettingsState> {
        self.state.draft.as_ref()
    }

    pub fn install(&mut self, snapshot: &crate::generated::SettingsUiState) {
        self.state.install(snapshot);
        if !self.ui_scale_drag_active {
            self.sync_applied_scale();
        }
    }

    pub fn settle_success(&mut self, snapshot: &crate::generated::SettingsUiState) {
        self.state.settle_success(snapshot);
        if !self.ui_scale_drag_active {
            self.sync_applied_scale();
        }
    }

    pub fn settle_failure(&mut self, snapshot: Option<&crate::generated::SettingsUiState>) {
        self.state.settle_failure(snapshot);
        self.ui_scale_drag_active = false;
        self.sync_applied_scale();
    }

    pub fn reset(&mut self, snapshot: &crate::generated::SettingsUiState) {
        self.state.settle_failure(Some(snapshot));
        self.ui_scale_drag_active = false;
        self.sync_applied_scale();
    }

    pub fn reset_transport(&mut self) {
        self.state = SettingsModel::default();
        self.applied_scale = 1.0;
        self.ui_scale_drag_active = false;
    }

    pub fn has_local_edits(&self) -> bool {
        self.state.has_local_edits()
    }

    pub fn open(&mut self) {
        self.state.open = true;
    }

    pub fn is_open(&self) -> bool {
        self.state.open
    }

    pub fn reset_confirmation(&self) -> bool {
        self.state.reset_confirmation
    }

    pub fn applied_scale(&self) -> f32 {
        self.applied_scale
    }

    fn apply_draft_scale(&mut self) {
        self.ui_scale_drag_active = false;
        self.sync_applied_scale();
    }

    fn sync_applied_scale(&mut self) {
        self.applied_scale = self
            .state
            .draft
            .as_ref()
            .map_or(1.0, |draft| draft.ui.uiscale);
    }
}

#[derive(Debug, Clone)]
pub enum Message {
    DarkModeChanged(bool),
    PerformanceChanged(bool),
    UiScaleChanged(f32),
    UiScaleReleased,
    FontSizeChanged(f32),
    SecondaryFontSizeChanged(f32),
    MonoFontSizeChanged(f32),
    TextInputFontSizeChanged(f32),
    #[cfg(target_arch = "wasm32")]
    DebounceElapsed(u64),
    ResetRequested,
    ResetConfirmed,
    ResetCancelled,
    Close,
}

#[derive(Debug, Clone)]
pub enum Outcome {
    SettingsEdited(EditSchedule),
    #[cfg(target_arch = "wasm32")]
    DebounceElapsed(u64),
    ResetRequested,
    Closed,
}

pub fn update(state: &mut SettingsModel, message: Message) -> Result<Option<Outcome>, String> {
    match message {
        Message::DarkModeChanged(value) => {
            return state
                .edit(EditCadence::Debounced, |draft| {
                    crate::generated::edit_uidarkmode(draft, value)
                })
                .map(|schedule| Some(Outcome::SettingsEdited(schedule)));
        }
        Message::PerformanceChanged(value) => {
            return state
                .edit(EditCadence::Debounced, |draft| {
                    crate::generated::edit_uishowworkspaceperformance(draft, value)
                })
                .map(|schedule| Some(Outcome::SettingsEdited(schedule)));
        }
        Message::UiScaleChanged(value) => {
            return state
                .edit(EditCadence::Debounced, |draft| {
                    crate::generated::edit_uiuiscale(draft, value)
                })
                .map(|schedule| Some(Outcome::SettingsEdited(schedule)));
        }
        Message::UiScaleReleased => {}
        Message::FontSizeChanged(value) => {
            return state
                .edit(EditCadence::Debounced, |draft| {
                    crate::generated::edit_uifontsize(draft, value)
                })
                .map(|schedule| Some(Outcome::SettingsEdited(schedule)));
        }
        Message::SecondaryFontSizeChanged(value) => {
            return state
                .edit(EditCadence::Debounced, |draft| {
                    crate::generated::edit_uisecondaryfontsize(draft, value)
                })
                .map(|schedule| Some(Outcome::SettingsEdited(schedule)));
        }
        Message::MonoFontSizeChanged(value) => {
            return state
                .edit(EditCadence::Debounced, |draft| {
                    crate::generated::edit_uimonofontsize(draft, value)
                })
                .map(|schedule| Some(Outcome::SettingsEdited(schedule)));
        }
        Message::TextInputFontSizeChanged(value) => {
            return state
                .edit(EditCadence::Debounced, |draft| {
                    crate::generated::edit_uitextinputfontsize(draft, value)
                })
                .map(|schedule| Some(Outcome::SettingsEdited(schedule)));
        }
        #[cfg(target_arch = "wasm32")]
        Message::DebounceElapsed(generation) => {
            return Ok(Some(Outcome::DebounceElapsed(generation)));
        }
        Message::ResetRequested => state.reset_confirmation = true,
        Message::ResetConfirmed => {
            state.reset_confirmation = false;
            return Ok(Some(Outcome::ResetRequested));
        }
        Message::ResetCancelled => state.reset_confirmation = false,
        Message::Close => {
            state.close();
            return Ok(Some(Outcome::Closed));
        }
    }
    Ok(None)
}

pub fn view<'a>(model: &'a ApplicationModel, state: &'a SettingsModel) -> Element<'a, Message> {
    let Some(draft) = state.draft.as_ref() else {
        return modal(
            "Settings",
            column![
                text("Waiting for the typed Settings snapshot."),
                button("Close").on_press(Message::Close)
            ]
            .spacing(12)
            .into(),
        );
    };
    let ui = &draft.ui;
    let ui_scale = constraint_bounds(crate::generated::constraint_uiuiscale());
    let font_size = constraint_bounds(crate::generated::constraint_uifontsize());
    let secondary_font_size = constraint_bounds(crate::generated::constraint_uisecondaryfontsize());
    let mono_font_size = constraint_bounds(crate::generated::constraint_uimonofontsize());
    let text_input_font_size =
        constraint_bounds(crate::generated::constraint_uitextinputfontsize());
    let edit_available = state.draft.is_some() && model.settings_edit_available();
    let reset_available = !state.has_local_edits() && model.settings_reset_available();
    let appearance = settings_group(
        "settings.group.appearance",
        "Appearance",
        column![
            container(
                checkbox(ui.darkmode)
                    .label("Dark mode")
                    .on_toggle_maybe(edit_available.then_some(Message::DarkModeChanged))
            )
            .id("settings.dark_mode"),
            container(
                checkbox(ui.showworkspaceperformance)
                    .label("Show FPS")
                    .on_toggle_maybe(edit_available.then_some(Message::PerformanceChanged))
            )
            .id("settings.show_fps"),
            container(setting_control(
                "settings.ui_scale",
                "UI scale",
                ui.uiscale,
                ui_scale,
                edit_available,
                Message::UiScaleChanged,
                Some(Message::UiScaleReleased)
            ))
            .id("settings.ui_scale"),
        ]
        .spacing(8)
        .into(),
    );
    let typography = settings_group(
        "settings.group.typography",
        "Typography",
        column![
            container(setting_control(
                "settings.font_size",
                "Primary font size",
                ui.fontsize,
                font_size,
                edit_available,
                Message::FontSizeChanged,
                None
            ))
            .id("settings.font_size"),
            container(setting_control(
                "settings.secondary_font_size",
                "Secondary font size",
                ui.secondaryfontsize,
                secondary_font_size,
                edit_available,
                Message::SecondaryFontSizeChanged,
                None
            ))
            .id("settings.secondary_font_size"),
            container(setting_control(
                "settings.mono_font_size",
                "Monospace font size",
                ui.monofontsize,
                mono_font_size,
                edit_available,
                Message::MonoFontSizeChanged,
                None
            ))
            .id("settings.mono_font_size"),
            container(setting_control(
                "settings.text_input_font_size",
                "Text input font size",
                ui.textinputfontsize,
                text_input_font_size,
                edit_available,
                Message::TextInputFontSizeChanged,
                None
            ))
            .id("settings.text_input_font_size"),
        ]
        .spacing(8)
        .into(),
    );
    let environment = settings_group(
        "settings.group.environment",
        "Environment",
        column![
            text("Browser: Firefox").size(12),
            text("Renderer: hardware WebGPU").size(12),
            text("Display: Wayland").size(12),
            text("Changes save automatically").size(12),
        ]
        .spacing(4)
        .into(),
    );
    let footer = container(
        row![
            container(
                button("Reset")
                    .on_press_maybe(reset_available.then_some(Message::ResetRequested))
                    .style(crate::fluent_theme::button_danger)
            )
            .id("settings.reset"),
            space::horizontal(),
            container(
                button("Close")
                    .on_press(Message::Close)
                    .style(crate::fluent_theme::button_secondary)
            )
            .id("settings.close"),
        ]
        .spacing(8)
        .align_y(Center),
    )
    .id("settings.footer");
    let body = column![appearance, typography, environment, footer].spacing(12);
    modal("Settings", body.into())
}

pub fn reset_confirmation(available: bool) -> Element<'static, Message> {
    modal(
        "Reset settings?",
        column![
            text("Restore every native-defined GUI and workflow default?"),
            row![
                button("Cancel").on_press(Message::ResetCancelled),
                space::horizontal(),
                container(
                    button("Reset all")
                        .on_press_maybe(available.then_some(Message::ResetConfirmed))
                        .style(crate::fluent_theme::button_danger)
                )
                .id("settings.reset.confirm"),
            ]
            .spacing(8),
        ]
        .spacing(16)
        .into(),
    )
}

fn setting_control<'a>(
    id: &'static str,
    label: &'a str,
    value: f32,
    range: Option<(f32, f32)>,
    available: bool,
    on_change: fn(f32) -> Message,
    on_release: Option<Message>,
) -> Element<'a, Message> {
    let Some(range) = range else {
        return column![
            text(label),
            text("Native range unavailable; this setting cannot be edited.").size(12),
        ]
        .spacing(5)
        .into();
    };
    if !available {
        return setting_value_row(id, label, value);
    }
    let control = slider(range.0..=range.1, value, on_change);
    let control: Element<'a, Message> = if let Some(message) = on_release {
        control.on_release(message).into()
    } else {
        control.into()
    };
    column![setting_value_row(id, label, value), control]
        .spacing(5)
        .into()
}

fn setting_value_row<'a>(id: &'static str, label: &'a str, value: f32) -> Element<'a, Message> {
    row![
        container(text(label).width(Length::Fixed(180.0))).id(format!("{id}.label")),
        space::horizontal(),
        container(
            text(format!("{value:.2}"))
                .size(12)
                .width(Length::Fixed(52.0))
                .align_x(iced::Right)
        )
        .id(format!("{id}.value")),
    ]
    .width(Fill)
    .align_y(Center)
    .into()
}

fn constraint_bounds(constraint: crate::generated::SettingsLeafConstraint) -> Option<(f32, f32)> {
    let (minimum, maximum) = constraint.minimum.zip(constraint.maximum)?;
    let range = (minimum as f32, maximum as f32);
    (constraint.finite && minimum.is_finite() && maximum.is_finite() && range.0 < range.1)
        .then_some(range)
}

fn settings_group<'a>(
    id: &'static str,
    title: &'static str,
    content: Element<'a, Message>,
) -> Element<'a, Message> {
    container(column![text(title).size(16), content].spacing(7))
        .id(id)
        .padding(10)
        .width(Fill)
        .style(crate::fluent_theme::container_card)
        .into()
}

fn modal<'a>(title: &'a str, body: Element<'a, Message>) -> Element<'a, Message> {
    crate::view::shared::modal(
        "settings.modal",
        520.0,
        column![text(title).size(25), body].spacing(16),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn settings_snapshot() -> crate::generated::SettingsUiState {
        crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .find_map(|fact| match fact.value {
                crate::generated::ApplicationSnapshot::Settings(value) => Some(value),
                _ => None,
            })
            .unwrap()
    }

    fn another_scale(current: f32) -> f32 {
        let (minimum, maximum) =
            constraint_bounds(crate::generated::constraint_uiuiscale()).unwrap();
        if (current - minimum).abs() > f32::EPSILON {
            minimum
        } else {
            maximum
        }
    }

    #[test]
    fn every_rendered_slider_uses_its_typed_generated_constraint() {
        let constraints = [
            crate::generated::constraint_uiuiscale(),
            crate::generated::constraint_uifontsize(),
            crate::generated::constraint_uisecondaryfontsize(),
            crate::generated::constraint_uimonofontsize(),
            crate::generated::constraint_uitextinputfontsize(),
        ];
        for constraint in constraints {
            assert_ne!(constraint.stable_field_id, 0);
            let bounds = constraint_bounds(constraint).expect("generated finite range");
            assert!(bounds.0 < bounds.1);
        }
    }

    #[test]
    fn pending_settings_mutation_keeps_the_modal_constructible() {
        let snapshots = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .map(|fact| fact.value)
            .collect();
        let mut model = crate::view_model::ApplicationModel::default();
        model
            .install_bootstrap(crate::generated::SCHEMA_FINGERPRINT, snapshots)
            .unwrap();
        let mut settings = SettingsModel::default();
        settings.install(model.settings_snapshot.as_ref().unwrap());
        settings
            .edit(EditCadence::Immediate, |draft| {
                crate::generated::edit_uidarkmode(draft, true)
            })
            .unwrap();
        drop(view(&model, &settings));
        assert!(settings.has_local_edits() || model.native_settings_unsettled());
    }

    #[test]
    fn show_fps_retains_the_canonical_debounced_setting() {
        let mut settings = installed_settings_model();
        let original = settings.draft.as_ref().unwrap().ui.showworkspaceperformance;
        let outcome = update(&mut settings, Message::PerformanceChanged(!original)).unwrap();
        assert!(matches!(
            outcome,
            Some(Outcome::SettingsEdited(EditSchedule::Debounce(_)))
        ));
        assert_eq!(
            settings.draft.as_ref().unwrap().ui.showworkspaceperformance,
            !original
        );
        assert_eq!(crate::workspace_fps::enabled(&settings), !original);
    }

    #[test]
    fn ui_scale_drag_previews_the_draft_and_release_applies_it_once() {
        let snapshot = settings_snapshot();
        let mut component = Component::default();
        component.install(&snapshot);
        let original = component.applied_scale();
        let changed = another_scale(original);

        assert!(matches!(
            component.update(Message::UiScaleChanged(changed)).unwrap(),
            Some(Outcome::SettingsEdited(EditSchedule::Debounce(_)))
        ));
        assert_eq!(component.draft().unwrap().ui.uiscale, changed);
        assert_eq!(component.applied_scale(), original);
        assert!(component.ui_scale_drag_active);

        assert!(
            component
                .update(Message::UiScaleReleased)
                .unwrap()
                .is_none()
        );
        assert_eq!(component.applied_scale(), changed);
        assert!(!component.ui_scale_drag_active);
        assert!(
            component
                .update(Message::UiScaleReleased)
                .unwrap()
                .is_none()
        );
        assert_eq!(component.applied_scale(), changed);
    }

    #[test]
    fn close_applies_a_pending_keyboard_scale_edit() {
        let snapshot = settings_snapshot();
        let mut component = Component::default();
        component.install(&snapshot);
        component.open();
        let changed = another_scale(component.applied_scale());
        component.update(Message::UiScaleChanged(changed)).unwrap();

        assert!(matches!(
            component.update(Message::Close).unwrap(),
            Some(Outcome::Closed)
        ));
        assert_eq!(component.applied_scale(), changed);
        assert!(!component.is_open());
        assert!(!component.ui_scale_drag_active);
    }

    #[test]
    fn settlements_rebase_scale_without_overwriting_an_active_drag() {
        let snapshot = settings_snapshot();
        let mut component = Component::default();
        component.install(&snapshot);
        let original = component.applied_scale();
        let changed = another_scale(original);
        component.update(Message::UiScaleChanged(changed)).unwrap();

        component.settle_success(&snapshot);
        assert_eq!(component.draft().unwrap().ui.uiscale, changed);
        assert_eq!(component.applied_scale(), original);

        component.settle_failure(Some(&snapshot));
        assert_eq!(component.draft().unwrap().ui.uiscale, original);
        assert_eq!(component.applied_scale(), original);
        assert!(!component.ui_scale_drag_active);

        let mut settled = snapshot.clone();
        settled.settingsstate.ui.uiscale = changed;
        component.settle_success(&settled);
        assert_eq!(component.draft().unwrap().ui.uiscale, changed);
        assert_eq!(component.applied_scale(), changed);
    }

    #[test]
    fn reset_install_and_transport_reset_rebase_draft_and_applied_scale() {
        let snapshot = settings_snapshot();
        let mut component = Component::default();
        component.install(&snapshot);
        let changed = another_scale(component.applied_scale());
        let mut reset = snapshot;
        reset.settingsstate.ui.uiscale = changed;

        component.reset(&reset);
        assert_eq!(component.draft().unwrap().ui.uiscale, changed);
        assert_eq!(component.applied_scale(), changed);

        component.reset_transport();
        assert!(component.draft().is_none());
        assert_eq!(component.applied_scale(), 1.0);
        assert!(!component.ui_scale_drag_active);
    }
}
