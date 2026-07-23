use crate::core::Pixels;
use crate::core::font::{Error, Family, Font};
use crate::futures::futures::channel::oneshot;
use crate::task::{self, Task};

use std::borrow::Cow;
use std::fmt;

pub enum Action {
        Load {
                bytes: Cow<'static, [u8]>,
                channel: oneshot::Sender<Result<(), Error>>,
    },

        List {
                channel: oneshot::Sender<Result<Vec<Family>, Error>>,
    },

        SetDefaults {
                font: Font,
                text_size: Pixels,
    },
}

impl fmt::Debug for Action {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Load { .. } => f.write_str("Load"),
            Self::List { .. } => f.write_str("List"),
            Self::SetDefaults { font, text_size } => f
                .debug_struct("SetDefaults")
                .field("font", font)
                .field("text_size", text_size)
                .finish(),
        }
    }
}

pub fn load(bytes: impl Into<Cow<'static, [u8]>>) -> Task<Result<(), Error>> {
    task::oneshot(|channel| {
        crate::Action::Font(Action::Load {
            bytes: bytes.into(),
            channel,
        })
    })
}

pub fn list() -> Task<Result<Vec<Family>, Error>> {
    task::oneshot(|channel| crate::Action::Font(Action::List { channel }))
}

pub fn set_defaults<Message>(font: Font, text_size: impl Into<Pixels>) -> Task<Message> {
    task::effect(crate::Action::Font(Action::SetDefaults {
        font,
        text_size: text_size.into(),
    }))
}
