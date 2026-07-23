use crate::container;
use crate::core::{self, Element};

pub struct Controls<'a, Message, Theme = crate::Theme, Renderer = crate::Renderer>
where
    Theme: container::Catalog,
    Renderer: core::Renderer,
{
    pub(super) full: Element<'a, Message, Theme, Renderer>,
    pub(super) compact: Option<Element<'a, Message, Theme, Renderer>>,
}

impl<'a, Message, Theme, Renderer> Controls<'a, Message, Theme, Renderer>
where
    Theme: container::Catalog,
    Renderer: core::Renderer,
{
        pub fn new(content: impl Into<Element<'a, Message, Theme, Renderer>>) -> Self {
        Self {
            full: content.into(),
            compact: None,
        }
    }

                pub fn dynamic(
        full: impl Into<Element<'a, Message, Theme, Renderer>>,
        compact: impl Into<Element<'a, Message, Theme, Renderer>>,
    ) -> Self {
        Self {
            full: full.into(),
            compact: Some(compact.into()),
        }
    }
}

impl<'a, Message, Theme, Renderer> From<Element<'a, Message, Theme, Renderer>>
    for Controls<'a, Message, Theme, Renderer>
where
    Theme: container::Catalog,
    Renderer: core::Renderer,
{
    fn from(value: Element<'a, Message, Theme, Renderer>) -> Self {
        Self::new(value)
    }
}
