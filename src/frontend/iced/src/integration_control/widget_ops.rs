//! Real widget discovery, visibility and input operations.
use super::*;
#[derive(Debug, Clone, Copy, Default)]
pub(super) struct ControlBounds {
    pub(super) target: Rectangle,
    pub(super) page: Rectangle,
    pub(super) horizontal: Rectangle,
}

pub(super) struct FindControl {
    pub(super) target: Id,
    pub(super) translation: Vector,
    pub(super) pending_translation: Vector,
    pub(super) bounds: Option<Rectangle>,
    pub(super) page: Rectangle,
    pub(super) horizontal: Rectangle,
}

impl FindControl {
    pub(super) fn capture(&mut self, id: Option<&Id>, bounds: Rectangle) {
        if id == Some(&self.target) {
            self.bounds = Some(Rectangle {
                x: bounds.x - self.translation.x,
                y: bounds.y - self.translation.y,
                ..bounds
            });
        }
    }
}

impl Operation<ControlBounds> for FindControl {
    fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<ControlBounds>)) {
        let parent_translation = self.translation;
        self.translation += self.pending_translation;
        self.pending_translation = Vector::ZERO;
        operate(self);
        self.translation = parent_translation;
    }

    fn container(&mut self, id: Option<&Id>, bounds: Rectangle) {
        self.capture(id, bounds);
    }

    fn scrollable(
        &mut self,
        id: Option<&Id>,
        bounds: Rectangle,
        _content_bounds: Rectangle,
        translation: Vector,
        _state: &mut dyn iced::advanced::widget::operation::Scrollable,
    ) {
        self.capture(id, bounds);
        let visible = Rectangle {
            x: bounds.x - self.translation.x,
            y: bounds.y - self.translation.y,
            ..bounds
        };
        if id == Some(&Id::from(crate::view::PAGE_SCROLL_ID)) {
            self.page = visible;
        } else if id == Some(&Id::from(crate::view::HORIZONTAL_SCROLL_ID)) {
            self.horizontal = visible;
        }
        self.pending_translation += translation;
    }

    fn text_input(
        &mut self,
        id: Option<&Id>,
        bounds: Rectangle,
        _state: &mut dyn iced::advanced::widget::operation::TextInput,
    ) {
        self.capture(id, bounds);
    }

    fn finish(&self) -> Outcome<ControlBounds> {
        Outcome::Some(ControlBounds {
            target: self.bounds.unwrap_or_default(),
            page: self.page,
            horizontal: self.horizontal,
        })
    }
}

pub(super) fn measure_control(control: String) -> Task<ControlBounds> {
    widget::operate(FindControl {
        target: Id::from(control),
        translation: Vector::ZERO,
        pending_translation: Vector::ZERO,
        bounds: None,
        page: Rectangle::default(),
        horizontal: Rectangle::default(),
    })
}

pub(super) fn locate(control: String, generation: u64) -> Task<RootMessage> {
    measure_control(control.clone()).map(move |bounds| {
        RootMessage::Integration(Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(Message::Located {
                control: control.clone(),
                bounds: bounds.target,
            }),
        })
    })
}

pub(super) fn reveal_axis(start: f32, size: f32, viewport_start: f32, viewport_size: f32) -> f32 {
    if viewport_size <= 0.0 || size <= 0.0 {
        return 0.0;
    }
    // Scrollable rounds its translation to whole logical pixels. Leave room
    // for that rounding while still requiring the entire target to be visible.
    let inset = ((viewport_size - size) * 0.5).clamp(0.0, 1.0);
    let viewport_start = viewport_start + inset;
    let viewport_size = viewport_size - inset * 2.0;
    if start < viewport_start || size > viewport_size {
        start - viewport_start
    } else {
        (start + size - viewport_start - viewport_size).max(0.0)
    }
}

#[derive(Debug, Clone, Copy)]
pub(super) enum AnnotationReveal {
    Control,
    Tail {
        count: usize,
        narrow: bool,
    },
    Geometry,
    Source {
        extent: [f32; 2],
        region: Rectangle,
        margin: f32,
    },
}

pub(super) fn contains_rectangle(outer: Rectangle, inner: Rectangle) -> bool {
    outer.width > 0.0
        && outer.height > 0.0
        && inner.width > 0.0
        && inner.height > 0.0
        && inner.x >= outer.x - 0.01
        && inner.y >= outer.y - 0.01
        && inner.x + inner.width <= outer.x + outer.width + 0.01
        && inner.y + inner.height <= outer.y + outer.height + 0.01
}

