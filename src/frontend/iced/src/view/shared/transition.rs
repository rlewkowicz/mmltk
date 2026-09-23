//! Retained, top-anchored intrinsic-height disclosure for ordinary form content.
use iced::advanced::{Layout, Shell, Widget, layout, mouse, overlay, renderer, widget};
use iced::animation::{Animation, Easing};
use iced::time::Instant;
use iced::{Element, Event, Length, Rectangle, Size, Vector};

/// Keep this widget mounted with a stable identity, even when its body is hidden.
/// A reservation retains only the largest measured height for its generation and
/// width. Passing `None` releases it; a new generation starts a fresh reservation.
pub fn disclosure<'a, Message: 'a>(
    id: &'static str,
    visible: bool,
    content: impl Into<crate::fluent_theme::Element<'a, Message>>,
) -> HeightTransition<'a, Message> {
    HeightTransition {
        id: widget::Id::from(id),
        visible,
        animate_resize: false,
        reservation: None,
        content: content.into(),
    }
}

pub struct HeightTransition<
    'a,
    Message,
    Theme = crate::fluent_theme::Theme,
    Renderer = iced::Renderer,
> {
    id: widget::Id,
    visible: bool,
    reservation: Option<u64>,
    animate_resize: bool,
    content: Element<'a, Message, Theme, Renderer>,
}

impl<Message, Theme, Renderer> HeightTransition<'_, Message, Theme, Renderer> {
    /// Animate replacement/metric reflow as well as visibility. Ordinary nested
    /// disclosures forward their children's already animated intrinsic extent.
    pub fn animate_resize(mut self) -> Self {
        self.animate_resize = true;
        self
    }

    pub fn reserve(mut self, generation: Option<u64>) -> Self {
        self.reservation = generation;
        self.animate_resize = true;
        self
    }
}

struct State {
    id: widget::Id,
    motion: Animation<f32>,
    instant: Instant,
    last_redraw: Option<Instant>,
    installed: bool,
    visible: bool,
    interactive: bool,
    width: f32,
    reservation: Option<u64>,
    reserved: f32,
}

impl State {
    fn new(id: widget::Id) -> Self {
        Self {
            id,
            motion: animation(0.0),
            instant: Instant::now(),
            last_redraw: None,
            installed: false,
            visible: false,
            interactive: false,
            width: 0.0,
            reservation: None,
            reserved: 0.0,
        }
    }

    fn measure(
        &mut self,
        size: Size,
        visible: bool,
        reservation: Option<u64>,
        animate_resize: bool,
        now: Instant,
    ) -> f32 {
        let width_changed = self.width != size.width;
        if width_changed || reservation != self.reservation {
            self.reserved = 0.0;
        }
        self.width = size.width;
        self.reservation = reservation;
        let mut target = if visible { size.height } else { 0.0 };
        if reservation.is_some() {
            self.reserved = self.reserved.max(target);
            target = self.reserved;
        }
        let visibility_changed = self.visible != visible;
        self.visible = visible;
        if !self.installed
            || width_changed
            || (!visibility_changed && !animate_resize && self.motion.value() != target)
        {
            // Width changes remeasure rather than carrying a wide layout's reservation.
            self.motion = animation(target);
            self.installed = true;
        } else if self.motion.value() != target {
            // Start at the displayed extent, including interrupted transitions.
            self.motion = animation(self.motion.interpolate(self.instant)).go(target, now);
        }
        self.instant = now;
        self.motion.interpolate(now)
    }

    fn advance(&mut self, now: Instant) -> bool {
        let changing = self.motion.is_animating(self.instant);
        self.instant = now;
        changing || self.motion.is_animating(now)
    }
}

impl<Message, Theme, Renderer> Widget<Message, Theme, Renderer>
    for HeightTransition<'_, Message, Theme, Renderer>
