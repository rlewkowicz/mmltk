use crate::cache::{self, Cached};
use crate::core::{Rectangle, Size};
use crate::geometry::{self, Frame};

pub use cache::Group;

pub struct Cache<Renderer>
where
    Renderer: geometry::Renderer,
{
    raw: crate::Cache<Data<<Renderer::Geometry as Cached>::Cache>>,
}

#[derive(Debug, Clone)]
struct Data<T> {
    bounds: Rectangle,
    geometry: T,
}

impl<Renderer> Cache<Renderer>
where
    Renderer: geometry::Renderer,
{
        pub fn new() -> Self {
        Cache {
            raw: cache::Cache::new(),
        }
    }

                            pub fn with_group(group: Group) -> Self {
        Cache {
            raw: crate::Cache::with_group(group),
        }
    }

        pub fn clear(&self) {
        self.raw.clear();
    }

                                            pub fn draw(
        &self,
        renderer: &Renderer,
        size: Size,
        draw_fn: impl FnOnce(&mut Frame<Renderer>),
    ) -> Renderer::Geometry {
        self.draw_with_bounds(renderer, Rectangle::with_size(size), draw_fn)
    }

                        pub fn draw_with_bounds(
        &self,
        renderer: &Renderer,
        bounds: Rectangle,
        draw_fn: impl FnOnce(&mut Frame<Renderer>),
    ) -> Renderer::Geometry {
        use std::ops::Deref;

        let state = self.raw.state();

        let previous = match state.borrow().deref() {
            cache::State::Empty { previous } => previous.as_ref().map(|data| data.geometry.clone()),
            cache::State::Filled { current } => {
                if current.bounds == bounds {
                    return Cached::load(&current.geometry);
                }

                Some(current.geometry.clone())
            }
        };

        let mut frame = Frame::with_bounds(renderer, bounds);
        draw_fn(&mut frame);

        let geometry = frame.into_geometry().cache(self.raw.group(), previous);
        let result = Cached::load(&geometry);

        *state.borrow_mut() = cache::State::Filled {
            current: Data { bounds, geometry },
        };

        result
    }
}

impl<Renderer> std::fmt::Debug for Cache<Renderer>
where
    Renderer: geometry::Renderer,
    <Renderer::Geometry as Cached>::Cache: std::fmt::Debug,
{
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self.raw)
    }
}

impl<Renderer> Default for Cache<Renderer>
where
    Renderer: geometry::Renderer,
{
    fn default() -> Self {
        Self::new()
    }
}
