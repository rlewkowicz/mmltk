use crate::core::clipboard::{Content, Error, Kind};
use crate::futures::futures::channel::oneshot;
use crate::task::{self, Task};

use std::path::PathBuf;
use std::sync::Arc;

#[derive(Debug)]
pub enum Action {
        Read {
                kind: Kind,
                channel: oneshot::Sender<Result<Content, Error>>,
    },

        Write {
                content: Content,

                channel: oneshot::Sender<Result<(), Error>>,
    },
}

pub fn read(kind: Kind) -> Task<Result<Arc<Content>, Error>> {
    task::oneshot(|channel| crate::Action::Clipboard(Action::Read { kind, channel }))
        .map(|result| result.map(Arc::new))
}

pub fn read_text() -> Task<Result<Arc<String>, Error>> {
    task::oneshot(|channel| {
        crate::Action::Clipboard(Action::Read {
            kind: Kind::Text,
            channel,
        })
    })
    .map(|result| {
        let Ok(Content::Text(text)) = result else {
            return Err(Error::ContentNotAvailable);
        };

        Ok(Arc::new(text))
    })
}

pub fn read_html() -> Task<Result<Arc<String>, Error>> {
    task::oneshot(|channel| {
        crate::Action::Clipboard(Action::Read {
            kind: Kind::Html,
            channel,
        })
    })
    .map(|result| {
        let Ok(Content::Html(html)) = result else {
            return Err(Error::ContentNotAvailable);
        };

        Ok(Arc::new(html))
    })
}

pub fn read_files() -> Task<Result<Arc<[PathBuf]>, Error>> {
    task::oneshot(|channel| {
        crate::Action::Clipboard(Action::Read {
            kind: Kind::Files,
            channel,
        })
    })
    .map(|result| {
        let Ok(Content::Files(files)) = result else {
            return Err(Error::ContentNotAvailable);
        };

        Ok(Arc::from(files))
    })
}

#[cfg(feature = "image")]
pub fn read_image() -> Task<Result<crate::core::clipboard::Image, Error>> {
    task::oneshot(|channel| {
        crate::Action::Clipboard(Action::Read {
            kind: Kind::Image,
            channel,
        })
    })
    .map(|result| {
        let Ok(Content::Image(image)) = result else {
            return Err(Error::ContentNotAvailable);
        };

        Ok(image)
    })
}

pub fn write(content: impl Into<Content>) -> Task<Result<(), Error>> {
    let content = content.into();

    task::oneshot(|channel| crate::Action::Clipboard(Action::Write { content, channel }))
}
