use std::path::PathBuf;
use std::sync::Arc;

#[derive(Debug, Clone)]
pub struct Clipboard {
        pub reads: Vec<ReadRequest>,
            pub write: Option<WriteRequest>,
}

impl Clipboard {
        pub fn new() -> Self {
        Self {
            reads: Vec::new(),
            write: None,
        }
    }

        pub fn merge(&mut self, other: &mut Self) {
        self.reads.append(&mut other.reads);
        self.write = other.write.take().or(self.write.take());
    }
}

impl Default for Clipboard {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Event {
        Read {
                target: Option<String>,
                result: Result<Arc<Content>, Error>,
    },

        Written {
                target: Option<String>,
                byte_count: usize,
                result: Result<(), Error>,
    },
}

#[derive(Debug, Clone)]
pub struct ReadRequest {
        pub kind: Kind,
        pub target: Option<String>,
}

#[derive(Debug, Clone)]
pub struct WriteRequest {
        pub content: Content,
        pub target: Option<String>,
}

#[derive(Debug, Clone, PartialEq)]
#[allow(missing_docs)]
#[non_exhaustive]
pub enum Content {
    Text(String),
    Html(String),
    #[cfg(feature = "image")]
    Image(Image),
    Files(Vec<PathBuf>),
}

impl From<String> for Content {
    fn from(text: String) -> Self {
        Self::Text(text)
    }
}

#[cfg(feature = "image")]
impl From<Image> for Content {
    fn from(image: Image) -> Self {
        Self::Image(image)
    }
}

impl From<Vec<PathBuf>> for Content {
    fn from(files: Vec<PathBuf>) -> Self {
        Self::Files(files)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[allow(missing_docs)]
#[non_exhaustive]
pub enum Kind {
    Text,
    Html,
    #[cfg(feature = "image")]
    Image,
    Files,
}

#[cfg(feature = "image")]
#[derive(Debug, Clone, PartialEq)]
pub struct Image {
        pub rgba: crate::Bytes,

        pub size: crate::Size<u32>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Error {
        ClipboardUnavailable,

        ClipboardOccupied,

                ContentNotAvailable,

            ConversionFailure,

        Unknown {
                        description: Arc<String>,
    },
}
