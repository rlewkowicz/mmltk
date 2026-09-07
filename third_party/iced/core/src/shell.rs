use crate::clipboard;
use crate::event;
use crate::window;
use crate::{Clipboard, InputMethod, Window};

use std::sync::Arc;

#[derive(Debug)]
pub struct Shell<'a, Message> {
    window: &'a dyn Window,
    messages: &'a mut Vec<Message>,
    waker: Waker,
    event_status: event::Status,
    redraw_request: window::RedrawRequest,
    input_method: InputMethod,
    is_layout_invalid: Option<Diff>,
    are_widgets_invalid: bool,
    clipboard: Clipboard,
}

impl<'a, Message> Shell<'a, Message> {
        pub fn new(window: &'a dyn Window, waker: Waker, messages: &'a mut Vec<Message>) -> Self {
        Self {
            window,
            messages,
            waker,
            event_status: event::Status::Ignored,
            redraw_request: window::RedrawRequest::Wait,
            is_layout_invalid: None,
            are_widgets_invalid: false,
            input_method: InputMethod::Disabled,
            clipboard: Clipboard {
                reads: Vec::new(),
                write: None,
            },
        }
    }

        pub fn local<'b, A>(&self, messages: &'b mut Vec<A>) -> Shell<'b, A>
    where
        'a: 'b,
    {
        Shell::new(self.window, self.waker.clone(), messages)
    }

        pub fn window(&self) -> &'a dyn Window {
        self.window
    }

        pub fn waker(&self) -> &Waker {
        &self.waker
    }

        #[must_use]
    pub fn is_empty(&self) -> bool {
        self.messages.is_empty()
    }

        pub fn publish(&mut self, message: Message) {
        self.messages.push(message);
    }

                    pub fn capture_event(&mut self) {
        self.event_status = event::Status::Captured;
    }

        #[must_use]
    pub fn event_status(&self) -> event::Status {
        self.event_status
    }

        #[must_use]
    pub fn is_event_captured(&self) -> bool {
        self.event_status == event::Status::Captured
    }

        pub fn request_redraw(&mut self) {
        self.redraw_request = window::RedrawRequest::NextFrame;
    }

        pub fn request_redraw_at(&mut self, redraw_request: impl Into<window::RedrawRequest>) {
        self.redraw_request = self.redraw_request.min(redraw_request.into());
    }

        #[must_use]
    pub fn redraw_request(&self) -> window::RedrawRequest {
        self.redraw_request
    }

                        pub fn replace_redraw_request(shell: &mut Self, redraw_request: window::RedrawRequest) {
        shell.redraw_request = redraw_request;
    }

                pub fn read_clipboard(&mut self, kind: clipboard::Kind) {
        self.clipboard
            .reads
            .push(clipboard::ReadRequest { kind, target: None });
    }

        pub fn read_clipboard_for(&mut self, kind: clipboard::Kind, target: impl Into<String>) {
        self.clipboard.reads.push(clipboard::ReadRequest {
            kind,
            target: Some(target.into()),
        });
    }

                pub fn write_clipboard(&mut self, content: clipboard::Content) {
        self.clipboard.write = Some(clipboard::WriteRequest {
            content,
            target: None,
        });
    }

        pub fn write_clipboard_for(&mut self, content: clipboard::Content, target: impl Into<String>) {
        self.clipboard.write = Some(clipboard::WriteRequest {
            content,
            target: Some(target.into()),
        });
    }

        pub fn clipboard_mut(&mut self) -> &mut Clipboard {
        &mut self.clipboard
    }

                    pub fn request_input_method<T: AsRef<str>>(&mut self, ime: &InputMethod<T>) {
        self.input_method.merge(ime);
    }

        #[must_use]
    pub fn input_method(&self) -> &InputMethod {
        &self.input_method
    }

        #[must_use]
    pub fn input_method_mut(&mut self) -> &mut InputMethod {
        &mut self.input_method
    }

        #[must_use]
    pub fn is_layout_invalid(&self) -> Option<Diff> {
        self.is_layout_invalid
    }

                pub fn invalidate_layout(&mut self) {
        self.invalidate_layout_with(Diff::Skip);
    }

        pub fn invalidate_layout_with(&mut self, diff: Diff) {
        self.is_layout_invalid = Some(diff);
    }

            pub fn revalidate_layout(&mut self, f: impl FnOnce(Diff)) {
        if let Some(diff) = self.is_layout_invalid.take() {
            f(diff);
        }
    }

            #[must_use]
    pub fn are_widgets_invalid(&self) -> bool {
        self.are_widgets_invalid
    }

                pub fn invalidate_widgets(&mut self) {
        self.are_widgets_invalid = true;
    }

                    pub fn merge<B>(&mut self, other: Shell<'_, B>, mut f: impl FnMut(B) -> Message) {
        self.merge_maybe(other, |message| Some(f(message)));
    }

                            pub fn merge_maybe<B>(&mut self, mut other: Shell<'_, B>, f: impl FnMut(B) -> Option<Message>) {
        self.messages.extend(other.messages.drain(..).filter_map(f));

        self.is_layout_invalid = match (self.is_layout_invalid, other.is_layout_invalid) {
            (Some(a), Some(b)) => Some(a.max(b)),
            _ => self.is_layout_invalid.or(other.is_layout_invalid),
        };

        self.are_widgets_invalid = self.are_widgets_invalid || other.are_widgets_invalid;
        self.redraw_request = self.redraw_request.min(other.redraw_request);
        self.event_status = self.event_status.merge(other.event_status);

        self.input_method.merge(&other.input_method);
        self.clipboard.merge(&mut other.clipboard);
    }
}

#[derive(Clone)]
pub struct Waker {
    wake: Arc<dyn Fn() + Send + Sync + 'static>,
}

impl Waker {
        pub fn new(wake: impl Fn() + Send + Sync + 'static) -> Self {
        Self {
            wake: Arc::new(wake),
        }
    }

        pub fn noop() -> Self {
        Self::new(|| {})
    }

                pub fn wake(&self) {
        (self.wake)();
    }
}

impl std::fmt::Debug for Waker {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Waker").finish()
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Diff {
        Skip,
        Perform,
}
