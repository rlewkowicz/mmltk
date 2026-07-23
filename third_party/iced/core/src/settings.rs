use crate::backend;
use crate::renderer;
use crate::{Backend, Font, Pixels};

use std::borrow::Cow;

#[derive(Debug, Clone)]
pub struct Settings {
                    pub id: Option<String>,

        pub fonts: Vec<Cow<'static, [u8]>>,

                pub default_font: Font,

                pub default_text_size: Pixels,

                pub backend: Backend,

                pub power_preference: backend::PowerPreference,

                                pub antialiasing: bool,

                        pub vsync: bool,
}

impl Default for Settings {
    fn default() -> Self {
        let renderer = renderer::Settings::default();

        Self {
            id: None,
            fonts: Vec::new(),
            default_font: renderer.default_font,
            default_text_size: renderer.default_text_size,
            backend: Backend::default(),
            power_preference: backend::PowerPreference::None,
            antialiasing: true,
            vsync: true,
        }
    }
}

impl From<&Settings> for renderer::Settings {
    fn from(settings: &Settings) -> Self {
        Self {
            default_font: settings.default_font,
            default_text_size: settings.default_text_size,
        }
    }
}
