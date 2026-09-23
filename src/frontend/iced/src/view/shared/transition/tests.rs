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
    assert_eq!(
        state.measure(size, false, None, true, middle + Duration::from_millis(200)),
        0.0
    );
    assert!(!state.advance(middle + Duration::from_millis(201)));

    let mut state = State::new(widget::Id::from("reserved"));
    assert_eq!(state.measure(size, true, Some(1), true, start), 120.0);
    assert_eq!(
        state.measure(Size::new(200.0, 40.0), true, Some(1), true, start),
        120.0
    );
    assert_eq!(
        state.measure(Size::new(200.0, 180.0), true, Some(1), true, start),
        120.0
    );
    assert_eq!(
        state.measure(
            Size::new(200.0, 40.0),
            true,
            Some(1),
            true,
            start + Duration::from_millis(200)
        ),
        180.0
    );
    // The terminal body replaces content; only the scalar reservation survived.
    assert_eq!(
        state.measure(
            Size::new(200.0, 24.0),
            true,
            None,
            true,
            start + Duration::from_millis(200)
        ),
        180.0
    );
    assert_eq!(
        state.measure(
            Size::new(200.0, 24.0),
            true,
            None,
            true,
            start + Duration::from_millis(400)
        ),
        24.0
    );
    state.measure(
        Size::new(200.0, 60.0),
        true,
        Some(2),
        true,
        start + Duration::from_millis(400),
    );
    assert_eq!(state.reserved, 60.0);
    assert_eq!(
        state.measure(
            Size::new(100.0, 90.0),
            true,
            Some(2),
            true,
            start + Duration::from_millis(450)
        ),
        90.0
    );
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
        assert!(
            state.measure(
                size,
                true,
                None,
                false,
                start + Duration::from_millis(millis)
            ) < 120.0
        );
    }
    assert_eq!(
        state.measure(size, true, None, false, start + Duration::from_millis(200)),
        120.0
    );
    assert_eq!(
        state.measure(
            Size::new(200.0, 150.0),
            true,
            None,
            false,
            start + Duration::from_millis(220)
        ),
        150.0
    );
    assert!(!state.advance(start + Duration::from_millis(221)));
    assert_eq!(
        state.measure(
            Size::new(200.0, 0.0),
            true,
            None,
            false,
            start + Duration::from_millis(222)
        ),
        0.0
    );
}

#[derive(Default)]
struct ControlState {
    focused: bool,
    events: usize,
}
impl Focusable for ControlState {
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
struct Control {
    height: f32,
}
impl<Theme, Renderer: renderer::Renderer> Widget<(), Theme, Renderer> for Control {
    fn size(&self) -> Size<Length> {
        Size::new(Length::Fill, Length::Shrink)
    }
    fn tag(&self) -> widget::tree::Tag {
        widget::tree::Tag::of::<ControlState>()
    }
    fn state(&self) -> widget::tree::State {
        widget::tree::State::new(ControlState::default())
    }
    fn layout(
        &mut self,
        _: &mut widget::Tree,
        _: &Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        layout::Node::new(Size::new(limits.max().width, self.height))
    }
    fn draw(
        &self,
        _: &widget::Tree,
        _: &mut Renderer,
        _: &Theme,
        _: &renderer::Style,
        _: Layout<'_>,
        _: mouse::Cursor,
        _: &Rectangle,
    ) {
    }
    fn operate(
        &mut self,
        tree: &mut widget::Tree,
        layout: Layout<'_>,
        _: &Renderer,
        operation: &mut dyn Operation,
    ) {
        operation.focusable(
            None,
            layout.bounds(),
            tree.state.downcast_mut::<ControlState>(),
        );
    }
    fn update(
        &mut self,
        tree: &mut widget::Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _: &Renderer,
        shell: &mut Shell<'_, ()>,
        _: &Rectangle,
    ) {
        tree.state.downcast_mut::<ControlState>().events += 1;
        if matches!(
            event,
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left))
        ) && cursor.is_over(layout.bounds())
        {
            shell.publish(());
        }
    }
}

