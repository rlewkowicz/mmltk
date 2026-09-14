use crate::generated::{
    PresentationSourceKind, WorkspaceMouse, WorkspaceMouseButton, WorkspaceMouseKind,
    WorkspacePoint, WorkspaceWheelUnit,
};
use iced::advanced::mouse::{Click, click};
use iced::{Event, Point, Rectangle, keyboard, mouse};
use std::sync::{Arc, Mutex};

#[derive(Clone, Default)]
pub struct Binding {
    connection: Arc<Mutex<Option<crate::transport_connection::Connection>>>,
    source: Option<PresentationSourceKind>,
    document_epoch: u64,
    radius: Option<u16>,
}

impl Binding {
    pub fn set_connection(&self, connection: Option<crate::transport_connection::Connection>) {
        *self.connection.lock().expect("workspace connection") = connection;
    }

    pub fn for_source(
        &self,
        source: PresentationSourceKind,
        document_epoch: u64,
        radius: Option<u16>,
    ) -> Self {
        Self {
            source: Some(source),
            document_epoch,
            radius,
            ..self.clone()
        }
    }

    pub fn send(&self, mut mouse: WorkspaceMouse) {
        let Some(source) = self.source else { return };
        mouse.source = source;
        mouse.documentepoch = self.document_epoch;
        if source == PresentationSourceKind::Annotation {
            if let Some(radius) = self.radius {
                mouse.brushradius = radius;
            }
        }
        if let Some(connection) = self
            .connection
            .lock()
            .expect("workspace connection")
            .as_mut()
        {
            let observed = crate::integration_control::reporting_enabled().then(|| mouse.clone());
            if connection.send_workspace_mouse(mouse).is_err() {
                connection.close();
            } else if let Some(observed) = observed {
                crate::integration_control::report_workspace_mouse(&observed);
            }
        }
    }

    pub fn cancel(&self) {
        self.send(record(WorkspaceMouseKind::Cancel, None));
    }
}

pub fn record(kind: WorkspaceMouseKind, point: Option<WorkspacePoint>) -> WorkspaceMouse {
    WorkspaceMouse {
        kind,
        point,
        ..crate::generated::default_workspace_mouse()
    }
}

#[derive(Default)]
pub(crate) struct Capture {
    inside: bool,
    pressed: Vec<mouse::Button>,
    modifiers: keyboard::Modifiers,
    last_click: Option<Click>,
    binding: Option<Binding>,
}

