use super::*;
use iced::advanced::widget::operation::{Focusable, Operation};
use iced::time::Duration;
use iced::{Point, window};

#[test]
fn initial_installation_reversal_replacement_and_reservation_use_measured_extent() {
    let start = Instant::now();
    let size = Size::new(200.0, 120.0);
    let mut state = State::new(widget::Id::from("section"));
    assert_eq!(state.measure(size, false, None, true, start), 0.0);
    assert_eq!(state.measure(size, true, None, true, start), 0.0);
    let middle = start + Duration::from_millis(100);
    let height = state.measure(size, true, None, true, middle);
    assert!(height > 0.0 && height < size.height);
    assert_eq!(state.measure(size, false, None, true, middle), height);
    assert_eq!(state.measure(size, false, None, true, middle + Duration::from_millis(200)), 0.0);
    assert!(!state.advance(middle + Duration::from_millis(201)));

    let mut state = State::new(widget::Id::from("reserved"));
    assert_eq!(state.measure(size, true, Some(1), true, start), 120.0);
    assert_eq!(state.measure(Size::new(200.0, 40.0), true, Some(1), true, start), 120.0);
    assert_eq!(state.measure(Size::new(200.0, 180.0), true, Some(1), true, start), 120.0);
    assert_eq!(state.measure(Size::new(200.0, 40.0), true, Some(1), true, start + Duration::from_millis(200)), 180.0);
    // The terminal body replaces content; only the scalar reservation survived.
    assert_eq!(state.measure(Size::new(200.0, 24.0), true, None, true, start + Duration::from_millis(200)), 180.0);
    assert_eq!(state.measure(Size::new(200.0, 24.0), true, None, true, start + Duration::from_millis(400)), 24.0);
    state.measure(Size::new(200.0, 60.0), true, Some(2), true, start + Duration::from_millis(400));
    assert_eq!(state.reserved, 60.0);
    assert_eq!(state.measure(Size::new(100.0, 90.0), true, Some(2), true, start + Duration::from_millis(450)), 90.0);
    assert_eq!(state.reserved, 90.0);
}

#[test]
fn unchanged_updates_do_not_restart_and_nested_reflow_is_not_animated_twice() {
    let start = Instant::now();
    let mut state = State::new(widget::Id::from("outer"));
    let size = Size::new(200.0, 120.0);
    state.measure(size, false, None, false, start);
    state.measure(size, true, None, false, start);
    for millis in [20, 80, 120, 199] {
        assert!(state.measure(size, true, None, false, start + Duration::from_millis(millis)) < 120.0);
    }
    assert_eq!(state.measure(size, true, None, false, start + Duration::from_millis(200)), 120.0);
    assert_eq!(state.measure(Size::new(200.0, 150.0), true, None, false, start + Duration::from_millis(220)), 150.0);
    assert!(!state.advance(start + Duration::from_millis(221)));
    assert_eq!(state.measure(Size::new(200.0, 0.0), true, None, false, start + Duration::from_millis(222)), 0.0);
}

#[derive(Default)]
struct ControlState { focused: bool, events: usize }
impl Focusable for ControlState {
    fn is_focused(&self) -> bool { self.focused }
    fn focus(&mut self) { self.focused = true; }
    fn unfocus(&mut self) { self.focused = false; }
}
struct Control { height: f32 }
impl<Theme> Widget<(), Theme, ()> for Control {
    fn size(&self) -> Size<Length> { Size::new(Length::Fill, Length::Shrink) }
    fn tag(&self) -> widget::tree::Tag { widget::tree::Tag::of::<ControlState>() }
    fn state(&self) -> widget::tree::State { widget::tree::State::new(ControlState::default()) }
    fn layout(&mut self, _: &mut widget::Tree, _: &(), limits: &layout::Limits) -> layout::Node {
        layout::Node::new(Size::new(limits.max().width, self.height))
    }
    fn draw(&self, _: &widget::Tree, _: &mut (), _: &Theme, _: &renderer::Style,
        _: Layout<'_>, _: mouse::Cursor, _: &Rectangle) {}
    fn operate(&mut self, tree: &mut widget::Tree, layout: Layout<'_>, _: &(), operation: &mut dyn Operation) {
        operation.focusable(None, layout.bounds(), tree.state.downcast_mut::<ControlState>());
    }
    fn update(&mut self, tree: &mut widget::Tree, event: &Event, layout: Layout<'_>, cursor: mouse::Cursor,
        _: &(), shell: &mut Shell<'_, ()>, _: &Rectangle) {
        tree.state.downcast_mut::<ControlState>().events += 1;
        if matches!(event, Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)))
            && cursor.is_over(layout.bounds()) {
            shell.publish(());
        }
    }
}

