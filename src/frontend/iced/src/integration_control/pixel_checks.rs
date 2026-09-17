//! Independent canvas pixel and FPS observations.
use crate::integration_control::probe::{
    ProbeReceipt, ScenarioOutput, current_fps_draw, current_receipt, observe_atlas_draw,
    probe_output, rearm_fps_sampling, same_probe,
};
#[cfg(target_arch = "wasm32")]
use crate::integration_control::probe::{atlas_probe_output, observer_generation};
use crate::integration_control::{
    COMPLETION_WITHOUT_INPUT, Driver, EXPLORE_GALLERY, Message, PIXEL_FIXTURE_ENABLED, Phase,
    pixel_checks, reporting, reporting_enabled,
};
#[cfg(target_arch = "wasm32")]
use crate::integration_control::{
    atlas_composition_js, atlas_pixels_js, boundary_pixels_js, fps_current_js, fps_pixels_js,
    upscale_pixels_js,
};
use crate::message::Message as RootMessage;
use crate::view_model::ApplicationModel;
use iced::{Rectangle, Task};
#[derive(Debug, Clone)]
pub enum FpsPixelOutcome {
    Invalidated,
    Cancelled,
    Captured(pixel_checks::FpsPixels),
    Failed,
}

impl reporting::FpsEvidence {
    #[cfg(target_arch = "wasm32")]
    pub(super) fn canvas_values(self) -> [f64; 11] {
        [
            f64::from(self.bounds.x),
            f64::from(self.bounds.y),
            f64::from(self.bounds.width),
            f64::from(self.bounds.height),
            f64::from(self.clip.x),
            f64::from(self.clip.y),
            f64::from(self.clip.width),
            f64::from(self.clip.height),
            self.frames as f64,
            self.seconds,
            f64::from(u8::from(self.dark)),
        ]
    }
}

/// Invalidation is request retirement, never measured pixel evidence.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProbeOutcome {
    Invalidated,
    Observed(u32, u32),
    Failed,
}

impl ProbeOutcome {
    #[cfg(any(target_arch = "wasm32", test))]
    pub(super) fn decode(status: Option<&str>, values: [Option<f64>; 2]) -> Self {
        let [Some(first), Some(second)] = values else {
            return Self::Failed;
        };
        if ![first, second].into_iter().all(|value| {
            value.is_finite()
                && value >= 0.0
                && value <= f64::from(u32::MAX)
                && value.fract() == 0.0
        }) {
            return Self::Failed;
        }
        match status {
            Some("invalidated") if first == 0.0 && second == 0.0 => Self::Invalidated,
            Some("observed") => Self::Observed(first as u32, second as u32),
            _ => Self::Failed,
        }
    }
}

pub(crate) fn sample_boundary_pixels(
    surface: crate::presentation_surface::Surface,
    control: &str,
    image: Rectangle,
    clip: Rectangle,
) {
    #[cfg(target_arch = "wasm32")]
    {
        if !crate::presentation_surface::pixel_trace::enabled() {
            return;
        }
        let Some(frame) = surface.frame else {
            return;
        };
        let [crop_x, crop_y, crop_width, crop_height] = surface.content_region();
        if crop_width == 0 || crop_height == 0 {
            return;
        }
        let coordinate = |index: usize, size: u32| {
            [
                0,
                191.min(size - 1),
                383.min(size - 1),
                (size - 1) / 2,
                size - 1,
            ][index]
        };
        let mut points = [0.0f32; 25 * 5];
        let mut count = 0;
        for index in 0..25 {
            let x = coordinate(index % 5, frame.content_width);
            let y = coordinate(index / 5, frame.content_height);
            if x < crop_x || y < crop_y || x >= crop_x + crop_width || y >= crop_y + crop_height {
                continue;
            }
            let screen = iced::Point::new(
                image.x + (x - crop_x) as f32 * image.width / crop_width as f32,
                image.y + (y - crop_y) as f32 * image.height / crop_height as f32,
            );
            let Some(visible) = clip.intersection(&image) else {
                continue;
            };
            if screen.x < visible.x + 2.0
                || screen.y < visible.y + 2.0
                || screen.x >= visible.x + visible.width - 2.0
                || screen.y >= visible.y + visible.height - 2.0
            {
                continue;
            }
            points[count..count + 5].copy_from_slice(&[
                x as f32,
                y as f32,
                screen.x,
                screen.y,
                index as f32,
            ]);
            count += 5;
        }
        boundary_pixels_js(
            &points[..count],
            &format!(
                "{{{}}}",
                crate::presentation_surface::surface_trace_fields(surface, surface)
            ),
            control,
            frame.content_sequence as f64,
            frame.presentation_revision as f64,
        );
    }
    #[cfg(not(target_arch = "wasm32"))]
    let _ = (surface, control, image, clip);
}

#[cfg(any(target_arch = "wasm32", test))]
pub(super) const ATLAS_GRID_SAMPLES: usize = 13;
#[cfg(any(target_arch = "wasm32", test))]
pub(super) const ATLAS_COMPOSITION_SAMPLES: usize = 256 * 4 + ATLAS_GRID_SAMPLES;

