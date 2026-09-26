//! The root Status interaction group. Operation ownership remains in the model.
pub mod environment;
mod overlay;
#[cfg(test)]
mod tests;
use crate::fluent_theme::{Element, Theme};
use crate::view_model::notices::{CopyToken, NoticeId, NoticeStore, Presentation};
use iced::widget::{column, container, keyed_column, row, scrollable, space, text};
use iced::{Center, Fill, Length};
use std::collections::HashMap;

pub const TRIGGER_ID: &str = "navigation.status";
pub const CLOSE_ID: &str = "status.close";
pub const PANEL_ID: &str = "status.panel";
pub const SCROLL_ID: &str = "status.scroll";
pub const STATUS_WIDTH: f32 = 192.0;
pub const SETTINGS_WIDTH: f32 = 96.0;
pub const CONTROL_HEIGHT: f32 = 34.0;
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Control {
    Trigger,
    Settings,
    Close,
    Copy(NoticeId),
    Detail(NoticeId),
    Dismiss(NoticeId),
}
impl Control {
    pub fn id(self) -> String {
        match self {
            Self::Trigger => TRIGGER_ID.into(),
            Self::Settings => "navigation.settings".into(),
            Self::Close => CLOSE_ID.into(),
            Self::Copy(id) => format!("status.{}.copy", id.0),
            Self::Detail(id) => format!("status.{}.detail", id.0),
            Self::Dismiss(id) => format!("status.{}.dismiss", id.0),
        }
    }
    #[cfg(target_arch = "wasm32")]
    pub fn name(self) -> String {
        match self {
            Self::Trigger => "Status notifications".into(),
            Self::Settings => "Settings".into(),
            Self::Close => "Close Status notifications".into(),
            Self::Copy(id) => format!("Copy notification {}", id.0),
            Self::Detail(id) => format!("Notification {} detail", id.0),
            Self::Dismiss(id) => format!("Dismiss notification {}", id.0),
        }
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Opening {
    Mouse,
    Keyboard,
    Touch,
}
#[derive(Debug, Clone)]
pub enum Message {
    Activate(Control, Opening),
    Hover(bool),
    Close,
    Outside,
    Focused(Control, bool),
    FocusChecked {
        revision: u64,
        focused: Option<Control>,
    },
    Traverse(bool),
    CopyResolved {
        token: CopyToken,
        result: Result<(), iced::clipboard::Error>,
    },
    #[cfg(target_arch = "wasm32")]
    Environment(environment::State),
    SelectDetail(NoticeId, iced::widget::text_editor::Action),
}
#[derive(Debug, Clone)]
struct Detail {
    present: bool,
    presentation: Presentation,
    content: iced::widget::text_editor::Content,
}
#[derive(Debug, Clone)]
pub struct Component {
    pub open: bool,
    pub environment: environment::State,
    pub focus: Option<Control>,
    opening: Opening,
    hover: bool,
    latch: bool,
    presentation: Option<Presentation>,
    order: Vec<NoticeId>,
    details: HashMap<NoticeId, Detail>,
}
impl Default for Component {
    fn default() -> Self {
        Self {
            open: false,
            environment: environment::State::default(),
            focus: None,
            opening: Opening::Mouse,
            hover: false,
            latch: false,
            presentation: None,
            order: Vec::new(),
            details: HashMap::new(),
        }
    }
}
impl Component {
    /// Reconcile only actual row changes; return a focus operation only when its row retired.
    pub fn sync(&mut self, notices: &NoticeStore) -> Option<Control> {
        if self.presentation.as_ref() == Some(notices.presentation()) {
            return None;
        }
        for detail in self.details.values_mut() {
            detail.present = false;
        }
        for notice in notices.rows() {
            let detail = self.details.entry(notice.id).or_insert_with(|| Detail {
                present: true,
                presentation: notice.presentation().clone(),
                content: iced::widget::text_editor::Content::with_text(&notice.detail),
            });
            detail.present = true;
            if &detail.presentation != notice.presentation() {
                detail.presentation = notice.presentation().clone();
                detail.content = iced::widget::text_editor::Content::with_text(&notice.detail);
            }
        }
        self.details.retain(|_, detail| detail.present);
        let fallback = match self.focus {
            Some(Control::Copy(id) | Control::Detail(id) | Control::Dismiss(id))
                if !self.details.contains_key(&id) =>
            {
                let index = self
                    .order
                    .iter()
                    .position(|owned| *owned == id)
                    .unwrap_or(0);
                let next = self
                    .order
                    .iter()
                    .skip(index + 1)
                    .find(|id| self.details.contains_key(*id))
                    .or_else(|| {
                        self.order[..index]
                            .iter()
                            .rev()
                            .find(|id| self.details.contains_key(*id))
                    });
                Some(next.copied().map_or(Control::Trigger, Control::Copy))
            }
            _ => None,
        };
        if let Some(control) = fallback {
            self.focus = Some(control);
        }
        if notices.is_empty() {
            self.close();
        }
        self.order.clear();
        self.order.extend(notices.rows().map(|notice| notice.id));
        self.presentation = Some(notices.presentation().clone());
        fallback
    }
    pub fn select_detail(&mut self, id: NoticeId, action: iced::widget::text_editor::Action) {
        if !action.is_edit()
            && let Some(detail) = self.details.get_mut(&id)
        {
            detail.content.perform(action);
            self.focus = Some(Control::Detail(id));
        }
    }
    pub fn close(&mut self) {
        self.open = false;
        self.latch = self.hover;
    }
    pub fn hover(&mut self, inside: bool, nonempty: bool) {
        if inside == self.hover {
            return;
        }
        self.hover = inside;
        if !inside {
            self.latch = false;
            if self.opening == Opening::Mouse {
                self.open = false;
            }
        } else if !self.open && !self.latch && nonempty {
            self.open = true;
            self.opening = Opening::Mouse;
        }
        crate::integration_control::status::hover_transition(inside, self.open, self.latch);
    }
    pub fn activate(&mut self, opening: Opening, nonempty: bool) {
        if self.open && self.opening != Opening::Mouse {
            self.close();
        } else if nonempty {
            self.open = true;
            self.opening = opening;
            self.latch = false;
        }
    }
    pub fn contains_control(&self, control: Control) -> bool {
        match control {
            Control::Trigger | Control::Settings => true,
            Control::Close => self.open,
            Control::Copy(id) | Control::Detail(id) | Control::Dismiss(id) => {
                self.open && self.details.contains_key(&id)
            }
        }
    }
    pub fn controls(&self, notices: &NoticeStore) -> Vec<Control> {
        let mut controls = vec![Control::Trigger];
        if self.open {
            controls.push(Control::Close);
            for notice in notices.rows() {
                controls.extend([
                    Control::Copy(notice.id),
                    Control::Detail(notice.id),
                    Control::Dismiss(notice.id),
                ]);
            }
        }
        controls
    }
    pub fn traverse(&self, reverse: bool, notices: &NoticeStore) -> Control {
        let controls = self.controls(notices);
        let index = self
            .focus
            .and_then(|focused| controls.iter().position(|control| *control == focused))
            .unwrap_or(0);
        controls[(index + if reverse { controls.len() - 1 } else { 1 }) % controls.len()]
    }
    pub fn header<'a>(&'a self, notices: &'a NoticeStore) -> Element<'a, Message> {
        let alert = !notices.is_empty();
        let label: Element<'a, Message> = if alert {
            row![
                overlay::warning_icon(),
                text(format!("Status: {}", notices.len()))
            ]
            .spacing(8)
            .align_y(Center)
            .into()
        } else {
            row![
                text("Status:"),
                text("Ok").color(iced::Color::from_rgb8(0, 160, 64))
            ]
            .spacing(5)
            .align_y(Center)
            .into()
        };
        row![
            overlay::control(
                Control::Trigger,
                container(label).center(Fill).into(),
                STATUS_WIDTH,
                alert,
                self.environment
            ),
            overlay::control(
                Control::Settings,
                container(text("Settings")).center(Fill).into(),
                SETTINGS_WIDTH,
                false,
                self.environment
            ),
        ]
        .spacing(8)
        .align_y(Center)
        .into()
    }
    pub fn wrap<'a>(
        &'a self,
        content: Element<'a, crate::message::Message>,
        notices: &'a NoticeStore,
    ) -> Element<'a, crate::message::Message> {
        overlay::host(content, self, notices)
    }
    fn panel<'a>(&'a self, notices: &'a NoticeStore, max_height: f32) -> Element<'a, Message> {
        let rows = keyed_column(notices.rows().map(|notice| {
            let controls = row![
                overlay::control(
                    Control::Copy(notice.id),
                    container(text("Copy")).center(Fill).into(),
                    64.0,
                    false,
                    self.environment
                ),
                overlay::control(
                    Control::Dismiss(notice.id),
                    container(text("×")).center(Fill).into(),
                    34.0,
                    false,
                    self.environment
                ),
            ]
            .spacing(8);
            let detail: Element<'a, Message> = if let Some(detail) = self.details.get(&notice.id) {
                let id = notice.id;
                iced::widget::text_editor(&detail.content)
                    .id(Control::Detail(id).id())
                    .size(14)
                    .padding(0)
                    .wrapping(iced::advanced::text::Wrapping::WordOrGlyph)
                    .on_action(move |action| Message::SelectDetail(id, action))
                    .key_binding(|press| {
                        use iced::widget::text_editor::Binding;
                        let binding = Binding::from_key_press(press)?;
                        matches!(
                            binding,
                            Binding::Copy
                                | Binding::Move(_)
                                | Binding::Select(_)
                                | Binding::SelectWord
                                | Binding::SelectLine
                                | Binding::SelectAll
                                | Binding::Unfocus
                        )
                        .then_some(binding)
                    })
                    .style(|theme: &Theme, _| iced::widget::text_editor::Style {
                        background: iced::Color::TRANSPARENT.into(),
                        border: iced::Border::default(),
                        placeholder: theme.tokens().neutral_foreground1,
                        value: theme.tokens().neutral_foreground1,
                        selection: theme.tokens().brand_background2,
                    })
                    .into()
            } else {
                text(&notice.detail)
                    .size(14)
                    .wrapping(iced::advanced::text::Wrapping::WordOrGlyph)
                    .into()
            };
            let row = container(
                column![
                    text(notice.title).size(18),
                    controls,
                    overlay::detail_text(notice.id, detail)
                ]
                .spacing(8),
            )
            .padding(12)
            .width(Fill)
            .style(crate::fluent_theme::container_card);
            (notice.id.0, row.into())
        }))
        .spacing(8);
        container(
            column![
                row![
                    text("Status").size(20),
                    space::horizontal(),
                    overlay::control(
                        Control::Close,
                        container(text("×")).center(Fill).into(),
                        34.0,
                        false,
                        self.environment
                    )
                ]
                .align_y(Center),
                scrollable(rows)
                    .id(SCROLL_ID)
                    .height(Length::Fill)
                    .style(crate::fluent_theme::scrollable_default),
            ]
            .spacing(10),
        )
        .id(PANEL_ID)
        .padding(12)
        .height(max_height)
        .width(Fill)
        .style(crate::fluent_theme::container_modal)
        .into()
    }
}
pub fn alert_style(theme: &Theme, pulse: f32, alert: bool) -> iced::widget::button::Style {
    let mut style =
        crate::fluent_theme::button_secondary(theme, iced::widget::button::Status::Active);
    style.border.width = if alert { 1.0 } else { 0.0 };
    if alert {
        style.border.color = iced::Color::WHITE;
        let base = match style.background {
            Some(iced::Background::Color(color)) => color,
            _ => iced::Color::TRANSPARENT,
        };
        style.background = Some(
            base.mix(iced::Color::from_rgb8(255, 151, 177), 0.18 + 0.14 * pulse)
                .into(),
        );
    }
    style
}

