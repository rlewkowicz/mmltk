use iced_core::{
    Alignment, Background, Border, Color, Element, Event, Layout, Length, Padding, Point,
    Rectangle, Shadow, Shell, Size, Widget,
    alignment::Vertical,
    keyboard,
    layout::{Limits, Node},
    mouse::{self, Cursor},
    renderer,
    widget::{
        self, Operation, Tree,
        tree::{State, Tag},
    },
};
use iced_widget::{
    Column, Container, Row, Text,
    text::{LineHeight, Wrapping},
    text_input::{self, Value, cursor},
};
use num_traits::{Num, NumAssignOps, bounds::Bounded};
use std::{
    fmt::Display,
    ops::{Bound, RangeBounds},
    str::FromStr,
};

use crate::iced_aw_font::advanced_text::{down_open, up_open};
use crate::style::{self, Status};
pub use crate::style::{
    StyleFn,
    number_input::{self, Catalog, Style},
};
use crate::widget::typed_input::TypedInput;

const DEFAULT_PADDING: Padding = Padding::new(5.0);

#[allow(missing_debug_implementations)]
pub struct NumberInput<'a, T, Message, Theme = iced_widget::Theme, Renderer = iced_widget::Renderer>
where
    Renderer: iced_core::text::Renderer<Font = iced_core::Font>,
    Theme: number_input::ExtendedCatalog,
{
        value: T,
        step: T,
        min: Bound<T>,
        max: Bound<T>,
        padding: iced_core::Padding,
        size: Option<iced_core::Pixels>,
        content: TypedInput<'a, T, InternalMessage<T>, Theme, Renderer>,
        on_change: Option<Box<dyn 'a + Fn(T) -> Message>>,
        #[allow(clippy::type_complexity)]
    on_submit: Option<Message>,
        on_paste: Option<Box<dyn 'a + Fn(T) -> Message>>,
        class: <Theme as style::number_input::Catalog>::Class<'a>,
        font: Renderer::Font,
        ignore_scroll_events: bool,
        ignore_buttons: bool,
}

#[derive(Debug, Clone, PartialEq)]
#[allow(clippy::enum_variant_names)]
enum InternalMessage<T> {
    OnChange(T),
    OnSubmit(Result<T, String>),
    OnPaste(T),
}