impl ControlBounds {
    pub(super) fn visible(self) -> Option<Rectangle> {
        self.target
            .intersection(&self.page)?
            .intersection(&self.horizontal)
            .filter(|bounds| bounds.width > 0.0 && bounds.height > 0.0)
    }

    pub(super) fn requested(self, reveal: AnnotationReveal) -> Option<Rectangle> {
        if self.page.width <= 0.0
            || self.page.height <= 0.0
            || self.horizontal.width <= 0.0
            || self.horizontal.height <= 0.0
            || self.target.width <= 0.0
            || self.target.height <= 0.0
        {
            return None;
        }
        Some(match reveal {
            AnnotationReveal::Control | AnnotationReveal::Tail { .. } => self.target,
            // Metadata-only inspection has no click or pixel sample.
            AnnotationReveal::Geometry => Rectangle {
                width: 1.0,
                height: 1.0,
                ..self.target
            },
            AnnotationReveal::Source {
                extent,
                region,
                margin,
            } => {
                if extent[0] <= 0.0 || extent[1] <= 0.0 {
                    return None;
                }
                let scale = (self.target.width / extent[0]).min(self.target.height / extent[1]);
                Rectangle {
                    x: self.target.x
                        + (self.target.width - extent[0] * scale) * 0.5
                        + region.x * scale
                        - margin,
                    y: self.target.y
                        + (self.target.height - extent[1] * scale) * 0.5
                        + region.y * scale
                        - margin,
                    width: region.width * scale + 2.0 * margin,
                    height: region.height * scale + 2.0 * margin,
                }
            }
        })
    }
}

pub(super) fn scroll_control_into_view(control: String, reveal: AnnotationReveal) -> Task<RootMessage> {
    measure_control(control).then(move |bounds| {
        let Some(target) = bounds.requested(reveal) else {
            return Task::none();
        };
        let x = reveal_axis(
            target.x,
            target.width,
            bounds.horizontal.x,
            bounds.horizontal.width,
        );
        let y = reveal_axis(target.y, target.height, bounds.page.y, bounds.page.height);
        iced::widget::operation::scroll_by(
            crate::view::PAGE_SCROLL_ID,
            AbsoluteOffset { x: 0.0, y },
        )
        .chain(iced::widget::operation::scroll_by(
            crate::view::HORIZONTAL_SCROLL_ID,
            AbsoluteOffset { x, y: 0.0 },
        ))
    })
}

pub(super) fn reveal_control(control: String, generation: u64, reveal: AnnotationReveal) -> Task<RootMessage> {
    measure_control(control.clone()).then(move |before| {
        let control = control.clone();
        scroll_control_into_view(control.clone(), reveal).chain(
            measure_control(control.clone()).map(move |bounds| {
                let viewport = bounds.page.intersection(&bounds.horizontal);
                let verified = bounds.visible().is_some()
                    && viewport.zip(bounds.requested(reveal)).is_some_and(
                        |(viewport, requested)| contains_rectangle(viewport, requested),
                    );
                let tail_verified = if let AnnotationReveal::Tail { count, narrow } = reveal {
                    let before_viewport = before.page.intersection(&before.horizontal);
                    let offscreen_gap = before_viewport.map_or(-1.0, |viewport| {
                        before.target.y - viewport.y - viewport.height
                    });
                    let valid = count >= 32 && offscreen_gap > 0.0 && verified;
                    if let Some(viewport) = viewport.filter(|_| valid) {
                        reporting::emit(|sink| {
                            sink.record(
                                "integration.annotation_tail",
                                &control,
                                if narrow { "narrow" } else { "wide" },
                                [
                                    count as f64,
                                    f64::from(offscreen_gap),
                                    f64::from(bounds.target.y - viewport.y),
                                    f64::from(
                                        viewport.y + viewport.height
                                            - bounds.target.y
                                            - bounds.target.height,
                                    ),
                                ],
                            )
                        });
                    }
                    valid
                } else {
                    true
                };
                if !verified || !tail_verified {
                    reporting::emit(|sink| {
                        for (detail, rectangle) in [
                            ("before-target", before.target),
                            ("before-page", before.page),
                            ("before-horizontal", before.horizontal),
                            ("after-target", bounds.target),
                            ("after-page", bounds.page),
                            ("after-horizontal", bounds.horizontal),
                        ] {
                            sink.record(
                                "integration.reveal_bounds",
                                &control,
                                detail,
                                [
                                    f64::from(rectangle.x),
                                    f64::from(rectangle.y),
                                    f64::from(rectangle.width),
                                    f64::from(rectangle.height),
                                ],
                            );
                        }
                    });
                }
                RootMessage::Integration(Message::Scoped {
                    generation,
                    receipt: None,
                    message: Box::new(Message::Located {
                        control: control.clone(),
                        // Source conversion always retains full surface geometry.
                        bounds: if verified && tail_verified {
                            bounds.target
                        } else {
                            Rectangle::default()
                        },
                    }),
                })
            }),
        )
    })
}

