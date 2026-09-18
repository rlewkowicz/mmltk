use super::{Surface, gallery};
use iced::widget::shader;
use iced::{Event, Point, Rectangle, mouse};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Placement {
    Contain,
    FixedGrid {
        columns: u32,
        rows: u32,
    },
    GalleryGrid {
        columns: u32,
        rows: u32,
        row_capacity: u32,
        row_origin: u32,
        first_row: u32,
    },
}

impl Placement {
    pub(super) fn logical_extent(self, physical: (u32, u32)) -> (u32, u32) {
        match self {
            Self::GalleryGrid { columns, rows, .. } if columns != 0 => {
                (physical.0, physical.0 / columns * rows)
            }
            _ => physical,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct SurfaceSample {
    pub width: u32,
    pub height: u32,
    pub x: u32,
    pub y: u32,
    pub content_x: f32,
    pub content_y: f32,
    pub pressed: bool,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SurfaceGesture {
    pub kind: SurfaceGestureKind,
    pub sample: SurfaceSample,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SurfaceGestureKind {
    Pointer,
    End,
    Cancel,
    Viewport,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct ViewportOwner {
    pub(super) zoom: f32,
    pub(super) pan_x: f32,
    pub(super) pan_y: f32,
    pub(super) pan_origin: Option<Point>,
    pub(super) pointer_active: bool,
    pub(super) last_pointer_sample: Option<SurfaceSample>,
    pub(super) content_session: Option<(u64, u64, u64)>,
}

impl Default for ViewportOwner {
    fn default() -> Self {
        Self {
            zoom: 1.0,
            pan_x: 0.0,
            pan_y: 0.0,
            pan_origin: None,
            pointer_active: false,
            last_pointer_sample: None,
            content_session: None,
        }
    }
}

impl ViewportOwner {
    fn transform(self) -> ViewTransform {
        ViewTransform {
            zoom: self.zoom,
            pan_x: self.pan_x,
            pan_y: self.pan_y,
        }
    }

    pub(super) fn transform_for(self, surface: Surface) -> ViewTransform {
        match surface.transform_identity() {
            Some(session) if self.content_session != Some(session) => ViewTransform::FIT,
            _ => self.transform(),
        }
    }

    pub(super) fn synchronize_source(&mut self, surface: Surface) -> Option<SurfaceGesture> {
        let Some(session) = surface.transform_identity() else {
            return None;
        };
        if self.content_session == Some(session) {
            return None;
        }
        let cancellation = self
            .pointer_active
            .then(|| self.last_pointer_sample)
            .flatten()
            .map(|mut sample| {
                sample.pressed = false;
                SurfaceGesture {
                    kind: SurfaceGestureKind::Cancel,
                    sample,
                }
            });
        self.zoom = 1.0;
        self.pan_x = 0.0;
        self.pan_y = 0.0;
        self.pan_origin = None;
        self.pointer_active = false;
        self.last_pointer_sample = None;
        self.content_session = Some(session);
        cancellation
    }

    fn finish_pointer(&mut self, sample: Option<SurfaceSample>) -> Option<SurfaceGesture> {
        if !std::mem::take(&mut self.pointer_active) {
            return None;
        }
        let (kind, mut sample) = match sample {
            Some(sample) => (SurfaceGestureKind::Pointer, sample),
            None => (SurfaceGestureKind::End, self.last_pointer_sample?),
        };
        sample.pressed = false;
        self.last_pointer_sample = None;
        Some(SurfaceGesture { kind, sample })
    }

    pub(super) fn update<Message>(
        &mut self,
        event: &Event,
        bounds: Rectangle,
        cursor: mouse::Cursor,
        surface: Surface,
        placement: Placement,
        publish: Option<&dyn Fn(SurfaceGesture) -> shader::Action<Message>>,
    ) -> Option<shader::Action<Message>> {
        if let Some(gesture) = self.synchronize_source(surface) {
            return Some(publish.map_or_else(shader::Action::capture, |publish| {
                publish(gesture).and_capture()
            }));
        }
        match event {
            Event::Mouse(mouse::Event::WheelScrolled { delta })
                if placement == Placement::Contain && cursor.is_over(bounds) =>
            {
                let amount = match delta {
                    mouse::ScrollDelta::Lines { y, .. } => *y * 0.12,
                    mouse::ScrollDelta::Pixels { y, .. } => *y * 0.002,
                };
                self.zoom = (self.zoom * (1.0 + amount)).clamp(0.25, 16.0);
                Some(shader::Action::request_redraw().and_capture())
            }
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left))
                if cursor.is_over(bounds) =>
            {
                let publish = publish?;
                let sample = self.sample(bounds, cursor, surface, placement, true);
                self.pointer_active = sample.is_some();
                self.last_pointer_sample = sample;
                Some(sample.map_or_else(shader::Action::capture, |sample| {
                    publish(SurfaceGesture {
                        kind: SurfaceGestureKind::Pointer,
                        sample,
                    })
                    .and_capture()
                }))
            }
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Right))
                if placement == Placement::Contain && cursor.is_over(bounds) =>
            {
                self.pan_origin = cursor.position();
                Some(shader::Action::capture())
            }
            Event::Mouse(mouse::Event::CursorMoved { .. }) => {
                // Iced supplies this event's cursor in the child's coordinate
                // space, including scroll/ancestor transforms and overlay gating.
                if let Some(prior) = self.pan_origin {
                    let position = cursor.position()?;
                    self.pan_x += position.x - prior.x;
                    self.pan_y += position.y - prior.y;
                    self.pan_origin = Some(position);
                    Some(shader::Action::request_redraw().and_capture())
                } else if self.pointer_active {
                    let Some(publish) = publish else {
                        self.pointer_active = false;
                        self.last_pointer_sample = None;
                        return Some(shader::Action::capture());
                    };
                    let sample = self.sample(bounds, cursor, surface, placement, true);
                    if sample.is_some() {
                        self.last_pointer_sample = sample;
                    }
                    Some(sample.map_or_else(shader::Action::capture, |sample| {
                        publish(SurfaceGesture {
                            kind: SurfaceGestureKind::Pointer,
                            sample,
                        })
                        .and_capture()
                    }))
                } else {
                    let publish = publish?;
                    self.sample(bounds, cursor, surface, placement, false)
                        .map(|sample| {
                            publish(SurfaceGesture {
                                kind: SurfaceGestureKind::Viewport,
                                sample,
                            })
                        })
                }
            }
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left))
                if self.pointer_active =>
            {
                let sample = self.sample(bounds, cursor, surface, placement, false);
                Some(match (publish, self.finish_pointer(sample)) {
                    (Some(publish), Some(gesture)) => publish(gesture).and_capture(),
                    _ => shader::Action::capture(),
                })
            }
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Right))
                if self.pan_origin.take().is_some() =>
            {
                Some(shader::Action::capture())
            }
            _ => None,
        }
    }

    pub(super) fn sample(
        self,
        bounds: Rectangle,
        cursor: mouse::Cursor,
        mut surface: Surface,
        mut placement: Placement,
        pressed: bool,
    ) -> Option<SurfaceSample> {
        let point = cursor.position_in(bounds)?;
        let mut origin_y = 0.0;
        if matches!(placement, Placement::GalleryGrid { .. }) {
            let (shown, snapshot) = gallery::displayed()?;
            origin_y = gallery::row_offset(placement, &snapshot, bounds.width);
            surface = shown;
            placement = gallery::placement(&snapshot);
        }
        surface.frame?;
        let geometry = placement_geometry(
            Rectangle {
                x: 0.0,
                y: origin_y,
                width: bounds.width,
                height: bounds.height,
            },
            surface.content_extent(),
            placement,
            self.transform_for(surface),
        )?;
        let (content_x, content_y) = inverse_content_point(
            geometry,
            point,
            placement.logical_extent(surface.content_extent()),
        )?;
        let [crop_x, crop_y, _, _] = surface.content_region();
        Some(SurfaceSample {
            width: bounds.width.max(1.0) as u32,
            height: bounds.height.max(1.0) as u32,
            x: point.x.max(0.0) as u32,
            y: point.y.max(0.0) as u32,
            content_x: content_x + crop_x as f32,
            content_y: content_y + crop_y as f32,
            pressed,
        })
    }
}

