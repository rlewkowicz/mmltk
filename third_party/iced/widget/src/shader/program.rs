use crate::core::Rectangle;
use crate::core::mouse;
use crate::renderer::wgpu::Primitive;
use crate::shader::{self, Action};

pub trait Program<Message> {
        type State: Default + 'static;

        type Primitive: Primitive + 'static;

                                fn update(
        &self,
        _state: &mut Self::State,
        _event: &shader::Event,
        _bounds: Rectangle,
        _cursor: mouse::Cursor,
    ) -> Option<Action<Message>> {
        None
    }

                fn draw(
        &self,
        state: &Self::State,
        cursor: mouse::Cursor,
        bounds: Rectangle,
    ) -> Self::Primitive;

                            fn mouse_interaction(
        &self,
        _state: &Self::State,
        _bounds: Rectangle,
        _cursor: mouse::Cursor,
    ) -> mouse::Interaction {
        mouse::Interaction::default()
    }
}
