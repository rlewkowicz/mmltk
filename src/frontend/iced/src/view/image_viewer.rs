//! Shared image geometry and controls; source selection and product policy stay local.
use crate::fluent_theme::Element;
use crate::presentation_surface::{self, Surface};
use iced::widget::{button, column, container, opaque, row, space, text};
use iced::{Center, Fill, Length, Padding};
pub fn image<'a, Message: 'a>(
    surface: Surface,
    labels: presentation_surface::labels::Source,
    input: Option<crate::workspace_input::Binding>,
    show_fps: bool,
    id: &'static str,
) -> Element<'a, Message> {
    presentation_surface::labels::view(
        presentation_surface::Program {
            surface,
            show_fps,
            input,
            local: None,
            publish: None,
            placement: presentation_surface::Placement::Contain,
            control_id: id,
        },
        labels,
    )
}
/// Complete modal viewer shared by source owners. IDs and actions stay with each owner.
pub struct PanelIds {
    pub image: &'static str,
    pub previous: &'static str,
    pub next: &'static str,
    pub close: &'static str,
    pub annotate: &'static str,
    pub upscale: [&'static str; 3],
}
pub fn panel<'a, Message: Clone + 'a>(
    selected: u32,
    image: Element<'a, Message>,
    source: Element<'a, Message>,
    ids: PanelIds,
    previous: Option<Message>,
    next: Option<Message>,
    close: Option<Message>,
    annotate: Option<Message>,
    active: Option<crate::generated::UpscaleKernel>,
    pending: Option<crate::generated::UpscaleKernel>,
    upscale_available: bool,
    upscale_requested: impl Fn(crate::generated::UpscaleKernel) -> Message,
) -> Element<'a, Message> {
    let upscale = crate::generated::UPSCALE_KERNEL_VALUES
        .iter()
        .copied()
        .fold(row![].spacing(5), |row, kernel| {
            row.push(
                container(
                    button(text(if pending == Some(kernel) {
                        format!("{}…", upscale_label(kernel))
                    } else {
                        upscale_label(kernel).to_owned()
                    }))
                    .style(if active == Some(kernel) {
                        crate::fluent_theme::button_primary
                    } else {
                        crate::fluent_theme::button_secondary
                    })
                    .on_press_maybe(upscale_available.then(|| upscale_requested(kernel))),
                )
                .id(ids.upscale[crate::generated::UPSCALE_KERNEL_VALUES
                    .iter()
                    .position(|value| *value == kernel)
                    .expect("canonical kernel")]),
            )
        });
    let panel = container(
        column![
            row![
                text(format!("Sample #{}", selected)).size(20),
                space::horizontal(),
                container(button("Previous").on_press_maybe(previous)).id(ids.previous),
                container(button("Next").on_press_maybe(next)).id(ids.next),
                container(button("×").on_press_maybe(close).padding([2, 9])).id(ids.close),
            ]
            .spacing(7)
            .align_y(Center),
            source,
            container(image)
                .id(ids.image)
                .width(Fill)
                .height(Fill)
                .style(crate::fluent_theme::container_workspace),
            row![
                text("Wheel to zoom · right-drag to pan")
                    .size(12)
                    .style(crate::fluent_theme::text_secondary),
                space::horizontal(),
                text("Upscale"),
                upscale,
            ]
            .spacing(7)
            .align_y(Center),
            container(
                button("Open in Annotation")
                    .on_press_maybe(annotate,)
                    .style(crate::fluent_theme::button_primary)
            )
            .id(ids.annotate),
        ]
        .spacing(10),
    )
    .padding(Padding::from([14, 14]))
    .width(Length::FillPortion(4))
    .height(Length::FillPortion(4))
    .style(crate::fluent_theme::container_modal);

    opaque(
        container(panel)
            .padding(32)
            .center(Fill)
            .width(Fill)
            .height(Fill)
            .style(|_theme| iced::widget::container::Style {
                background: Some(iced::Color::BLACK.into()),
                ..Default::default()
            }),
    )
    .into()
}
pub fn upscale_label(kernel: crate::generated::UpscaleKernel) -> &'static str {
    match kernel {
        crate::generated::UpscaleKernel::Default => "Basic",
        crate::generated::UpscaleKernel::ShiftLut => "Fast",
        crate::generated::UpscaleKernel::RealPlksr => "Neural",
    }
}

/// Source owners provide cohesive groups; wrapping preserves natural typography sizes.
pub fn control_groups<'a, Message: 'a>(
    groups: impl IntoIterator<Item = Element<'a, Message>>,
) -> Element<'a, Message> {
    iced::widget::Row::with_children(groups)
        .spacing(20)
        .align_y(Center)
        .wrap()
        .into()
}
/// Reserve the exact responsive control layout beneath an opaque detail panel.
/// This subtree supplies measurement only: no widget identities, input, or drawing
/// are exposed, so only the detail controls participate in widget operations.
pub fn control_footprint<'a, Message: 'a>(controls: Element<'a, Message>) -> Element<'a, Message> {
    Element::new(ControlFootprint(controls))
}
struct ControlFootprint<'a, Message>(Element<'a, Message>);
impl<Message> iced::advanced::Widget<Message, crate::fluent_theme::Theme, iced::Renderer>
    for ControlFootprint<'_, Message>
{
    fn diff(&mut self, tree: &mut iced::advanced::widget::Tree) {
        tree.diff_children(std::slice::from_mut(&mut self.0));
    }
    fn size(&self) -> iced::Size<Length> {
        self.0.as_widget().size()
    }
    fn layout(
        &mut self,
        tree: &mut iced::advanced::widget::Tree,
        renderer: &iced::Renderer,
        limits: &iced::advanced::layout::Limits,
    ) -> iced::advanced::layout::Node {
        self.0
            .as_widget_mut()
            .layout(&mut tree.children[0], renderer, limits)
    }
    fn draw(
        &self,
        _tree: &iced::advanced::widget::Tree,
        _renderer: &mut iced::Renderer,
        _theme: &crate::fluent_theme::Theme,
        _style: &iced::advanced::renderer::Style,
        _layout: iced::advanced::Layout<'_>,
        _cursor: iced::advanced::mouse::Cursor,
        _viewport: &iced::Rectangle,
    ) {
    }
}
