use crate::{Color, Radians, Rectangle, Size};

use rustc_hash::FxHasher;
use std::borrow::Cow;
use std::hash::{Hash, Hasher as _};
use std::path::PathBuf;
use std::sync::Arc;

#[derive(Debug, Clone, PartialEq)]
pub struct Svg<H = Handle> {
        pub handle: H,

                                pub color: Option<Color>,

        pub rotation: Radians,

                pub opacity: f32,
}

impl Svg<Handle> {
        pub fn new(handle: impl Into<Handle>) -> Self {
        Self {
            handle: handle.into(),
            color: None,
            rotation: Radians(0.0),
            opacity: 1.0,
        }
    }

        pub fn color(mut self, color: impl Into<Color>) -> Self {
        self.color = Some(color.into());
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

impl From<&Handle> for Svg {
    fn from(handle: &Handle) -> Self {
        Svg::new(handle.clone())
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Handle {
    id: u64,
    data: Arc<Data>,
}

impl Handle {
            pub fn from_path(path: impl Into<PathBuf>) -> Handle {
        Self::from_data(Data::Path(path.into()))
    }

                        pub fn from_memory(bytes: impl Into<Cow<'static, [u8]>>) -> Handle {
        Self::from_data(Data::Bytes(bytes.into()))
    }

    fn from_data(data: Data) -> Handle {
        let mut hasher = FxHasher::default();
        data.hash(&mut hasher);

        Handle {
            id: hasher.finish(),
            data: Arc::new(data),
        }
    }

        pub fn id(&self) -> u64 {
        self.id
    }

        pub fn data(&self) -> &Data {
        &self.data
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

impl Hash for Handle {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        self.id.hash(state);
    }
}

#[derive(Clone, Hash, PartialEq, Eq)]
pub enum Data {
        Path(PathBuf),

                Bytes(Cow<'static, [u8]>),
}

impl std::fmt::Debug for Data {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Data::Path(path) => write!(f, "Path({path:?})"),
            Data::Bytes(_) => write!(f, "Bytes(...)"),
        }
    }
}

pub trait Renderer: crate::Renderer {
        fn measure_svg(&self, handle: &Handle) -> Size<u32>;

        fn draw_svg(&mut self, svg: Svg, bounds: Rectangle, clip_bounds: Rectangle);
}
