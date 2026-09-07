use crate::color;
use crate::core::{Rectangle, Transformation};
use crate::gradient;

use bytemuck::{Pod, Zeroable};

use std::sync::Arc;
use std::sync::atomic::{self, AtomicU64};

#[derive(Debug, Clone, PartialEq)]
pub enum Mesh {
        Solid {
                buffers: Indexed<SolidVertex2D>,

                transformation: Transformation,

                clip_bounds: Rectangle,
    },
        Gradient {
                buffers: Indexed<GradientVertex2D>,

                transformation: Transformation,

                clip_bounds: Rectangle,
    },
}

impl Mesh {
        pub fn indices(&self) -> &[u32] {
        match self {
            Self::Solid { buffers, .. } => &buffers.indices,
            Self::Gradient { buffers, .. } => &buffers.indices,
        }
    }

        pub fn transformation(&self) -> Transformation {
        match self {
            Self::Solid { transformation, .. } | Self::Gradient { transformation, .. } => {
                *transformation
            }
        }
    }

        pub fn clip_bounds(&self) -> Rectangle {
        match self {
            Self::Solid {
                clip_bounds,
                transformation,
                ..
            }
            | Self::Gradient {
                clip_bounds,
                transformation,
                ..
            } => *clip_bounds * *transformation,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Indexed<T> {
        pub vertices: Vec<T>,

                pub indices: Vec<u32>,
}

#[derive(Copy, Clone, Debug, PartialEq, Zeroable, Pod)]
#[repr(C)]
pub struct SolidVertex2D {
        pub position: [f32; 2],

        pub color: color::Packed,
}

#[derive(Copy, Clone, Debug, PartialEq, Zeroable, Pod)]
#[repr(C)]
pub struct GradientVertex2D {
        pub position: [f32; 2],

        pub gradient: gradient::Packed,
}

#[derive(Debug, Clone, Copy, Default)]
pub struct AttributeCount {
        pub solid_vertices: usize,

        pub solids: usize,

        pub gradient_vertices: usize,

        pub gradients: usize,

        pub indices: usize,
}

pub fn attribute_count_of(meshes: &[Mesh]) -> AttributeCount {
    meshes
        .iter()
        .fold(AttributeCount::default(), |mut count, mesh| {
            match mesh {
                Mesh::Solid { buffers, .. } => {
                    count.solids += 1;
                    count.solid_vertices += buffers.vertices.len();
                    count.indices += buffers.indices.len();
                }
                Mesh::Gradient { buffers, .. } => {
                    count.gradients += 1;
                    count.gradient_vertices += buffers.vertices.len();
                    count.indices += buffers.indices.len();
                }
            }

            count
        })
}

#[derive(Debug, Clone)]
pub struct Cache {
    id: Id,
    batch: Arc<[Mesh]>,
    version: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Id(u64);

impl Cache {
        pub fn new(meshes: Arc<[Mesh]>) -> Self {
        static NEXT_ID: AtomicU64 = AtomicU64::new(0);

        Self {
            id: Id(NEXT_ID.fetch_add(1, atomic::Ordering::Relaxed)),
            batch: meshes,
            version: 0,
        }
    }

        pub fn id(&self) -> Id {
        self.id
    }

        pub fn version(&self) -> usize {
        self.version
    }

        pub fn batch(&self) -> &Arc<[Mesh]> {
        &self.batch
    }

        pub fn is_empty(&self) -> bool {
        self.batch.is_empty()
    }

        pub fn update(&mut self, meshes: Arc<[Mesh]>) {
        self.batch = meshes;
        self.version += 1;
    }
}

pub trait Renderer {
        fn draw_mesh(&mut self, mesh: Mesh);

        fn draw_mesh_cache(&mut self, cache: Cache);
}