impl<'a, Theme: 'a, Renderer: renderer::Renderer + 'a> From<Control>
    for Element<'a, (), Theme, Renderer>
{
    fn from(control: Control) -> Self {
        Element::new(control)
    }
}

fn section(visible: bool) -> HeightTransition<'static, (), (), ()> {
    HeightTransition {
        id: widget::Id::from("test.section"),
        visible,
        animate_resize: false,
        reservation: None,
        content: Element::new(Control { height: 120.0 }),
    }
}
fn limits() -> layout::Limits {
    layout::Limits::new(Size::ZERO, Size::new(200.0, 1000.0))
}

#[test]
fn natural_child_and_parent_reflow_retain_identity_and_sibling_position() {
    let mut element: Element<'_, (), (), ()> =
        iced::widget::column![section(false), Control { height: 20.0 }].into();
    let mut tree = widget::Tree::new(&element);
    tree.diff(&mut element);
    let first = element.as_widget_mut().layout(&mut tree, &(), &limits());
    assert_eq!(first.size().height, 20.0);
    tree.children[0].children[0]
        .state
        .downcast_mut::<ControlState>()
        .events = 7;
    let mut element: Element<'_, (), (), ()> =
        iced::widget::column![section(true), Control { height: 20.0 }].into();
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
    assert_eq!(
        tree.children[0].children[0]
            .state
            .downcast_ref::<ControlState>()
            .events,
        7
    );
}

#[test]
fn hidden_and_clipped_controls_reject_input_and_focus_and_settled_motion_is_quiet() {
    struct FocusCount(usize);
    impl Operation for FocusCount {
        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation)) {
            operate(self);
        }
        fn focusable(&mut self, _: Option<&widget::Id>, _: Rectangle, _: &mut dyn Focusable) {
            self.0 += 1;
        }
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
        let node = layout::Node::with_children(
            Size::new(200.0, height),
            vec![layout::Node::new(Size::new(200.0, 120.0))],
        );
        let mut count = FocusCount(0);
        element
            .as_widget_mut()
            .operate(&mut tree, Layout::new(&node), &(), &mut count);
        assert_eq!(count.0, expected_focus);
        let mut messages = Vec::new();
        let mut shell = Shell::new(
            &window::Headless,
            iced_runtime::core::shell::Waker::new(|| {}),
            &mut messages,
        );
        let viewport = Rectangle::new(Point::ORIGIN, Size::new(200.0, 1000.0));
        element.as_widget_mut().update(
            &mut tree,
            &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
            Layout::new(&node),
            mouse::Cursor::Available(Point::new(10.0, cursor_y)),
            &(),
            &mut shell,
            &viewport,
        );
        assert_eq!(messages.len(), expected_messages);
    }
    let mut element: Element<'_, (), (), ()> = section(false).into();
    let mut tree = widget::Tree::new(&element);
    tree.diff(&mut element);
    let node = element.as_widget_mut().layout(&mut tree, &(), &limits());
    let mut messages = Vec::new();
    let mut shell = Shell::new(
        &window::Headless,
        iced_runtime::core::shell::Waker::new(|| {}),
        &mut messages,
    );
    element.as_widget_mut().update(
        &mut tree,
        &Event::Window(window::Event::RedrawRequested(Instant::now())),
        Layout::new(&node),
        mouse::Cursor::Unavailable,
        &(),
        &mut shell,
        &Rectangle::new(Point::ORIGIN, Size::new(200.0, 1000.0)),
    );
    assert_eq!(shell.redraw_request(), window::RedrawRequest::Wait);
    assert!(shell.is_layout_invalid().is_none());
    assert_eq!(
        tree.children[0].state.downcast_ref::<ControlState>().events,
        0
    );
}