impl<'a, Theme: 'a> From<Control> for Element<'a, (), Theme, ()> {
    fn from(control: Control) -> Self { Element::new(control) }
}

fn section(visible: bool) -> HeightTransition<'static, (), (), ()> {
    HeightTransition { id: widget::Id::from("test.section"), visible, animate_resize: false,
        reservation: None, content: Element::new(Control { height: 120.0 }) }
}
fn limits() -> layout::Limits { layout::Limits::new(Size::ZERO, Size::new(200.0, 1000.0)) }

#[test]
fn natural_child_and_parent_reflow_retain_identity_and_sibling_position() {
    let mut element: Element<'_, (), (), ()> = iced::widget::column![section(false), Control { height: 20.0 }].into();
    let mut tree = widget::Tree::new(&element);
    tree.diff(&mut element);
    let first = element.as_widget_mut().layout(&mut tree, &(), &limits());
    assert_eq!(first.size().height, 20.0);
    tree.children[0].children[0].state.downcast_mut::<ControlState>().events = 7;
    let mut element: Element<'_, (), (), ()> = iced::widget::column![section(true), Control { height: 20.0 }].into();
    tree.diff(&mut element);
    element.as_widget_mut().layout(&mut tree, &(), &limits());
    let state = tree.children[0].state.downcast_mut::<State>();
    state.motion = animation(0.0).go(120.0, Instant::now() - Duration::from_millis(100));
    let middle = element.as_widget_mut().layout(&mut tree, &(), &limits());
    let children = middle.children();
    assert!(children[0].size().height > 0.0 && children[0].size().height < 120.0);
    assert_eq!(children[0].children()[0].size().height, 120.0);
    assert_eq!(children[1].bounds().y, children[0].size().height);
    assert_eq!(middle.size().height, children[0].size().height + 20.0);
    assert_eq!(tree.children[0].children[0].state.downcast_ref::<ControlState>().events, 7);
}

#[test]
fn hidden_and_clipped_controls_reject_input_and_focus_and_settled_motion_is_quiet() {
    struct FocusCount(usize);
    impl Operation for FocusCount {
        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation)) { operate(self); }
        fn focusable(&mut self, _: Option<&widget::Id>, _: Rectangle, _: &mut dyn Focusable) { self.0 += 1; }
    }
    for (visible, height, cursor_y, expected_messages, expected_focus) in [
        (false, 0.0, 80.0, 0, 0),
        (false, 120.0, 80.0, 0, 0),
        (true, 40.0, 80.0, 0, 0),
        (true, 40.0, 20.0, 1, 0),
        (true, 120.0, 80.0, 1, 1),
    ] {
        let mut element: Element<'_, (), (), ()> = section(visible).into();
        let mut tree = widget::Tree::new(&element);
        tree.diff(&mut element);
        element.as_widget_mut().layout(&mut tree, &(), &limits());
        let node = layout::Node::with_children(Size::new(200.0, height), vec![layout::Node::new(Size::new(200.0, 120.0))]);
        let mut count = FocusCount(0);
        element.as_widget_mut().operate(&mut tree, Layout::new(&node), &(), &mut count);
        assert_eq!(count.0, expected_focus);
        let mut messages = Vec::new();
        let mut shell = Shell::new(&window::Headless, iced_runtime::core::shell::Waker::new(|| {}), &mut messages);
        let viewport = Rectangle::new(Point::ORIGIN, Size::new(200.0, 1000.0));
        element.as_widget_mut().update(&mut tree, &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
            Layout::new(&node), mouse::Cursor::Available(Point::new(10.0, cursor_y)), &(), &mut shell, &viewport);
        assert_eq!(messages.len(), expected_messages);
    }
    let mut element: Element<'_, (), (), ()> = section(false).into();
    let mut tree = widget::Tree::new(&element);
    tree.diff(&mut element);
    let node = element.as_widget_mut().layout(&mut tree, &(), &limits());
    let mut messages = Vec::new();
    let mut shell = Shell::new(&window::Headless, iced_runtime::core::shell::Waker::new(|| {}), &mut messages);
    element.as_widget_mut().update(&mut tree, &Event::Window(window::Event::RedrawRequested(Instant::now())),
        Layout::new(&node), mouse::Cursor::Unavailable, &(), &mut shell, &Rectangle::new(Point::ORIGIN, Size::new(200.0, 1000.0)));
    assert_eq!(shell.redraw_request(), window::RedrawRequest::Wait);
    assert!(shell.is_layout_invalid().is_none());
    assert_eq!(tree.children[0].state.downcast_ref::<ControlState>().events, 0);
}

