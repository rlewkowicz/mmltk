#[derive(Default)]
pub(super) struct Component {
    input: crate::workspace_input::Binding,
}

impl Component {
    pub fn set_connection(&self, connection: Option<crate::transport_connection::Connection>) {
        self.input.set_connection(connection);
    }
    pub fn binding(&self, model: &crate::view_model::ApplicationModel, radius: u16) -> crate::workspace_input::Binding {
        self.input.for_source(crate::generated::PresentationSourceKind::Annotation,
            model.annotation.snapshot.as_ref().map_or(0, |snapshot| snapshot.inputdocumentepoch), radius)
    }
    pub fn cancel(&self, model: &crate::view_model::ApplicationModel) {
        self.binding(model, 12).cancel();
    }
}

pub(super) fn view(
    surface: Option<crate::presentation_surface::Surface>,
    aspect: crate::generated::WorkspaceAspectRatio,
    settings_available: bool,
    width: f32,
    input: crate::workspace_input::Binding,
    keyboard: std::sync::Arc<std::sync::atomic::AtomicBool>,
) -> crate::fluent_theme::Element<'static, super::Message> {
    use crate::view::workspace;
    use iced::widget::{column, container, shader};
    let (width, height) = workspace::surface_extent(width, aspect);
    let image: crate::fluent_theme::Element<'static, super::Message> = shader(crate::presentation_surface::Program {
        input: Some(input),
        surface: surface.unwrap_or_else(crate::presentation_surface::Surface::empty),
        publish: None,
        local: Some(std::sync::Arc::new(move |gesture| {
            if gesture.kind == crate::presentation_surface::SurfaceGestureKind::Pointer {
                keyboard.store(true, std::sync::atomic::Ordering::Relaxed);
            }
            None
        })),
        placement: crate::presentation_surface::Placement::Contain,
        control_id: workspace::STABLE_ID,
    }).width(iced::Fill).height(iced::Fill).into();
    let image = if surface.is_none() {
        iced::widget::stack![image, container(iced::widget::text("Open an image from Explore to begin annotating"))
            .center(iced::Fill).width(iced::Fill).height(iced::Fill)].into()
    } else { image };
    column![
        crate::view::aspect_ratio::selector(aspect, settings_available,
            crate::view::aspect_ratio::Scope::Workspace,
            |aspect| super::Message::Workspace(workspace::Message::AspectSelected(aspect))),
        container(container(image).id(workspace::STABLE_ID)
            .width(iced::Length::Fixed(width)).height(iced::Length::Fixed(height)))
            .id(super::WORKSPACE_ID).style(crate::fluent_theme::container_workspace)
    ].spacing(8).width(iced::Fill).into()
}