#[test]
fn page_scroller_keeps_absolute_offsets_after_wheel_and_scrollbar_drag_reflow() {
    use crate::test_support::Renderer;

    struct Offset(f32);
    impl Operation for Offset {
        fn traverse(&mut self, _operate: &mut dyn FnMut(&mut dyn Operation)) {}
        fn scrollable(
            &mut self,
            id: Option<&widget::Id>,
            _: Rectangle,
            _: Rectangle,
            translation: Vector,
            _: &mut dyn widget::operation::Scrollable,
        ) {
            assert_eq!(id, Some(&widget::Id::from(crate::view::PAGE_SCROLL_ID)));
            self.0 = translation.y;
        }
    }
    fn page(height: f32) -> Element<'static, (), iced::Theme, Renderer> {
        iced::widget::scrollable(Control { height })
            .id(crate::view::PAGE_SCROLL_ID)
            .width(200)
            .height(200)
            .into()
    }
    let renderer = Renderer;
    let mut element = page(1000.0);
    let mut tree = widget::Tree::new(&element);
    tree.diff(&mut element);
    let viewport = Rectangle::new(Point::ORIGIN, Size::new(200.0, 200.0));
    let mut node = element
        .as_widget_mut()
        .layout(&mut tree, &renderer, &limits());
    for (index, events) in [
        vec![Event::Mouse(mouse::Event::WheelScrolled {
            delta: mouse::ScrollDelta::Pixels { x: 0.0, y: -100.0 },
        })],
        vec![
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
            Event::Mouse(mouse::Event::CursorMoved {
                position: Point::new(195.0, 130.0),
            }),
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)),
        ],
    ]
    .into_iter()
    .enumerate()
    {
        for event in events {
            let mut messages = Vec::new();
            let mut shell = Shell::new(
                &window::Headless,
                iced_runtime::core::shell::Waker::new(|| {}),
                &mut messages,
            );
            element.as_widget_mut().update(
                &mut tree,
                &event,
                Layout::new(&node),
                mouse::Cursor::Available(Point::new(195.0, 130.0)),
                &renderer,
                &mut shell,
                &viewport,
            );
        }
        let mut before = Offset(0.0);
        element
            .as_widget_mut()
            .operate(&mut tree, Layout::new(&node), &renderer, &mut before);
        assert!(before.0 > 0.0);
        element = page(1800.0 + index as f32 * 800.0);
        tree.diff(&mut element);
        node = element
            .as_widget_mut()
            .layout(&mut tree, &renderer, &limits());
        let mut after = Offset(0.0);
        element
            .as_widget_mut()
            .operate(&mut tree, Layout::new(&node), &renderer, &mut after);
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
        let mut shell = Shell::new(
            &window::Headless,
            iced_runtime::core::shell::Waker::new(|| {}),
            &mut messages,
        );
        element.as_widget_mut().update(
            &mut tree,
            &Event::Window(window::Event::RedrawRequested(now)),
            Layout::new(&node),
            mouse::Cursor::Unavailable,
            &(),
            &mut shell,
            &viewport,
        );
        assert_eq!(shell.is_layout_invalid().is_some(), expected_invalid);
        assert_eq!(shell.redraw_request(), window::RedrawRequest::NextFrame);
    }
    let mut messages = Vec::new();
    let mut shell = Shell::new(
        &window::Headless,
        iced_runtime::core::shell::Waker::new(|| {}),
        &mut messages,
    );
    element.as_widget_mut().update(
        &mut tree,
        &Event::Window(window::Event::RedrawRequested(
            now + Duration::from_millis(10),
        )),
        Layout::new(&node),
        mouse::Cursor::Unavailable,
        &(),
        &mut shell,
        &Rectangle::default(),
    );
    assert_eq!(shell.redraw_request(), window::RedrawRequest::Wait);
    assert!(shell.is_layout_invalid().is_none());
}