#[cfg(any(target_arch = "wasm32", test))]
pub(super) struct AtlasCompositionSamples {
    pub(super) points: [f32; ATLAS_COMPOSITION_SAMPLES * 10],
    pub(super) count: usize,
    pub(super) cards: [u32; 256],
    pub(super) card_count: usize,
}

#[cfg(any(target_arch = "wasm32", test))]
pub(super) fn atlas_composition_samples(draw: &AtlasDraw) -> Option<AtlasCompositionSamples> {
    let snapshot = &draw.snapshot;
    if !pixel_fixture_enabled()
        || snapshot.augmentation.enabled
        || snapshot.overlay.showlabels
        || !snapshot.overlay.showmasks
        || !snapshot.overlay.showboxes
        || !matches!(snapshot.viewport.columns, 4 | 10)
        || snapshot.gallery.slots.iter().any(|ready| !*ready)
    {
        return None;
    }
    let Some(frame) = draw.surface.frame else {
        return None;
    };
    // The fixture is a constant clean image and a rectangular mask with a
    // central hole. Four isolated samples exclude all label geometry.
    let mut points = [0.0f32; ATLAS_COMPOSITION_SAMPLES * 10];
    let mut count = 0;
    let mut cards = [0u32; 256];
    let mut card_count = 0;
    let columns = snapshot.viewport.columns.max(1);
    if snapshot.gallery.layout.cardextent == 0
        || snapshot.gallery.layout.rowcount == 0
        || snapshot.gallery.layout.columns != columns
    {
        return None;
    }
    let side = snapshot.gallery.layout.cardextent as f32;
    let logical_height = side * snapshot.gallery.layout.rowcount as f32;
    for label in &snapshot.labels {
        let slot = (label.box_.first.y / side) as usize * columns as usize
            + (label.box_.first.x / side) as usize;
        if snapshot.order.visibleindices.get(slot) != Some(&label.compiledindex) {
            continue;
        }
        if !snapshot.gallery.slots.get(slot).copied().unwrap_or(false) {
            continue;
        }
        let card_origin = iced::Point::new(
            draw.image.x
                + (slot % columns as usize) as f32 * side * draw.image.width
                    / frame.content_width as f32,
            draw.image.y
                + (slot / columns as usize) as f32 * side * draw.image.height / logical_height,
        );
        let card_end = iced::Point::new(
            card_origin.x + side * draw.image.width / frame.content_width as f32 - 1.0,
            card_origin.y + side * draw.image.height / logical_height - 1.0,
        );
        // Eligibility is independent of whether individual probes succeed:
        // all ready annotated cards fully inside the visible clip.
        if !draw.clip.contains(card_origin) || !draw.clip.contains(card_end) {
            continue;
        }
        let Some(color) = snapshot.dataset.palette.get(label.category as usize) else {
            continue;
        };
        let color = crate::presentation_surface::labels::class_color(color);
        let rgb = [color.r, color.g, color.b].map(|value| (value * 255.0).round());
        let width = label.box_.second.x - label.box_.first.x;
        let height = label.box_.second.y - label.box_.first.y;
        if cards[..card_count].contains(&label.compiledindex) {
            continue;
        }
        if card_count == cards.len() {
            return None;
        }
        cards[card_count] = label.compiledindex;
        card_count += 1;
        let start = count;
        for (kind, (relative_x, relative_y)) in [(0.2, 0.7), (0.5, 0.5), (0.0, 0.7), (-0.25, 0.7)]
            .into_iter()
            .enumerate()
        {
            if count + 10 > points.len() {
                break;
            }
            let x = (label.box_.first.x + width * relative_x).floor()
                + if kind == 2 { -0.5 } else { 0.5 };
            let y = (label.box_.first.y + height * relative_y).floor() + 0.5;
            let screen = iced::Point::new(
                draw.image.x + x * draw.image.width / frame.content_width as f32,
                draw.image.y + y * draw.image.height / logical_height,
            );
            if !draw.clip.contains(screen) {
                continue;
            }
            let base = [48.0, 80.0, 112.0];
            // Canvas pixel centers generally do not coincide with source
            // texel centers. Follow the existing linear sampler, including
            // the one-pixel stroke's clean/mask neighbours.
            let texel_x = (screen.x.floor() + 0.5 - draw.image.x) * frame.content_width as f32
                / draw.image.width
                - 0.5;
            let texel_y =
                (screen.y.floor() + 0.5 - draw.image.y) * logical_height / draw.image.height - 0.5;
            let alpha_at = |px: f32, py: f32| {
                let left = label.box_.first.x.floor() - 1.0;
                let top = label.box_.first.y.floor() - 1.0;
                let right = label.box_.second.x.ceil();
                let bottom = label.box_.second.y.ceil();
                if ((px == left || px == right) && py >= top && py <= bottom)
                    || ((py == top || py == bottom) && px >= left && px <= right)
                {
                    return 1.0;
                }
                let center_x = px + 0.5;
                let center_y = py + 0.5;
                let inside = center_x >= label.box_.first.x
                    && center_x < label.box_.second.x
                    && center_y >= label.box_.first.y
                    && center_y < label.box_.second.y;
                let hole = center_x >= label.box_.first.x + width * 0.375
                    && center_x < label.box_.first.x + width * 0.625
                    && center_y >= label.box_.first.y + height * 0.375
                    && center_y < label.box_.first.y + height * 0.625;
                if inside && !hole { 0.36 } else { 0.0 }
            };
            let tx = texel_x.fract();
            let ty = texel_y.fract();
            let expected: [f32; 3] = std::array::from_fn(|channel| {
                let color_at = |dx: f32, dy: f32| {
                    let alpha = alpha_at(texel_x.floor() + dx, texel_y.floor() + dy);
                    (base[channel] * (1.0 - alpha) + rgb[channel] * alpha).round()
                };
                ((color_at(0.0, 0.0) * (1.0 - tx) + color_at(1.0, 0.0) * tx) * (1.0 - ty)
                    + (color_at(0.0, 1.0) * (1.0 - tx) + color_at(1.0, 1.0) * tx) * ty)
                    .round()
            });
            points[count..count + 10].copy_from_slice(&[
                x,
                y,
                screen.x,
                screen.y,
                expected[0],
                expected[1],
                expected[2],
                255.0,
                kind as f32,
                label.compiledindex as f32,
            ]);
            count += 10;
        }
        if count - start != 40 {
            return None;
        }
    }
    // Real canvas samples bound each black/white/black line by adjacent
    // clean fixture pixels. The outer edges have one image-side neighbor;
    // the interior boundary has two. No CPU shader implementation is used.
    // Pick visible cell interiors so crossing grid lines cannot mask a defect.
    // The vertical scrollbar covers the right image edge. Sample the top
    // horizontal outer edge over the fixture's second, unpadded background
    // card, using the same three-pixel raster strip.
    let cell = draw.image.width / columns as f32;
    let first_row = ((draw.clip.y - draw.image.y) / cell - 0.5).ceil().max(0.0);
    let y = draw.image.y + (first_row + 0.5) * cell;
    let centers = [
        draw.image.x + 1.5,
        draw.image.x + cell + 0.5,
        draw.image.y + 1.5,
    ];
    for (edge, center) in centers.into_iter().enumerate() {
        let offsets: &[f32] = if edge == 1 {
            &[-2.0, -1.0, 0.0, 1.0, 2.0]
        } else {
            &[-1.0, 0.0, 1.0, 2.0]
        };
        for &offset in offsets {
            let point = if edge == 2 {
                iced::Point::new(draw.image.x + cell * 1.5, center + offset)
            } else {
                iced::Point::new(center + offset, y)
            };
            if !draw.clip.contains(point) || !draw.image.contains(point) {
                return None;
            }
            let color = if offset.abs() == 2.0 {
                [48.0, 80.0, 112.0]
            } else if offset == 0.0 {
                [255.0; 3]
            } else {
                [0.0; 3]
            };
            points[count..count + 10].copy_from_slice(&[
                point.x - draw.image.x,
                point.y - draw.image.y,
                point.x,
                point.y,
                color[0],
                color[1],
                color[2],
                255.0,
                4.0,
                0.0,
            ]);
            count += 10;
        }
    }
    Some(AtlasCompositionSamples {
        points,
        count,
        cards,
        card_count,
    })
}