#[test]
fn page_scroller_keeps_absolute_offsets_after_wheel_and_scrollbar_drag_reflow() {
    struct Offset(f32);
    impl Operation for Offset {
        fn traverse(&mut self, _operate: &mut dyn FnMut(&mut dyn Operation)) {}
        fn scrollable(&mut self, id: Option<&widget::Id>, _: Rectangle, _: Rectangle,
            translation: Vector, _: &mut dyn widget::operation::Scrollable) {
            assert_eq!(id, Some(&widget::Id::from(crate::view::PAGE_SCROLL_ID)));
            self.0 = translation.y;
        }
    }
    fn page(height: f32) -> Element<'static, (), iced::Theme, ()> {
        iced::widget::scrollable(Control { height })
            .id(crate::view::PAGE_SCROLL_ID).width(200).height(200).into()
    }
    let mut element = page(1000.0);
    let mut tree = widget::Tree::new(&element);
    tree.diff(&mut element);
    let viewport = Rectangle::new(Point::ORIGIN, Size::new(200.0, 200.0));
    let mut node = element.as_widget_mut().layout(&mut tree, &(), &limits());
    for (index, events) in [
        vec![Event::Mouse(mouse::Event::WheelScrolled { delta: mouse::ScrollDelta::Pixels { x: 0.0, y: -100.0 } })],
        vec![
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
            Event::Mouse(mouse::Event::CursorMoved { position: Point::new(195.0, 130.0) }),
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)),
        ],
    ].into_iter().enumerate() {
        for event in events {
            let mut messages = Vec::new();
            let mut shell = Shell::new(&window::Headless, iced_runtime::core::shell::Waker::new(|| {}), &mut messages);
            element.as_widget_mut().update(&mut tree, &event, Layout::new(&node),
                mouse::Cursor::Available(Point::new(195.0, 130.0)), &(), &mut shell, &viewport);
        }
        let mut before = Offset(0.0);
        element.as_widget_mut().operate(&mut tree, Layout::new(&node), &(), &mut before);
        assert!(before.0 > 0.0);
        element = page(1800.0 + index as f32 * 800.0);
        tree.diff(&mut element);
        node = element.as_widget_mut().layout(&mut tree, &(), &limits());
        let mut after = Offset(0.0);
        element.as_widget_mut().operate(&mut tree, Layout::new(&node), &(), &mut after);
        assert_eq!(before.0, after.0);
    }
}

#[test]
fn redraw_replay_invalidates_once_and_hidden_ancestors_do_not_schedule_motion() {
    let mut element: Element<'_, (), (), ()> = section(true).into();
    let mut tree = widget::Tree::new(&element);
    tree.diff(&mut element);
    let node = element.as_widget_mut().layout(&mut tree, &(), &limits());
    let now = Instant::now();
    let state = tree.state.downcast_mut::<State>();
    state.motion = animation(0.0).go(120.0, now - Duration::from_millis(100));
    state.instant = now - Duration::from_millis(100);
    let viewport = Rectangle::new(Point::ORIGIN, Size::new(200.0, 1000.0));
    for expected_invalid in [true, false] {
        let mut messages = Vec::new();
        let mut shell = Shell::new(&window::Headless, iced_runtime::core::shell::Waker::new(|| {}), &mut messages);
        element.as_widget_mut().update(&mut tree, &Event::Window(window::Event::RedrawRequested(now)),
            Layout::new(&node), mouse::Cursor::Unavailable, &(), &mut shell, &viewport);
        assert_eq!(shell.is_layout_invalid().is_some(), expected_invalid);
        assert_eq!(shell.redraw_request(), window::RedrawRequest::NextFrame);
    }
    let mut messages = Vec::new();
    let mut shell = Shell::new(&window::Headless, iced_runtime::core::shell::Waker::new(|| {}), &mut messages);
    element.as_widget_mut().update(&mut tree,
        &Event::Window(window::Event::RedrawRequested(now + Duration::from_millis(10))),
        Layout::new(&node), mouse::Cursor::Unavailable, &(), &mut shell, &Rectangle::default());
    assert_eq!(shell.redraw_request(), window::RedrawRequest::Wait);
    assert!(shell.is_layout_invalid().is_none());
}