fn real_control(checkbox: bool) -> crate::fluent_theme::Element<'static, bool> {
    if checkbox {
        iced::widget::checkbox(false)
            .label("Retained option")
            .on_toggle(|value| value)
            .into()
    } else {
        iced::widget::button("Retained action")
            .on_press(true)
            .into()
    }
}

fn retained(
    visible: bool,
    nested: bool,
    content: crate::fluent_theme::Element<'static, bool>,
) -> crate::fluent_theme::Element<'static, bool> {
    let content = if nested {
        disclosure("test.retained.inner", true, content).into()
    } else {
        content
    };
    disclosure("test.retained.outer", visible, content).into()
}

fn pointer_event(pressed: bool, touch: bool, position: Point) -> Event {
    if touch {
        let id = iced::touch::Finger(1);
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

fn renderer() -> iced::Renderer {
    use iced::advanced::renderer::Headless;
    iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
        Default::default(),
        Some("wgpu"),
    ))
    .expect("retained controls require the container renderer")
}

struct InputHarness<'a> {
    element: crate::fluent_theme::Element<'static, bool>,
    tree: widget::Tree,
    node: layout::Node,
    renderer: &'a iced::Renderer,
}

impl<'a> InputHarness<'a> {
    fn new(
        mut element: crate::fluent_theme::Element<'static, bool>,
        renderer: &'a iced::Renderer,
    ) -> Self {
        let mut tree = widget::Tree::new(&element);
        tree.diff(&mut element);
        let node = element
            .as_widget_mut()
            .layout(&mut tree, renderer, &limits());
        Self {
            element,
            tree,
            node,
            renderer,
        }
    }

    fn replace(&mut self, element: crate::fluent_theme::Element<'static, bool>) {
        self.element = element;
        self.tree.diff(&mut self.element);
        self.layout();
    }

    fn layout(&mut self) {
        self.node = self
            .element
            .as_widget_mut()
            .layout(&mut self.tree, self.renderer, &limits());
    }

    fn send(&mut self, event: Event, cursor: mouse::Cursor) -> Vec<bool> {
        let mut messages = Vec::new();
        let mut shell = Shell::new(
            &window::Headless,
            iced_runtime::core::shell::Waker::new(|| {}),
            &mut messages,
        );
        self.element.as_widget_mut().update(
            &mut self.tree,
            &event,
            Layout::new(&self.node),
            cursor,
            self.renderer,
            &mut shell,
            &Rectangle::new(Point::ORIGIN, Size::new(200.0, 1000.0)),
        );
        messages
    }

    fn scroll(&mut self, offset: Vector) {
        use iced::advanced::widget::operation::scrollable::{AbsoluteOffset, scroll_to};
        for id in ["test.horizontal", "test.vertical"] {
            self.element.as_widget_mut().operate(
                &mut self.tree,
                Layout::new(&self.node),
                self.renderer,
                &mut scroll_to(
                    widget::Id::from(id),
                    AbsoluteOffset {
                        x: Some(offset.x),
                        y: Some(offset.y),
                    },
                ),
            );
        }
    }

    fn text_cursor(&mut self) -> iced::widget::text_input::Cursor {
        type TextState = iced::widget::text_input::State<
            <iced::Renderer as iced::advanced::text::Renderer>::Paragraph,
        >;
        tree_with_tag(&mut self.tree, widget::tree::Tag::of::<TextState>())
            .unwrap()
            .state
            .downcast_ref::<TextState>()
            .cursor()
    }
}

fn tree_with_tag(tree: &mut widget::Tree, tag: widget::tree::Tag) -> Option<&mut widget::Tree> {
    if tree.tag == tag {
        Some(tree)
    } else {
        tree.children
            .iter_mut()
            .find_map(|child| tree_with_tag(child, tag))
    }
}