pub(super) fn sample_atlas_composition(draw: &AtlasDraw) {
    #[cfg(target_arch = "wasm32")]
    {
        if !crate::presentation_surface::pixel_trace::enabled() {
            return;
        }
        let Some(samples) = atlas_composition_samples(draw) else {
            return;
        };
        let frame = draw.surface.frame.expect("sampleable atlas composition");
        let Some(mut output) = atlas_probe_output(true) else {
            return;
        };
        let receipt = draw.clone();
        let canvas_probe = output.canvas_probe.clone();
        let completed = pixel_result_callback(move |outcome| {
            let _ = output.try_send(Message::AtlasComposition { receipt, outcome });
        });
        atlas_composition_js(
            &canvas_probe,
            &samples.points[..samples.count],
            &samples.cards[..samples.card_count],
            &format!(
                "{{{}{},\"image_x\":{},\"image_y\":{},\"image_width\":{},\"image_height\":{}}}",
                crate::presentation_surface::surface_trace_fields(draw.surface, draw.surface),
                crate::presentation_surface::gallery_trace_fields(Some(&draw.snapshot)),
                draw.image.x,
                draw.image.y,
                draw.image.width,
                draw.image.height
            ),
            frame.content_sequence as f64,
            frame.presentation_revision as f64,
            draw.snapshot.viewport.columns,
            &completed,
        );
    }
    #[cfg(not(target_arch = "wasm32"))]
    let _ = draw;
}

pub(super) fn pixel_fixture_enabled() -> bool {
    PIXEL_FIXTURE_ENABLED.with(std::cell::Cell::get)
}

