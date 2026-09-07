use crate::{Pixels, Rectangle};

use std::ops::Range;

#[derive(Debug, Clone, PartialEq)]
pub enum InputMethod<T = String> {
        Disabled,
        Enabled {
                                cursor: Rectangle,
                purpose: Purpose,
                                                preedit: Option<Preedit<T>>,
    },
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct Preedit<T = String> {
        pub content: T,
        pub selection: Option<Range<usize>>,
        pub text_size: Option<Pixels>,
}

impl<T> Preedit<T> {
        pub fn new() -> Self
    where
        T: Default,
    {
        Self::default()
    }

        pub fn to_owned(&self) -> Preedit
    where
        T: AsRef<str>,
    {
        Preedit {
            content: self.content.as_ref().to_owned(),
            selection: self.selection.clone(),
            text_size: self.text_size,
        }
    }
}

impl Preedit {
        pub fn as_ref(&self) -> Preedit<&str> {
        Preedit {
            content: &self.content,
            selection: self.selection.clone(),
            text_size: self.text_size,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Purpose {
        #[default]
    Normal,
        Secure,
                Terminal,
}

impl InputMethod {
                                                                                                        pub fn merge<T: AsRef<str>>(&mut self, other: &InputMethod<T>) {
        if let InputMethod::Enabled { .. } = self {
            return;
        }

        *self = other.to_owned();
    }

        pub fn is_enabled(&self) -> bool {
        matches!(self, Self::Enabled { .. })
    }
}

impl<T> InputMethod<T> {
        pub fn to_owned(&self) -> InputMethod
    where
        T: AsRef<str>,
    {
        match self {
            Self::Disabled => InputMethod::Disabled,
            Self::Enabled {
                cursor,
                purpose,
                preedit,
            } => InputMethod::Enabled {
                cursor: *cursor,
                purpose: *purpose,
                preedit: preedit.as_ref().map(Preedit::to_owned),
            },
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum Event {
                                Opened,

                                Preedit(String, Option<Range<usize>>),

                Commit(String),

                                    Closed,
}