where
    Renderer: renderer::Renderer,
{
    fn size(&self) -> Size<Length> {
        Size::new(self.content.as_widget().size().width, Length::Shrink)
    }

    fn tag(&self) -> widget::tree::Tag {
        widget::tree::Tag::of::<State>()
    }
    fn state(&self) -> widget::tree::State {
        widget::tree::State::new(State::new(self.id.clone()))
    }

    fn diff(&mut self, tree: &mut widget::Tree) {
        if tree.state.downcast_ref::<State>().id != self.id {
            tree.state = self.state();
            tree.children.clear();
        }
        tree.diff_children(std::slice::from_mut(&mut self.content));
    }

    fn layout(
        &mut self,
        tree: &mut widget::Tree,
        renderer: &Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        let child_limits = layout::Limits::new(
            Size::new(limits.min().width, 0.0),
            Size::new(limits.max().width, f32::INFINITY),
        );
        let child =
            self.content
                .as_widget_mut()
                .layout(&mut tree.children[0], renderer, &child_limits);
        let height = tree.state.downcast_mut::<State>().measure(
            Size::new(limits.max().width, child.size().height),
            self.visible,
            self.reservation,
            self.animate_resize,
            Instant::now(),
        );
        layout::Node::with_children(Size::new(child.size().width, height), vec![child])
    }

    fn update(
        &mut self,
        tree: &mut widget::Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        renderer: &Renderer,
        shell: &mut Shell<'_, Message>,
        viewport: &Rectangle,
    ) {
        let bounds = layout.bounds();
        // A zero-height opening still needs its first frame. Offscreen/hidden
        // ancestors do not receive continuing redraw requests from this owner.
        let on_screen = Rectangle {
            height: bounds.height.max(1.0),
            ..bounds
        }
        .intersection(viewport)
        .is_some();
        if let Event::Window(iced::window::Event::RedrawRequested(now)) = event {
            let state = tree.state.downcast_mut::<State>();
            // Iced can replay one redraw after layout invalidation. Sample and
            // invalidate once per timestamp, while retaining the next-frame request.
            if state.last_redraw != Some(*now) {
                state.last_redraw = Some(*now);
                let changed = state.advance(*now);
                if on_screen && (changed || state.motion.interpolate(*now) != bounds.height) {
                    shell.invalidate_layout();
                }
            }
            if on_screen && state.motion.is_animating(*now) {
                shell.request_redraw();
            }
        }
        let state = tree.state.downcast_mut::<State>();
        let cancel_input = state.interactive && !self.visible;
        state.interactive = self.visible;
        let clip = bounds
            .intersection(viewport)
            .filter(|clip| clip.height > 0.0 && self.visible);
        let cancellations = if cancel_input {
            // Checkbox mouse presses and selectable-text gestures retire on
            // window unfocus. Buttons and text-input/slider drags retire on
            // FingerLost. Neither event completes a checkbox/button activation.
            [
                Some(Event::Window(iced::window::Event::Unfocused)),
                Some(Event::Touch(iced::touch::Event::FingerLost {
                    id: iced::touch::Finger(0),
                    position: iced::Point::ORIGIN,
                })),
            ]
        } else if clip.is_none()
            && matches!(
                event,
                Event::Window(iced::window::Event::Unfocused)
                    | Event::Touch(iced::touch::Event::FingerLost { .. })
            )
        {
            // Forward each cancellation through retained nested disclosures once.
            [Some(event.clone()), None]
        } else {
            [None, None]
        };
        for cancellation in cancellations.into_iter().flatten() {
            // A slider can emit its release message even for FingerLost. Keep
            // every cancellation effect local to outgoing widget custody.
            let mut discarded = Vec::new();
            let mut local = shell.local(&mut discarded);
            self.content.as_widget_mut().update(
                &mut tree.children[0],
                &cancellation,
                layout.children().next().unwrap(),
                mouse::Cursor::Unavailable,
                renderer,
                &mut local,
                &Rectangle::default(),
            );
        }
        if cancel_input
            || (self.visible && bounds.height < layout.children().next().unwrap().bounds().height)
        {
            self.content.as_widget_mut().operate(
                &mut tree.children[0],
                layout.children().next().unwrap(),
                renderer,
                &mut UnfocusClipped(self.visible.then_some(bounds)),
            );
        }
        let clip = match clip {
            Some(clip) => clip,
            // Scrolling can remove a still-mounted control before its accepted
            // gesture ends. Retire that custody without offering a hit target;
            // nested disclosures must deliver the same release to their child.
            None if self.visible
                && matches!(
                    event,
                    Event::Mouse(mouse::Event::ButtonReleased(_))
                        | Event::Touch(iced::touch::Event::FingerLifted { .. })
                ) =>
            {
                Rectangle::default()
            }
            None => return,
        };
        let cursor = clipped_cursor(cursor, clip);
        match event {
            Event::Touch(iced::touch::Event::FingerPressed { .. }
                | iced::touch::Event::FingerMoved { .. })
                | Event::Mouse(mouse::Event::CursorMoved { .. })
                // Iced scrollers translate the cursor and viewport, while the
                // original event remains in the child's normal event convention.
                if cursor.position().is_none() => return,
            _ => {}
        }
        self.content.as_widget_mut().update(
            &mut tree.children[0],
            event,
            layout.children().next().unwrap(),
            cursor,
            renderer,
            shell,
            &clip,
        );
    }

    fn draw(
        &self,
        tree: &widget::Tree,
        renderer: &mut Renderer,
        theme: &Theme,
        style: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        let Some(clip) = layout
            .bounds()
            .intersection(viewport)
            .filter(|clip| clip.height > 0.0)
        else {
            return;
        };
        renderer.with_layer(clip, |renderer| {
            self.content.as_widget().draw(
                &tree.children[0],
                renderer,
                theme,
                style,
                layout.children().next().unwrap(),
                clipped_cursor(cursor, clip),
                &clip,
            )
        });
    }

    fn operate(
        &mut self,
        tree: &mut widget::Tree,
        layout: Layout<'_>,
        renderer: &Renderer,
        operation: &mut dyn widget::Operation,
    ) {
        operation.container(Some(&self.id), layout.bounds());
        if self.visible && layout.bounds().height > 0.0 {
            operation.traverse(&mut |operation| {
                self.content.as_widget_mut().operate(
                    &mut tree.children[0],
                    layout.children().next().unwrap(),
                    renderer,
                    &mut VisibleOperation {
                        operation,
                        clip: layout.bounds(),
                    },
                )
            });
        }
    }

    fn mouse_interaction(
        &self,
        tree: &widget::Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
        renderer: &Renderer,
    ) -> mouse::Interaction {
        let Some(clip) = layout
            .bounds()
            .intersection(viewport)
            .filter(|clip| self.visible && cursor.is_over(*clip))
        else {
            return mouse::Interaction::None;
        };
        self.content.as_widget().mouse_interaction(
            &tree.children[0],
            layout.children().next().unwrap(),
            cursor,
            &clip,
            renderer,
        )
    }

    fn overlay<'b>(
        &'b mut self,
        tree: &'b mut widget::Tree,
        layout: Layout<'b>,
        renderer: &Renderer,
        viewport: &Rectangle,
        translation: Vector,
    ) -> Option<overlay::Element<'b, Message, Theme, Renderer>> {
        let child = layout.children().next().unwrap();
        // Popups only escape a fully revealed body; clipped/outgoing controls
        // cannot leave an independently interactive overlay behind.
        if !self.visible
            || layout.bounds().height < child.bounds().height
            || layout.bounds().intersection(viewport).is_none()
        {
            return None;
        }
        self.content.as_widget_mut().overlay(
            &mut tree.children[0],
            child,
            renderer,
            viewport,
            translation,
        )
    }
}

