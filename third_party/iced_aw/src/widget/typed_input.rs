use iced_core::layout::{Layout, Limits, Node};
use iced_core::mouse::{self, Cursor};
use iced_core::widget::{
    Operation, Tree, Widget,
    tree::{State, Tag},
};
use iced_core::{Element, Length, Padding, Pixels, Rectangle};
use iced_core::{Event, Size};
use iced_core::{Shell, widget};
use iced_widget::text_input::{self, TextInput};

use std::{fmt::Display, str::FromStr};

const DEFAULT_PADDING: Padding = Padding::new(5.0);

pub struct TypedInput<'a, T, Message, Theme = iced_widget::Theme, Renderer = iced_widget::Renderer>
where
    Renderer: iced_core::text::Renderer,
    Theme: text_input::Catalog,
{
        value: T,
        text_input: text_input::TextInput<'a, InternalMessage, Theme, Renderer>,
    text: String,
        on_change: Option<Box<dyn 'a + Fn(T) -> Message>>,
        #[allow(clippy::type_complexity)]
    on_submit: Option<Box<dyn 'a + Fn(Result<T, String>) -> Message>>,
        on_paste: Option<Box<dyn 'a + Fn(T) -> Message>>,
}

#[derive(Debug, Clone, PartialEq)]
#[allow(clippy::enum_variant_names)]
enum InternalMessage {
    OnChange(String),
    OnSubmit,
    OnPaste(String),
}

