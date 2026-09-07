use crate::core::Color;
use crate::core::backend;
use crate::core::font;
use crate::core::renderer;
use crate::futures::{MaybeSend, MaybeSync};
use crate::{Shell, Viewport};

use raw_window_handle::{HasDisplayHandle, HasWindowHandle};
use thiserror::Error;

use std::borrow::Cow;
use std::fmt::Debug;

pub trait Compositor: Sized {
        type Renderer;

        type Surface;

        fn new(
        settings: backend::Settings,
        display: impl Display + Clone,
        compatible_window: impl Window + Clone,
        shell: Shell,
    ) -> impl Future<Output = Result<Self, backend::Error>>;

        fn create_renderer(&self, settings: renderer::Settings) -> Self::Renderer;

                fn create_surface(
        &mut self,
        window: impl Window + Clone,
        width: u32,
        height: u32,
    ) -> Self::Surface;

                fn configure_surface(&mut self, surface: &mut Self::Surface, width: u32, height: u32);

        fn information(&self) -> Information;

        fn load_font(&mut self, font: Cow<'static, [u8]>) -> Result<(), font::Error> {
        crate::text::font_system()
            .write()
            .expect("Write to font system")
            .load_font(font);

        Ok(())
    }

        fn list_fonts(&mut self) -> Result<Vec<font::Family>, font::Error> {
        use std::collections::BTreeSet;

        let font_system = crate::text::font_system()
            .read()
            .expect("Read from font system");

        let families = BTreeSet::from_iter(font_system.families());

        Ok(families.into_iter().map(font::Family::name).collect())
    }

                    fn present(
        &mut self,
        renderer: &mut Self::Renderer,
        surface: &mut Self::Surface,
        viewport: &Viewport,
        background_color: Color,
        on_pre_present: impl FnOnce(),
    ) -> Result<(), SurfaceError>;

                    fn screenshot(
        &mut self,
        renderer: &mut Self::Renderer,
        viewport: &Viewport,
        background_color: Color,
    ) -> Vec<u8>;
}

pub trait Window: HasWindowHandle + Debug + MaybeSend + MaybeSync + 'static {}

impl<T> Window for T where T: HasWindowHandle + Debug + MaybeSend + MaybeSync + 'static {}

pub trait Display: HasDisplayHandle + Debug + Send + Sync + 'static {}

impl<T> Display for T where T: HasDisplayHandle + Debug + Send + Sync + 'static {}

pub trait Default {
        type Compositor: Compositor<Renderer = Self>;
}

#[derive(Clone, PartialEq, Eq, Debug, Error)]
pub enum SurfaceError {
        #[error("A timeout was encountered while trying to acquire the next frame")]
    Timeout,
        #[error("The underlying surface has changed, and therefore the surface must be updated.")]
    Outdated,
        #[error("The surface has been lost and needs to be recreated")]
    Lost,
        #[error("There is no more memory left to allocate a new frame")]
    OutOfMemory,
        #[error("The surface is occluded and must not be drawn to")]
    Occluded,
        #[error("Acquiring a texture failed with a generic error")]
    Other,
}

#[derive(Debug)]
pub struct Information {
        pub adapter: String,
        pub backend: String,
}

#[cfg(debug_assertions)]
impl Compositor for () {
    type Renderer = ();
    type Surface = ();

    async fn new(
        _settings: backend::Settings,
        _display: impl Display,
        _compatible_window: impl Window + Clone,
        _shell: Shell,
    ) -> Result<Self, backend::Error> {
        Ok(())
    }

    fn create_renderer(&self, _settings: renderer::Settings) -> Self::Renderer {}

    fn create_surface(
        &mut self,
        _window: impl Window + Clone,
        _width: u32,
        _height: u32,
    ) -> Self::Surface {
    }

    fn configure_surface(&mut self, _surface: &mut Self::Surface, _width: u32, _height: u32) {}

    fn load_font(&mut self, _font: Cow<'static, [u8]>) -> Result<(), font::Error> {
        Ok(())
    }

    fn list_fonts(&mut self) -> Result<Vec<font::Family>, font::Error> {
        Ok(Vec::new())
    }

    fn information(&self) -> Information {
        Information {
            adapter: String::from("Null Renderer"),
            backend: String::from("Null"),
        }
    }

    fn present(
        &mut self,
        _renderer: &mut Self::Renderer,
        _surface: &mut Self::Surface,
        _viewport: &Viewport,
        _background_color: Color,
        _on_pre_present: impl FnOnce(),
    ) -> Result<(), SurfaceError> {
        Ok(())
    }

    fn screenshot(
        &mut self,
        _renderer: &mut Self::Renderer,
        _viewport: &Viewport,
        _background_color: Color,
    ) -> Vec<u8> {
        vec![]
    }
}

#[cfg(debug_assertions)]
impl Default for () {
    type Compositor = ();
}