#[cfg(target_arch = "wasm32")]
pub(super) fn pixel_result_callback(
    completed: impl FnOnce(ProbeOutcome) + 'static,
) -> wasm_bindgen::JsValue {
    // JsValue parameters keep malformed adapter values observable before conversion.
    let generation = observer_generation();
    wasm_bindgen::closure::Closure::once_into_js(
        move |status: wasm_bindgen::JsValue,
              first: wasm_bindgen::JsValue,
              second: wasm_bindgen::JsValue| {
            if observer_generation() != generation || !reporting_enabled() {
                return;
            }
            let status = status.as_string();
            let outcome =
                ProbeOutcome::decode(status.as_deref(), [first.as_f64(), second.as_f64()]);
            if outcome == ProbeOutcome::Failed && status.as_deref() != Some("failed") {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.failure",
                        "",
                        "pixel callback returned invalid counts",
                        [0.0; 4],
                    )
                });
            }
            completed(outcome);
        },
    )
}

#[cfg(target_arch = "wasm32")]
pub(super) fn sample_workspace_fps(
    mut output: ScenarioOutput,
    evidence: reporting::FpsEvidence,
    scale: f32,
) {
    use wasm_bindgen::JsCast;
    let receipt = output.canvas_probe.clone();
    let completed = wasm_bindgen::closure::Closure::once_into_js(
        move |status: wasm_bindgen::JsValue,
              extent: wasm_bindgen::JsValue,
              pixels: wasm_bindgen::JsValue| {
            // Reset/disable still settles the once callback. It never copies stale bytes.
            let current = reporting_enabled() && observer_generation() == output.generation;
            let status = status.as_string();
            let outcome = if !current {
                FpsPixelOutcome::Cancelled
            } else if status.as_deref() == Some("invalidated")
                && extent.as_f64() == Some(0.0)
                && pixels.as_f64() == Some(0.0)
            {
                FpsPixelOutcome::Invalidated
            } else if status.as_deref() == Some("observed")
                && js_sys::Array::is_array(&extent)
                && pixels.is_instance_of::<js_sys::Uint8ClampedArray>()
            {
                let extent = js_sys::Array::from(&extent);
                let pixels = pixels.unchecked_into::<js_sys::Uint8ClampedArray>();
                match (extent.get(0).as_f64(), extent.get(1).as_f64()) {
                    (Some(width), Some(height))
                        if extent.length() == 2
                            && [width, height].into_iter().all(|v| {
                                v.is_finite()
                                    && v > 0.0
                                    && v <= f64::from(u32::MAX)
                                    && v.fract() == 0.0
                            })
                            && pixel_checks::FpsPixels::valid_extent(
                                width as u32,
                                height as u32,
                                pixels.length() as usize,
                            ) =>
                    {
                        FpsPixelOutcome::Captured(pixel_checks::FpsPixels {
                            width: width as u32,
                            height: height as u32,
                            rgba: pixels.to_vec(),
                        })
                    }
                    _ => FpsPixelOutcome::Failed,
                }
            } else {
                FpsPixelOutcome::Failed
            };
            output.send(Message::WorkspaceFpsPixels(outcome));
        },
    );
    fps_pixels_js(
        &receipt,
        &evidence.canvas_values(),
        f64::from(scale),
        &completed,
    );
}

#[cfg(target_arch = "wasm32")]
pub(super) fn sample_upscale_pixels(
    mut output: ScenarioOutput,
    image_pixels: Rectangle,
    button_css: Rectangle,
    source: u64,
    presentation: u64,
) {
    let canvas_probe = output.canvas_probe.clone();
    let completed = pixel_result_callback(move |outcome| {
        let _ = output.try_send(Message::UpscalePixels {
            source,
            presentation,
            outcome,
        });
    });
    upscale_pixels_js(
        &canvas_probe,
        &[
            image_pixels.x,
            image_pixels.y,
            image_pixels.width,
            image_pixels.height,
        ],
        &[
            button_css.x + 4.0,
            button_css.y + 4.0,
            (button_css.width - 8.0).max(1.0),
            (button_css.height - 8.0).max(1.0),
        ],
        source as f64,
        presentation as f64,
        &completed,
    );
}

#[cfg(not(target_arch = "wasm32"))]
pub(super) fn sample_upscale_pixels(
    _output: ScenarioOutput,
    _image: Rectangle,
    _button: Rectangle,
    _source: u64,
    _presentation: u64,
) {
}

#[derive(Debug, Clone, PartialEq)]
pub struct AtlasDraw {
    pub surface: crate::presentation_surface::Surface,
    pub snapshot: std::sync::Arc<crate::generated::ExploreImageMetadata>,
    pub bounds: Rectangle,
    pub image: Rectangle,
    pub clip: Rectangle,
}

impl AtlasDraw {
    pub(super) fn visible_slot(&self, compiled_index: u32) -> Option<usize> {
        self.snapshot
            .order
            .visibleindices
            .iter()
            .position(|value| *value == compiled_index)
    }
}

#[cfg(target_arch = "wasm32")]
pub(super) fn atlas_resize_dimensions(step: u8) -> (f64, f64) {
    [
        (1200.0, 850.0),
        (1000.0, 1020.0),
        (1300.0, 760.0),
        (1500.0, 600.0),
    ][step as usize]
}