impl Capture {
    pub fn event(
        &mut self,
        binding: &Binding,
        event: &Event,
        bounds: Rectangle,
        cursor: mouse::Cursor,
        point: Option<WorkspacePoint>,
    ) {
        let owner = binding
            .source
            .map(|source| (source, binding.document_epoch));
        let previous = self.binding.as_ref().and_then(|binding| {
            binding
                .source
                .map(|source| (source, binding.document_epoch))
        });
        if previous != owner {
            if !self.pressed.is_empty() {
                if let Some(previous) = &self.binding {
                    previous.cancel();
                }
            }
            self.pressed.clear();
            self.inside = false;
            self.last_click = None;
        }
        self.binding = Some(binding.clone());
        if let Event::Keyboard(keyboard::Event::ModifiersChanged(modifiers)) = event {
            self.modifiers = *modifiers;
        }
        let over = cursor.is_over(bounds);
        let modifiers = self.modifiers;
        let send = |kind| {
            let mut value = record(kind, point.clone());
            value.modifiers = u8::from(modifiers.shift())
                | (u8::from(modifiers.control()) << 1)
                | (u8::from(modifiers.alt()) << 2)
                | (u8::from(modifiers.logo()) << 3);
            value
        };
        if matches!(event, Event::Mouse(mouse::Event::CursorMoved { .. })) && over != self.inside {
            binding.send(send(if over {
                WorkspaceMouseKind::Enter
            } else {
                WorkspaceMouseKind::Leave
            }));
            self.inside = over;
        }
        match event {
            Event::Mouse(mouse::Event::CursorEntered) if over && !self.inside => {
                binding.send(send(WorkspaceMouseKind::Enter));
                self.inside = true;
            }
            Event::Mouse(mouse::Event::CursorMoved { .. }) if over || !self.pressed.is_empty() => {
                binding.send(send(WorkspaceMouseKind::Motion));
            }
            Event::Mouse(mouse::Event::ButtonPressed(button)) if over => {
                let mut value = send(WorkspaceMouseKind::Press);
                let click = Click::new(
                    cursor.position().unwrap_or(Point::ORIGIN),
                    *button,
                    self.last_click,
                );
                value.clickcount = match click.kind() {
                    click::Kind::Single => 1,
                    click::Kind::Double => 2,
                    click::Kind::Triple => 3,
                };
                self.last_click = Some(click);
                (value.button, value.otherbutton) = button_value(*button);
                if !self.pressed.contains(button) {
                    self.pressed.push(*button);
                }
                binding.send(value);
            }
            Event::Mouse(mouse::Event::ButtonReleased(button))
                if over || self.pressed.contains(button) =>
            {
                let mut value = send(WorkspaceMouseKind::Release);
                (value.button, value.otherbutton) = button_value(*button);
                self.pressed.retain(|pressed| pressed != button);
                binding.send(value);
            }
            Event::Mouse(mouse::Event::WheelScrolled { delta }) if over => {
                let mut value = send(WorkspaceMouseKind::Wheel);
                let (unit, x, y) = match delta {
                    mouse::ScrollDelta::Lines { x, y } => (WorkspaceWheelUnit::Lines, *x, *y),
                    mouse::ScrollDelta::Pixels { x, y } => (WorkspaceWheelUnit::Pixels, *x, *y),
                };
                value.wheelunit = unit;
                value.wheel = WorkspacePoint { x, y };
                binding.send(value);
            }
            Event::Mouse(mouse::Event::CursorLeft) => {
                if self.inside {
                    binding.send(send(WorkspaceMouseKind::Leave));
                }
                self.inside = false;
            }
            Event::Window(iced::window::Event::Unfocused) => {
                binding.send(send(WorkspaceMouseKind::Cancel));
                self.pressed.clear();
                self.inside = false;
            }
            _ => {}
        }
    }
}

impl Drop for Capture {
    fn drop(&mut self) {
        if !self.pressed.is_empty() {
            if let Some(binding) = &self.binding {
                binding.cancel();
            }
        }
    }
}