impl<'a, Message: 'a, Theme: 'a, Renderer: renderer::Renderer + 'a>
    From<HeightTransition<'a, Message, Theme, Renderer>> for Element<'a, Message, Theme, Renderer>
{
    fn from(value: HeightTransition<'a, Message, Theme, Renderer>) -> Self {
        Element::new(value)
    }
}

fn animation(height: f32) -> Animation<f32> {
    Animation::new(height).quick().easing(Easing::EaseInOut)
}

fn clipped_cursor(cursor: mouse::Cursor, clip: Rectangle) -> mouse::Cursor {
    if cursor.is_over(clip) {
        cursor
    } else {
        mouse::Cursor::Unavailable
    }
}

fn contains(clip: Rectangle, bounds: Rectangle) -> bool {
    bounds.height > 0.0 && bounds.width > 0.0 && bounds.is_within(&clip)
}

struct UnfocusClipped(Option<Rectangle>);
impl widget::Operation for UnfocusClipped {
    fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn widget::Operation)) {
        operate(self);
    }
    fn focusable(
        &mut self,
        _id: Option<&widget::Id>,
        bounds: Rectangle,
        state: &mut dyn widget::operation::Focusable,
    ) {
        if !self.0.is_some_and(|clip| contains(clip, bounds)) {
            state.unfocus();
        }
    }
}

/// Widget operations have no viewport parameter. Filter their geometry here so
/// focus traversal and accessibility see the same reveal as rendering/input.
struct VisibleOperation<'a> {
    operation: &'a mut dyn widget::Operation,
    clip: Rectangle,
}
impl widget::Operation for VisibleOperation<'_> {
    fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn widget::Operation)) {
        let clip = self.clip;
        self.operation
            .traverse(&mut |operation| operate(&mut VisibleOperation { operation, clip }));
    }
    fn container(&mut self, id: Option<&widget::Id>, bounds: Rectangle) {
        if let Some(bounds) = self.clip.intersection(&bounds) {
            self.operation.container(id, bounds);
        }
    }
    fn focusable(
        &mut self,
        id: Option<&widget::Id>,
        bounds: Rectangle,
        state: &mut dyn widget::operation::Focusable,
    ) {
        if contains(self.clip, bounds) {
            self.operation.focusable(id, bounds, state);
        } else {
            state.unfocus();
        }
    }
    fn text_input(
        &mut self,
        id: Option<&widget::Id>,
        bounds: Rectangle,
        state: &mut dyn widget::operation::TextInput,
    ) {
        if contains(self.clip, bounds) {
            self.operation.text_input(id, bounds, state);
        }
    }
    fn text(&mut self, id: Option<&widget::Id>, bounds: Rectangle, text: &str) {
        if let Some(bounds) = self.clip.intersection(&bounds) {
            self.operation.text(id, bounds, text);
        }
    }
    fn scrollable(
        &mut self,
        id: Option<&widget::Id>,
        bounds: Rectangle,
        content: Rectangle,
        translation: Vector,
        state: &mut dyn widget::operation::Scrollable,
    ) {
        if contains(self.clip, bounds) {
            self.operation
                .scrollable(id, bounds, content, translation, state);
        }
    }
    fn custom(
        &mut self,
        id: Option<&widget::Id>,
        bounds: Rectangle,
        state: &mut dyn std::any::Any,
    ) {
        if contains(self.clip, bounds) {
            self.operation.custom(id, bounds, state);
        }
    }
}

#[cfg(test)]
mod tests;