#[test]
fn outgoing_real_controls_retire_mouse_and_touch_presses_before_reopening() {
    use iced::advanced::renderer::Headless;

    fn control(visible: bool, nested: bool, checkbox: bool) -> crate::fluent_theme::Element<'static, bool> {
        let content: crate::fluent_theme::Element<'static, bool> = if checkbox {
            iced::widget::checkbox(false)
                .label("Retained option")
                .on_toggle(|value| value)
                .into()
        } else {
            iced::widget::button("Retained action").on_press(true).into()
        };
        let content = if nested {
            disclosure("test.retained.inner", true, content).into()
        } else {
            content
        };
        disclosure("test.retained.outer", visible, content).into()
    }

    fn pointer_event(pressed: bool, touch: bool) -> Event {
        if touch {
            let id = iced::touch::Finger(1);
            let position = Point::new(8.0, 8.0);
            Event::Touch(if pressed {
                iced::touch::Event::FingerPressed { id, position }
            } else {
                iced::touch::Event::FingerLifted { id, position }
            })
        } else {
            Event::Mouse(if pressed {
                mouse::Event::ButtonPressed(mouse::Button::Left)
            } else {
                mouse::Event::ButtonReleased(mouse::Button::Left)
            })
        }
    }

    fn send(
        element: &mut crate::fluent_theme::Element<'_, bool>,
        tree: &mut widget::Tree,
        node: &layout::Node,
        renderer: &iced::Renderer,
        event: Event,
    ) -> Vec<bool> {
        let mut messages = Vec::new();
        let mut shell = Shell::new(
            &window::Headless,
            iced_runtime::core::shell::Waker::new(|| {}),
            &mut messages,
        );
        element.as_widget_mut().update(
            tree,
            &event,
            Layout::new(node),
            mouse::Cursor::Available(Point::new(8.0, 8.0)),
            renderer,
            &mut shell,
            &Rectangle::new(Point::ORIGIN, Size::new(200.0, 1000.0)),
        );
        messages
    }

    let renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
        Default::default(), Some("wgpu"),
    )).expect("retained controls require the container renderer");
    // Checkbox mouse activation is release-based; button presses retain both
    // mouse and touch custody. Checkbox touch activation is immediate instead.
    for (checkbox, touch) in [(true, false), (false, false), (false, true)] {
        for nested in [false, true] {
            let mut element = control(true, nested, checkbox);
            let mut tree = widget::Tree::new(&element);
            tree.diff(&mut element);
            let initial = element.as_widget_mut().layout(&mut tree, &renderer, &limits());
            assert!(send(&mut element, &mut tree, &initial, &renderer, pointer_event(true, touch)).is_empty());

            element = control(false, nested, checkbox);
            tree.diff(&mut element);
            let hidden = element.as_widget_mut().layout(&mut tree, &renderer, &limits());
            assert!(send(&mut element, &mut tree, &hidden, &renderer,
                Event::Window(window::Event::RedrawRequested(Instant::now()))).is_empty());

            element = control(true, nested, checkbox);
            tree.diff(&mut element);
            // Sample the revealed endpoint without a sleep or rebuilding the
            // retained child. Geometry timing has separate component coverage.
            tree.state.downcast_mut::<State>().motion = animation(initial.size().height);
            let reopened = element.as_widget_mut().layout(&mut tree, &renderer, &limits());
            assert!(send(&mut element, &mut tree, &reopened, &renderer, pointer_event(false, touch)).is_empty());
            assert!(send(&mut element, &mut tree, &reopened, &renderer, pointer_event(true, touch)).is_empty());
            assert_eq!(send(&mut element, &mut tree, &reopened, &renderer, pointer_event(false, touch)), vec![true]);
        }
    }
}