impl<'a, T, Message, Theme, Renderer> TypedInput<'a, T, Message, Theme, Renderer>
where
    T: Display + FromStr,
    Message: Clone,
    Renderer: iced_core::text::Renderer,
    Theme: text_input::Catalog,
{
                        #[must_use]
    pub fn new(placeholder: &str, value: &T) -> Self
    where
        T: 'a + Clone,
    {
        let padding = DEFAULT_PADDING;

        Self {
            value: value.clone(),
            text_input: text_input::TextInput::new(placeholder, format!("{value}").as_str())
                .padding(padding)
                .width(Length::Fixed(127.0))
                .class(<Theme as text_input::Catalog>::default()),
            text: value.to_string(),
            on_change: None,
            on_submit: None,
            on_paste: None,
        }
    }

        #[must_use]
    pub fn id(mut self, id: impl Into<widget::Id>) -> Self {
        self.text_input = self.text_input.id(id);
        self
    }

        #[must_use]
    pub fn secure(mut self, is_secure: bool) -> Self {
        self.text_input = self.text_input.secure(is_secure);
        self
    }

                #[must_use]
    pub fn on_input<F>(mut self, callback: F) -> Self
    where
        F: 'a + Fn(T) -> Message,
    {
        self.text_input = self.text_input.on_input(InternalMessage::OnChange);
        self.on_change = Some(Box::new(callback));
        self
    }

                #[must_use]
    pub fn on_input_maybe<F>(mut self, callback: Option<F>) -> Self
    where
        F: 'a + Fn(T) -> Message,
    {
        if let Some(callback) = callback {
            self.text_input = self.text_input.on_input(InternalMessage::OnChange);
            self.on_change = Some(Box::new(callback));
        } else {
            if self.on_submit.is_none() {
                #[allow(unused_assignments)]
                let mut f = Some(InternalMessage::OnChange);
                f = None;
                self.text_input = self.text_input.on_input_maybe(f);
            }
            self.on_change = None;
        }
        self
    }

                    #[must_use]
    pub fn on_submit<F>(mut self, callback: F) -> Self
    where
        F: 'a + Fn(Result<T, String>) -> Message,
    {
        self.text_input = self
            .text_input
            .on_input(InternalMessage::OnChange)
            .on_submit(InternalMessage::OnSubmit);
        self.on_submit = Some(Box::new(callback));
        self
    }

                    #[must_use]
    pub fn on_submit_maybe<F>(mut self, callback: Option<F>) -> Self
    where
        F: 'a + Fn(Result<T, String>) -> Message,
    {
        if let Some(callback) = callback {
            self.text_input = self
                .text_input
                .on_input(InternalMessage::OnChange)
                .on_submit(InternalMessage::OnSubmit);
            self.on_submit = Some(Box::new(callback));
        } else {
            if self.on_change.is_none() {
                #[allow(unused_assignments)]
                let mut f = Some(InternalMessage::OnChange);
                f = None;
                self.text_input = self.text_input.on_input_maybe(f);
            }
            #[allow(unused_assignments)]
            let mut f = Some(InternalMessage::OnSubmit);
            f = None;
            self.text_input = self.text_input.on_submit_maybe(f);
            self.on_change = None;
        }
        self
    }

        #[must_use]
    pub fn on_paste<F>(mut self, callback: F) -> Self
    where
        F: 'a + Fn(T) -> Message,
    {
        self.text_input = self.text_input.on_paste(InternalMessage::OnPaste);
        self.on_paste = Some(Box::new(callback));
        self
    }

        #[must_use]
    pub fn on_paste_maybe<F>(mut self, callback: Option<F>) -> Self
    where
        F: 'a + Fn(T) -> Message,
    {
        if let Some(callback) = callback {
            self.text_input = self.text_input.on_paste(InternalMessage::OnPaste);
            self.on_paste = Some(Box::new(callback));
        } else {
            #[allow(unused_assignments)]
            let mut f = Some(InternalMessage::OnPaste);
            f = None;
            self.text_input = self.text_input.on_paste_maybe(f);
            self.on_paste = None;
        }
        self
    }

        #[must_use]
    pub fn font(mut self, font: Renderer::Font) -> Self {
        self.text_input = self.text_input.font(font);
        self
    }

        #[must_use]
    pub fn icon(mut self, icon: iced_widget::text_input::Icon<Renderer::Font>) -> Self {
        self.text_input = self.text_input.icon(icon);
        self
    }

        #[must_use]
    pub fn width(mut self, width: impl Into<Length>) -> Self {
        self.text_input = self.text_input.width(width);
        self
    }

        #[must_use]
    pub fn padding(mut self, padding: impl Into<Padding>) -> Self {
        self.text_input = self.text_input.padding(padding);
        self
    }

        #[must_use]
    pub fn size(mut self, size: impl Into<Pixels>) -> Self {
        self.text_input = self.text_input.size(size);
        self
    }

        #[must_use]
    pub fn line_height(mut self, line_height: impl Into<iced_widget::text::LineHeight>) -> Self {
        self.text_input = self.text_input.line_height(line_height);
        self
    }

        #[must_use]
    pub fn align_x(mut self, alignment: impl Into<iced_core::alignment::Horizontal>) -> Self {
        self.text_input = self.text_input.align_x(alignment);
        self
    }

        #[must_use]
    pub fn style(
        mut self,
        style: impl Fn(&Theme, text_input::Status) -> text_input::Style + 'a,
    ) -> Self
    where
        <Theme as text_input::Catalog>::Class<'a>: From<text_input::StyleFn<'a, Theme>>,
    {
        self.text_input = self.text_input.style(style);
        self
    }

        #[must_use]
    pub fn class(mut self, class: impl Into<<Theme as text_input::Catalog>::Class<'a>>) -> Self {
        self.text_input = self.text_input.class(class);
        self
    }

        pub fn text(&self) -> &str {
        &self.text
    }
}

impl<'a, T, Message, Theme, Renderer> Widget<Message, Theme, Renderer>
    for TypedInput<'a, T, Message, Theme, Renderer>