impl<'a, T, Message, Theme, Renderer> NumberInput<'a, T, Message, Theme, Renderer>
where
    T: Num + NumAssignOps + PartialOrd + Display + FromStr + Clone + Bounded + 'a,
    Message: Clone + 'a,
    Renderer: iced_core::text::Renderer<Font = iced_core::Font>,
    Theme: number_input::ExtendedCatalog,
{
                            pub fn new<F>(value: &T, bounds: impl RangeBounds<T>, on_change: F) -> Self
    where
        F: 'a + Fn(T) -> Message + Clone,
    {
        let padding = DEFAULT_PADDING;

        Self {
            value: value.clone(),
            step: T::one(),
            min: bounds.start_bound().cloned(),
            max: bounds.end_bound().cloned(),
            padding,
            size: None,
            content: TypedInput::new("", value)
                .on_input(InternalMessage::OnChange)
                .padding(padding)
                .width(Length::Fixed(127.0))
                .class(Theme::default_input()),
            on_change: Some(Box::new(on_change)),
            on_submit: None,
            on_paste: None,
            class: <Theme as style::number_input::Catalog>::default(),
            font: Renderer::Font::default(),
            ignore_scroll_events: false,
            ignore_buttons: false,
        }
    }

        #[must_use]
    pub fn id(mut self, id: impl Into<widget::Id>) -> Self {
        self.content = self.content.id(id.into());
        self
    }

                #[must_use]
    pub fn on_input<F>(mut self, callback: F) -> Self
    where
        F: 'a + Fn(T) -> Message,
    {
        self.content = self.content.on_input(InternalMessage::OnChange);
        self.on_change = Some(Box::new(callback));
        self
    }

                #[must_use]
    pub fn on_input_maybe<F>(mut self, callback: Option<F>) -> Self
    where
        F: 'a + Fn(T) -> Message,
    {
        if let Some(callback) = callback {
            self.content = self.content.on_input(InternalMessage::OnChange);
            self.on_change = Some(Box::new(callback));
        } else {
            if self.on_submit.is_none() {
                #[allow(unused_assignments)]
                let mut f = Some(InternalMessage::OnChange);
                f = None;
                self.content = self.content.on_input_maybe(f);
            }
            self.on_change = None;
        }
        self
    }

            #[must_use]
    pub fn on_submit(mut self, message: Message) -> Self {
        self.content = self.content.on_submit(InternalMessage::OnSubmit);
        self.on_submit = Some(message);
        self
    }

                    #[must_use]
    pub fn on_submit_maybe(mut self, message: Option<Message>) -> Self {
        if let Some(message) = message {
            self.content = self.content.on_submit(InternalMessage::OnSubmit);
            self.on_submit = Some(message);
        } else {
            if self.on_change.is_none() {
                #[allow(unused_assignments)]
                let mut f = Some(InternalMessage::OnChange);
                f = None;
                self.content = self.content.on_input_maybe(f);
            }
            #[allow(unused_assignments)]
            let mut f = Some(InternalMessage::OnSubmit);
            f = None;
            self.content = self.content.on_submit_maybe(f);
            self.on_change = None;
        }
        self
    }

        #[must_use]
    pub fn on_paste<F>(mut self, callback: F) -> Self
    where
        F: 'a + Fn(T) -> Message,
    {
        self.content = self.content.on_paste(InternalMessage::OnPaste);
        self.on_paste = Some(Box::new(callback));
        self
    }

        #[must_use]
    pub fn on_paste_maybe<F>(mut self, callback: Option<F>) -> Self
    where
        F: 'a + Fn(T) -> Message,
    {
        if let Some(callback) = callback {
            self.content = self.content.on_paste(InternalMessage::OnPaste);
            self.on_paste = Some(Box::new(callback));
        } else {
            #[allow(unused_assignments)]
            let mut f = Some(InternalMessage::OnPaste);
            f = None;
            self.content = self.content.on_paste_maybe(f);
            self.on_paste = None;
        }
        self
    }

                    #[allow(clippy::needless_pass_by_value)]
    #[must_use]
    pub fn font(mut self, font: Renderer::Font) -> Self {
        self.font = font;
        self.content = self.content.font(font);
        self
    }

        #[must_use]
    pub fn icon(mut self, icon: iced_widget::text_input::Icon<Renderer::Font>) -> Self {
        self.content = self.content.icon(icon);
        self
    }

        #[must_use]
    pub fn width(mut self, width: impl Into<Length>) -> Self {
        self.content = self.content.width(width);
        self
    }

        #[deprecated(since = "0.11.1", note = "use `width` instead")]
    #[must_use]
    pub fn content_width(self, width: impl Into<Length>) -> Self {
        self.width(width)
    }

        #[must_use]
    pub fn padding(mut self, padding: impl Into<iced_core::Padding>) -> Self {
        let padding = padding.into();
        self.padding = padding;
        self.content = self.content.padding(padding);
        self
    }

        #[must_use]
    pub fn set_size(mut self, size: impl Into<iced_core::Pixels>) -> Self {
        let size = size.into();
        self.size = Some(size);
        self.content = self.content.size(size);
        self
    }

        #[must_use]
    pub fn line_height(mut self, line_height: impl Into<iced_widget::text::LineHeight>) -> Self {
        self.content = self.content.line_height(line_height);
        self
    }

        #[must_use]
    pub fn align_x(mut self, alignment: impl Into<iced_core::alignment::Horizontal>) -> Self {
        self.content = self.content.align_x(alignment);
        self
    }

        #[must_use]
    pub fn style(mut self, style: impl Fn(&Theme, Status) -> Style + 'a) -> Self
    where
        <Theme as style::number_input::Catalog>::Class<'a>: From<StyleFn<'a, Theme, Style>>,
    {
        self.class = (Box::new(style) as StyleFn<'a, Theme, Style>).into();
        self
    }
        #[must_use]
    pub fn input_style(
        mut self,
        style: impl Fn(&Theme, text_input::Status) -> text_input::Style + 'a,
    ) -> Self
    where
        <Theme as text_input::Catalog>::Class<'a>: From<text_input::StyleFn<'a, Theme>>,
    {
        self.content = self.content.style(style);
        self
    }

        #[must_use]
    pub fn class(
        mut self,
        class: impl Into<<Theme as style::number_input::Catalog>::Class<'a>>,
    ) -> Self {
        self.class = class.into();
        self
    }

                                #[must_use]
    pub fn bounds(mut self, bounds: impl RangeBounds<T>) -> Self {
        self.min = bounds.start_bound().cloned();
        self.max = bounds.end_bound().cloned();

        self
    }

        #[must_use]
    pub fn step(mut self, step: T) -> Self {
        self.step = step;
        self
    }

            #[must_use]
    pub fn ignore_buttons(mut self, ignore: bool) -> Self {
        self.ignore_buttons = ignore;
        self
    }

            #[must_use]
    pub fn ignore_scroll(mut self, ignore: bool) -> Self {
        self.ignore_scroll_events = ignore;
        self
    }

        fn decrease_value(&mut self, shell: &mut Shell<Message>) {
        if self.value.clone() > self.min() + self.step.clone()
            && self.valid(&(self.value.clone() - self.step.clone()))
        {
            self.value -= self.step.clone();
        } else if self.value > self.min() {
            self.value = self.min();
        } else {
            return;
        }
        if let Some(on_change) = &self.on_change {
            shell.publish(on_change(self.value.clone()));
        }
    }

        fn increase_value(&mut self, shell: &mut Shell<Message>) {
        if self.value < self.max() - self.step.clone()
            && self.valid(&(self.value.clone() + self.step.clone()))
        {
            self.value += self.step.clone();
        } else if self.value < self.max() {
            self.value = self.max();
        } else {
            return;
        }
        if let Some(on_change) = &self.on_change {
            shell.publish(on_change(self.value.clone()));
        }
    }

            fn min(&self) -> T {
        match &self.min {
            Bound::Included(n) => n.clone(),
            Bound::Excluded(n) => n.clone() + self.step.clone(),
            Bound::Unbounded => T::min_value(),
        }
    }

            fn max(&self) -> T {
        match &self.max {
            Bound::Included(n) => n.clone(),
            Bound::Excluded(n) => n.clone() - self.step.clone(),
            Bound::Unbounded => T::max_value(),
        }
    }

        fn valid(&self, value: &T) -> bool {
        (match &self.min {
            Bound::Included(n) if *n > *value => false,
            Bound::Excluded(n) if *n >= *value => false,
            _ => true,
        }) && (match &self.max {
            Bound::Included(n) if *n < *value => false,
            Bound::Excluded(n) if *n <= *value => false,
            _ => true,
        })
    }

        fn can_increase(&self) -> bool {
        (self.value < self.max() - self.step.clone()
            && self.valid(&(self.value.clone() + self.step.clone())))
            || self.value < self.max()
    }

        fn can_decrease(&self) -> bool {
        (self.value.clone() > self.min() + self.step.clone()
            && self.valid(&(self.value.clone() - self.step.clone())))
            || self.value > self.min()
    }

            fn disabled(&self) -> bool {
        match (&self.min, &self.max) {
            (Bound::Included(n) | Bound::Excluded(n), Bound::Included(m) | Bound::Excluded(m)) => {
                *n >= *m
            }
            _ => false,
        }
    }
}

impl<'a, T, Message, Theme, Renderer> Widget<Message, Theme, Renderer>
    for NumberInput<'a, T, Message, Theme, Renderer>
where
    T: Num + NumAssignOps + PartialOrd + Display + FromStr + ToString + Clone + Bounded + 'a,
    Message: 'a + Clone,
    Renderer: 'a + iced_core::text::Renderer<Font = iced_core::Font>,
    Theme: number_input::ExtendedCatalog,
{
    fn tag(&self) -> Tag {
        Tag::of::<ModifierState>()
    }
    fn state(&self) -> State {
        State::new(ModifierState::default())
    }

    fn diff(&mut self, tree: &mut Tree) {
        tree.children.truncate(1);
        if let Some(content_tree) = tree.children.first_mut() {
            if content_tree.tag != self.content.tag() {
                *content_tree = Tree {
                    tag: self.content.tag(),
                    state: self.content.state(),
                    children: Vec::new(),
                };
            }
            self.content.diff(content_tree);
        } else {
            let mut content_tree = Tree {
                tag: self.content.tag(),
                state: self.content.state(),
                children: Vec::new(),
            };
            self.content.diff(&mut content_tree);
            tree.children.push(content_tree);
        }
    }

    fn size(&self) -> Size<Length> {
        Widget::size(&self.content)
    }

    fn layout(&mut self, tree: &mut Tree, renderer: &Renderer, limits: &Limits) -> Node {
        let num_size = self.size();
        let limits = limits.width(num_size.width).height(Length::Shrink);
        let content = self
            .content
            .layout(&mut tree.children[0], renderer, &limits);
        let limits2 = Limits::new(Size::new(0.0, 0.0), content.size());
        let txt_size = self.size.unwrap_or_else(|| renderer.default_size());

        let icon_size = txt_size * 2.5 / 4.0;
        let btn_mod = |c| {
            Container::<Message, Theme, Renderer>::new(Text::new(format!(" {c} ")).size(icon_size))
                .center_y(Length::Shrink)
                .center_x(Length::Shrink)
        };

        let default_padding = DEFAULT_PADDING;

        let mut element = if self.padding.top < default_padding.top
            || self.padding.bottom < default_padding.bottom
            || self.padding.right < default_padding.right
        {
            Element::new(
                Row::<Message, Theme, Renderer>::new()
                    .spacing(1)
                    .width(Length::Shrink)
                    .push(btn_mod('+'))
                    .push(btn_mod('-')),
            )
        } else {
            Element::new(
                Column::<Message, Theme, Renderer>::new()
                    .spacing(1)
                    .width(Length::Shrink)
                    .push(btn_mod('▲'))
                    .push(btn_mod('▼')),
            )
        };

        let input_tree = if let Some(child_tree) = tree.children.get_mut(1) {
            child_tree.diff(element.as_widget_mut());
            child_tree
        } else {
            let mut child_tree = Tree::new(element.as_widget());
            child_tree.diff(element.as_widget_mut());
            tree.children.insert(1, child_tree);
            &mut tree.children[1]
        };

        let mut modifier = element
            .as_widget_mut()
            .layout(input_tree, renderer, &limits2.loose());
        let intrinsic = Size::new(
            content.size().width - 1.0,
            content.size().height.max(modifier.size().height),
        );
        modifier = modifier.align(Alignment::End, Alignment::Center, intrinsic);

        let size = limits.resolve(num_size.width, Length::Shrink, intrinsic);
        Node::with_children(size, vec![content, modifier])
    }

    fn operate(
        &mut self,
        tree: &mut Tree,
        layout: Layout<'_>,
        renderer: &Renderer,
        operation: &mut dyn Operation,
    ) {
        operation.container(None, layout.bounds());

        let mut children = layout.children();

        if let Some(content_layout) = children.next() {
            self.content
                .operate(&mut tree.children[0], content_layout, renderer, operation);
        }

        if let Some(modifier_layout) = children.next()
            && !self.ignore_buttons
        {
            let txt_size = self.size.unwrap_or_else(|| renderer.default_size());
            let icon_size = txt_size * 2.5 / 4.0;

            let btn_mod = |c| {
                Container::<Message, Theme, Renderer>::new(
                    Text::new(format!(" {c} ")).size(icon_size),
                )
                .center_y(Length::Shrink)
                .center_x(Length::Shrink)
            };

            let default_padding = DEFAULT_PADDING;

            let mut element = if self.padding.top < default_padding.top
                || self.padding.bottom < default_padding.bottom
                || self.padding.right < default_padding.right
            {
                Element::new(
                    Row::<Message, Theme, Renderer>::new()
                        .spacing(1)
                        .width(Length::Shrink)
                        .push(btn_mod('+'))
                        .push(btn_mod('-')),
                )
            } else {
                Element::new(
                    Column::<Message, Theme, Renderer>::new()
                        .spacing(1)
                        .width(Length::Shrink)
                        .push(btn_mod('▲'))
                        .push(btn_mod('▼')),
                )
            };

            let modifier_tree = if let Some(child_tree) = tree.children.get_mut(1) {
                child_tree.diff(element.as_widget_mut());
                child_tree
            } else {
                let mut child_tree = Tree::new(element.as_widget());
                child_tree.diff(element.as_widget_mut());
                tree.children.insert(1, child_tree);
                &mut tree.children[1]
            };

            element
                .as_widget_mut()
                .operate(modifier_tree, modifier_layout, renderer, operation);
        }
    }

    #[allow(clippy::too_many_lines, clippy::cognitive_complexity)]
    fn update(
        &mut self,
        state: &mut Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: Cursor,
        renderer: &Renderer,
        shell: &mut Shell<Message>,
        viewport: &Rectangle,
    ) {
        let mut children = layout.children();
        let content = children.next().expect("fail to get content layout");
        let mut mod_children = children
            .next()
            .expect("fail to get modifiers layout")
            .children();
        let inc_bounds = mod_children
            .next()
            .expect("fail to get increase mod layout")
            .bounds();
        let dec_bounds = mod_children
            .next()
            .expect("fail to get decrease mod layout")
            .bounds();

        if self.disabled() {
            return;
        }
        let can_decrease = self.can_decrease();
        let can_increase = self.can_increase();

        let cursor_position = cursor.position().unwrap_or_default();
        let mouse_over_widget = layout.bounds().contains(cursor_position);
        let mouse_over_inc = inc_bounds.contains(cursor_position);
        let mouse_over_dec = dec_bounds.contains(cursor_position);
        let mouse_over_button = mouse_over_inc || mouse_over_dec;

        let modifiers = state.state.downcast_mut::<ModifierState>();
        let mut value = self.content.text().to_owned();

        let child = state.children.get_mut(0).expect("fail to get child");
        let text_input = child
            .state
            .downcast_mut::<text_input::State<Renderer::Paragraph>>();

        let mut messages = Vec::new();
        let mut sub_shell = shell.local(&mut messages);

        let mut forward_to_text = |widget: &mut Self, child| {
            widget.content.update(
                child,
                &event.clone(),
                content,
                cursor,
                renderer,
                &mut sub_shell,
                viewport,
            );
        };

        let supports_negative = self.min() < T::zero();
        let check_value = |value: &str| {
            T::from_str(value).is_ok() || value.is_empty() || value == "-" && supports_negative
        };

        match &event {
            Event::Keyboard(key) => {
                if !text_input.is_focused() {
                    return;
                }

                match key {
                    keyboard::Event::ModifiersChanged(_) => forward_to_text(self, child),
                    keyboard::Event::KeyReleased { .. } => return,
                    keyboard::Event::KeyPressed {
                        key,
                        text,
                        modifiers,
                        ..
                    } => {
                        let cursor = text_input.cursor();

                        let has_value = !modifiers.command()
                            && text
                                .as_ref()
                                .is_some_and(|t| t.chars().any(|c| !c.is_control()));

                        match key.as_ref() {
                            keyboard::Key::Named(keyboard::key::Named::Enter) => {
                                forward_to_text(self, child);
                            }
                            keyboard::Key::Character("c" | "a") if modifiers.command() => {
                                forward_to_text(self, child);
                            }
                            keyboard::Key::Character("x") if modifiers.command() => {
                                if let Some((start, end)) = cursor.selection(&Value::new(&value)) {
                                    let _ = value.drain(start..end);
                                    if check_value(&value) {
                                        forward_to_text(self, child);
                                    } else {
                                        return;
                                    }
                                } else {
                                    return;
                                }
                            }
                            keyboard::Key::Character("v") if modifiers.command() => {
                                forward_to_text(self, child);
                            }
                            keyboard::Key::Named(keyboard::key::Named::Backspace) => {
                                match cursor.state(&Value::new(&value)) {
                                    cursor::State::Selection { start, end } => {
                                        let _ = value.drain(sorted_range(start, end));
                                    }
                                    cursor::State::Index(idx) if idx > 0 => {
                                        if modifiers.command() {
                                            let _ =
                                                value.drain((value.starts_with('-').into())..idx);
                                        } else {
                                            let _ = value.remove(idx - 1);
                                        }
                                    }
                                    cursor::State::Index(_) => return,
                                }

                                shell.capture_event();

                                if check_value(&value) {
                                    forward_to_text(self, child);
                                } else {
                                    return;
                                }
                            }
                            keyboard::Key::Named(keyboard::key::Named::Delete) => {
                                match cursor.state(&Value::new(&value)) {
                                    cursor::State::Selection { start, end } => {
                                        let _ = value.drain(sorted_range(start, end));
                                    }
                                    cursor::State::Index(idx) if idx < value.len() => {
                                        if idx == 0 && value.starts_with('-') {
                                            let _ = value.remove(0);
                                        } else if modifiers.command() {
                                            let _ = value.drain(idx..);
                                        } else {
                                            let _ = value.remove(idx);
                                        }
                                    }
                                    cursor::State::Index(_) => return,
                                }

                                shell.capture_event();

                                if check_value(&value) {
                                    forward_to_text(self, child);
                                } else {
                                    return;
                                }
                            }
                            keyboard::Key::Named(keyboard::key::Named::ArrowDown)
                                if can_decrease && !has_value =>
                            {
                                shell.capture_event();
                                shell.request_redraw();
                                self.decrease_value(shell);
                            }
                            keyboard::Key::Named(keyboard::key::Named::ArrowUp)
                                if can_increase && !has_value =>
                            {
                                shell.capture_event();
                                shell.request_redraw();

                                self.increase_value(shell);
                            }
                            keyboard::Key::Named(
                                keyboard::key::Named::ArrowLeft
                                | keyboard::key::Named::ArrowRight
                                | keyboard::key::Named::Home
                                | keyboard::key::Named::End,
                            ) if !has_value => forward_to_text(self, child),
                            _ => match text {
                                Some(text) => {
                                    match cursor.state(&Value::new(&value)) {
                                        cursor::State::Index(idx) => {
                                            value.insert_str(idx, text);
                                        }
                                        cursor::State::Selection { start, end } => {
                                            value.replace_range(sorted_range(start, end), text);
                                        }
                                    }

                                    shell.capture_event();
                                    shell.request_redraw();

                                    if check_value(&value) {
                                        forward_to_text(self, child);
                                    } else {
                                        return;
                                    }
                                }
                                None => return,
                            },
                        }
                    }
                }
            }
            Event::Mouse(mouse::Event::WheelScrolled { delta })
                if mouse_over_widget && !self.ignore_scroll_events =>
            {
                match delta {
                    mouse::ScrollDelta::Lines { y, .. } | mouse::ScrollDelta::Pixels { y, .. } => {
                        if y.is_sign_positive() {
                            self.increase_value(shell);
                        } else {
                            self.decrease_value(shell);
                        }
                    }
                }
                shell.capture_event();
                shell.request_redraw();
            }
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left))
                if mouse_over_button && !self.ignore_buttons =>
            {
                if mouse_over_dec {
                    modifiers.decrease_pressed = true;
                    self.decrease_value(shell);
                } else {
                    modifiers.increase_pressed = true;
                    self.increase_value(shell);
                }
                shell.capture_event();
                shell.request_redraw();
            }
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left))
                if mouse_over_button =>
            {
                if mouse_over_dec {
                    modifiers.decrease_pressed = false;
                } else {
                    modifiers.increase_pressed = false;
                }
                shell.capture_event();
                shell.request_redraw();
            }
            _ => forward_to_text(self, child),
        }

        let mut invalidate_layout = false;
        let mut invalidate_widgets = false;
        shell.merge_maybe(sub_shell, |message| {
            invalidate_layout = true;
            match message {
                InternalMessage::OnChange(value) => {
                    if self.valid(&value) && (self.value != value || self.value.is_zero()) {
                        self.value = value.clone();
                        return self.on_change.as_ref().map(|on_change| on_change(value));
                    }
                }
                InternalMessage::OnSubmit(result) => {
                    if let Err(text) = result {
                        assert!(
                            text.is_empty(),
                            "We shouldn't be able to submit a number input with an invalid value"
                        );
                    }
                    if let Some(on_submit) = &self.on_submit {
                        return Some(on_submit.clone());
                    }
                }
                InternalMessage::OnPaste(value) => {
                    if self.valid(&value) && self.value != value {
                        self.value = value.clone();
                        return self.on_paste.as_ref().map(|on_paste| on_paste(value));
                    }
                    if !self.valid(&value) {
                        invalidate_widgets = true;
                    }
                }
            }
            None
        });
        if invalidate_layout {
            shell.invalidate_layout();
        }
        if invalidate_widgets {
            shell.invalidate_widgets();
        }
    }

    fn mouse_interaction(
        &self,
        _state: &Tree,
        layout: Layout<'_>,
        cursor: Cursor,
        _viewport: &Rectangle,
        _renderer: &Renderer,
    ) -> mouse::Interaction {
        let bounds = layout.bounds();
        let mut children = layout.children();
        let _content_layout = children.next().expect("fail to get content layout");
        let mut mod_children = children
            .next()
            .expect("fail to get modifiers layout")
            .children();
        let inc_bounds = mod_children
            .next()
            .expect("fail to get increase mod layout")
            .bounds();
        let dec_bounds = mod_children
            .next()
            .expect("fail to get decrease mod layout")
            .bounds();
        let is_mouse_over = bounds.contains(cursor.position().unwrap_or_default());
        let is_decrease_disabled = !self.can_decrease();
        let is_increase_disabled = !self.can_increase();
        let mouse_over_decrease = dec_bounds.contains(cursor.position().unwrap_or_default());
        let mouse_over_increase = inc_bounds.contains(cursor.position().unwrap_or_default());

        if ((mouse_over_decrease && !is_decrease_disabled)
            || (mouse_over_increase && !is_increase_disabled))
            && !self.ignore_buttons
        {
            mouse::Interaction::Pointer
        } else if is_mouse_over {
            mouse::Interaction::Text
        } else {
            mouse::Interaction::default()
        }
    }

    fn draw(
        &self,
        state: &Tree,
        renderer: &mut Renderer,
        theme: &Theme,
        style: &renderer::Style,
        layout: Layout<'_>,
        cursor: Cursor,
        viewport: &Rectangle,
    ) {
        let mut children = layout.children();
        let content_layout = children.next().expect("fail to get content layout");
        let mut mod_children = children
            .next()
            .expect("fail to get modifiers layout")
            .children();
        let inc_bounds = mod_children
            .next()
            .expect("fail to get increase mod layout")
            .bounds();
        let dec_bounds = mod_children
            .next()
            .expect("fail to get decrease mod layout")
            .bounds();
        self.content.draw(
            &state.children[0],
            renderer,
            theme,
            style,
            content_layout,
            cursor,
            viewport,
        );
        let is_decrease_disabled = !self.can_decrease();
        let is_increase_disabled = !self.can_increase();

        let decrease_btn_style = if is_decrease_disabled {
            style::number_input::Catalog::style(theme, &self.class, Status::Disabled)
        } else if state.state.downcast_ref::<ModifierState>().decrease_pressed {
            style::number_input::Catalog::style(theme, &self.class, Status::Pressed)
        } else {
            style::number_input::Catalog::style(theme, &self.class, Status::Active)
        };

        let increase_btn_style = if is_increase_disabled {
            style::number_input::Catalog::style(theme, &self.class, Status::Disabled)
        } else if state.state.downcast_ref::<ModifierState>().increase_pressed {
            style::number_input::Catalog::style(theme, &self.class, Status::Pressed)
        } else {
            style::number_input::Catalog::style(theme, &self.class, Status::Active)
        };

        let txt_size = self.size.unwrap_or_else(|| renderer.default_size());

        let icon_size = txt_size * 2.5 / 4.0;

        if self.ignore_buttons {
            return;
        }
        if dec_bounds.intersects(viewport) {
            renderer.fill_quad(
                renderer::Quad {
                    bounds: dec_bounds,
                    border: Border {
                        radius: (3.0).into(),
                        width: 0.0,
                        color: Color::TRANSPARENT,
                    },
                    shadow: Shadow::default(),
                    snap: false,
                },
                decrease_btn_style
                    .button_background
                    .unwrap_or(Background::Color(Color::TRANSPARENT)),
            );
        }

        let (content, font, shaping) = down_open();
        renderer.fill_text(
            iced_core::text::Text {
                content,
                bounds: Size::new(dec_bounds.width, dec_bounds.height),
                size: icon_size,
                font,
                line_height: LineHeight::Relative(1.3),
                shaping,
                wrapping: Wrapping::default(),
                ellipsis: iced_core::text::Ellipsis::None,
                hint_factor: renderer.scale_factor(),
                align_x: Alignment::Center.into(),
                align_y: Vertical::Center,
            },
            Point::new(dec_bounds.center_x(), dec_bounds.center_y()),
            decrease_btn_style.icon_color,
            dec_bounds,
        );

        if inc_bounds.intersects(viewport) {
            renderer.fill_quad(
                renderer::Quad {
                    bounds: inc_bounds,
                    border: Border {
                        radius: (3.0).into(),
                        width: 0.0,
                        color: Color::TRANSPARENT,
                    },
                    shadow: Shadow::default(),
                    snap: false,
                },
                increase_btn_style
                    .button_background
                    .unwrap_or(Background::Color(Color::TRANSPARENT)),
            );
        }

        let (content, font, shaping) = up_open();
        renderer.fill_text(
            iced_core::text::Text {
                content,
                bounds: Size::new(inc_bounds.width, inc_bounds.height),
                size: icon_size,
                font,
                line_height: LineHeight::Relative(1.3),
                shaping,
                wrapping: Wrapping::default(),
                ellipsis: iced_core::text::Ellipsis::None,
                hint_factor: renderer.scale_factor(),
                align_x: Alignment::Center.into(),
                align_y: Vertical::Center,
            },
            Point::new(inc_bounds.center_x(), inc_bounds.center_y()),
            increase_btn_style.icon_color,
            inc_bounds,
        );
    }
}