fn scrolled(
    content: crate::fluent_theme::Element<'static, bool>,
    nested: bool,
    horizontal: bool,
) -> crate::fluent_theme::Element<'static, bool> {
    use iced::widget::{Space, column, row, scrollable};
    let body = column![
        Space::new().height(300),
        row![
            Space::new().width(if horizontal { 300 } else { 0 }),
            retained(true, nested, content)
        ],
        Space::new().height(600),
    ]
    .width(600);
    scrollable(scrollable(body).id("test.vertical").width(600).height(190))
        .id("test.horizontal")
        .direction(iced::widget::scrollable::Direction::Horizontal(
            Default::default(),
        ))
        .width(200)
        .height(200)
        .into()
}

#[test]
fn outgoing_real_controls_retire_mouse_and_touch_presses_before_reopening() {
    let renderer = renderer();
    let position = Point::new(8.0, 8.0);
    let cursor = mouse::Cursor::Available(position);
    // Checkbox mouse activation is release-based; button presses retain both
    // mouse and touch custody. Checkbox touch activation is immediate instead.
    for (checkbox, touch) in [(true, false), (false, false), (false, true)] {
        for nested in [false, true] {
            let mut input =
                InputHarness::new(retained(true, nested, real_control(checkbox)), &renderer);
            let height = input.node.size().height;
            assert!(
                input
                    .send(pointer_event(true, touch, position), cursor)
                    .is_empty()
            );

            input.replace(retained(false, nested, real_control(checkbox)));
            assert!(
                input
                    .send(
                        Event::Window(window::Event::RedrawRequested(Instant::now())),
                        cursor
                    )
                    .is_empty()
            );

            input.replace(retained(true, nested, real_control(checkbox)));
            // Sample the revealed endpoint without a sleep or rebuilding the
            // retained child. Geometry timing has separate component coverage.
            input.tree.state.downcast_mut::<State>().motion = animation(height);
            input.layout();
            assert!(
                input
                    .send(pointer_event(false, touch, position), cursor)
                    .is_empty()
            );
            assert!(
                input
                    .send(pointer_event(true, touch, position), cursor)
                    .is_empty()
            );
            assert_eq!(
                input.send(pointer_event(false, touch, position), cursor),
                vec![true]
            );
        }
    }
}

#[test]
fn composed_scrollers_admit_visible_touch_and_settle_offscreen_control_releases() {
    let renderer = renderer();
    for nested in [false, true] {
        for horizontal in [false, true] {
            let offset = Vector::new(if horizontal { 200.0 } else { 0.0 }, 200.0);
            let position = Point::new(if horizontal { 108.0 } else { 8.0 }, 108.0);
            let cursor = mouse::Cursor::Available(position);
            for checkbox in [false, true] {
                for touch in [false, true] {
                    let mut input = InputHarness::new(
                        scrolled(real_control(checkbox), nested, horizontal),
                        &renderer,
                    );
                    input.scroll(offset);
                    for unavailable in [
                        mouse::Cursor::Unavailable,
                        mouse::Cursor::Levitating(position),
                    ] {
                        assert!(
                            input
                                .send(pointer_event(true, touch, position), unavailable)
                                .is_empty()
                        );
                        assert!(
                            input
                                .send(pointer_event(false, touch, position), cursor)
                                .is_empty()
                        );
                    }
                    let pressed = input.send(pointer_event(true, touch, position), cursor);
                    // Checkbox touch activation occurs on press; all other
                    // combinations complete only on an admitted release.
                    assert_eq!(
                        pressed,
                        if checkbox && touch {
                            vec![true]
                        } else {
                            vec![]
                        }
                    );
                    input.scroll(Vector::new(offset.x, 500.0));
                    assert!(
                        input
                            .send(pointer_event(false, touch, position), cursor)
                            .is_empty()
                    );
                    assert!(
                        input
                            .send(pointer_event(true, touch, position), cursor)
                            .is_empty()
                    );
                    input.scroll(offset);
                    assert!(
                        input
                            .send(pointer_event(false, touch, position), cursor)
                            .is_empty()
                    );
                    let pressed = input.send(pointer_event(true, touch, position), cursor);
                    let released = input.send(pointer_event(false, touch, position), cursor);
                    assert_eq!([pressed, released].concat(), vec![true]);

                    input.send(pointer_event(true, touch, position), cursor);
                    input.scroll(Vector::new(offset.x, 500.0));
                    let loss = if checkbox && !touch {
                        Event::Window(window::Event::Unfocused)
                    } else {
                        Event::Touch(iced::touch::Event::FingerLost {
                            id: iced::touch::Finger(1),
                            position,
                        })
                    };
                    assert!(input.send(loss, mouse::Cursor::Unavailable).is_empty());
                    input.scroll(offset);
                    assert!(
                        input
                            .send(pointer_event(false, touch, position), cursor)
                            .is_empty()
                    );
                }
            }
        }
    }
}

