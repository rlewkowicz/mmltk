//! One ordinary button, with a non-interactive, widget-local border decoration.
mod border;

use crate::fluent_theme::{Element, Theme};
use crate::generated::FeatureId;
use iced::advanced::{Layout, Shell, Widget, layout, mouse, renderer, widget};
use iced::widget::{button, column, container, text};
use iced::{Event, Fill, Font, Length, Padding, Rectangle, Size};
use super::{Composition, Region, FIELD_SPACING, PRIMARY_ACTION_HEIGHT};

fn label(page: FeatureId, active: bool) -> &'static str {
    match (page, active) {
        (FeatureId::Train, false) => "Start Training",
        (FeatureId::Train, true) => "Stop Training",
        (FeatureId::Validate, false) => "Start Validation",
        (FeatureId::Validate, true) => "Stop Validation",
        (FeatureId::Predict, false) => "Run Predict",
        (FeatureId::Predict, true) => "Stop Predict",
        (FeatureId::Export, false) => "Run Export",
        (FeatureId::Export, true) => "Stop Export",
        (FeatureId::Live, false) => "Start Live",
        (FeatureId::Live, true) => "Stop Live",
        (FeatureId::Annotate, _) => "Save Annotations",
        (FeatureId::Explore, _) => unreachable!("Explore has no workflow primary action"),
    }
}

enum Capability { StartStop, SaveOnly }
struct Action<Message> {
    label: &'static str,
    danger: bool,
    on_press: Option<Message>,
}
impl<Message> Action<Message> {
    fn new(page: FeatureId, active: bool, start: Option<Message>, stop: Option<Message>) -> Self {
        let capability = if page == FeatureId::Annotate { Capability::SaveOnly } else { Capability::StartStop };
        let (danger, on_press) = match (capability, active) {
            (_, false) => (false, start),
            (Capability::StartStop, true) => (true, stop),
            (Capability::SaveOnly, true) => (false, None),
        };
        Self { label: label(page, active), danger, on_press }
    }
}

pub fn view<'a, Message: Clone + 'a>(
    page: FeatureId,
    active: bool,
    start: Option<Message>,
    stop: Option<Message>,
    progress: Element<'a, Message>,
) -> Element<'a, Message> {
    let composition = Composition::new(page, 0.0);
    let presentation = Action::new(page, active, start, stop);
    let label = text(presentation.label)
        .font(Font::new("Bitstream Vera Sans").weight(iced::font::Weight::Bold))
        .size(16).width(Fill).height(Fill).align_x(iced::Center).align_y(iced::Center);
    let action = button(label)
        .on_press_maybe(presentation.on_press)
        .style(if presentation.danger { crate::fluent_theme::button_workflow_stop } else { crate::fluent_theme::button_workflow_primary })
        .padding(0).width(Fill).height(Fill);
    let decorated = Element::new(Decorated { child: action.into(), active, page });
    let framed = container(decorated)
        .id(composition.stable_id(Region::PrimaryAction))
        .padding(1).width(Fill).height(Length::Fixed(PRIMARY_ACTION_HEIGHT))
        .style(crate::fluent_theme::container_primary_frame);
    column![
        container(progress).id(composition.stable_id(Region::PrimaryProgress))
            .padding(Padding { bottom: 1.0, ..Padding::ZERO }).width(Fill),
        framed
    ].spacing(FIELD_SPACING).into()
}

#[derive(Default)]
pub(super) struct Animation {
    origin: Option<iced::time::Instant>,
    phase: f32,
    resources: border::Resources,
}

impl Animation {
    fn redraw(&mut self, active: bool, visible: bool, now: iced::time::Instant) -> bool {
        if !active { self.origin = None; self.phase = 0.0; return false; }
        if !visible { return false; }
        let origin = *self.origin.get_or_insert(now);
        self.phase = (now.duration_since(origin).as_secs_f64() / 2.4).fract() as f32;
        true
    }
}