fn button_value(button: mouse::Button) -> (WorkspaceMouseButton, u16) {
    match button {
        mouse::Button::Left => (WorkspaceMouseButton::Left, 0),
        mouse::Button::Right => (WorkspaceMouseButton::Right, 0),
        mouse::Button::Middle => (WorkspaceMouseButton::Middle, 0),
        mouse::Button::Back => (WorkspaceMouseButton::Back, 0),
        mouse::Button::Forward => (WorkspaceMouseButton::Forward, 0),
        mouse::Button::Other(value) => (WorkspaceMouseButton::Other, value),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn assert_output(connection: &crate::transport_connection::Connection, expected: Vec<Vec<u8>>) {
        let mut actual = Vec::new();
        connection
            .flush(|bytes| {
                actual.push(bytes.to_vec());
                Ok(())
            })
            .unwrap();
        assert_eq!(actual, expected);
    }

    #[test]
    fn native_defaults_and_annotation_radius_overrides_use_both_encoders() {
        let (connection, _capture) = crate::transport_connection::Connection::test_channel();
        let input = Binding::default();
        input.set_connection(Some(connection.clone()));
        let mut scratch = Vec::new();
        let mut encoded = Vec::new();
        let mut expected = Vec::new();
        for source in crate::generated::PRESENTATION_SOURCE_KIND_VALUES {
            if *source == PresentationSourceKind::None {
                continue;
            }
            for radius in [None, Some(7), Some(19)] {
                let binding = input.for_source(*source, 2, radius);
                binding.send(record(WorkspaceMouseKind::Motion, None));
                let mut mouse = crate::generated::default_workspace_mouse();
                mouse.source = *source;
                mouse.peerepoch = 1;
                mouse.documentepoch = 2;
                if *source == PresentationSourceKind::Annotation {
                    if let Some(radius) = radius {
                        mouse.brushradius = radius;
                    }
                }
                crate::generated::encode_workspace_mouse_into(&mouse, &mut scratch, &mut encoded)
                    .unwrap();
                let owned = crate::generated::encode_workspace_mouse(mouse)
                    .unwrap()
                    .encode()
                    .unwrap();
                assert_eq!(encoded, owned);
                expected.push(owned);
            }
        }
        assert_output(&connection, expected);
    }

    #[test]
    fn every_source_retains_all_mouse_kinds_without_an_image() {
        for source in crate::generated::PRESENTATION_SOURCE_KIND_VALUES {
            if *source == PresentationSourceKind::None {
                continue;
            }
            let (connection, _capture) = crate::transport_connection::Connection::test_channel();
            let binding = Binding::default().for_source(*source, 3, None);
            binding.set_connection(Some(connection.clone()));
            let mut expected = Vec::new();
            for kind in crate::generated::WORKSPACE_MOUSE_KIND_VALUES {
                let mut mouse = record(*kind, None);
                mouse.button = WorkspaceMouseButton::Other;
                mouse.otherbutton = u16::MAX;
                mouse.clickcount = 2;
                mouse.modifiers = 15;
                mouse.wheelunit = WorkspaceWheelUnit::Pixels;
                mouse.wheel = WorkspacePoint { x: -0.125, y: 0.25 };
                binding.send(mouse.clone());
                mouse.source = *source;
                mouse.peerepoch = 1;
                mouse.documentepoch = 3;
                expected.push(
                    crate::generated::encode_workspace_mouse(mouse)
                        .unwrap()
                        .encode()
                        .unwrap(),
                );
            }
            assert_output(&connection, expected);
        }
    }

    #[test]
    fn capture_keeps_release_outside_and_cancels_a_retired_widget() {
        let (connection, _channel) = crate::transport_connection::Connection::test_channel();
        let binding = Binding::default().for_source(PresentationSourceKind::Live, 0, None);
        binding.set_connection(Some(connection.clone()));
        let bounds = Rectangle::new(Point::ORIGIN, iced::Size::new(80.0, 80.0));
        let inside = mouse::Cursor::Available(Point::new(20.0, 30.0));
        let outside = mouse::Cursor::Available(Point::new(90.0, 30.0));
        let point = Some(WorkspacePoint { x: 1.25, y: 2.5 });
        let mut capture = Capture::default();
        capture.event(
            &binding,
            &Event::Mouse(mouse::Event::CursorEntered),
            bounds,
            inside,
            point.clone(),
        );
        capture.event(
            &binding,
            &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
            bounds,
            inside,
            point.clone(),
        );
        capture.event(
            &binding,
            &Event::Mouse(mouse::Event::CursorMoved {
                position: Point::new(90.0, 30.0),
            }),
            bounds,
            outside,
            None,
        );
        capture.event(
            &binding,
            &Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)),
            bounds,
            outside,
            None,
        );
        capture.event(
            &binding,
            &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Right)),
            bounds,
            inside,
            point.clone(),
        );
        drop(capture);
        let expected = [
            (
                WorkspaceMouseKind::Enter,
                point.clone(),
                WorkspaceMouseButton::Left,
                0,
            ),
            (
                WorkspaceMouseKind::Press,
                point.clone(),
                WorkspaceMouseButton::Left,
                1,
            ),
            (
                WorkspaceMouseKind::Leave,
                None,
                WorkspaceMouseButton::Left,
                0,
            ),
            (
                WorkspaceMouseKind::Motion,
                None,
                WorkspaceMouseButton::Left,
                0,
            ),
            (
                WorkspaceMouseKind::Release,
                None,
                WorkspaceMouseButton::Left,
                0,
            ),
            (
                WorkspaceMouseKind::Press,
                point,
                WorkspaceMouseButton::Right,
                1,
            ),
            (
                WorkspaceMouseKind::Cancel,
                None,
                WorkspaceMouseButton::Left,
                0,
            ),
        ]
        .into_iter()
        .map(|(kind, point, button, clicks)| {
            let mut value = record(kind, point);
            value.button = button;
            value.clickcount = clicks;
            value.source = PresentationSourceKind::Live;
            value.peerepoch = 1;
            crate::generated::encode_workspace_mouse(value)
                .unwrap()
                .encode()
                .unwrap()
        })
        .collect::<Vec<_>>();
        assert_output(&connection, expected);
    }

    #[test]
    fn captured_buttons_and_wheel_keep_fractional_image_coordinates() {
        let (connection, _capture) = crate::transport_connection::Connection::test_channel();
        let binding = Binding::default().for_source(PresentationSourceKind::Predict, 0, None);
        binding.set_connection(Some(connection.clone()));
        let mut capture = Capture::default();
        let bounds = Rectangle::new(Point::ORIGIN, iced::Size::new(80.0, 80.0));
        let cursor = mouse::Cursor::Available(Point::new(20.0, 30.0));
        let point = Some(WorkspacePoint { x: 1.25, y: 2.5 });
        let mut expected = Vec::new();
        for button in [
            mouse::Button::Left,
            mouse::Button::Right,
            mouse::Button::Middle,
            mouse::Button::Back,
            mouse::Button::Forward,
            mouse::Button::Other(256),
        ] {
            for pressed in [true, false] {
                let event = Event::Mouse(if pressed {
                    mouse::Event::ButtonPressed(button)
                } else {
                    mouse::Event::ButtonReleased(button)
                });
                capture.event(&binding, &event, bounds, cursor, point.clone());
                let mut mouse = record(
                    if pressed {
                        WorkspaceMouseKind::Press
                    } else {
                        WorkspaceMouseKind::Release
                    },
                    point.clone(),
                );
                (mouse.button, mouse.otherbutton) = button_value(button);
                mouse.clickcount = u8::from(pressed);
                mouse.source = PresentationSourceKind::Predict;
                mouse.peerepoch = 1;
                expected.push(
                    crate::generated::encode_workspace_mouse(mouse)
                        .unwrap()
                        .encode()
                        .unwrap(),
                );
            }
        }
        for delta in [
            mouse::ScrollDelta::Lines { x: -0.5, y: 1.25 },
            mouse::ScrollDelta::Pixels { x: 0.125, y: -0.25 },
        ] {
            capture.event(
                &binding,
                &Event::Mouse(mouse::Event::WheelScrolled { delta }),
                bounds,
                cursor,
                point.clone(),
            );
            let mut mouse = record(WorkspaceMouseKind::Wheel, point.clone());
            let (unit, x, y) = match delta {
                mouse::ScrollDelta::Lines { x, y } => (WorkspaceWheelUnit::Lines, x, y),
                mouse::ScrollDelta::Pixels { x, y } => (WorkspaceWheelUnit::Pixels, x, y),
            };
            mouse.wheelunit = unit;
            mouse.wheel = WorkspacePoint { x, y };
            mouse.source = PresentationSourceKind::Predict;
            mouse.peerepoch = 1;
            expected.push(
                crate::generated::encode_workspace_mouse(mouse)
                    .unwrap()
                    .encode()
                    .unwrap(),
            );
        }
        assert_output(&connection, expected);
    }
}