#[test]
fn scrolled_text_selection_obeys_reveal_and_cursor_availability_and_retires_on_release() {
    let renderer = renderer();
    let value = iced::widget::text_input::Value::new("A retained editable value");
    for nested in [false, true] {
        for touch in [false, true] {
            for lost in [false, true] {
                let text = iced::widget::text_input("", "A retained editable value")
                    .width(160)
                    .on_input(|_| true);
                let mut input = InputHarness::new(scrolled(text.into(), nested, false), &renderer);
                let start = Point::new(6.0, 108.0);
                let end = Point::new(120.0, 108.0);
                let moved = |position| {
                    if touch {
                        Event::Touch(iced::touch::Event::FingerMoved {
                            id: iced::touch::Finger(1),
                            position,
                        })
                    } else {
                        Event::Mouse(mouse::Event::CursorMoved { position })
                    }
                };
                input.scroll(Vector::new(0.0, 200.0));
                input.send(
                    pointer_event(true, touch, start),
                    mouse::Cursor::Available(start),
                );
                let initial = input.text_cursor();
                for cursor in [mouse::Cursor::Unavailable, mouse::Cursor::Levitating(end)] {
                    input.send(moved(end), cursor);
                    assert_eq!(input.text_cursor(), initial);
                }
                input.send(moved(end), mouse::Cursor::Available(end));
                let selected = input.text_cursor();
                assert!(selected.selection(&value).is_some());

                // Sample an in-progress reveal using its retained animation;
                // events still pass through both real scrollers and disclosures.
                let state = tree_with_tag(&mut input.tree, widget::tree::Tag::of::<State>())
                    .unwrap()
                    .state
                    .downcast_mut::<State>();
                let height = state.motion.value();
                state.motion =
                    animation(0.0).go(height, Instant::now() - Duration::from_millis(100));
                input.layout();
                let clipped = Point::new(20.0, 100.0 + height - 1.0);
                input.send(moved(clipped), mouse::Cursor::Available(clipped));
                assert_eq!(input.text_cursor(), selected);

                input.scroll(Vector::new(0.0, 500.0));
                let release = if lost {
                    Event::Touch(iced::touch::Event::FingerLost {
                        id: iced::touch::Finger(1),
                        position: end,
                    })
                } else {
                    pointer_event(false, touch, end)
                };
                assert!(
                    input
                        .send(release, mouse::Cursor::Available(end))
                        .is_empty()
                );
                input.scroll(Vector::new(0.0, 200.0));
                tree_with_tag(&mut input.tree, widget::tree::Tag::of::<State>())
                    .unwrap()
                    .state
                    .downcast_mut::<State>()
                    .motion = animation(height);
                input.layout();
                input.send(moved(start), mouse::Cursor::Available(start));
                assert_eq!(input.text_cursor(), selected);
                // A different click location avoids the double-click gesture.
                input.send(
                    pointer_event(true, touch, end),
                    mouse::Cursor::Available(end),
                );
                let fresh = input.text_cursor();
                input.send(moved(start), mouse::Cursor::Available(start));
                assert_ne!(input.text_cursor(), fresh);
            }
        }
    }
}
