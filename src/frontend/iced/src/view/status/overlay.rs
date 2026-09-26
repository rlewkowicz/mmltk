use super::*;
use crate::message::Message as AppMessage;
use iced::advanced::Renderer as _;
use iced::advanced::{Layout, Shell, Widget, layout, mouse, overlay, renderer, widget};
use iced::{Color, Event, Length, Point, Rectangle, Size, Vector};

pub fn warning_icon<'a>() -> Element<'a, Message> {
    struct Triangle;
    impl iced::widget::canvas::Program<Message, Theme> for Triangle {
        type State = ();
        fn draw(
            &self,
            _: &(),
            renderer: &iced::Renderer,
            _: &Theme,
            bounds: Rectangle,
            _: mouse::Cursor,
        ) -> Vec<iced::widget::canvas::Geometry> {
            let mut frame = iced::widget::canvas::Frame::new(renderer, bounds.size());
            let triangle = iced::widget::canvas::Path::new(|path| {
                path.move_to(Point::new(9.0, 1.0));
                path.line_to(Point::new(18.0, 18.0));
                path.line_to(Point::new(0.0, 18.0));
                path.close();
            });
            frame.fill(&triangle, Color::from_rgb8(255, 211, 48));
            frame.fill_rectangle(
                Point::new(8.0, 6.0),
                Size::new(2.0, 6.0),
                Color::from_rgb8(45, 35, 15),
            );
            frame.fill_rectangle(
                Point::new(8.0, 14.0),
                Size::new(2.0, 2.0),
                Color::from_rgb8(45, 35, 15),
            );
            vec![frame.into_geometry()]
        }
    }
    iced::widget::canvas(Triangle).width(18).height(19).into()
}
struct Button<'a> {
    control: Control,
    id: widget::Id,
    content: Element<'a, Message>,
    width: f32,
    alert: bool,
    environment: environment::State,
}
#[derive(Default)]
struct ButtonState {
    focused: bool,
    reported: bool,
    pressed: bool,
    epoch: Option<iced::time::Instant>,
    pulse: f32,
}
impl widget::operation::Focusable for ButtonState {
    fn is_focused(&self) -> bool {
        self.focused
    }
    fn focus(&mut self) {
        self.focused = true;
    }
    fn unfocus(&mut self) {
        self.focused = false;
    }
}
pub(super) fn control<'a>(
    control: Control,
    content: Element<'a, Message>,
    width: f32,
    alert: bool,
    environment: environment::State,
) -> Element<'a, Message> {
    Element::new(Button {
        id: control.id().into(),
        control,
        content,
        width,
        alert,
        environment,
    })
}
impl Widget<Message, Theme, iced::Renderer> for Button<'_> {
    fn size(&self) -> Size<Length> {
        Size::new(Length::Fixed(self.width), Length::Fixed(CONTROL_HEIGHT))
    }
    fn tag(&self) -> widget::tree::Tag {
        widget::tree::Tag::of::<ButtonState>()
    }
    fn state(&self) -> widget::tree::State {
        widget::tree::State::new(ButtonState::default())
    }
    fn diff(&mut self, tree: &mut widget::Tree) {
        tree.diff_children(std::slice::from_mut(&mut self.content));
    }
    fn layout(
        &mut self,
        tree: &mut widget::Tree,
        renderer: &iced::Renderer,
        _: &layout::Limits,
    ) -> layout::Node {
        self.content.as_widget_mut().layout(
            &mut tree.children[0],
            renderer,
            &layout::Limits::new(
                Size::new(self.width, CONTROL_HEIGHT),
                Size::new(self.width, CONTROL_HEIGHT),
            ),
        )
    }
    fn operate(
        &mut self,
        tree: &mut widget::Tree,
        layout: Layout<'_>,
        _: &iced::Renderer,
        operation: &mut dyn widget::Operation,
    ) {
        operation.container(Some(&self.id), layout.bounds());
        operation.focusable(
            Some(&self.id),
            layout.bounds(),
            tree.state.downcast_mut::<ButtonState>(),
        );
    }
    fn update(
        &mut self,
        tree: &mut widget::Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _: &iced::Renderer,
        shell: &mut Shell<'_, Message>,
        viewport: &Rectangle,
    ) {
        let state = tree.state.downcast_mut::<ButtonState>();
        if state.focused != state.reported {
            state.reported = state.focused;
            shell.publish(Message::Focused(self.control, state.focused));
            shell.request_redraw();
        }
        let over = cursor.is_over(layout.bounds());
        match event {
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)) if over => {
                state.pressed = true;
                shell.capture_event();
            }
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)) if state.pressed => {
                state.pressed = false;
                if over {
                    shell.publish(Message::Activate(self.control, Opening::Mouse));
                }
                shell.capture_event();
            }
            Event::Touch(iced::touch::Event::FingerPressed { position, .. })
                if layout.bounds().contains(*position) =>
            {
                shell.publish(Message::Activate(self.control, Opening::Touch));
                shell.capture_event();
            }
            Event::Keyboard(iced::keyboard::Event::KeyPressed { key, modifiers, .. })
                if state.focused =>
            {
                use iced::keyboard::key::Named;
                match key.as_ref() {
                    iced::keyboard::Key::Named(Named::Enter | Named::Space) => {
                        shell.publish(Message::Activate(self.control, Opening::Keyboard));
                        shell.capture_event();
                    }
                    iced::keyboard::Key::Named(Named::Tab) => {
                        shell.publish(Message::Traverse(modifiers.shift()));
                        shell.capture_event();
                    }
                    iced::keyboard::Key::Named(Named::Escape) => {
                        shell.publish(Message::Close);
                        shell.capture_event();
                    }
                    _ => {}
                }
            }
            Event::Window(iced::window::Event::RedrawRequested(now))
                if self.alert
                    && self.environment.visible
                    && layout.bounds().intersection(viewport).is_some() =>
            {
                if self.environment.reduced_motion {
                    state.pulse = 0.5;
                } else {
                    let epoch = *state.epoch.get_or_insert(*now);
                    state.pulse = (1.0
                        - (now.duration_since(epoch).as_secs_f32() * std::f32::consts::PI).cos())
                        * 0.5;
                    shell.request_redraw();
                }
            }
            _ => {}
        }
    }
    fn draw(
        &self,
        tree: &widget::Tree,
        renderer: &mut iced::Renderer,
        theme: &Theme,
        _: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        let state = tree.state.downcast_ref::<ButtonState>();
        let style = alert_style(theme, state.pulse, self.alert);
        if state.focused {
            let bounds = layout.bounds();
            renderer.fill_quad(
                renderer::Quad {
                    bounds: Rectangle {
                        x: bounds.x - 3.0,
                        y: bounds.y - 3.0,
                        width: bounds.width + 6.0,
                        height: bounds.height + 6.0,
                    },
                    border: iced::Border {
                        color: Color::from_rgb8(0, 120, 215),
                        width: 2.0,
                        radius: style.border.radius,
                    },
                    ..renderer::Quad::default()
                },
                Color::TRANSPARENT,
            );
        }
        renderer.fill_quad(
            renderer::Quad {
                bounds: layout.bounds(),
                border: style.border,
                ..renderer::Quad::default()
            },
            style.background.unwrap_or(Color::TRANSPARENT.into()),
        );
        self.content.as_widget().draw(
            &tree.children[0],
            renderer,
            theme,
            &renderer::Style {
                text_color: style.text_color,
            },
            layout,
            cursor,
            viewport,
        );
        crate::integration_control::status::control_draw(
            self.control,
            layout.bounds(),
            theme.is_dark(),
            self.alert,
            state.focused,
            self.environment,
            style,
        );
    }
    fn mouse_interaction(
        &self,
        _: &widget::Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _: &Rectangle,
        _: &iced::Renderer,
    ) -> mouse::Interaction {
        if cursor.is_over(layout.bounds()) {
            mouse::Interaction::Pointer
        } else {
            mouse::Interaction::None
        }
    }
}
struct Host<'a> {
    content: Element<'a, AppMessage>,
    panel: Element<'a, AppMessage>,
    component: &'a Component,
    nonempty: bool,
}
// Pointer transitions are widget state: Iced can deliver several moves before
// applying the resulting application messages. Keep every edge in that batch.
struct PointerRegion {
    inside: bool,
}
impl PointerRegion {
    fn observe(&mut self, event: &Event, inside: bool) -> Option<Message> {
        let inside = match event {
            Event::Mouse(mouse::Event::CursorMoved { .. }) => inside,
            Event::Mouse(mouse::Event::CursorLeft) => false,
            _ => return None,
        };
        if inside == self.inside {
            return None;
        }
        self.inside = inside;
        Some(Message::Hover(inside))
    }
}
pub(super) fn host<'a>(
    content: Element<'a, AppMessage>,
    component: &'a Component,
    notices: &'a NoticeStore,
) -> Element<'a, AppMessage> {
    Element::new(Host {
        content,
        panel: if component.open {
            component.panel(notices, 600.0).map(AppMessage::Status)
        } else {
            iced::widget::space::horizontal().width(0).height(0).into()
        },
        component,
        nonempty: !notices.is_empty(),
    })
}
pub(super) fn trigger(bounds: Size) -> Rectangle {
    Rectangle {
        x: (bounds.width - STATUS_WIDTH - SETTINGS_WIDTH - 18.0).max(0.0),
        y: 9.0,
        width: STATUS_WIDTH,
        height: CONTROL_HEIGHT,
    }
}
pub(super) fn panel_bounds(bounds: Size, height: f32) -> Rectangle {
    let width = (bounds.width * 0.6).min((bounds.width - 24.0).max(1.0));
    let height = height.min((bounds.height - 24.0).max(1.0));
    Rectangle {
        x: trigger(bounds)
            .x
            .min((bounds.width - width - 12.0).max(12.0)),
        y: 51.0_f32.min((bounds.height - height - 12.0).max(12.0)),
        width,
        height,
    }
}
fn pointer(event: &Event, cursor: mouse::Cursor) -> Option<Point> {
    match event {
        Event::Touch(
            iced::touch::Event::FingerPressed { position, .. }
            | iced::touch::Event::FingerMoved { position, .. }
            | iced::touch::Event::FingerLifted { position, .. },
        ) => Some(*position),
        _ => cursor.position(),
    }
}
fn activation(event: &Event) -> bool {
    matches!(
        event,
        Event::Mouse(mouse::Event::ButtonPressed(_))
            | Event::Touch(iced::touch::Event::FingerPressed { .. })
    )
}
fn traverse_status(event: &Event, shell: &mut Shell<'_, AppMessage>) -> bool {
    if let Event::Keyboard(iced::keyboard::Event::KeyPressed {
        key: iced::keyboard::Key::Named(iced::keyboard::key::Named::Tab),
        modifiers,
        ..
    }) = event
    {
        shell.publish(AppMessage::Status(Message::Traverse(modifiers.shift())));
        shell.capture_event();
        return true;
    }
    false
}
impl Widget<AppMessage, Theme, iced::Renderer> for Host<'_> {
    fn tag(&self) -> widget::tree::Tag {
        widget::tree::Tag::of::<PointerRegion>()
    }
    fn state(&self) -> widget::tree::State {
        widget::tree::State::new(PointerRegion {
            inside: self.component.hover,
        })
    }
    fn size(&self) -> Size<Length> {
        Size::new(Length::Fill, Length::Fill)
    }
    fn diff(&mut self, tree: &mut widget::Tree) {
        tree.diff_children(&mut [self.content.as_widget_mut(), self.panel.as_widget_mut()]);
    }
    fn layout(
        &mut self,
        tree: &mut widget::Tree,
        renderer: &iced::Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        self.content
            .as_widget_mut()
            .layout(&mut tree.children[0], renderer, limits)
    }
    fn operate(
        &mut self,
        tree: &mut widget::Tree,
        layout: Layout<'_>,
        renderer: &iced::Renderer,
        operation: &mut dyn widget::Operation,
    ) {
        self.content
            .as_widget_mut()
            .operate(&mut tree.children[0], layout, renderer, operation);
    }
    fn update(
        &mut self,
        tree: &mut widget::Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        renderer: &iced::Renderer,
        shell: &mut Shell<'_, AppMessage>,
        viewport: &Rectangle,
    ) {
        if !self.component.open {
            let inside = cursor.is_over(trigger(layout.bounds().size()));
            if let Some(message) = tree
                .state
                .downcast_mut::<PointerRegion>()
                .observe(event, inside)
            {
                shell.publish(AppMessage::Status(message));
            }
        }
        self.content.as_widget_mut().update(
            &mut tree.children[0],
            event,
            layout,
            cursor,
            renderer,
            shell,
            viewport,
        );
        if !shell.is_event_captured() {
            // CLEANUP-IGNORE: CPD joins this Tab action to required Iced draw/mouse methods; DetailText publishes a distinct focus observation.
            traverse_status(event, shell);
        }
    }
    // CLEANUP-IGNORE: Required Iced draw and mouse methods forward this host's child; DetailText has a distinct message and state owner.
    fn draw(
        &self,
        tree: &widget::Tree,
        renderer: &mut iced::Renderer,
        theme: &Theme,
        style: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        self.content.as_widget().draw(
            &tree.children[0],
            renderer,
            theme,
            style,
            layout,
            cursor,
            viewport,
        );
    }
    // CLEANUP-IGNORE: Required Iced mouse dispatch forwards the host child, while other wrappers map their own layouts.
    fn mouse_interaction(
        &self,
        tree: &widget::Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
        renderer: &iced::Renderer,
    ) -> mouse::Interaction {
        self.content.as_widget().mouse_interaction(
            &tree.children[0],
            layout,
            // CLEANUP-IGNORE: The fragment ends a child call and starts an unrelated overlay signature; panel construction differs by owner.
            cursor,
            viewport,
            renderer,
        )
    }
    fn overlay<'b>(
        &'b mut self,
        tree: &'b mut widget::Tree,
        layout: Layout<'b>,
        renderer: &iced::Renderer,
        viewport: &Rectangle,
        translation: Vector,
    ) -> Option<overlay::Element<'b, AppMessage, Theme, iced::Renderer>> {
        if self.component.open && self.nonempty {
            Some(overlay::Element::new(Box::new(Popup {
                content: &mut self.panel,
                tree: &mut tree.children[1],
                pointer_region: tree.state.downcast_mut::<PointerRegion>(),
                component: self.component,
                viewport: *viewport,
            })))
        } else {
            self.content.as_widget_mut().overlay(
                &mut tree.children[0],
                layout,
                renderer,
                viewport,
                translation,
            )
        }
    }
}
struct Popup<'a, 'b> {
    content: &'a mut Element<'b, AppMessage>,
    tree: &'a mut widget::Tree,
    pointer_region: &'a mut PointerRegion,
    component: &'a Component,
    viewport: Rectangle,
}
impl overlay::Overlay<AppMessage, Theme, iced::Renderer> for Popup<'_, '_> {
    fn layout(&mut self, renderer: &iced::Renderer, bounds: Size) -> layout::Node {
        self.viewport = Rectangle::with_size(bounds);
        let rect = panel_bounds(bounds, (bounds.height - 64.0).clamp(1.0, 600.0));
        self.content
            .as_widget_mut()
            .layout(
                self.tree,
                renderer,
                &layout::Limits::new(Size::new(rect.width, 0.0), rect.size()),
            )
            .move_to(Point::new(rect.x, rect.y))
    }
    fn draw(
        &self,
        renderer: &mut iced::Renderer,
        theme: &Theme,
        style: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
    ) {
        self.content.as_widget().draw(
            self.tree,
            renderer,
            theme,
            style,
            layout,
            cursor,
            &self.viewport,
        );
    }
    fn operate(
        &mut self,
        layout: Layout<'_>,
        renderer: &iced::Renderer,
        operation: &mut dyn widget::Operation,
    ) {
        self.content
            .as_widget_mut()
            .operate(self.tree, layout, renderer, operation);
    }
    fn update(
        &mut self,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        renderer: &iced::Renderer,
        shell: &mut Shell<'_, AppMessage>,
    ) {
        let trigger = trigger(self.viewport.size());
        let panel = layout.bounds();
        let gap = Rectangle {
            x: trigger.x.max(panel.x),
            y: trigger.y + trigger.height,
            width: ((trigger.x + trigger.width).min(panel.x + panel.width)
                - trigger.x.max(panel.x))
            .max(0.0),
            height: (panel.y - trigger.y - trigger.height).max(0.0),
        };
        let point = pointer(event, cursor);
        let inside = point.is_some_and(|point| {
            trigger.contains(point) || panel.contains(point) || gap.contains(point)
        });
        if let Some(message) = self.pointer_region.observe(event, inside) {
            shell.publish(AppMessage::Status(message));
        }
        if activation(event)
            && !point.is_some_and(|point| panel.contains(point) || trigger.contains(point))
        {
            shell.publish(AppMessage::Status(Message::Outside));
            shell.capture_event();
            return;
        }
        if matches!(
            event,
            Event::Keyboard(iced::keyboard::Event::KeyPressed {
                key: iced::keyboard::Key::Named(iced::keyboard::key::Named::Escape),
                ..
            })
        ) {
            shell.publish(AppMessage::Status(Message::Close));
            shell.capture_event();
            return;
        }
        if self.component.focus.is_some() && traverse_status(event, shell) {
            return;
        }
        self.content.as_widget_mut().update(
            self.tree,
            event,
            layout,
            cursor,
            renderer,
            shell,
            &self.viewport,
        );
        if point.is_some_and(|point| panel.contains(point))
            && matches!(event, Event::Mouse(_) | Event::Touch(_))
        {
            shell.capture_event();
        }
    }
    fn mouse_interaction(
        &self,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        renderer: &iced::Renderer,
    ) -> mouse::Interaction {
        self.content.as_widget().mouse_interaction(
            self.tree,
            layout,
            cursor,
            &self.viewport,
            renderer,
        )
    }
}