pub(super) fn atlas_scroll_window(size: iced::Size, columns: u32) -> (u32, f32) {
    let visible = size.height / (size.width / columns.max(1) as f32);
    let rows = visible.ceil().max(1.0) as u32;
    // Cross the next row boundary with margin on either side, using the
    // widget's actual logical size rather than rounded GPU clip coordinates.
    (rows, (rows as f32 - visible + 1.0) * 0.5)
}

pub(crate) fn report_atlas_draw(draw: AtlasDraw, dark: bool, scale: f32) {
    let snapshot = draw.snapshot.clone();
    let frame = draw.surface.frame.expect("encoded atlas frame");
    let image = draw.image;
    let clip = image
        .intersection(&draw.clip)
        .expect("visible encoded atlas");
    let visibility = u8::from(snapshot.overlay.showboxes)
        | (u8::from(snapshot.overlay.showmasks) << 1)
        | (u8::from(snapshot.overlay.showlabels) << 2);
    reporting::emit(|sink| {
        sink.record(
            "integration.atlas_geometry",
            EXPLORE_GALLERY,
            "uniform-square",
            [
                frame.presentation_revision as f64,
                frame.content_sequence as f64,
                (image.width / snapshot.viewport.columns.max(1) as f32) as f64,
                (image.height / snapshot.viewport.rowcount.max(1) as f32) as f64,
            ],
        )
    });
    reporting::emit(|sink| {
        sink.record(
            "integration.atlas_scale",
            EXPLORE_GALLERY,
            "source-to-screen",
            [
                frame.presentation_revision as f64,
                frame.content_sequence as f64,
                (image.width / frame.content_width.max(1) as f32) as f64,
                (image.height
                    / (snapshot.gallery.layout.cardextent.max(1) as f32
                        * snapshot.viewport.rowcount.max(1) as f32)) as f64,
            ],
        )
    });
    reporting::emit(|sink| {
        sink.record(
            "integration.atlas_clip",
            EXPLORE_GALLERY,
            "scrollable-clip",
            [
                frame.presentation_revision as f64,
                frame.content_sequence as f64,
                (clip.y - image.y) as f64,
                (image.y + image.height - clip.y - clip.height) as f64,
            ],
        )
    });
    reporting::emit(|sink| {
        sink.record(
            "integration.atlas_visibility",
            EXPLORE_GALLERY,
            if dark { "dark" } else { "light" },
            [
                frame.content_sequence as f64,
                visibility as f64,
                snapshot
                    .gallery
                    .slots
                    .iter()
                    .filter(|ready| **ready)
                    .count() as f64,
                snapshot.gallery.slots.len() as f64,
            ],
        )
    });
    let new_draw = observe_atlas_draw(&draw, visibility, scale);
    if new_draw {
        sample_atlas_composition(&draw);
    }
    if new_draw
        && (COMPLETION_WITHOUT_INPUT.with(std::cell::Cell::get) || pixel_fixture_enabled())
        && !snapshot.gallery.slots.is_empty()
        && (pixel_fixture_enabled() || snapshot.gallery.slots.iter().all(|ready| *ready))
    {
        sample_atlas_pixels(draw);
    }
}

#[cfg(target_arch = "wasm32")]
pub(super) fn sample_atlas_pixels(draw: AtlasDraw) {
    let rectangles = atlas_pixel_rectangles(&draw);
    let frame = draw.surface.frame.expect("drawn atlas publication");
    let columns = draw.snapshot.viewport.columns.max(1);
    let side = draw.image.width / columns as f32;
    let cards: Vec<u32> = rectangles
        .chunks_exact(4)
        .map(|rect| {
            let column = ((rect[0] + rect[2] * 0.5 - draw.image.x) / side) as usize;
            let row = ((rect[1] + rect[3] * 0.5 - draw.image.y) / side) as usize;
            draw.snapshot.order.visibleindices[row * columns as usize + column]
        })
        .collect();
    let fields = format!(
        "{{{}{},\"image\":[{},{},{},{}],\"overlay_boxes\":{},\"overlay_masks\":{},\"overlay_labels\":{}}}",
        crate::presentation_surface::surface_trace_fields(draw.surface, draw.surface),
        crate::presentation_surface::gallery_trace_fields(Some(&draw.snapshot)),
        draw.image.x,
        draw.image.y,
        draw.image.width,
        draw.image.height,
        draw.snapshot.overlay.showboxes,
        draw.snapshot.overlay.showmasks,
        draw.snapshot.overlay.showlabels
    );
    let Some(mut output) = atlas_probe_output(false) else {
        return;
    };
    let canvas_probe = output.canvas_probe.clone();
    let completed = pixel_result_callback(move |outcome| {
        let _ = output.try_send(Message::AtlasPixels {
            receipt: draw,
            outcome,
        });
    });
    atlas_pixels_js(
        &canvas_probe,
        &rectangles,
        &cards,
        &fields,
        frame.content_sequence as f64,
        frame.presentation_revision as f64,
        &completed,
    );
}