where
    T: Display + FromStr + Clone + PartialEq,
    Message: 'a + Clone,
    Renderer: 'a + iced_core::text::Renderer,
    Theme: text_input::Catalog,
{
    fn tag(&self) -> Tag {
        <TextInput<_, _, _> as Widget<_, _, _>>::tag(&self.text_input)
    }
    fn state(&self) -> State {
        <TextInput<_, _, _> as Widget<_, _, _>>::state(&self.text_input)
    }

    fn diff(&mut self, state: &mut Tree) {
        <TextInput<_, _, _> as Widget<_, _, _>>::diff(&mut self.text_input, state);
    }

    fn size(&self) -> Size<Length> {
        <TextInput<_, _, _> as Widget<_, _, _>>::size(&self.text_input)
    }

    fn layout(&mut self, state: &mut Tree, renderer: &Renderer, limits: &Limits) -> Node {
        <TextInput<_, _, _> as Widget<_, _, _>>::layout(
            &mut self.text_input,
            state,
            renderer,
            limits,
        )
    }

    fn draw(
        &self,
        state: &Tree,
        renderer: &mut Renderer,
        theme: &Theme,
        style: &iced_core::renderer::Style,
        layout: Layout<'_>,
        cursor: Cursor,
        viewport: &Rectangle,
    ) {
        <TextInput<_, _, _> as Widget<_, _, _>>::draw(
            &self.text_input,
            state,
            renderer,
            theme,
            style,
            layout,
            cursor,
            viewport,
        );
    }

    fn mouse_interaction(
        &self,
        state: &Tree,
        layout: Layout<'_>,
        cursor: Cursor,
        viewport: &Rectangle,
        renderer: &Renderer,
    ) -> mouse::Interaction {
        <TextInput<_, _, _> as Widget<_, _, _>>::mouse_interaction(
            &self.text_input,
            state,
            layout,
            cursor,
            viewport,
            renderer,
        )
    }

    fn operate(
        &mut self,
        state: &mut Tree,
        layout: Layout<'_>,
        renderer: &Renderer,
        operation: &mut dyn Operation,
    ) {
        <TextInput<_, _, _> as Widget<_, _, _>>::operate(
            &mut self.text_input,
            state,
            layout,
            renderer,
            operation,
        );
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
        let mut messages = Vec::new();
        let mut sub_shell = shell.local(&mut messages);
        self.text_input.update(
            state,
            event,
            layout,
            cursor,
            renderer,
            &mut sub_shell,
            viewport,
        );

        let mut invalidate_layout = false;
        shell.merge_maybe(sub_shell, |message| {
            invalidate_layout = true;
            match message {
                InternalMessage::OnChange(value) => {
                    self.text = value;

                    if let Ok(value) = T::from_str(&self.text)
                        && self.value != value
                    {
                        self.value = value.clone();
                        return self.on_change.as_ref().map(|on_change| on_change(value));
                    }
                }
                InternalMessage::OnSubmit => {
                    if let Some(on_submit) = &self.on_submit {
                        let value = match T::from_str(&self.text) {
                            Ok(v) => Ok(v),
                            Err(_) => Err(self.text.clone()),
                        };
                        return Some(on_submit(value));
                    }
                }
                InternalMessage::OnPaste(value) => {
                    self.text = value;

                    if let Ok(value) = T::from_str(&self.text)
                        && self.value != value
                    {
                        self.value = value.clone();
                        return self.on_paste.as_ref().map(|on_paste| on_paste(value));
                    }
                }
            }
            None
        });
        if invalidate_layout {
            shell.invalidate_layout();
        }
    }
}

impl<'a, T, Message, Theme, Renderer> From<TypedInput<'a, T, Message, Theme, Renderer>>
    for Element<'a, Message, Theme, Renderer>
