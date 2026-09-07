use crate::border;
use crate::{Bytes, Radians, Rectangle, Size};

use rustc_hash::FxHasher;

use std::hash::{Hash, Hasher};
use std::io;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Weak};

#[derive(Debug, Clone, PartialEq)]
pub struct Image<H = Handle> {
        pub handle: H,

        pub filter_method: FilterMethod,

        pub rotation: Radians,

                pub border_radius: border::Radius,

                pub opacity: f32,
}

impl Image<Handle> {
        pub fn new(handle: impl Into<Handle>) -> Self {
        Self {
            handle: handle.into(),
            filter_method: FilterMethod::default(),
            rotation: Radians(0.0),
            border_radius: border::Radius::default(),
            opacity: 1.0,
        }
    }

        pub fn filter_method(mut self, filter_method: FilterMethod) -> Self {
        self.filter_method = filter_method;
        self
    }

        pub fn rotation(mut self, rotation: impl Into<Radians>) -> Self {
        self.rotation = rotation.into();
        self
    }

        pub fn opacity(mut self, opacity: impl Into<f32>) -> Self {
        self.opacity = opacity.into();
        self
    }
}

impl From<&Handle> for Image {
    fn from(handle: &Handle) -> Self {
        Image::new(handle.clone())
    }
}

#[derive(Clone, PartialEq, Eq)]
pub enum Handle {
                            Path(Id, PathBuf),

                        Bytes(Id, Bytes),

                        Rgba {
                id: Id,
                width: u32,
                height: u32,
                pixels: Bytes,
    },
}

impl Handle {
                pub fn from_path<T: Into<PathBuf>>(path: T) -> Handle {
        let path = path.into();

        Self::Path(Id::path(&path), path)
    }

                            pub fn from_bytes(bytes: impl Into<Bytes>) -> Handle {
        Self::Bytes(Id::unique(), bytes.into())
    }

                                pub fn from_rgba(width: u32, height: u32, pixels: impl Into<Bytes>) -> Handle {
        Self::Rgba {
            id: Id::unique(),
            width,
            height,
            pixels: pixels.into(),
        }
    }

        pub fn id(&self) -> Id {
        match self {
            Handle::Path(id, _) | Handle::Bytes(id, _) | Handle::Rgba { id, .. } => *id,
        }
    }
}

impl<T> From<T> for Handle
where
    T: Into<PathBuf>,
{
    fn from(path: T) -> Handle {
        Handle::from_path(path.into())
    }
}

impl From<&Handle> for Handle {
    fn from(value: &Handle) -> Self {
        value.clone()
    }
}

impl std::fmt::Debug for Handle {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Path(id, path) => write!(f, "Path({id:?}, {path:?})"),
            Self::Bytes(id, _) => write!(f, "Bytes({id:?}, ...)"),
            Self::Rgba {
                id, width, height, ..
            } => {
                write!(f, "Pixels({id:?}, {width} * {height})")
            }
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Id(_Id);

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
enum _Id {
    Unique(u64),
    Hash(u64),
}

impl Id {
    fn unique() -> Self {
        use std::sync::atomic::{self, AtomicU64};

        static NEXT_ID: AtomicU64 = AtomicU64::new(0);

        Self(_Id::Unique(NEXT_ID.fetch_add(1, atomic::Ordering::Relaxed)))
    }

    fn path(path: impl AsRef<Path>) -> Self {
        let hash = {
            let mut hasher = FxHasher::default();
            path.as_ref().hash(&mut hasher);

            hasher.finish()
        };

        Self(_Id::Hash(hash))
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum FilterMethod {
        #[default]
    Linear,
        Nearest,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Allocation(Arc<Memory>);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Memory {
    handle: Handle,
    size: Size<u32>,
}

impl Allocation {
        pub fn downgrade(&self) -> Weak<Memory> {
        Arc::downgrade(&self.0)
    }

        pub fn upgrade(weak: &Weak<Memory>) -> Option<Allocation> {
        Weak::upgrade(weak).map(Allocation)
    }

        pub fn handle(&self) -> &Handle {
        &self.0.handle
    }

        pub fn size(&self) -> Size<u32> {
        self.0.size
    }
}

#[allow(unsafe_code)]
pub unsafe fn allocate(handle: &Handle, size: Size<u32>) -> Allocation {
    Allocation(Arc::new(Memory {
        handle: handle.clone(),
        size,
    }))
}

pub trait Renderer: crate::Renderer {
                type Handle: Clone;

                    fn load_image(&self, handle: &Self::Handle) -> Result<Allocation, Error>;

                            fn measure_image(&self, handle: &Self::Handle) -> Option<Size<u32>>;

                                fn draw_image(&mut self, image: Image<Self::Handle>, bounds: Rectangle, clip_bounds: Rectangle);
}

#[derive(Debug, Clone, thiserror::Error)]
pub enum Error {
        #[error("the image data was invalid or could not be decoded: {0}")]
    Invalid(Arc<dyn std::error::Error + Send + Sync>),
        #[error("the image file could not be opened: {0}")]
    Inaccessible(Arc<io::Error>),
        #[error("loading images is unsupported")]
    Unsupported,
        #[error("the image is empty")]
    Empty,
        #[error("not enough memory to allocate the image")]
    OutOfMemory,
}

impl From<io::Error> for Error {
    fn from(error: io::Error) -> Self {
        Self::Inaccessible(Arc::new(error))
    }
}