struct DetailText<'a> {
    id: NoticeId,
    content: Element<'a, Message>,
}
#[derive(Default)]
struct DetailFocus {
    focused: bool,
}
impl widget::Operation for DetailFocus {
    fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn widget::Operation)) {
        operate(self);
    }
    fn focusable(
        &mut self,
        _: Option<&widget::Id>,
        _: Rectangle,
        state: &mut dyn widget::operation::Focusable,
    ) {
        self.focused |= state.is_focused();
    }
}
pub(super) fn detail_text(id: NoticeId, content: Element<'_, Message>) -> Element<'_, Message> {
    Element::new(DetailText { id, content })
}
impl Widget<Message, Theme, iced::Renderer> for DetailText<'_> {
    fn size(&self) -> Size<Length> {
        self.content.as_widget().size()
    }
    fn tag(&self) -> widget::tree::Tag {
        widget::tree::Tag::of::<DetailFocus>()
    }
    fn state(&self) -> widget::tree::State {
        widget::tree::State::new(DetailFocus::default())
    }
    fn diff(&mut self, tree: &mut widget::Tree) {
        // CLEANUP-IGNORE: DetailText owns one child and Host owns two; following layout/operate signatures are required by Iced.
        tree.diff_children(std::slice::from_mut(&mut self.content));
    }
    // CLEANUP-IGNORE: Iced requires explicit child layout and operation forwarding for this focus-observing widget.
    fn layout(
        &mut self,
        tree: &mut widget::Tree,
        renderer: &iced::Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        self.content
            .as_widget_mut()
            .layout(&mut tree.children[0], renderer, limits)
    }
    fn operate(
        &mut self,
        tree: &mut widget::Tree,
        layout: Layout<'_>,
        renderer: &iced::Renderer,
        operation: &mut dyn widget::Operation,
    ) {
        self.content
            // CLEANUP-IGNORE: Required operate forwarding borders an update signature; text selection and button animation are different behaviors.
            .as_widget_mut()
            .operate(&mut tree.children[0], layout, renderer, operation);
    }
    // CLEANUP-IGNORE: Iced fixes this update signature; DetailText's selection and focus behavior differs from StatusText.
    fn update(
        &mut self,
        tree: &mut widget::Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        renderer: &iced::Renderer,
        shell: &mut Shell<'_, Message>,
        viewport: &Rectangle,
    ) {
        // The outer list owns scrolling. Read-only text still handles selection and copying.
        if !matches!(event, Event::Mouse(mouse::Event::WheelScrolled { .. })) {
            self.content.as_widget_mut().update(
                &mut tree.children[0],
                event,
                layout,
                cursor,
                renderer,
                shell,
                viewport,
            );
        }
        let mut focus = DetailFocus::default();
        self.content
            .as_widget_mut()
            .operate(&mut tree.children[0], layout, renderer, &mut focus);
        let state = tree.state.downcast_mut::<DetailFocus>();
        if state.focused != focus.focused {
            state.focused = focus.focused;
            shell.publish(Message::Focused(Control::Detail(self.id), focus.focused));
        }
    }
    // CLEANUP-IGNORE: Iced fixes this draw signature and child rendering call; message and retained state belong to DetailText.
    fn draw(
        &self,
        tree: &widget::Tree,
        renderer: &mut iced::Renderer,
        theme: &Theme,
        style: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        self.content.as_widget().draw(
            &tree.children[0],
            renderer,
            theme,
            style,
            layout,
            cursor,
            viewport,
        );
    }
    fn mouse_interaction(
        &self,
        tree: &widget::Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
        renderer: &iced::Renderer,
    ) -> mouse::Interaction {
        self.content.as_widget().mouse_interaction(
            &tree.children[0],
            layout,
            cursor,
            viewport,
            renderer,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn batched_pointer_edges_rearm_a_manually_closed_panel_without_focus_changes() {
        let mut component = Component::default();
        component.hover(true, true);
        component.close();
        component.focus = Some(Control::Trigger);
        let mut region = PointerRegion { inside: true };
        let moved = Event::Mouse(mouse::Event::CursorMoved {
            position: Point::ORIGIN,
        });
        let messages: Vec<_> = [true, false, false, true, true]
            .into_iter()
            .filter_map(|inside| region.observe(&moved, inside))
            .collect();
        assert!(
            !component.open,
            "input collection does not mutate application state"
        );
        assert_eq!(
            messages.len(),
            2,
            "unchanged pointer positions remain quiet"
        );
        for message in messages {
            let Message::Hover(inside) = message else {
                panic!("pointer regions emit only hover transitions");
            };
            component.hover(inside, true);
        }
        assert!(component.open);
        assert!(!component.latch);
        assert_eq!(component.focus, Some(Control::Trigger));
    }

    #[test]
    fn cursor_exit_rearms_hover_even_when_no_notices_are_present() {
        let mut component = Component::default();
        component.hover(true, true);
        component.close();
        let mut region = PointerRegion { inside: true };
        let Some(Message::Hover(inside)) =
            region.observe(&Event::Mouse(mouse::Event::CursorLeft), true)
        else {
            panic!("cursor exit must settle the physical pointer region");
        };
        component.hover(inside, false);
        assert!(!component.open);
        assert!(!component.latch);
        assert!(
            region
                .observe(&Event::Mouse(mouse::Event::CursorLeft), true)
                .is_none()
        );
        let moved = Event::Mouse(mouse::Event::CursorMoved {
            position: Point::ORIGIN,
        });
        let Some(Message::Hover(inside)) = region.observe(&moved, true) else {
            panic!("reentry is a new pointer edge");
        };
        component.hover(inside, true);
        assert!(component.open);
    }
}