#[derive(Default, Clone, Debug)]
pub struct ModifierState {
        pub decrease_pressed: bool,
        pub increase_pressed: bool,
}

impl<'a, T, Message, Theme, Renderer> From<NumberInput<'a, T, Message, Theme, Renderer>>
    for Element<'a, Message, Theme, Renderer>
where
    T: 'a + Num + NumAssignOps + PartialOrd + Display + FromStr + Clone + Bounded,
    Message: 'a + Clone,
    Renderer: 'a + iced_core::text::Renderer<Font = iced_core::Font>,
    Theme: 'a + number_input::ExtendedCatalog,
{
    fn from(num_input: NumberInput<'a, T, Message, Theme, Renderer>) -> Self {
        Element::new(num_input)
    }
}

fn sorted_range<T: PartialOrd>(a: T, b: T) -> std::ops::Range<T> {
    if a >= b { b..a } else { a..b }
}

#[cfg(test)]
mod tests {
    use super::*;
    use iced_widget::Renderer;

    #[derive(Clone, Debug)]
    #[allow(dead_code)]
    enum TestMessage {
        Changed(u32),
        Submit,
    }

    type TestNumberInput<'a> = NumberInput<'a, u32, TestMessage, iced_widget::Theme, Renderer>;

    #[test]
    fn number_input_new_creates_instance() {
        let value = 10u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed);