pub(super) fn sidebar_reveal_offset(pane: Rectangle, target: Rectangle) -> Option<AbsoluteOffset> {
    let visible_top = pane.y + SIDEBAR_HEADER_HEIGHT + SIDEBAR_REVEAL_INSET;
    let visible_bottom = pane.y + pane.height - SIDEBAR_REVEAL_INSET;
    let target_bottom = target.y + target.height;
    let y = if target.y < visible_top {
        target.y - visible_top
    } else if target_bottom > visible_bottom {
        target_bottom - visible_bottom
    } else {
        0.0
    };
    (y.abs() > f32::EPSILON).then_some(AbsoluteOffset { x: 0.0, y })
}

pub(super) fn sidebar_control_visible(pane: Rectangle, target: Rectangle) -> bool {
    let visible_top = pane.y + SIDEBAR_HEADER_HEIGHT + SIDEBAR_VISIBLE_INSET;
    let visible_bottom = pane.y + pane.height - SIDEBAR_VISIBLE_INSET;
    target.x >= pane.x
        && target.x + target.width <= pane.x + pane.width
        && target.y >= visible_top
        && target.y + target.height <= visible_bottom
}

#[cfg(target_arch = "wasm32")]
pub(super) fn click(bounds: Rectangle) -> bool {
    click_js(
        f64::from(bounds.x + bounds.width * 0.5),
        f64::from(bounds.y + bounds.height * 0.5),
    ) == 1
}

#[cfg(target_arch = "wasm32")]
pub(super) fn click_after_surface_draw(
    bounds: Rectangle,
    control: &str,
    source_revision: u64,
    allow_newer: bool,
) -> bool {
    click_after_surface_draw_js(
        f64::from(bounds.x + bounds.width * 0.5),
        f64::from(bounds.y + bounds.height * 0.5),
        control,
        source_revision as f64,
        allow_newer,
    ) == 1
}

pub(super) fn gallery_slot_bounds(bounds: Rectangle, columns: u32, slot: u32, clipped_top: f32) -> Rectangle {
    let columns = columns.max(1);
    let card_extent = bounds.width / columns as f32;
    Rectangle {
        x: bounds.x + (slot % columns) as f32 * card_extent,
        y: bounds.y - clipped_top + (slot / columns) as f32 * card_extent,
        width: card_extent,
        height: card_extent,
    }
}

#[cfg(target_arch = "wasm32")]
pub(super) fn click_number_edge(bounds: Rectangle, upper: bool) -> bool {
    click_js(
        f64::from(bounds.x + bounds.width - 2.0),
        f64::from(bounds.y + if upper { 2.0 } else { bounds.height - 2.0 }),
    ) == 1
}

#[cfg(not(target_arch = "wasm32"))]
pub(super) fn click_number_edge(_bounds: Rectangle, _upper: bool) -> bool {
    false
}

#[cfg(target_arch = "wasm32")]
pub(super) fn wheel_number_input(bounds: Rectangle) -> bool {
    wheel_js(
        f64::from(bounds.x + bounds.width * 0.5),
        f64::from(bounds.y + bounds.height * 0.5),
    ) == 1
}

#[cfg(target_arch = "wasm32")]
pub(super) fn replace_number_input(bounds: Rectangle, value: &str, selection_length: usize) -> bool {
    let Ok(selection_length) = u32::try_from(selection_length) else {
        return false;
    };
    replace_number_js(
        f64::from(bounds.x + bounds.width * 0.5),
        f64::from(bounds.y + bounds.height * 0.5),
        value,
        selection_length,
    ) == 1
}