#[cfg(not(target_arch = "wasm32"))]
pub(super) fn sample_atlas_pixels(_draw: AtlasDraw) {}

#[cfg(any(target_arch = "wasm32", test))]
pub(super) fn atlas_pixel_rectangles(draw: &AtlasDraw) -> Vec<f32> {
    let snapshot = &draw.snapshot;
    let columns = snapshot.viewport.columns.max(1);
    let side = draw.image.width / columns as f32;
    let mut rectangles = Vec::with_capacity(snapshot.gallery.slots.len() * 4);
    for (slot, ready) in snapshot.gallery.slots.iter().enumerate() {
        if !*ready {
            continue;
        }
        // Interior samples exclude the grid and card boundary. Intersect with
        // the real draw clip so an offscreen tile cannot satisfy acceptance.
        let tile = Rectangle {
            x: draw.image.x + (slot as u32 % columns) as f32 * side + side * 0.2,
            y: draw.image.y + (slot as u32 / columns) as f32 * side + side * 0.2,
            width: side * 0.6,
            height: side * 0.6,
        };
        if let Some(visible) = tile.intersection(&draw.clip)
            && visible.width >= 2.0
            && visible.height >= 2.0
        {
            rectangles.extend([visible.x, visible.y, visible.width, visible.height]);
        }
    }
    rectangles
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct SampleablePresentation {
    pub(super) source_revision: u64,
    pub(super) presentation_revision: u64,
    pub(super) content_width: u32,
    pub(super) content_height: u32,
    pub(super) capability_width: u32,
    pub(super) capability_height: u32,
}

pub(super) fn sampleable_presentation(
    frame: Option<crate::presentation_surface::FrameReady>,
    source: crate::generated::PresentationSourceKind,
    source_revision: u64,
) -> Option<SampleablePresentation> {
    let frame = frame?;
    if let Some(crate::presentation_surface::ExploreDisplay::Detail(retained, content)) =
        crate::presentation_surface::explore_display(None)
        && retained.frame == Some(frame)
        && ((source == crate::generated::PresentationSourceKind::Explore
            && content.input_frame().revision == source_revision)
            || (content.frame().source.kind == source
                && content.frame().revision == source_revision))
    {
        return Some(SampleablePresentation {
            source_revision: content.frame().revision,
            presentation_revision: frame.presentation_revision,
            content_width: frame.content_width,
            content_height: frame.content_height,
            capability_width: retained.width,
            capability_height: retained.height,
        });
    }
    let product = crate::presentation_surface::metadata::product(frame)?;
    let surface = crate::presentation_surface::metadata::surface(frame)?;
    (product.source.kind == source && product.revision == source_revision).then_some(
        SampleablePresentation {
            source_revision,
            presentation_revision: frame.presentation_revision,
            content_width: frame.content_width,
            content_height: frame.content_height,
            capability_width: surface.width,
            capability_height: surface.height,
        },
    )
}

pub(super) fn displayed_detail(
    surface: Option<crate::presentation_surface::Surface>,
    snapshot: &crate::generated::ExploreSnapshot,
) -> Option<crate::presentation_surface::Surface> {
    let crate::presentation_surface::ExploreDisplay::Detail(shown, content) =
        crate::presentation_surface::explore_display(surface)?
    else {
        return None;
    };
    (content.viewer_identity()
        == snapshot
            .selectedimage
            .map(|image| (snapshot.dataset.identity, image))
        && content.input_frame() == &snapshot.frame)
        .then_some(shown)
}

pub(super) fn fully_drawn_gallery(
    frame: Option<crate::presentation_surface::FrameReady>,
    snapshot: &crate::generated::ExploreSnapshot,
    drawn: Option<(u64, u64)>,
) -> Option<SampleablePresentation> {
    if !snapshot.ready
        || snapshot.busy
        || snapshot.mode != crate::generated::ExploreMode::Gallery
        || snapshot.gallery.generation == 0
        || snapshot.gallery.slots.is_empty()
        || snapshot.gallery.slots.len() != snapshot.order.visibleindices.len()
        || snapshot.gallery.slots.iter().any(|ready| !*ready)
    {
        return None;
    }
    let sampleable = sampleable_presentation(
        frame,
        crate::generated::PresentationSourceKind::Explore,
        snapshot.frame.revision,
    )?;
    (drawn == Some((sampleable.presentation_revision, sampleable.source_revision)))
        .then_some(sampleable)
}

/// Mutable observations owned by this scenario or mechanism.
pub(super) struct State {
    workspace_fps_baseline: bool,
    workspace_fps_verified: bool,
    workspace_fps_evidence: Option<reporting::FpsEvidence>,
    workspace_fps_probe: Option<ScenarioOutput>,
    workspace_fps_receipt: Option<ProbeReceipt>,
    workspace_fps_scale: f32,
    workspace_fps_result: Option<FpsPixelOutcome>,
    workspace_fps_failure: Option<&'static str>,
}
impl Default for State {
    fn default() -> Self {
        Self {
            workspace_fps_baseline: false,
            workspace_fps_verified: false,
            workspace_fps_evidence: None,
            workspace_fps_probe: None,
            workspace_fps_receipt: None,
            workspace_fps_scale: 1.0,
            workspace_fps_result: None,
            workspace_fps_failure: None,
        }
    }
}

#[derive(Debug, Clone)]
pub struct FpsPixels {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
}

impl FpsPixels {
    pub(super) fn valid_extent(width: u32, height: u32, bytes: usize) -> bool {
        width > 4
            && height > 4
            && bytes <= 1_048_576
            && (width as usize)
                .checked_mul(height as usize)
                .and_then(|pixels| pixels.checked_mul(4))
                == Some(bytes)
    }
}

pub(super) fn verify_workspace_fps_pixels(
    image: &FpsPixels,
    evidence: reporting::FpsEvidence,
) -> bool {
    if !reporting_enabled() {
        return false;
    }
    let bounds = evidence.bounds;
    let clip = evidence.clip;
    if ![
        bounds.x,
        bounds.y,
        bounds.width,
        bounds.height,
        clip.x,
        clip.y,
        clip.width,
        clip.height,
    ]
    .into_iter()
    .all(f32::is_finite)
        || bounds.width <= 0.0
        || bounds.height <= 0.0
        || clip.width <= 0.0
        || clip.height <= 0.0
        || bounds.x < clip.x
        || bounds.y < clip.y
        || bounds.x + bounds.width > clip.x + clip.width
        || bounds.y + bounds.height > clip.y + clip.height
        || (clip.x + clip.width - bounds.x - bounds.width - 6.0).abs() > 0.01
        || (bounds.y - clip.y - 6.0).abs() > 0.01
        || !FpsPixels::valid_extent(image.width, image.height, image.rgba.len())
        || evidence.frames == 0
        || !evidence.seconds.is_finite()
        || evidence.seconds < 0.5
    {
        return false;
    }
    let mut background = 0;
    let mut foreground = 0;
    let mut border = 0;
    let mut border_background = 0;
    for y in 0..image.height {
        for x in 0..image.width {
            let offset = (y as usize * image.width as usize + x as usize) * 4;
            let pixel = &image.rgba[offset..offset + 4];
            if pixel[3] != 255 {
                return false;
            }
            let low = pixel[..3].iter().copied().min().unwrap();
            let high = pixel[..3].iter().copied().max().unwrap();
            let back = if evidence.dark { high <= 8 } else { low >= 247 };
            let front = if evidence.dark {
                low >= 160
            } else {
                high <= 95
            };
            background += usize::from(back);
            foreground += usize::from(front);
            if x == 0 || x + 1 == image.width || y == 0 || y + 1 == image.height {
                border += 1;
                border_background += usize::from(back);
            }
        }
    }
    let valid = background > foreground && foreground >= 12 && border_background * 10 >= border * 9;
    reporting::emit(|sink| {
        sink.record(
            "integration.workspace_fps_pixels",
            EXPLORE_GALLERY,
            if valid {
                "visible-counter"
            } else {
                "invalid-counter"
            },
            [
                evidence.frames as f64,
                evidence.seconds,
                foreground as f64,
                border_background as f64,
            ],
        )
    });
    valid
}

impl State {
    pub(super) fn advance_pixel_checks(
        &mut self,
        driver: &mut Driver,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        applied_scale: f32,
    ) -> Task<RootMessage> {
        match driver.phase.clone() {
            Phase::AwaitWorkspaceFps => {
                if self.workspace_fps_evidence.is_none()
                    || settings.has_local_edits()
                    || model.native_settings_unsettled()
                    || !crate::workspace_fps::enabled(settings)
                {
                    return Task::none();
                }
                let evidence = self.workspace_fps_evidence.expect("observed FPS evidence");
                let current = current_fps_draw();
                if current != Some(evidence)
                    || self.workspace_fps_receipt != current_receipt(EXPLORE_GALLERY)
                {
                    self.rearm_workspace_fps(driver);
                    return Task::none();
                }
                let Some(output) = probe_output(EXPLORE_GALLERY) else {
                    return Task::none();
                };
                self.workspace_fps_probe = Some(output.clone());
                self.workspace_fps_scale = applied_scale;
                driver.phase = Phase::AwaitWorkspaceFpsPixels;
                #[cfg(target_arch = "wasm32")]
                sample_workspace_fps(output, evidence, applied_scale);
                Task::none()
            }
            Phase::AwaitWorkspaceFpsPixels => {
                if self.workspace_fps_scale != applied_scale || !self.workspace_fps_probe_current()
                {
                    self.rearm_workspace_fps(driver);
                } else if let Some(outcome) = self.workspace_fps_result.take() {
                    let valid = match (outcome, self.workspace_fps_evidence) {
                        (FpsPixelOutcome::Captured(image), Some(evidence)) => {
                            pixel_checks::verify_workspace_fps_pixels(&image, evidence)
                        }
                        _ => false,
                    };
                    self.workspace_fps_probe = None;
                    self.workspace_fps_failure = (!valid).then_some(WORKSPACE_FPS_PIXEL_FAILURE);
                    return self.restore_workspace_fps(driver);
                }
                Task::none()
            }
            Phase::RestoreWorkspaceFps => self.restore_workspace_fps(driver),
            Phase::AwaitWorkspaceFpsRestored
                if !settings.has_local_edits()
                    && !model.native_settings_unsettled()
                    && crate::workspace_fps::enabled(settings) == self.workspace_fps_baseline
                    && model.settings_snapshot.as_ref().is_some_and(|snapshot| {
                        snapshot.settingsstate.ui.showworkspaceperformance
                            == self.workspace_fps_baseline
                    }) =>
            {
                if let Some(failure) = self.workspace_fps_failure {
                    driver.fail(failure);
                    Task::none()
                } else {
                    self.workspace_fps_verified = true;
                    driver.advance_to(Phase::AwaitExploreReady)
                }
            }
            _ => Task::none(),
        }
    }
    pub(super) fn workspace_fps_probe_current(&self) -> bool {
        if !reporting_enabled() {
            return false;
        }
        let (Some(output), Some(evidence)) =
            (&self.workspace_fps_probe, self.workspace_fps_evidence)
        else {
            return false;
        };
        if output.receipt != current_receipt(EXPLORE_GALLERY)
            || current_fps_draw() != Some(evidence)
        {
            return false;
        }
        #[cfg(target_arch = "wasm32")]
        {
            fps_current_js(&output.canvas_probe, &evidence.canvas_values())
        }
        #[cfg(not(target_arch = "wasm32"))]
        {
            true
        }
    }
    pub(super) fn cancel_workspace_fps(&mut self, driver: &mut Driver) {
        self.workspace_fps_probe = None;
        self.workspace_fps_result = None;
        self.workspace_fps_failure = Some("Workspace FPS capture was cancelled");
        driver.phase = Phase::RestoreWorkspaceFps;
    }
    pub(super) fn restore_workspace_fps(&mut self, driver: &mut Driver) -> Task<RootMessage> {
        driver.phase = Phase::AwaitWorkspaceFpsRestored;
        Task::done(RootMessage::Settings(
            crate::view::settings::Message::PerformanceChanged(self.workspace_fps_baseline),
        ))
    }
    pub(super) fn rearm_workspace_fps(&mut self, driver: &mut Driver) {
        self.workspace_fps_probe = None;
        self.workspace_fps_receipt = None;
        self.workspace_fps_evidence = None;
        self.workspace_fps_result = None;
        rearm_fps_sampling();
        driver.phase = Phase::AwaitWorkspaceFps;
    }
}

impl State {
    pub(super) fn callback(
        &mut self,
        driver: &mut Driver,
        message: Message,
        request_receipt: Option<ProbeReceipt>,
    ) {
        match message {
            Message::WorkspaceFpsDrawn(evidence) => {
                if driver.phase == Phase::AwaitWorkspaceFps
                    && request_receipt
                        .as_ref()
                        .is_some_and(|receipt| receipt.control == EXPLORE_GALLERY)
                {
                    self.workspace_fps_evidence = Some(evidence);
                    self.workspace_fps_receipt = request_receipt;
                }
                return;
            }
            Message::WorkspaceFpsPixels(outcome) => {
                let current = self.workspace_fps_probe_current();
                if !reporting_enabled() || matches!(outcome, FpsPixelOutcome::Cancelled) {
                    self.cancel_workspace_fps(driver);
                } else if !current || matches!(outcome, FpsPixelOutcome::Invalidated) {
                    self.rearm_workspace_fps(driver);
                } else {
                    // The next driver advance validates the applied UI scale as
                    // well as the frozen draw before classifying or reporting pixels.
                    self.workspace_fps_result = Some(outcome);
                }
                return;
            }
            _ => unreachable!("callback routed to the wrong scenario owner"),
        }
    }
}

impl State {
    pub(super) fn accepts_fps_completion(
        &self,
        driver: &Driver,
        cancellation: bool,
        owner: Option<&std::sync::Arc<()>>,
    ) -> bool {
        driver.phase == Phase::AwaitWorkspaceFpsPixels
            && (cancellation || self.workspace_fps_result.is_none())
            && self
                .workspace_fps_probe
                .as_ref()
                .is_some_and(|output| same_probe(&output.probe, owner))
    }
    pub(super) fn begin_workspace_fps(
        &mut self,
        driver: &mut Driver,
        settings: &crate::view::settings::SettingsModel,
    ) -> Option<Task<RootMessage>> {
        if !reporting_enabled() || self.workspace_fps_verified {
            return None;
        }
        self.workspace_fps_baseline = crate::workspace_fps::enabled(settings);
        self.workspace_fps_evidence = None;
        self.workspace_fps_failure = None;
        rearm_fps_sampling();
        driver.phase = Phase::AwaitWorkspaceFps;
        Some(Task::done(RootMessage::Settings(
            crate::view::settings::Message::PerformanceChanged(true),
        )))
    }
}

pub(super) const WORKSPACE_FPS_PIXEL_FAILURE: &str =
    "Workspace FPS canvas capture did not contain its upper-right counter background and text";

#[cfg(test)]
pub(in crate::integration_control) mod tests;