        assert_eq!(input.value, 10);
        assert_eq!(input.step, 1);
        assert!(matches!(input.min, Bound::Included(0)));
        assert!(matches!(input.max, Bound::Included(100)));
        assert!(!input.ignore_scroll_events);
        assert!(!input.ignore_buttons);
    }

    #[test]
    fn number_input_with_step() {
        let value = 10u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed).step(5);

        assert_eq!(input.step, 5);
    }

    #[test]
    fn number_input_ignore_buttons() {
        let value = 10u32;
        let input =
            TestNumberInput::new(&value, 0..=100, TestMessage::Changed).ignore_buttons(true);

        assert!(input.ignore_buttons);
    }

    #[test]
    fn number_input_ignore_scroll() {
        let value = 10u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed).ignore_scroll(true);

        assert!(input.ignore_scroll_events);
    }

    #[test]
    fn number_input_bounds() {
        let value = 50u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed).bounds(10..=90);

        assert!(matches!(input.min, Bound::Included(10)));
        assert!(matches!(input.max, Bound::Included(90)));
    }

    #[test]
    fn number_input_can_increase() {
        let value = 50u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed).step(10);

        assert!(input.can_increase());
    }

    #[test]
    fn number_input_cannot_increase_at_max() {
        let value = 100u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed);

        assert!(!input.can_increase());
    }

    #[test]
    fn number_input_can_decrease() {
        let value = 50u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed).step(10);

        assert!(input.can_decrease());
    }

    #[test]
    fn number_input_cannot_decrease_at_min() {
        let value = 0u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed);

        assert!(!input.can_decrease());
    }

    #[test]
    fn number_input_valid_value() {
        let value = 50u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed);

        assert!(input.valid(&50));
        assert!(input.valid(&0));
        assert!(input.valid(&100));
        assert!(!input.valid(&150));
    }

    #[test]
    fn number_input_min_max_values() {
        let value = 50u32;
        let input = TestNumberInput::new(&value, 10..=90, TestMessage::Changed);

        assert_eq!(input.min(), 10);
        assert_eq!(input.max(), 90);
    }

    #[test]
    fn number_input_min_max_with_excluded_bounds() {
        let value = 50u32;
        let input = TestNumberInput::new(&value, 10..90, TestMessage::Changed).step(1);

        assert_eq!(input.min(), 10); 
        assert_eq!(input.max(), 89); 
    }

    #[test]
    fn number_input_disabled_when_bounds_too_tight() {
        let value = 50u32;
        let input = TestNumberInput::new(&value, 50..=50, TestMessage::Changed);
        assert!(input.disabled());

        let input = TestNumberInput::new(&value, 49..=50, TestMessage::Changed);
        assert!(!input.disabled());
    }

    #[test]
    fn number_input_tag_returns_modifier_state_tag() {
        let value = 10u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed);

        let tag = Widget::<TestMessage, iced_widget::Theme, Renderer>::tag(&input);
        assert_eq!(tag, Tag::of::<ModifierState>());
    }

    #[test]
    fn number_input_has_one_child() {
        let value = 10u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed);

        let children = Widget::<TestMessage, iced_widget::Theme, Renderer>::children(&input);
        assert_eq!(children.len(), 1); 
    }

    #[test]
    fn number_input_different_values() {
        let test_values = [(0, 0..=100), (50, 0..=100), (100, 0..=100), (25, 10..=50)];

        for (value, range) in test_values {
            let input = TestNumberInput::new(&value, range, TestMessage::Changed);
            assert_eq!(input.value, value);
        }
    }

    #[test]
    fn modifier_state_defaults() {
        let state = ModifierState::default();

        assert!(!state.decrease_pressed);
        assert!(!state.increase_pressed);
    }

    #[test]
    fn number_input_with_on_submit() {
        let value = 10u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed)
            .on_submit(TestMessage::Submit);

        assert!(input.on_submit.is_some());
    }

    #[test]
    fn number_input_padding() {
        let value = 10u32;
        let custom_padding = Padding::new(10.0);
        let input =
            TestNumberInput::new(&value, 0..=100, TestMessage::Changed).padding(custom_padding);

        assert_eq!(input.padding, custom_padding);
    }

    #[test]
    fn number_input_size() {
        let value = 10u32;
        let input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed).set_size(20.0);

        assert_eq!(input.size, Some(iced_core::Pixels(20.0)));
    }

    #[test]
    fn number_input_width() {
        let value = 10u32;
        let _input = TestNumberInput::new(&value, 0..=100, TestMessage::Changed).width(200);

    }

    #[test]
    fn sorted_range_ascending() {
        let range = sorted_range(1, 10);
        assert_eq!(range.start, 1);
        assert_eq!(range.end, 10);
    }

    #[test]
    fn sorted_range_descending() {
        let range = sorted_range(10, 1);
        assert_eq!(range.start, 1);
        assert_eq!(range.end, 10);
    }

    #[test]
    fn sorted_range_equal() {
        let range = sorted_range(5, 5);
        assert_eq!(range.start, 5);
        assert_eq!(range.end, 5);
    }
}