#[cfg(target_arch = "wasm32")]
pub(super) fn paste_number_input(bounds: Rectangle) -> bool {
    let Some(mut output) = scenario_output()
    else {
        return false;
    };
    let completed = wasm_bindgen::closure::Closure::once_into_js(move |delivered: bool| {
        output.send(Message::NumberPasteDelivered(delivered));
    });
    paste_number_js(
        f64::from(bounds.x + bounds.width * 0.5),
        f64::from(bounds.y + bounds.height * 0.5),
        &completed,
    ) == 1
}

#[cfg(not(target_arch = "wasm32"))]
pub(super) fn paste_number_input(_bounds: Rectangle) -> bool {
    false
}

#[cfg(not(target_arch = "wasm32"))]
pub(super) fn replace_number_input(_bounds: Rectangle, _value: &str, _selection_length: usize) -> bool {
    false
}

#[cfg(not(target_arch = "wasm32"))]
pub(super) fn wheel_number_input(_bounds: Rectangle) -> bool {
    false
}

#[cfg(not(target_arch = "wasm32"))]
pub(super) fn click(_bounds: Rectangle) -> bool {
    false
}

#[cfg(not(target_arch = "wasm32"))]
pub(super) fn click_after_surface_draw(
    _bounds: Rectangle,
    _control: &str,
    _source_revision: u64,
    _allow_newer: bool,
) -> bool {
    false
}

/// Mutable observations owned by this scenario or mechanism.
pub(super) struct RevealState {
    location_pending: bool,
    numeric_replacement: String,
    reveal_offset: AbsoluteOffset,
}
impl Default for RevealState {
    fn default() -> Self {
        Self {
            location_pending: false,
            numeric_replacement: String::new(),
            reveal_offset: AbsoluteOffset::default(),
        }
    }
}

impl RevealState {
    pub(super) fn arm_revealed(
        &mut self, driver: &mut Driver,
        scrollable: &'static str,
        control: impl Into<String>,
    ) -> Task<RootMessage> {
        if self.location_pending {
            return Task::none();
        }
        self.location_pending = true;
        iced::widget::operation::scroll_by(scrollable, self.reveal_offset)
            .chain(locate(control.into(), driver.generation))
    }
}

impl RevealState {
    pub(super) fn arm(&mut self, driver: &Driver, control: impl Into<String>) -> Task<RootMessage> {
        if self.location_pending { return Task::none(); }
        self.location_pending = true;
        locate(control.into(), driver.generation)
    }
}

impl RevealState {
    pub(super) fn arm_scrolled(
        &mut self, driver: &Driver,
        control: impl Into<String>,
        offset: RelativeOffset,
    ) -> Task<RootMessage> {
        if self.location_pending { return Task::none(); }
        self.location_pending = true;
        iced::widget::operation::snap_to(crate::view::PAGE_SCROLL_ID, offset)
            .chain(locate(control.into(), driver.generation))
    }
}

impl RevealState {
    pub(super) fn location_completed(&mut self) { self.location_pending = false; }
    pub(super) fn location_pending(&self) -> bool { self.location_pending }
    pub(super) fn begin_location(&mut self) -> bool {
        if self.location_pending { return false; }
        self.location_pending = true;
        true
    }
    pub(super) fn replace_numeric_value(&mut self, replacement: String) { self.numeric_replacement = replacement; }
    pub(super) fn replace_numeric_input(&self, bounds: Rectangle, selection: usize) -> bool {
        replace_number_input(bounds, &self.numeric_replacement, selection)
    }
    pub(super) fn measured_reveal(&mut self, offset: AbsoluteOffset) { self.reveal_offset = offset; }
}

#[cfg(test)]
pub(super) struct Fixture {
    pub(super) location_pending: bool,
}
#[cfg(test)]
impl RevealState {
    pub(super) fn fixture(&self) -> Fixture {
        Fixture {
            location_pending: self.location_pending.clone(),
        }
    }
    pub(super) fn configure_fixture<R>(&mut self, edit: impl FnOnce(&mut Fixture) -> R) -> R {
        let mut fixture = self.fixture();
        let result = edit(&mut fixture);
        self.location_pending = fixture.location_pending;
        result
    }
}

const SIDEBAR_HEADER_HEIGHT: f32 = 48.0;
const SIDEBAR_REVEAL_INSET: f32 = 16.0;
const SIDEBAR_VISIBLE_INSET: f32 = 8.0;