#[derive(Debug, Clone, Copy)]
pub(super) struct ViewTransform {
    pub(super) zoom: f32,
    pub(super) pan_x: f32,
    pub(super) pan_y: f32,
}

impl ViewTransform {
    pub(super) const FIT: Self = Self {
        zoom: 1.0,
        pan_x: 0.0,
        pan_y: 0.0,
    };
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub(super) struct PlacementGeometry {
    pub(super) x: f32,
    pub(super) y: f32,
    pub(super) width: f32,
    pub(super) height: f32,
}

pub(super) fn placement_geometry(
    bounds: Rectangle,
    content: (u32, u32),
    placement: Placement,
    transform: ViewTransform,
) -> Option<PlacementGeometry> {
    if ![
        bounds.x,
        bounds.y,
        bounds.width,
        bounds.height,
        transform.zoom,
        transform.pan_x,
        transform.pan_y,
    ]
    .iter()
    .all(|value| value.is_finite())
        || bounds.width <= 0.0
        || bounds.height <= 0.0
        || content.0 == 0
        || content.1 == 0
    {
        return None;
    }
    let (base_width, base_height) = match placement {
        Placement::Contain => {
            let aspect = content.0 as f32 / content.1 as f32;
            if bounds.width / bounds.height > aspect {
                (bounds.height * aspect, bounds.height)
            } else {
                (bounds.width, bounds.width / aspect)
            }
        }
        Placement::FixedGrid { columns, rows }
            if columns != 0 && rows != 0 && content.0 % columns == 0 && content.1 % rows == 0 =>
        {
            (
                bounds.width,
                bounds.width * content.1 as f32 / content.0 as f32,
            )
        }
        Placement::FixedGrid { .. } => return None,
        Placement::GalleryGrid {
            columns,
            rows,
            row_capacity,
            row_origin,
            ..
        } if columns != 0
            && rows != 0
            && rows <= row_capacity
            && row_origin < row_capacity
            && content.0 % columns == 0
            && content.1 % row_capacity == 0
            && content.0 / columns == content.1 / row_capacity =>
        {
            (bounds.width, bounds.width / columns as f32 * rows as f32)
        }
        Placement::GalleryGrid { .. } => return None,
    };
    let (zoom, pan_x, pan_y) = match placement {
        Placement::Contain => (transform.zoom, transform.pan_x, transform.pan_y),
        Placement::GalleryGrid { .. } | Placement::FixedGrid { .. } => (1.0, 0.0, 0.0),
    };
    let width = base_width * zoom;
    let height = base_height * zoom;
    Some(PlacementGeometry {
        x: bounds.x + (bounds.width - width) * 0.5 + pan_x,
        y: bounds.y
            + if matches!(placement, Placement::Contain) {
                (bounds.height - height) * 0.5 + pan_y
            } else {
                0.0
            },
        width,
        height,
    })
}

pub(super) fn inverse_content_point(
    geometry: PlacementGeometry,
    point: Point,
    content: (u32, u32),
) -> Option<(f32, f32)> {
    if !point.x.is_finite()
        || !point.y.is_finite()
        || point.x < geometry.x
        || point.y < geometry.y
        || point.x >= geometry.x + geometry.width
        || point.y >= geometry.y + geometry.height
        || content.0 == 0
        || content.1 == 0
    {
        return None;
    }
    Some((
        (((point.x - geometry.x) / geometry.width) * content.0 as f32).clamp(0.0, content.0 as f32),
        (((point.y - geometry.y) / geometry.height) * content.1 as f32)
            .clamp(0.0, content.1 as f32),
    ))
}

pub(crate) fn physical_bounds(bounds: Rectangle, scale: f32) -> Rectangle {
    Rectangle {
        x: bounds.x * scale,
        y: bounds.y * scale,
        width: bounds.width * scale,
        height: bounds.height * scale,
    }
}

pub(super) fn physical_pan(transform: ViewTransform, scale: f32) -> (f32, f32) {
    (transform.pan_x * scale, transform.pan_y * scale)
}

fn content_uv_scale(surface: Surface) -> [f32; 2] {
    let width_height = surface.content_extent();
    [
        width_height.0 as f32 / surface.width as f32,
        width_height.1 as f32 / surface.height as f32,
    ]
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub(super) struct GeometryKey {
    pub(super) uv_scale: [f32; 2],
    pub(super) uv_offset: [f32; 2],
    pub(super) draw_extent: [f32; 2],
    pub(super) grid: [u32; 2],
    pub(super) gallery: u32,
    pub(super) image_origin: [f32; 2],
    pub(super) atlas: [u32; 2],
}

pub(super) fn geometry_key(
    surface: Surface,
    bounds: Rectangle,
    placement: Placement,
    transform: ViewTransform,
) -> GeometryKey {
    let content = surface.content_extent();
    let [x, y, _, _] = surface.content_region();
    let draw_extent = placement_geometry(bounds, content, placement, transform)
        .map_or([0.0, 0.0], |geometry| [geometry.width, geometry.height]);
    let (grid, gallery) = match placement {
        Placement::Contain => ([0, 0], 0),
        Placement::GalleryGrid { columns, rows, .. } | Placement::FixedGrid { columns, rows } => {
            ([columns, rows], 1)
        }
    };
    GeometryKey {
        uv_scale: content_uv_scale(surface),
        uv_offset: [
            x as f32 / surface.width as f32,
            y as f32 / surface.height as f32,
        ],
        draw_extent,
        grid,
        gallery,
        image_origin: placement_geometry(bounds, content, placement, transform)
            .map_or([0.0, 0.0], |g| [g.x, g.y]),
        atlas: match placement {
            Placement::GalleryGrid {
                row_capacity,
                row_origin,
                ..
            } => [row_capacity, row_origin],
            Placement::Contain | Placement::FixedGrid { .. } => [0, 0],
        },
    }
}

#[cfg(test)]
mod tests {
    use super::super::{same_allocation, surface_for_content_session};
    use super::*;
    use crate::view_model::test_support::physical_frame as frame_ready;
    #[test]
    fn rectangular_fixed_grid_shares_border_geometry_without_scroll_or_zoom() {
        let bounds = Rectangle {
            x: 11.0,
            y: 17.0,
            width: 400.0,
            height: 450.0,
        };
        let placement = Placement::FixedGrid {
            columns: 2,
            rows: 3,
        };
        let transform = ViewTransform {
            zoom: 3.0,
            pan_x: 20.0,
            pan_y: 40.0,
        };
        let geometry = placement_geometry(bounds, (512, 576), placement, transform).unwrap();
        assert_eq!(
            (geometry.x, geometry.y, geometry.width, geometry.height),
            (11.0, 17.0, 400.0, 450.0)
        );
        assert_eq!(
            inverse_content_point(geometry, Point::new(211.0, 167.0), (512, 576)),
            Some((256.0, 192.0))
        );
        assert_eq!(placement.logical_extent((512, 576)), (512, 576));
    }

    #[test]
    fn viewport_keeps_fit_pan_and_zoom_local() {
        let mut viewport = ViewportOwner::default();
        viewport.zoom = 2.0;
        viewport.pan_x = 12.0;
        viewport.pan_y = -4.0;
        let transform = viewport.transform();
        assert_eq!(transform.zoom, 2.0);
        assert_eq!(transform.pan_x, 12.0);
        assert_eq!(transform.pan_y, -4.0);
        assert_eq!(physical_pan(transform, 1.5), (18.0, -6.0));
    }

    #[test]
    fn viewport_transform_is_local_to_one_presentation_source() {
        let mut viewport = ViewportOwner::default();
        assert!(
            viewport
                .synchronize_source(surface_for_content_session(1))
                .is_none()
        );
        viewport.zoom = 0.25;
        viewport.pan_x = 12.0;
        viewport.pan_y = -4.0;
        assert_eq!(
            viewport.transform_for(surface_for_content_session(1)).zoom,
            0.25
        );

        let reset = viewport.transform_for(surface_for_content_session(2));
        assert_eq!(reset.zoom, 1.0);
        assert_eq!((reset.pan_x, reset.pan_y), (0.0, 0.0));
        assert!(
            viewport
                .synchronize_source(surface_for_content_session(2))
                .is_none()
        );
        assert_eq!(viewport.content_session, Some((2, 0, 0)));
        assert_eq!(viewport.transform().zoom, 1.0);
    }

    #[test]
    fn final_content_cells_keep_continuous_fractions_through_crop_pan_and_zoom() {
        let bounds = Rectangle {
            x: 40.0,
            y: 30.0,
            width: 640.0,
            height: 480.0,
        };
        let viewport = ViewportOwner {
            zoom: 0.5,
            pan_x: 10.0,
            pan_y: -5.0,
            content_session: surface_for_content_session(1).transform_identity(),
            ..ViewportOwner::default()
        };
        for crop in [None, Some([100, 60, 20, 10]), Some([7, 9, 1, 1])] {
            let surface = Surface {
                crop,
                ..surface_for_content_session(1)
            };
            let extent = surface.content_extent();
            let [crop_x, crop_y, _, _] = surface.content_region();
            let geometry = placement_geometry(
                Rectangle::with_size(bounds.size()),
                extent,
                Placement::Contain,
                viewport.transform(),
            )
            .unwrap();
            let mut previous = None;
            for fraction in [0.25, 0.75] {
                let content = Point::new(
                    extent.0 as f32 - 1.0 + fraction,
                    extent.1 as f32 - 1.0 + fraction,
                );
                let cursor = mouse::Cursor::Available(Point::new(
                    bounds.x + geometry.x + content.x / extent.0 as f32 * geometry.width,
                    bounds.y + geometry.y + content.y / extent.1 as f32 * geometry.height,
                ));
                let sample = viewport
                    .sample(bounds, cursor, surface, Placement::Contain, true)
                    .unwrap();
                assert!((sample.content_x - (crop_x as f32 + content.x)).abs() < 0.0001);
                assert!((sample.content_y - (crop_y as f32 + content.y)).abs() < 0.0001);
                if let Some((x, y)) = previous {
                    assert!(sample.content_x > x && sample.content_y > y);
                }
                previous = Some((sample.content_x, sample.content_y));
            }
            assert!(
                inverse_content_point(
                    geometry,
                    Point::new(geometry.x + geometry.width, geometry.y),
                    extent
                )
                .is_none()
            );
            assert!(
                inverse_content_point(
                    geometry,
                    Point::new(geometry.x, geometry.y + geometry.height),
                    extent
                )
                .is_none()
            );
        }
    }

    #[test]
    fn iced_batched_events_preserve_cursor_order_and_overlay_capture() {
        use iced_runtime::core;
        use std::{cell::RefCell, rc::Rc};
        type Observed = (core::Event, core::mouse::Cursor);

        struct TestRenderer;
        impl core::Renderer for TestRenderer {
            fn start_layer(&mut self, _: Rectangle) {}
            fn end_layer(&mut self) {}
            fn start_transformation(&mut self, _: core::Transformation) {}
            fn end_transformation(&mut self) {}
            fn fill_quad(&mut self, _: core::renderer::Quad, _: impl Into<core::Background>) {}
            fn allocate_image(
                &mut self,
                _: &core::image::Handle,
                callback: impl FnOnce(Result<core::image::Allocation, core::image::Error>)
                + Send
                + 'static,
            ) {
                callback(Err(core::image::Error::Unsupported));
            }
            fn hint(&mut self, _: f32) {}
            fn scale_factor(&self) -> Option<f32> {
                None
            }
            fn reset(&mut self, _: core::Rectangle) {}
        }
        impl core::text::Renderer for TestRenderer {
            type Font = core::Font;
            type Paragraph = iced::advanced::graphics::text::Paragraph;
            type Editor = iced::advanced::graphics::text::Editor;
            const ICON_FONT: core::Font = core::Font::DEFAULT;
            const CHECKMARK_ICON: char = ' ';
            const ARROW_DOWN_ICON: char = ' ';
            const SCROLL_UP_ICON: char = ' ';
            const SCROLL_DOWN_ICON: char = ' ';
            const SCROLL_LEFT_ICON: char = ' ';
            const SCROLL_RIGHT_ICON: char = ' ';
            const ICED_LOGO: char = ' ';
            fn default_font(&self) -> core::Font {
                core::Font::DEFAULT
            }
            fn default_size(&self) -> core::Pixels {
                core::Pixels(16.0)
            }
            fn fill_paragraph(
                &mut self,
                _: &Self::Paragraph,
                _: Point,
                _: core::Color,
                _: Rectangle,
            ) {
            }
            fn fill_editor(&mut self, _: &Self::Editor, _: Point, _: core::Color, _: Rectangle) {}
            fn fill_text(&mut self, _: core::Text<String>, _: Point, _: core::Color, _: Rectangle) {
            }
        }
        struct Probe {
            overlay_events: Option<Rc<RefCell<Vec<Observed>>>>,
            size: core::Size,
            viewport: ViewportOwner,
            gestures: Rc<RefCell<Vec<SurfaceGesture>>>,
        }
        struct ModalProbe(Rc<RefCell<Vec<Observed>>>);

        impl core::Widget<Observed, iced::Theme, TestRenderer> for Probe {
            fn size(&self) -> core::Size<core::Length> {
                core::Size::new(
                    core::Length::Fixed(self.size.width),
                    core::Length::Fixed(self.size.height),
                )
            }
            fn layout(
                &mut self,
                _tree: &mut core::widget::Tree,
                _renderer: &TestRenderer,
                _limits: &core::layout::Limits,
            ) -> core::layout::Node {
                core::layout::Node::new(self.size)
            }
            fn draw(
                &self,
                _tree: &core::widget::Tree,
                _renderer: &mut TestRenderer,
                _theme: &iced::Theme,
                _style: &core::renderer::Style,
                _layout: core::Layout<'_>,
                _cursor: core::mouse::Cursor,
                _viewport: &core::Rectangle,
            ) {
            }
            fn update(
                &mut self,
                _tree: &mut core::widget::Tree,
                event: &core::Event,
                layout: core::Layout<'_>,
                cursor: core::mouse::Cursor,
                _renderer: &TestRenderer,
                shell: &mut core::Shell<'_, Observed>,
                _viewport: &core::Rectangle,
            ) {
                shell.publish((event.clone(), cursor));
                let publish = |gesture| {
                    self.gestures.borrow_mut().push(gesture);
                    shader::Action::<()>::capture()
                };
                let _ = self.viewport.update(
                    event,
                    layout.bounds(),
                    cursor,
                    surface_for_content_session(1),
                    Placement::Contain,
                    Some(&publish),
                );
            }
            fn overlay<'a>(
                &'a mut self,
                _tree: &'a mut core::widget::Tree,
                _layout: core::Layout<'a>,
                _renderer: &TestRenderer,
                _viewport: &core::Rectangle,
                _translation: core::Vector,
            ) -> Option<core::overlay::Element<'a, Observed, iced::Theme, TestRenderer>>
            {
                self.overlay_events
                    .as_ref()
                    .map(|events| core::overlay::Element::new(Box::new(ModalProbe(events.clone()))))
            }
        }
        impl core::Overlay<Observed, iced::Theme, TestRenderer> for ModalProbe {
            fn layout(
                &mut self,
                _renderer: &TestRenderer,
                _bounds: core::Size,
            ) -> core::layout::Node {
                core::layout::Node::new(core::Size::new(50.0, 100.0))
            }
            fn draw(
                &self,
                _renderer: &mut TestRenderer,
                _theme: &iced::Theme,
                _style: &core::renderer::Style,
                _layout: core::Layout<'_>,
                _cursor: core::mouse::Cursor,
            ) {
            }
            fn update(
                &mut self,
                event: &core::Event,
                layout: core::Layout<'_>,
                cursor: core::mouse::Cursor,
                _renderer: &TestRenderer,
                shell: &mut core::Shell<'_, Observed>,
            ) {
                self.0.borrow_mut().push((event.clone(), cursor));
                if matches!(
                    event,
                    core::Event::Mouse(core::mouse::Event::ButtonPressed(_))
                ) && cursor.is_over(layout.bounds())
                {
                    shell.capture_event();
                }
            }
            fn mouse_interaction(
                &self,
                layout: core::Layout<'_>,
                cursor: core::mouse::Cursor,
                _renderer: &TestRenderer,
            ) -> core::mouse::Interaction {
                if cursor.is_over(layout.bounds()) {
                    core::mouse::Interaction::Pointer
                } else {
                    core::mouse::Interaction::None
                }
            }
        }

        let first = Point::new(10.25, 20.5);
        let second = Point::new(80.75, 40.125);
        let first_cursor = mouse::Cursor::Available(first);
        let second_cursor = mouse::Cursor::Available(second);
        let unavailable = mouse::Cursor::Unavailable;
        let events = vec![
            (
                Event::Mouse(mouse::Event::CursorMoved { position: first }),
                first_cursor,
            ),
            (
                Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
                first_cursor,
            ),
            (
                Event::Mouse(mouse::Event::CursorMoved { position: second }),
                second_cursor,
            ),
            (
                Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)),
                second_cursor,
            ),
            (
                Event::Mouse(mouse::Event::CursorMoved { position: first }),
                unavailable,
            ),
            (
                Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
                unavailable,
            ),
        ];
        for modal in [false, true] {
            let observed_overlay = Rc::new(RefCell::new(Vec::new()));
            let root = core::Element::new(Probe {
                overlay_events: modal.then(|| observed_overlay.clone()),
                size: core::Size::new(100.0, 100.0),
                viewport: ViewportOwner::default(),
                gestures: Rc::default(),
            });
            let mut renderer = TestRenderer;
            let mut ui = iced_runtime::UserInterface::build(
                root,
                core::Size::new(100.0, 100.0),
                iced_runtime::user_interface::Cache::default(),
                &mut renderer,
            );
            let mut observed_base = Vec::new();
            let (_, statuses) = ui.update(
                &core::window::Headless,
                &core::shell::Waker::noop(),
                &events,
                second_cursor,
                &mut renderer,
                &mut observed_base,
            );
            if modal {
                assert_eq!(*observed_overlay.borrow(), events);
                assert_eq!(
                    observed_base,
                    vec![
                        (events[0].0.clone(), unavailable),
                        events[2].clone(),
                        events[3].clone(),
                        events[4].clone(),
                        events[5].clone(),
                    ]
                );
                assert_eq!(
                    statuses,
                    vec![
                        core::event::Status::Ignored,
                        core::event::Status::Captured,
                        core::event::Status::Ignored,
                        core::event::Status::Ignored,
                        core::event::Status::Ignored,
                        core::event::Status::Ignored
                    ]
                );
            } else {
                assert_eq!(observed_base, events);
                assert_eq!(statuses, vec![core::event::Status::Ignored; events.len()]);
            }
        }
        for (outer_scroll, inner_scroll) in [(220.0, 35.0), (217.25, 37.5)] {
            for modal in [false, true] {
                let gestures = Rc::new(RefCell::new(Vec::new()));
                let probe = core::Element::new(Probe {
                    overlay_events: modal.then(|| Rc::default()),
                    size: core::Size::new(640.0, 480.0),
                    viewport: ViewportOwner::default(),
                    gestures: gestures.clone(),
                });
                let inner = iced::widget::scrollable(probe).id("inner").height(300.0);
                let content =
                    iced::widget::column![iced::widget::space::vertical().height(200.0), inner,]
                        .spacing(0);
                let root = iced::widget::scrollable(content).id("outer").height(100.0);
                let mut renderer = TestRenderer;
                let mut ui = iced_runtime::UserInterface::build(
                    root,
                    core::Size::new(640.0, 100.0),
                    iced_runtime::user_interface::Cache::default(),
                    &mut renderer,
                );
                for (id, y) in [("outer", outer_scroll), ("inner", inner_scroll)] {
                    ui.operate(
                        &renderer,
                        &mut core::widget::operation::scrollable::scroll_to::<()>(
                            core::widget::Id::new(id),
                            core::widget::operation::scrollable::AbsoluteOffset {
                                x: None,
                                y: Some(y),
                            },
                        ),
                    );
                }
                let mut observed = Vec::new();
                let _ = ui.update(
                    &core::window::Headless,
                    &core::shell::Waker::noop(),
                    &events,
                    second_cursor,
                    &mut renderer,
                    &mut observed,
                );
                // Scrollable rounds each physical translation independently;
                // the event's remaining fractional coordinates stay untouched.
                let translation =
                    core::Vector::new(0.0, outer_scroll.round() + inner_scroll.round());
                let expected_events = if modal {
                    vec![
                        (events[0].0.clone(), unavailable),
                        events[2].clone(),
                        events[3].clone(),
                        events[4].clone(),
                        events[5].clone(),
                    ]
                } else {
                    events.clone()
                };
                assert_eq!(
                    observed,
                    expected_events
                        .into_iter()
                        .map(|(event, cursor)| (event, cursor + translation))
                        .collect::<Vec<_>>()
                );
                let gestures = gestures.borrow();
                let expected = if modal {
                    vec![second]
                } else {
                    vec![first, first, second, second]
                };
                assert_eq!(gestures.len(), expected.len());
                for (gesture, raw) in gestures.iter().zip(expected) {
                    assert_eq!(gesture.sample.content_x, raw.x);
                    assert_eq!(gesture.sample.content_y, raw.y + translation.y - 200.0);
                }
            }
        }
    }

    #[test]
    fn event_time_cursor_keeps_fractional_motion_and_button_order() {
        let mut viewport = ViewportOwner::default();
        let surface = surface_for_content_session(1);
        let bounds = Rectangle::with_size(iced::Size::new(640.0, 480.0));
        let captured = std::cell::RefCell::new(Vec::new());
        let publish = |gesture| {
            captured.borrow_mut().push(gesture);
            shader::Action::<()>::capture()
        };
        let first = Point::new(10.25, 20.5);
        let second = Point::new(30.75, 40.125);
        for (event, position) in [
            (
                Event::Mouse(mouse::Event::CursorMoved { position: first }),
                first,
            ),
            (
                Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
                first,
            ),
            (
                Event::Mouse(mouse::Event::CursorMoved { position: second }),
                second,
            ),
            (
                Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)),
                second,
            ),
        ] {
            let _ = viewport.update(
                &event,
                bounds,
                mouse::Cursor::Available(position),
                surface,
                Placement::Contain,
                Some(&publish),
            );
        }
        let gestures = captured.borrow();
        assert_eq!(gestures.len(), 4);
        assert_eq!(gestures[1].sample.content_x, first.x);
        assert_eq!(gestures[1].sample.content_y, first.y);
        assert_eq!(gestures[2].sample.content_x, second.x);
        assert_eq!(gestures[2].sample.content_y, second.y);
        assert!(gestures[1].sample.pressed && gestures[2].sample.pressed);
        assert!(!gestures[3].sample.pressed);
        drop(gestures);
        captured.borrow_mut().clear();
        let _ = viewport.update(
            &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
            bounds,
            mouse::Cursor::Unavailable,
            surface,
            Placement::Contain,
            Some(&publish),
        );
        assert!(captured.borrow().is_empty());
        assert!(!viewport.pointer_active);
        for cursor in [mouse::Cursor::Unavailable, mouse::Cursor::Levitating(first)] {
            let _ = viewport.update(
                &Event::Mouse(mouse::Event::CursorMoved { position: first }),
                bounds,
                cursor,
                surface,
                Placement::Contain,
                Some(&publish),
            );
            assert!(captured.borrow().is_empty());
        }

        let _ = viewport.update(
            &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Right)),
            bounds,
            mouse::Cursor::Available(first),
            surface,
            Placement::Contain,
            Some(&publish),
        );
        let raw_window_position = Point::new(1.0, 2.0);
        let motion = Event::Mouse(mouse::Event::CursorMoved {
            position: raw_window_position,
        });
        let _ = viewport.update(
            &motion,
            bounds,
            mouse::Cursor::Levitating(second),
            surface,
            Placement::Contain,
            Some(&publish),
        );
        assert_eq!(viewport.pan_origin, Some(first));
        let _ = viewport.update(
            &motion,
            bounds,
            mouse::Cursor::Available(second),
            surface,
            Placement::Contain,
            Some(&publish),
        );
        assert_eq!(
            (viewport.pan_x, viewport.pan_y),
            (second.x - first.x, second.y - first.y)
        );
        let _ = viewport.update(
            &Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Right)),
            bounds,
            mouse::Cursor::Unavailable,
            surface,
            Placement::Contain,
            Some(&publish),
        );
        assert!(viewport.pan_origin.is_none());
    }

    #[test]
    fn captured_release_outside_surface_uses_last_valid_sample_once() {
        let sample = SurfaceSample {
            width: 640,
            height: 480,
            x: 120,
            y: 80,
            content_x: 60.0,
            content_y: 40.0,
            pressed: true,
        };
        let mut viewport = ViewportOwner {
            pointer_active: true,
            last_pointer_sample: Some(sample),
            ..ViewportOwner::default()
        };
        let end = viewport.finish_pointer(None).expect("local end");
        assert_eq!(end.kind, SurfaceGestureKind::End);
        assert_eq!(end.sample.content_x, sample.content_x);
        assert_eq!(end.sample.content_y, sample.content_y);
        assert!(!end.sample.pressed);
        assert!(viewport.finish_pointer(None).is_none());
    }

    #[test]
    fn captured_content_session_replacement_cancels_once() {
        let sample = SurfaceSample {
            width: 640,
            height: 480,
            x: 120,
            y: 80,
            content_x: 60.0,
            content_y: 40.0,
            pressed: true,
        };
        let mut viewport = ViewportOwner::default();
        assert!(
            viewport
                .synchronize_source(surface_for_content_session(1))
                .is_none()
        );
        viewport.pointer_active = true;
        viewport.last_pointer_sample = Some(sample);
        let cancel = viewport
            .synchronize_source(surface_for_content_session(2))
            .expect("local cancellation");
        assert_eq!(cancel.kind, SurfaceGestureKind::Cancel);
        assert!(!cancel.sample.pressed);
        assert!(
            viewport
                .synchronize_source(surface_for_content_session(2))
                .is_none()
        );
    }

    #[test]
    fn content_geometry_samples_only_the_completed_high_water_rectangle() {
        let scale = content_uv_scale(Surface {
            high: 1,
            low: 2,
            width: 1_280,
            height: 720,
            frame: Some(frame_ready(1, 2, 3, 640, 360)),
            crop: None,
            viewer_identity: None,
            fit_revision: 0,
        });
        assert_eq!(scale, [0.5, 0.5]);
    }

    #[test]
    fn stored_crop_changes_sampling_without_replacing_the_native_frame() {
        let full = surface_for_content_session(1);
        let cropped = Surface {
            crop: Some([80, 60, 480, 360]),
            ..full
        };
        let bounds = Rectangle::with_size(iced::Size::new(1000.0, 700.0));
        let transform = ViewTransform::FIT;
        let key = geometry_key(cropped, bounds, Placement::Contain, transform);
        assert_eq!(cropped.frame, full.frame);
        assert_eq!(key.uv_offset, [0.125, 0.125]);
        assert_eq!(key.uv_scale, [0.75, 0.75]);
        assert_ne!(
            key,
            geometry_key(full, bounds, Placement::Contain, transform)
        );
        let mut state = ViewportOwner::default();
        let selected = Surface {
            viewer_identity: Some((9, 2)),
            ..full
        };
        state.synchronize_source(selected);
        state.zoom = 2.0;
        state.pan_x = 17.0;
        let upscale = Surface {
            frame: Some(frame_ready(5, 99, 88, 2560, 1920)),
            ..selected
        };
        assert_eq!(state.transform_for(upscale).zoom, 2.0);
        assert_eq!(state.transform_for(upscale).pan_x, 17.0);
        assert_eq!(
            state
                .transform_for(Surface {
                    fit_revision: 1,
                    ..upscale
                })
                .zoom,
            1.0
        );
        assert_eq!(
            state
                .transform_for(Surface {
                    viewer_identity: Some((9, 3)),
                    ..upscale
                })
                .zoom,
            1.0
        );
    }

    #[test]
    fn square_wide_and_tall_products_fit_actual_remaining_space() {
        let bounds = Rectangle {
            x: 31.0,
            y: 47.0,
            width: 913.0,
            height: 517.0,
        };
        for content in [(512, 512), (768, 384), (192, 384)] {
            let geometry =
                placement_geometry(bounds, content, Placement::Contain, ViewTransform::FIT)
                    .unwrap();
            assert!((geometry.x + geometry.width * 0.5 - bounds.center().x).abs() < 0.001);
            assert!((geometry.y + geometry.height * 0.5 - bounds.center().y).abs() < 0.001);
            assert!(geometry.width <= bounds.width && geometry.height <= bounds.height);
            assert!(
                (geometry.width - bounds.width).abs() < 0.001
                    || (geometry.height - bounds.height).abs() < 0.001
            );
            assert!(
                (geometry.width / geometry.height - content.0 as f32 / content.1 as f32).abs()
                    < 0.001
            );
        }
    }

    #[test]
    fn contain_and_gallery_share_forward_and_inverse_geometry() {
        let bounds = Rectangle {
            x: 0.0,
            y: 0.0,
            width: 100.0,
            height: 100.0,
        };
        let transform = ViewTransform::FIT;
        let contain =
            placement_geometry(bounds, (200, 100), Placement::Contain, transform).unwrap();
        assert_eq!(
            contain,
            PlacementGeometry {
                x: 0.0,
                y: 25.0,
                width: 100.0,
                height: 50.0,
            }
        );
        assert_eq!(
            inverse_content_point(contain, Point::new(50.0, 50.0), (200, 100)),
            Some((100.0, 50.0))
        );
        assert!(inverse_content_point(contain, Point::new(50.0, 10.0), (200, 100)).is_none());

        let gallery = placement_geometry(
            bounds,
            (80, 60),
            Placement::GalleryGrid {
                first_row: 0,
                columns: 4,
                rows: 3,
                row_capacity: 3,
                row_origin: 0,
            },
            ViewTransform {
                zoom: 4.0,
                pan_x: 20.0,
                pan_y: -10.0,
            },
        )
        .unwrap();
        assert_eq!(
            gallery,
            PlacementGeometry {
                x: 0.0,
                y: 0.0,
                width: 100.0,
                height: 75.0,
            }
        );
        assert_eq!(
            inverse_content_point(gallery, Point::new(75.0, 50.0), (80, 60)),
            Some((60.0, 40.0))
        );
    }

    #[test]
    fn contain_maps_full_square_and_non_square_frames_in_every_workspace_ratio() {
        let frames = [(640, 640), (640, 360), (480, 640)];
        for aspect in crate::generated::WORKSPACE_ASPECT_RATIO_VALUES
            .iter()
            .copied()
        {
            let (width, height) = crate::view::aspect_ratio::extent_for_width(800.0, aspect);
            let bounds = Rectangle {
                x: 0.0,
                y: 0.0,
                width,
                height,
            };
            for content in frames {
                let geometry =
                    placement_geometry(bounds, content, Placement::Contain, ViewTransform::FIT)
                        .unwrap();
                assert!(geometry.width <= bounds.width && geometry.height <= bounds.height);
                assert!(
                    (geometry.width / geometry.height - content.0 as f32 / content.1 as f32).abs()
                        < 0.000_1
                );
                assert_eq!(
                    inverse_content_point(geometry, Point::new(geometry.x, geometry.y), content,),
                    Some((0.0, 0.0))
                );
                let edge = inverse_content_point(
                    geometry,
                    Point::new(
                        geometry.x + geometry.width - 0.001,
                        geometry.y + geometry.height - 0.001,
                    ),
                    content,
                )
                .unwrap();
                assert!(edge.0 > (content.0 - 1) as f32 && edge.0 <= content.0 as f32);
                assert!(edge.1 > (content.1 - 1) as f32 && edge.1 <= content.1 as f32);
            }
        }
    }

    #[test]
    fn gallery_scale_and_hit_boundaries_agree_after_fractional_scroll_and_dpi() {
        for dpi in [1.0, 1.5, 2.0] {
            let bounds = Rectangle {
                x: 15.0 * dpi,
                y: -37.25 * dpi,
                width: 600.0 * dpi,
                height: 450.0 * dpi,
            };
            let geometry = placement_geometry(
                bounds,
                (400, 400),
                Placement::GalleryGrid {
                    first_row: 0,
                    columns: 4,
                    rows: 4,
                    row_capacity: 4,
                    row_origin: 0,
                },
                ViewTransform::FIT,
            )
            .unwrap();
            let scale = geometry.width / 400.0;
            assert!((scale - geometry.height / 400.0).abs() < 0.0001);
            let cell_edge = Point::new(geometry.x + 100.0 * scale, geometry.y + 100.0 * scale);
            assert_eq!(
                inverse_content_point(geometry, cell_edge, (400, 400)),
                Some((100.0, 100.0))
            );
            assert_eq!(
                inverse_content_point(
                    geometry,
                    Point::new(cell_edge.x - scale * 0.5, cell_edge.y - scale * 0.5),
                    (400, 400)
                ),
                Some((99.5, 99.5))
            );
            assert!(
                inverse_content_point(
                    geometry,
                    Point::new(geometry.x + geometry.width, geometry.y),
                    (400, 400)
                )
                .is_none()
            );
            assert!(
                inverse_content_point(
                    geometry,
                    Point::new(geometry.x, geometry.y + geometry.height),
                    (400, 400)
                )
                .is_none()
            );
        }
        assert!(
            placement_geometry(
                Rectangle {
                    width: 100.0,
                    height: 100.0,
                    ..Rectangle::default()
                },
                (64, 64),
                Placement::GalleryGrid {
                    first_row: 0,
                    columns: 2,
                    rows: 1,
                    row_capacity: 1,
                    row_origin: 0,
                },
                ViewTransform::FIT
            )
            .is_none()
        );
    }

    #[test]
    fn translated_gallery_bounds_preserve_uvs_when_the_gpu_viewport_is_clipped() {
        let mut surface = surface_for_content_session(1);
        surface.width = 400;
        surface.height = 500;
        surface.frame = Some(frame_ready(1, 41, 51, 400, 500));
        let placement = Placement::GalleryGrid {
            columns: 4,
            rows: 5,
            row_capacity: 5,
            row_origin: 0,
            first_row: 2,
        };
        for scale in [1.0, 1.5, 2.0] {
            // These are the bounds supplied by Iced after its scroll transform,
            // not the shader widget's original document-space layout.
            let translated = physical_bounds(
                Rectangle {
                    x: 30.0,
                    y: -37.25,
                    width: 600.0,
                    height: 750.0,
                },
                scale,
            );
            let key = geometry_key(surface, translated, placement, ViewTransform::FIT);
            let geometry = placement_geometry(
                translated,
                surface.content_extent(),
                placement,
                ViewTransform::FIT,
            )
            .unwrap();
            let image = Rectangle {
                x: geometry.x,
                y: geometry.y,
                width: geometry.width,
                height: geometry.height,
            };
            let clip = physical_bounds(
                Rectangle {
                    x: 30.0,
                    y: 0.0,
                    width: 600.0,
                    height: 500.0,
                },
                scale,
            );
            let visible = image.intersection(&clip).unwrap();
            assert_eq!(visible.y, 0.0);
            assert!(key.image_origin[1] < 0.0);
            let uv_y = (visible.y - key.image_origin[1]) / key.draw_extent[1];
            assert!((uv_y - 37.25 / 750.0).abs() < 0.00001);
            let point =
                inverse_content_point(geometry, Point::new(visible.x, visible.y), (400, 500))
                    .unwrap();
            assert_eq!(point.0, 0.0);
            assert!((point.1 - 37.25 / 1.5).abs() < 0.0001);
            assert!(
                placement_geometry(translated, (400, 400), placement, ViewTransform::FIT,)
                    .is_none()
            );
        }
    }

    #[test]
    fn circular_gallery_geometry_maps_two_source_spans_without_stretching_cells() {
        let mut surface = surface_for_content_session(1);
        surface.width = 400;
        surface.height = 800;
        surface.frame = Some(frame_ready(1, 41, 51, 400, 800));
        let placement = Placement::GalleryGrid {
            columns: 4,
            rows: 5,
            row_capacity: 8,
            row_origin: 7,
            first_row: 7,
        };
        let bounds = Rectangle {
            x: 0.0,
            y: -37.5,
            width: 600.0,
            height: 700.0,
        };
        let geometry = placement_geometry(
            bounds,
            surface.content_extent(),
            placement,
            ViewTransform::FIT,
        )
        .unwrap();
        assert_eq!(geometry.height, 750.0);
        assert_eq!(
            placement.logical_extent(surface.content_extent()),
            (400, 500)
        );
        let key = geometry_key(surface, bounds, placement, ViewTransform::FIT);
        assert_eq!(key.atlas, [8, 7]);
        // One row before wrap, followed by four rows at the allocation start.
        let source_rows: Vec<_> = (0..5)
            .map(|row| (key.atlas[1] + row) % key.atlas[0])
            .collect();
        assert_eq!(source_rows, [7, 0, 1, 2, 3]);
        assert_eq!(
            inverse_content_point(
                geometry,
                Point::new(150.0, 262.5),
                placement.logical_extent(surface.content_extent())
            ),
            Some((100.0, 200.0))
        );
    }

    #[test]
    fn resize_changes_only_uniform_geometry_for_one_import_identity() {
        let surface = surface_for_content_session(1);
        let transform = ViewTransform::FIT;
        let placement = Placement::GalleryGrid {
            first_row: 0,
            columns: 4,
            rows: 3,
            row_capacity: 3,
            row_origin: 0,
        };
        let first = geometry_key(
            surface,
            Rectangle {
                width: 400.0,
                height: 300.0,
                ..Rectangle::default()
            },
            placement,
            transform,
        );
        let resized = geometry_key(
            surface,
            Rectangle {
                width: 800.0,
                height: 600.0,
                ..Rectangle::default()
            },
            placement,
            transform,
        );
        assert!(same_allocation(surface, surface));
        assert_ne!(first, resized);
        assert_eq!(first.gallery, 1);
        assert_eq!(first.grid, [4, 3]);
        let contain = geometry_key(
            surface,
            Rectangle {
                width: 400.0,
                height: 300.0,
                ..Rectangle::default()
            },
            Placement::Contain,
            transform,
        );
        assert_eq!(contain.gallery, 0);
        assert_eq!(contain.grid, [0, 0]);
    }
}