struct Decorated<'a, Message> { child: Element<'a, Message>, active: bool, page: FeatureId }
impl<Message> Widget<Message, Theme, iced::Renderer> for Decorated<'_, Message> {
    fn tag(&self) -> widget::tree::Tag { widget::tree::Tag::of::<Animation>() }
    fn state(&self) -> widget::tree::State { widget::tree::State::new(Animation::default()) }
    fn diff(&mut self, tree: &mut widget::Tree) {
        if !self.active {
            let state = tree.state.downcast_mut::<Animation>();
            state.origin = None;
            state.phase = 0.0;
        }
        tree.diff_children(std::slice::from_mut(&mut self.child));
    }
    fn size(&self) -> Size<Length> { self.child.as_widget().size() }
    fn layout(&mut self, tree: &mut widget::Tree, renderer: &iced::Renderer, limits: &layout::Limits) -> layout::Node {
        self.child.as_widget_mut().layout(&mut tree.children[0], renderer, limits)
    }
    fn operate(&mut self, tree: &mut widget::Tree, layout: Layout<'_>, renderer: &iced::Renderer, operation: &mut dyn widget::Operation) {
        self.child.as_widget_mut().operate(&mut tree.children[0], layout, renderer, operation);
    }
    fn update(&mut self, tree: &mut widget::Tree, event: &Event, layout: Layout<'_>, cursor: mouse::Cursor, renderer: &iced::Renderer, shell: &mut Shell<'_, Message>, viewport: &Rectangle) {
        self.child.as_widget_mut().update(&mut tree.children[0], event, layout, cursor, renderer, shell, viewport);
        if let Event::Window(iced::window::Event::RedrawRequested(now)) = event {
            let visible = layout.bounds().intersection(viewport).is_some_and(|clip| clip.width > 0.0 && clip.height > 0.0);
            if tree.state.downcast_mut::<Animation>().redraw(self.active, visible, *now) { shell.request_redraw(); }
        }
    }
    fn mouse_interaction(&self, tree: &widget::Tree, layout: Layout<'_>, cursor: mouse::Cursor, viewport: &Rectangle, renderer: &iced::Renderer) -> mouse::Interaction {
        self.child.as_widget().mouse_interaction(&tree.children[0], layout, cursor, viewport, renderer)
    }
    fn draw(&self, tree: &widget::Tree, renderer: &mut iced::Renderer, theme: &Theme, style: &renderer::Style, layout: Layout<'_>, cursor: mouse::Cursor, viewport: &Rectangle) {
        self.child.as_widget().draw(&tree.children[0], renderer, theme, style, layout, cursor, viewport);
        let reporting = crate::integration_control::reporting_enabled();
        if !self.active && !reporting { return; }
        let state = tree.state.downcast_ref::<Animation>();
        let Some(iced::Background::Color(blue)) = crate::fluent_theme::button_primary(theme, iced::widget::button::Status::Active).background else { return; };
        let Some(clip) = layout.bounds().intersection(viewport).filter(|clip| clip.width > 0.0 && clip.height > 0.0) else { return; };
        if reporting {
            use iced::advanced::Renderer as _;
            crate::integration_control::primary_action_draw(Composition::new(self.page, 0.0).stable_id(Region::PrimaryAction), label(self.page, self.active), self.active, theme.is_dark(), state.phase, layout.bounds(), clip, renderer.scale_factor().unwrap_or(1.0), blue);
        }
        if !self.active { return; }
        // Stack-only Shader construction submits through Iced's public primitive API.
        // The tree and its GPU uniform owner survive ordinary view reconstruction.
        let shader = iced::widget::shader(border::Border { phase: state.phase, blue, resources: state.resources.clone() });
        <_ as Widget<Message, Theme, iced::Renderer>>::draw(&shader, tree, renderer, theme, style, layout, cursor, viewport);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_labels_and_save_only_capability() {
        for (page, idle, active) in [
            (FeatureId::Train, "Start Training", "Stop Training"),
            (FeatureId::Validate, "Start Validation", "Stop Validation"),
            (FeatureId::Predict, "Run Predict", "Stop Predict"),
            (FeatureId::Export, "Run Export", "Stop Export"),
            (FeatureId::Live, "Start Live", "Stop Live"),
            (FeatureId::Annotate, "Save Annotations", "Save Annotations"),
        ] {
            let idle_action = Action::new(page, false, Some(1), Some(2));
            assert_eq!(idle_action.label, idle);
            assert_eq!(idle_action.on_press, Some(1));
            assert!(!idle_action.danger);
            let active_action = Action::new(page, true, Some(1), Some(2));
            assert_eq!(active_action.label, active);
            assert_eq!(active_action.danger, page != FeatureId::Annotate);
            assert_eq!(active_action.on_press, (page != FeatureId::Annotate).then_some(2));
            assert!(Action::new(page, true, Some(1), None::<i32>).on_press.is_none());
        }
    }
    #[test]
    fn redraw_is_active_visible_and_has_a_continuous_clockwise_cycle() {
        let mut animation = Animation::default();
        let now = iced::time::Instant::now();
        assert!(!animation.redraw(false, true, now));
        assert!(!animation.redraw(true, false, now));
        assert!(animation.redraw(true, true, now));
        assert!(animation.redraw(true, true, now + std::time::Duration::from_millis(600)));
        assert!((animation.phase - 0.25).abs() < 0.0001);
        animation.redraw(true, true, now + std::time::Duration::from_millis(2400));
        assert_eq!(animation.phase, 0.0);
        assert!(!animation.redraw(false, true, now));
        assert!(animation.origin.is_none());
    }
}