where
    T: 'a + Display + FromStr + Clone + PartialEq,
    Message: 'a + Clone,
    Renderer: 'a + iced_core::text::Renderer,
    Theme: 'a + text_input::Catalog,
{
    fn from(typed_input: TypedInput<'a, T, Message, Theme, Renderer>) -> Self {
        Element::new(typed_input)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use iced_widget::Renderer;

    #[derive(Clone, Debug)]
    #[allow(dead_code)]
    enum TestMessage {
        Changed(u32),
        Submit(Result<u32, String>),
        Paste(u32),
    }

    type TestTypedInput<'a> = TypedInput<'a, u32, TestMessage, iced_widget::Theme, Renderer>;

    #[test]
    fn typed_input_new_creates_instance() {
        let value = 42u32;
        let input = TestTypedInput::new("Enter a number", &value);

        assert_eq!(input.value, 42);
        assert_eq!(input.text, "42");
        assert!(input.on_change.is_none());
        assert!(input.on_submit.is_none());
        assert!(input.on_paste.is_none());
    }

    #[test]
    fn typed_input_text_getter() {
        let value = 123u32;
        let input = TestTypedInput::new("Enter a number", &value);

        assert_eq!(input.text(), "123");
    }

    #[test]
    fn typed_input_with_on_input() {
        let value = 42u32;
        let input = TestTypedInput::new("Enter a number", &value).on_input(TestMessage::Changed);

        assert!(input.on_change.is_some());
    }

    #[test]
    fn typed_input_with_on_submit() {
        let value = 42u32;
        let input = TestTypedInput::new("Enter a number", &value).on_submit(TestMessage::Submit);

        assert!(input.on_submit.is_some());
    }

    #[test]
    fn typed_input_with_on_paste() {
        let value = 42u32;
        let input = TestTypedInput::new("Enter a number", &value).on_paste(TestMessage::Paste);

        assert!(input.on_paste.is_some());
    }

    #[test]
    fn typed_input_on_input_maybe_some() {
        let value = 42u32;
        let input = TestTypedInput::new("Enter a number", &value)
            .on_input_maybe(Some(TestMessage::Changed));

        assert!(input.on_change.is_some());
    }

    #[test]
    fn typed_input_on_input_maybe_none() {
        let value = 42u32;
        let input: TestTypedInput = TestTypedInput::new("Enter a number", &value)
            .on_input_maybe(None::<fn(u32) -> TestMessage>);

        assert!(input.on_change.is_none());
    }

    #[test]
    fn typed_input_on_submit_maybe_some() {
        let value = 42u32;
        let input = TestTypedInput::new("Enter a number", &value)
            .on_submit_maybe(Some(TestMessage::Submit));

        assert!(input.on_submit.is_some());
    }

    #[test]
    fn typed_input_on_submit_maybe_none() {
        let value = 42u32;
        let input: TestTypedInput = TestTypedInput::new("Enter a number", &value)
            .on_submit_maybe(None::<fn(Result<u32, String>) -> TestMessage>);

        assert!(input.on_submit.is_none());
    }

    #[test]
    fn typed_input_on_paste_maybe_some() {
        let value = 42u32;
        let input =
            TestTypedInput::new("Enter a number", &value).on_paste_maybe(Some(TestMessage::Paste));

        assert!(input.on_paste.is_some());
    }

    #[test]
    fn typed_input_on_paste_maybe_none() {
        let value = 42u32;
        let input: TestTypedInput = TestTypedInput::new("Enter a number", &value)
            .on_paste_maybe(None::<fn(u32) -> TestMessage>);

        assert!(input.on_paste.is_none());
    }

    #[test]
    fn typed_input_secure_mode() {
        let value = 1234u32;
        let _input = TestTypedInput::new("Enter PIN", &value).secure(true);
    }

    #[test]
    fn typed_input_with_different_types() {
        {
            #[derive(Clone, Debug)]
            #[allow(dead_code)]
            enum I32Message {
                Changed(i32),
            }
            let value = -42i32;
            let _input: TypedInput<'_, i32, I32Message, iced_widget::Theme, Renderer> =
                TypedInput::new("Enter number", &value).on_input(I32Message::Changed);
        }

        {
            #[derive(Clone, Debug)]
            #[allow(dead_code)]
            enum F64Message {
                Changed(f64),
            }
            let value = 2.5;
            let _input: TypedInput<'_, f64, F64Message, iced_widget::Theme, Renderer> =
                TypedInput::new("Enter number", &value).on_input(F64Message::Changed);
        }

        {
            #[derive(Clone, Debug)]
            #[allow(dead_code)]
            enum StringMessage {
                Changed(String),
            }
            let value = "hello".to_owned();
            let _input: TypedInput<'_, String, StringMessage, iced_widget::Theme, Renderer> =
                TypedInput::new("Enter text", &value).on_input(StringMessage::Changed);
        }
    }

    #[test]
    fn typed_input_tag_returns_text_input_tag() {
        let value = 42u32;
        let input = TestTypedInput::new("Enter a number", &value);

        let tag = Widget::<TestMessage, iced_widget::Theme, Renderer>::tag(&input);
        let text_input_tag = <TextInput<_, _, _> as Widget<_, _, _>>::tag(&input.text_input);
        assert_eq!(tag, text_input_tag);
    }

    #[test]
    fn typed_input_children_delegates_to_text_input() {
        let value = 42u32;
        let input = TestTypedInput::new("Enter a number", &value);

        let children = Widget::<TestMessage, iced_widget::Theme, Renderer>::children(&input);
        let text_input_children =
            <TextInput<_, _, _> as Widget<_, _, _>>::children(&input.text_input);
        assert_eq!(children.len(), text_input_children.len());
    }
}