pub fn check_focus(controls: Vec<Control>, revision: u64) -> iced::Task<crate::message::Message> {
    struct Find {
        controls: Vec<(iced::advanced::widget::Id, Control)>,
        focused: Option<Control>,
    }
    impl iced::advanced::widget::Operation<Option<Control>> for Find {
        fn traverse(
            &mut self,
            operate: &mut dyn FnMut(&mut dyn iced::advanced::widget::Operation<Option<Control>>),
        ) {
            operate(self);
        }
        fn focusable(
            &mut self,
            id: Option<&iced::advanced::widget::Id>,
            _: iced::Rectangle,
            state: &mut dyn iced::advanced::widget::operation::Focusable,
        ) {
            if state.is_focused() {
                self.focused = self
                    .controls
                    .iter()
                    .find(|(owned, _)| Some(owned) == id)
                    .map(|(_, control)| *control);
            }
        }
        fn finish(&self) -> iced::advanced::widget::operation::Outcome<Option<Control>> {
            iced::advanced::widget::operation::Outcome::Some(self.focused)
        }
    }
    iced::advanced::widget::operate(Find {
        controls: controls
            .into_iter()
            .map(|control| (control.id().into(), control))
            .collect(),
        focused: None,
    })
    .map(move |focused| {
        crate::message::Message::Status(Message::FocusChecked { revision, focused })
    })
}
