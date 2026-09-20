use super::{PlacementGeometry, Program, Surface, WorkspaceViewport, placement_geometry};
use crate::fluent_theme::{Element, Theme};
use iced::advanced::Renderer as _;
use iced::advanced::text::{Paragraph as _, Renderer as _};
use iced::advanced::{Layout, Shell, Widget, layout, mouse, renderer, text, widget};
use iced::{Color, Event, Fill, Point, Rectangle, Size};

fn label_bounds(
    geometry: PlacementGeometry,
    region: [u32; 4],
    bounds: &crate::generated::AnnotationBox,
    width: f32,
) -> Rectangle {
    Rectangle {
        x: geometry.x + (bounds.first.x - region[0] as f32) * geometry.width / region[2] as f32,
        y: geometry.y + (bounds.first.y - region[1] as f32) * geometry.height / region[3] as f32,
        width,
        height: 19.0,
    }
}

fn cell_bounds(geometry: PlacementGeometry, region: [u32; 4], cell: [u32; 4]) -> Rectangle {
    Rectangle {
        x: geometry.x + (cell[0] as f32 - region[0] as f32) * geometry.width / region[2] as f32,
        y: geometry.y + (cell[1] as f32 - region[1] as f32) * geometry.height / region[3] as f32,
        width: cell[2] as f32 * geometry.width / region[2] as f32,
        height: cell[3] as f32 * geometry.height / region[3] as f32,
    }
}

pub(crate) struct GalleryContent {
    pub(crate) metadata: std::sync::Arc<crate::generated::ExploreImageMetadata>,
    captions: Vec<CachedLabel>,
}
impl GalleryContent {
    pub(crate) fn new(metadata: std::sync::Arc<crate::generated::ExploreImageMetadata>) -> Self {
        let captions = metadata.dataset.classnames.iter()
            .map(|name| CachedLabel::new(name.value.clone())).collect();
        Self { metadata, captions }
    }
}

pub(crate) struct PredictionContent {
    pub(super) metadata: std::sync::Arc<crate::generated::PredictImageMetadata>,
    labels: Vec<CachedLabel>,
}
pub(super) struct CachedLabel {
    text: String,
    width: f32,
    paragraph: iced::advanced::graphics::text::Paragraph,
    background: Option<[u8; 3]>,
    cell: Option<[u32; 4]>,
}
fn label_text<Content>(content: Content, width: f32) -> text::Text<Content> {
    text::Text {
        content,
        bounds: Size::new(width - 6.0, 19.0),
        size: iced::Pixels(12.0),
        line_height: text::LineHeight::default(),
        font: iced::Font::DEFAULT,
        align_x: text::Alignment::Left,
        align_y: iced::alignment::Vertical::Center,
        shaping: text::Shaping::Advanced,
        wrapping: text::Wrapping::None,
        ellipsis: text::Ellipsis::default(),
        hint_factor: None,
    }
}
impl CachedLabel {
    pub(super) fn new(text: String) -> Self {
        let width = (text.chars().count() as f32 * 7.5 + 8.0).max(20.0);
        let paragraph =
            iced::advanced::graphics::text::Paragraph::with_text(label_text(text.as_str(), width));
        Self {
            text,
            width,
            paragraph,
            background: None,
            cell: None,
        }
    }
}
impl PredictionContent {
    pub(crate) fn new(
        metadata: impl Into<std::sync::Arc<crate::generated::PredictImageMetadata>>,
    ) -> Self {
        let metadata = metadata.into();
        let labels = metadata
            .labels
            .iter()
            .map(|item| {
                let text = match item.classdomain {
                    crate::generated::ClassReferenceDomain::Foreground => {
                        format!("{} {}", item.name, item.confidence)
                    }
                    crate::generated::ClassReferenceDomain::RawOutputSlot => {
                        format!("Raw slot {} {}", item.classreference, item.confidence)
                    }
                };
                CachedLabel::new(text)
            })
            .collect();
        Self { metadata, labels }
    }
}

pub(crate) struct ValidationContent {
    pub(crate) metadata: std::sync::Arc<crate::generated::ValidationImageMetadata>,
    labels: Vec<(usize, usize, crate::generated::AnnotationBox, CachedLabel)>,
    pub(crate) upscale: Option<std::sync::Arc<crate::generated::UpscaleImageMetadata>>,
}
impl ValidationContent {
    pub(crate) fn new(
        metadata: impl Into<std::sync::Arc<crate::generated::ValidationImageMetadata>>,
    ) -> Self {
        let metadata = metadata.into();
        let mut labels = Vec::new();
        for (sample_index, sample) in metadata.samples.iter().enumerate().filter(|(_, sample)| {
            sample.available
                && (!metadata.detail || metadata.selected.as_ref() == Some(&sample.identity))
        }) {
            for (label_index, label) in sample.labels.iter().enumerate() {
                let mut bounds = label.box_.clone();
                for point in [&mut bounds.first, &mut bounds.second] {
                    point.x = sample.crop.x as f32
                        + point.x * sample.crop.width as f32 / sample.pixelextent.width as f32;
                    point.y = sample.crop.y as f32
                        + point.y * sample.crop.height as f32 / sample.pixelextent.height as f32;
                }
                labels.push((sample_index, label_index, bounds, {
                    let mut cached = CachedLabel::new(label.name.clone());
                    cached.background = Some(label.rgb.0);
                    cached.cell = Some(if metadata.detail {
                        [
                            0,
                            0,
                            metadata.frame.extent.width,
                            metadata.frame.extent.height,
                        ]
                    } else {
                        let width = metadata.frame.extent.width / 2;
                        let height = metadata.frame.extent.height / 3;
                        [
                            sample_index as u32 % 2 * width,
                            sample_index as u32 / 2 * height,
                            width,
                            height,
                        ]
                    });
                    cached
                }));
            }
        }
        Self {
            metadata,
            labels,
            upscale: None,
        }
    }
    pub(crate) fn frame(&self) -> &crate::generated::VisualFrame {
        self.upscale
            .as_ref()
            .map_or(&self.metadata.frame, |value| &value.frame)
    }
    pub(crate) fn with_upscale(
        mut self,
        upscale: std::sync::Arc<crate::generated::UpscaleImageMetadata>,
    ) -> Self {
        let scale_x = upscale.frame.extent.width as f32 / self.metadata.frame.extent.width as f32;
        let scale_y = upscale.frame.extent.height as f32 / self.metadata.frame.extent.height as f32;
        for (_, _, bounds, cached) in &mut self.labels {
            if let Some(cell) = &mut cached.cell {
                cell[0] = (cell[0] as f32 * scale_x) as u32;
                cell[1] = (cell[1] as f32 * scale_y) as u32;
                cell[2] = (cell[2] as f32 * scale_x) as u32;
                cell[3] = (cell[3] as f32 * scale_y) as u32;
            }
            for point in [&mut bounds.first, &mut bounds.second] {
                point.x *= scale_x;
                point.y *= scale_y;
            }
        }
        self.upscale = Some(upscale);
        self
    }
}

#[derive(Clone)]
pub(crate) enum Source {
    Gallery(std::sync::Arc<GalleryContent>, bool),
    Detail(super::DetailContent, bool),
    Prediction(std::sync::Arc<PredictionContent>),
    Validation(std::sync::Arc<ValidationContent>, bool, bool),
    Hidden,
}

impl Source {
    fn caption_layers(&self) -> [Self; 2] {
        match self {
            Self::Validation(content, gt, det) => [
                Self::Validation(content.clone(), *gt, false),
                Self::Validation(content.clone(), false, *det),
            ],
            _ => [self.clone(), Self::Hidden],
        }
    }
    fn visit(
        &self,
        mut label: impl FnMut(
            u16,
            &crate::generated::AnnotationBox,
            &str,
            &crate::generated::AnnotationColor,
            usize,
            Option<&crate::generated::ExploreOverlay>,
            &CachedLabel,
        ),
    ) {
        match self {
            Self::Gallery(snapshot, true) => {
                for item in &snapshot.metadata.labels {
                    if let (Some(name), Some(color)) = (
                        snapshot.captions.get(item.category as usize),
                        snapshot.metadata.dataset.palette.get(item.category as usize),
                    ) {
                        label(
                            item.category,
                            &item.box_,
                            &name.text,
                            color,
                            snapshot.metadata.dataset.classnames.len(),
                            Some(&snapshot.metadata.overlay),
                            name,
                        );
                    }
                }
            }
            Self::Detail(content, true) => {
                let scene = content.scene();
                let overlay = content.overlay();
                for object in &scene.objects {
                    if !object.enabled
                        || !crate::view::explore::dataset::selection_contains(
                            &overlay.classselection,
                            u32::from(object.category),
                        )
                    {
                        continue;
                    }
                    if let (Some(name), Some(color)) = (
                        content.labels.get(object.category as usize),
                        scene.palette.get(object.category as usize),
                    ) {
                        label(
                            object.category,
                            &object.box_,
                            &name.text,
                            color,
                            scene.categories.len(),
                            Some(overlay),
                            name,
                        );
                    }
                }
            }
            Self::Validation(content, ground_truth, predictions) => {
                for (sample, index, bounds, cached) in &content.labels {
                    let item = &content.metadata.samples[*sample].labels[*index];
                    if (item.groundtruth
                        && (!ground_truth || !content.metadata.overlays.groundtruthlayer))
                        || (!item.groundtruth
                            && (!predictions || !content.metadata.overlays.predictionlayer))
                    {
                        continue;
                    }
                    label(
                        item.category as u16,
                        bounds,
                        &cached.text,
                        &item.color,
                        0,
                        None,
                        cached,
                    );
                }
            }
            Self::Prediction(snapshot) => {
                for (item, cached) in snapshot.metadata.labels.iter().zip(&snapshot.labels) {
                    label(
                        item.classreference as u16,
                        &item.box_,
                        &cached.text,
                        &item.color,
                        0,
                        None,
                        cached,
                    );
                }
            }
            _ => {}
        }
    }
}

pub(crate) fn view<'a, Message: 'a>(
    program: Program<Message>,
    source: Source,
) -> Element<'a, Message> {
    let surface = program.surface;
    let placement = program.placement;
    let transform_surface = program.surface;
    let show_fps = program.show_fps;
    let control_id = program.control_id;
    Element::new(Labelled {
        child: iced::widget::shader(program)
            .width(Fill)
            .height(Fill)
            .into(),
        surface,
        transform_surface,
        placement,
        source,
        show_fps,
        control_id,
    })
}

struct Labelled<'a, Message> {
    child: Element<'a, Message>,
    surface: Surface,
    transform_surface: Surface,
    placement: super::Placement,
    source: Source,
    show_fps: bool,
    control_id: &'static str,
}

impl<Message> Widget<Message, Theme, iced::Renderer> for Labelled<'_, Message> {
    fn diff(&mut self, tree: &mut widget::Tree) {
        tree.diff_children(std::slice::from_mut(&mut self.child));
    }
    fn size(&self) -> Size<iced::Length> {
        self.child.as_widget().size()
    }
    fn layout(
        &mut self,
        tree: &mut widget::Tree,
        renderer: &iced::Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        self.child
            .as_widget_mut()
            .layout(&mut tree.children[0], renderer, limits)
    }
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
        self.child.as_widget_mut().update(
            &mut tree.children[0],
            event,
            layout,
            cursor,
            renderer,
            shell,
            viewport,
        );
        // The visible browser window drives retained draws independently of
        // native content, optional FPS, and troubleshooting activation.
        if matches!(
            event,
            Event::Window(iced::window::Event::RedrawRequested(_))
        ) && layout
            .bounds()
            .intersection(viewport)
            .is_some_and(|clip| clip.width > 0.0 && clip.height > 0.0)
        {
            shell.request_redraw();
        }
    }
    fn mouse_interaction(
        &self,
        tree: &widget::Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
        renderer: &iced::Renderer,
    ) -> mouse::Interaction {
        self.child.as_widget().mouse_interaction(
            &tree.children[0],
            layout,
            cursor,
            viewport,
            renderer,
        )
    }
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
        self.child.as_widget().draw(
            &tree.children[0],
            renderer,
            theme,
            style,
            layout,
            cursor,
            viewport,
        );
        let state = tree.children[0].state.downcast_ref::<WorkspaceViewport>();
        self.draw_labels(state, renderer, theme, layout, viewport);
        if self.show_fps
            && let Some(meter) = &state.fps
            && let Some(clip) = layout.bounds().intersection(viewport)
        {
            // Iced batches parent-layer quads before its shader primitives.
            // Keep both meter background and glyphs above the workspace image.
            renderer.with_layer(clip, |renderer| {
                crate::workspace_fps::draw(renderer, theme, meter, clip, self.control_id);
            });
        }
    }
}

impl<Message> Labelled<'_, Message> {
    fn draw_labels(
        &self,
        state: &WorkspaceViewport,
        renderer: &mut iced::Renderer,
        theme: &Theme,
        layout: Layout<'_>,
        viewport: &Rectangle,
    ) {
        if matches!(
            self.source,
            Source::Hidden | Source::Gallery(_, false) | Source::Detail(_, false)
        ) {
            return;
        }
        let mut bounds = layout.bounds();
        let mut placement = self.placement;
        if let Source::Gallery(snapshot, _) = &self.source {
            bounds.y += super::gallery::row_offset(placement, &snapshot.metadata, bounds.width);
            placement = super::gallery::placement(&snapshot.metadata);
        }
        let Some(geometry) = placement_geometry(
            bounds,
            self.surface.display_extent(),
            placement,
            state.viewport.transform_for(self.transform_surface),
        ) else {
            return;
        };
        let image_bounds = Rectangle {
            x: geometry.x,
            y: geometry.y,
            width: geometry.width,
            height: geometry.height,
        };
        let Some(clip) = layout
            .bounds()
            .intersection(viewport)
            .and_then(|clip| clip.intersection(&image_bounds))
        else {
            return;
        };
        super::gallery::observe_theme(theme);
        let mut region = self.surface.content_region();
        if matches!(self.source, Source::Gallery(..)) {
            let extent = placement.logical_extent(self.surface.content_extent());
            region = [0, 0, extent.0, extent.1];
        }
        // Separate Iced layers keep Det backgrounds above GT glyphs too:
        // the renderer batches a layer's quads before its text.
        for source in self
            .source
            .caption_layers()
            .into_iter()
            .filter(|source| !matches!(source, Source::Hidden))
        {
            renderer.with_layer(clip, |renderer| {
                source.visit(
                    |category, bounds, _name, color, catalog_count, overlay, cached| {
                        let rect = label_bounds(geometry, region, bounds, cached.width);
                        let Some(label_clip) = cached.cell.map_or(Some(clip), |cell| {
                            cell_bounds(geometry, region, cell).intersection(&clip)
                        }) else {
                            return;
                        };
                        let Some(background) = rect.intersection(&label_clip) else {
                            return;
                        };
                        let color = cached.background
                            .map(|rgb| Color::from_rgb8(rgb[0], rgb[1], rgb[2]))
                            .unwrap_or_else(|| class_color(color));
                        renderer.fill_quad(
                            renderer::Quad {
                                bounds: if cached.cell.is_some() {
                                    background
                                } else {
                                    rect
                                },
                                ..Default::default()
                            },
                            color,
                        );
                        let position = Point::new(rect.x + 3.0, rect.y + 9.5);
                        let foreground =
                            if color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722 > 0.55 {
                                Color::BLACK
                            } else {
                                Color::WHITE
                            };
                        renderer.fill_paragraph(
                            &cached.paragraph,
                            position,
                            foreground,
                            label_clip,
                        );
                        if crate::integration_control::reporting_enabled()
                            && let Some(frame) = self.surface.frame
                            && let Some(overlay) = overlay
                        {
                            crate::integration_control::report_viewer_label(
                                category,
                                color,
                                catalog_count,
                                overlay,
                                frame,
                                matches!(self.source, Source::Detail(..)),
                            );
                        }
                    },
                );
            });
        }
    }
}

pub(crate) fn class_color(color: &crate::generated::AnnotationColor) -> Color {
    let hue = color.hue.rem_euclid(360.0) / 60.0;
    let chroma = color.value * color.saturation;
    let secondary = chroma * (1.0 - (hue.rem_euclid(2.0) - 1.0).abs());
    let (r, g, b) = match hue as u32 {
        0 => (chroma, secondary, 0.0),
        1 => (secondary, chroma, 0.0),
        2 => (0.0, chroma, secondary),
        3 => (0.0, secondary, chroma),
        4 => (secondary, 0.0, chroma),
        _ => (chroma, 0.0, secondary),
    };
    let minimum = color.value - chroma;
    Color::from_rgb(r + minimum, g + minimum, b + minimum)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validation_labels_cache_paragraphs_and_toggle_each_domain_independently() {
        let content = std::sync::Arc::new(ValidationContent::new(
            crate::view_model::test_support::validation_image_metadata(),
        ));
        assert_eq!(content.labels.len(), 2);
        assert!((content.labels[0].2.first.x - 51.2).abs() < 0.001);
        assert!((content.labels[0].2.first.y - 38.4).abs() < 0.001);
        let pointer = content.labels[0].3.text.as_ptr();
        for (gt, predictions, expected) in [
            (true, true, 2),
            (true, false, 1),
            (false, true, 1),
            (false, false, 0),
        ] {
            let mut count = 0;
            Source::Validation(content.clone(), gt, predictions).visit(
                |_, _, name, _, _, _, cached| {
                    assert_eq!(cached.text, name);
                    assert_eq!(name, "paired name");
                    if gt && count == 0 {
                        assert_eq!(name.as_ptr(), pointer);
                    }
                    count += 1;
                },
            );
            assert_eq!(count, expected);
        }
        assert_eq!(content.metadata.samples[0].identity.generation, 7);
        assert!(!content.metadata.samples[2].available);
    }

    #[test]
    fn validation_caption_layers_preserve_order_and_detection_wins_all_visibility_combinations() {
        use iced::advanced::renderer::Headless;
        let mut renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
            Default::default(),
            Some("wgpu"),
        ))
        .expect("caption composition requires the container GPU backend");
        let size = Size::new(512.0, 576.0);
        let viewport = Rectangle::with_size(size);
        let node = layout::Node::new(size);
        for detail in [false, true] {
            for flags in 0u8..16 {
                let mut metadata = crate::view_model::test_support::validation_image_metadata();
                metadata.detail = detail;
                metadata.selected = detail.then(|| metadata.samples[0].identity.clone());
                for sample in &mut metadata.samples[1..] {
                    sample.available = false;
                }
                if detail {
                    metadata.samples[0].crop = crate::generated::VisualRegion {
                        x: 0,
                        y: 0,
                        width: 512,
                        height: 576,
                    };
                }
                metadata.overlays.groundtruthlayer = flags & 1 != 0;
                metadata.overlays.predictionlayer = flags & 2 != 0;
                let mut gt = metadata.samples[0].labels[0].clone();
                gt.groundtruth = true;
                gt.rgb.0 = [0, 255, 255];
                gt.name = "MMMMMMMM".into();
                let mut det = gt.clone();
                det.groundtruth = false;
                det.rgb.0 = [255, 0, 0];
                // Spaces leave the Det background unobstructed. Any surviving
                // GT glyph in this rectangle proves incorrect Iced layer order.
                det.name = "        ".into();
                metadata.samples[0].labels = vec![det.clone(), gt.clone(), det, gt];
                let content = std::sync::Arc::new(ValidationContent::new(metadata));
                let source = Source::Validation(content.clone(), flags & 4 != 0, flags & 8 != 0);
                let gt_visible = flags & 5 == 5;
                let det_visible = flags & 10 == 10;
                let mut seen = Vec::new();
                let mut paragraphs = Vec::new();
                for layer in source.caption_layers() {
                    layer.visit(|_, _, _, _, _, _, cached| {

                        seen.push(cached.background.unwrap());
                        paragraphs.push(cached.text.as_ptr());
                    });
                }
                let expected_paragraphs: Vec<_> = [1, 3, 0, 2]
                    .into_iter()
                    .filter(|index| {
                        if index % 2 == 1 {
                            gt_visible
                        } else {
                            det_visible
                        }
                    })
                    .map(|index| content.labels[index].3.text.as_ptr())
                    .collect();
                assert_eq!(paragraphs, expected_paragraphs);
                let expected: Vec<_> = (0..if gt_visible { 2 } else { 0 })
                    .map(|_| [0, 255, 255])
                    .chain((0..if det_visible { 2 } else { 0 }).map(|_| [255, 0, 0]))
                    .collect();
                assert_eq!(seen, expected);
                let mut surface = Surface::empty();
                surface.width = 512;
                surface.height = 576;
                let labelled = Labelled::<()> {
                    child: iced::widget::space::horizontal().into(),
                    surface,
                    transform_surface: surface,
                    placement: super::super::Placement::Contain,
                    source,
                    show_fps: false,
                    control_id: "validation.caption.case",
                };
                renderer.reset(viewport);
                labelled.draw_labels(
                    &WorkspaceViewport::default(),
                    &mut renderer,
                    &crate::fluent_theme::app_theme(false),
                    Layout::new(&node),
                    &viewport,
                );
                let pixels = renderer.screenshot(
                    &iced::widget::shader::Viewport::with_physical_size(Size::new(512, 576), 1.0),
                    Color::WHITE,
                );
                let pixel = |x: usize, y: usize| -> [u8; 3] {
                    pixels[(y * 512 + x) * 4..(y * 512 + x) * 4 + 3]
                        .try_into()
                        .unwrap()
                };
                let y = if detail { 116 } else { 39 };
                assert_eq!(
                    pixel(52, y),
                    if det_visible {
                        [255, 0, 0]
                    } else if gt_visible {
                        [0, 255, 255]
                    } else {
                        [255; 3]
                    }
                );
                if det_visible {
                    for row in y + 2..y + 14 {
                        for column in 55..100 {
                            assert_eq!(pixel(column, row), [255, 0, 0]);
                        }
                    }
                }
            }
        }
    }

    #[test]
    fn explore_captions_keep_the_original_single_render_layer() {
        let explore = std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(
            &crate::view_model::test_support::explore_snapshot(),
        ));
        for visible in [false, true] {
            let layers = Source::Gallery(std::sync::Arc::new(GalleryContent::new(explore.clone())), visible).caption_layers();
            assert!(
                matches!(&layers[0], Source::Gallery(value, labels) if std::sync::Arc::ptr_eq(&value.metadata, &explore) && *labels == visible)
            );
            assert!(matches!(&layers[1], Source::Hidden));
            let layers = Source::Detail(
                super::super::DetailContent::new(explore.clone(), None),
                visible,
            )
            .caption_layers();
            assert!(
                matches!(&layers[0], Source::Detail(value, labels) if std::sync::Arc::ptr_eq(&value.explore, &explore) && *labels == visible)
            );
            assert!(matches!(&layers[1], Source::Hidden));
        }
    }

    #[test]
    fn explore_prepared_captions_follow_the_exact_paired_scene_and_unicode_width() {
        use std::sync::Arc;
        let (model, _) = crate::view_model::test_support::explore_presentation();
        let mut native = crate::generated::ExploreImageMetadata::from(model.explore.snapshot.as_ref().unwrap());
        native.scene.categories = vec![crate::generated::ClassName { value: "source".into() }];
        native.scene.palette = vec![crate::generated::AnnotationColor { hue: 120.0, saturation: 0.7, value: 0.8 }];
        native.scene.objects = vec![crate::view_model::test_support::annotation_object(0)];
        native.overlay.showlabels = true;
        native.overlay.classselection.mode = crate::generated::ExploreClassSelectionMode::All;
        native.dataset.classnames = vec![crate::generated::ClassName { value: "人é🙂".into() }];
        native.dataset.palette = native.scene.palette.clone();
        native.labels = vec![crate::generated::ExploreLabel {
            box_: native.scene.objects[0].box_.clone(), category: 0, compiledindex: 0,
        }];
        let gallery = Arc::new(GalleryContent::new(Arc::new(native.clone())));
        assert_eq!(gallery.captions[0].text, "人é🙂");
        assert_eq!(gallery.captions[0].width, 30.5);
        assert_eq!(CachedLabel::new(String::new()).width, 20.0);
        for _ in 0..3 {
            Source::Gallery(gallery.clone(), true).visit(|category, _, name, color, _, _, cached| {
                assert_eq!((category, name), (0, "人é🙂"));
                assert_eq!(color, &native.dataset.palette[0]);
                assert!(std::ptr::eq(cached, &gallery.captions[0]));
                assert_eq!(cached.paragraph.compare(label_text((), 30.5)), text::Difference::None);
            });
        }
        let native = Arc::new(native);
        let original = super::super::DetailContent::new(native.clone(), None);
        let mut derived = crate::generated::UpscaleImageMetadata {
            input: native.frame.clone(), frame: native.frame.clone(), scene: native.scene.clone(),
        };
        derived.frame.source.kind = crate::generated::PresentationSourceKind::Upscale;
        derived.frame.extent = native.frame.extent.checked_scale(
            crate::generated::UpscaleImageMetadata::OUTPUT_SCALE,
        ).unwrap();
        derived.frame.content = native.frame.content.checked_scale(
            crate::generated::UpscaleImageMetadata::OUTPUT_SCALE,
        ).unwrap();
        derived.scene.categories.push(crate::generated::ClassName { value: "人é🙂".into() });
        derived.scene.palette.push(crate::generated::AnnotationColor { hue: 240.0, saturation: 1.0, value: 1.0 });
        derived.scene.objects[0].category = 1;
        derived.scene.objects[0].box_.first.x = 123.0;
        let derived = Arc::new(derived);
        let prepared = super::super::DetailContent::new(native.clone(), Some(derived.clone()));
        assert!(std::ptr::eq(prepared.scene(), &derived.scene));
        assert!(std::ptr::eq(prepared.overlay(), &native.overlay));
        assert!(Arc::ptr_eq(&prepared.clone().labels, &prepared.labels));
        for _ in 0..3 {
            let mut count = 0;
            Source::Detail(prepared.clone(), true).visit(|category, bounds, name, color, categories, overlay, cached| {
                count += 1;
                assert_eq!((category, categories, name), (1, 2, "人é🙂"));
                assert_eq!(bounds.first.x, 123.0);
                assert_eq!(color, &derived.scene.palette[1]);
                assert!(std::ptr::eq(overlay.unwrap(), &native.overlay));

                assert!(std::ptr::eq(cached, &prepared.labels[1]));
                assert_eq!(cached.width, 30.5);
                assert_eq!(cached.paragraph.compare(label_text((), cached.width)), text::Difference::None);
            });
            assert_eq!(count, 1);
        }
        // The same input frame does not give its source catalog authority over
        // the separately paired derived scene.
        assert_eq!(original.input_frame(), prepared.input_frame());
        Source::Detail(original, true).visit(|category, _, name, _, _, _, cached| {
            assert_eq!((category, name), (0, "source"));
            assert_eq!(cached.width, 53.0);
        });
        let mut filtered = (*native).clone();
        filtered.overlay.classselection.mode = crate::generated::ExploreClassSelectionMode::Subset;
        filtered.overlay.classselection.classes = vec![0];
        let hidden = super::super::DetailContent::new(Arc::new(filtered), Some(derived));
        Source::Detail(hidden, true).visit(|_, _, _, _, _, _, _| panic!("paired source filter excludes derived category"));
    }

    #[test]
    fn prediction_labels_cache_exact_text_and_keep_source_products_distinct() {
        let model = crate::view_model::test_support::bootstrapped();
        let mut snapshot = model.predict_snapshot.unwrap();
        snapshot.labels = (0..4096)
            .map(|index| crate::generated::PredictLabel {
                box_: crate::generated::AnnotationBox {
                    first: crate::generated::AnnotationPoint {
                        x: index as f32,
                        y: 2.0,
                    },
                    second: crate::generated::AnnotationPoint {
                        x: index as f32 + 10.0,
                        y: 12.0,
                    },
                },
                classreference: if index == 0 { 0 } else { 79 },
                classdomain: if index == 0 {
                    crate::generated::ClassReferenceDomain::Foreground
                } else {
                    crate::generated::ClassReferenceDomain::RawOutputSlot
                },
                confidence: 0.75,
                color: crate::generated::AnnotationColor {
                    hue: 120.0,
                    saturation: 1.0,
                    value: 0.8,
                },
                name: if index == 0 {
                    "person".into()
                } else {
                    String::new()
                },
            })
            .collect();
        let retained = std::sync::Arc::new(PredictionContent::new(
            crate::generated::PredictImageMetadata::from(&snapshot),
        ));
        assert_eq!(retained.labels.len(), 4096);
        assert_eq!(retained.labels[0].text, "person 0.75");
        assert_eq!(retained.labels[4095].text, "Raw slot 79 0.75");
        let pointer = retained.labels[0].text.as_ptr();
        let source = Source::Prediction(retained.clone());
        for _ in 0..3 {
            source.visit(|_, _, text, _, _, _, cached| {
                assert_eq!(cached.text, text);
                if text == "person 0.75" {
                    assert_eq!(text.as_ptr(), pointer);
                }
            });
        }
        snapshot.contentidentity += 1;
        snapshot.labels[0].name = "changed".into();
        let replacement =
            PredictionContent::new(crate::generated::PredictImageMetadata::from(&snapshot));
        assert_eq!(replacement.labels[0].text, "changed 0.75");
        assert_eq!(retained.labels[0].text, "person 0.75");
        assert_eq!(
            replacement.metadata.frame.source,
            retained.metadata.frame.source
        );
        assert_ne!(
            replacement.metadata.contentidentity,
            retained.metadata.contentidentity
        );
    }

    #[test]
    fn labelled_workspace_retains_the_actual_shader_pan_zoom_and_fps_state() {
        use iced::advanced::renderer::Headless;
        super::super::reset_test_releases();
        let (mut model, frame) = crate::view_model::test_support::explore_presentation();
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.scene.categories = vec![crate::generated::ClassName {
            value: "person".into(),
        }];
        snapshot.scene.palette = vec![crate::generated::AnnotationColor {
            hue: 120.0,
            saturation: 1.0,
            value: 0.8,
        }];
        snapshot.overlay.showlabels = true;
        snapshot.overlay.classselection.mode = crate::generated::ExploreClassSelectionMode::All;
        snapshot.scene.objects = [(100.0, 100.0), (30.0, 45.0)]
            .into_iter()
            .map(|(x, y)| {
                let mut object = crate::view_model::test_support::annotation_object(0);
                object.box_.first = crate::generated::AnnotationPoint { x, y };
                object.box_.second = crate::generated::AnnotationPoint {
                    x: x + 30.0,
                    y: y + 30.0,
                };
                object
            })
            .collect();
        super::super::metadata::install_explore(frame, snapshot);
        assert!(super::super::accept_publication(frame));
        super::super::authorize_draw(Some(frame));
        super::super::complete_sample(frame);
        let surface = crate::view_model::test_support::physical_surface(frame);
        let source = Source::Detail(
            super::super::DetailContent::new(std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(
                    &*snapshot,
                )), None),
            true,
        );
        let program = |show_fps| Program::<()> {
            show_fps,
            input: None,
            local: None,
            publish: None,
            surface,
            placement: super::super::Placement::Contain,
            control_id: crate::view::workspace::STABLE_ID,
        };
        let _renderer_cleanup = super::super::TestRendererCleanup;
        let mut renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
            Default::default(),
            Some("wgpu"),
        ))
        .expect("label-wrapper acceptance requires the container GPU backend");
        let mut element = view(program(true), source.clone());
        let mut tree = widget::Tree::new(&element);
        tree.diff(element.as_widget_mut());
        let bounds = Rectangle::new(Point::new(10.0, 20.0), Size::new(640.0, 480.0));
        let node = element
            .as_widget_mut()
            .layout(
                &mut tree,
                &renderer,
                &layout::Limits::new(bounds.size(), bounds.size()),
            )
            .move_to(bounds.position());
        let now = iced::time::Instant::now();
        let deliver = |element: &mut Element<'_, ()>,
                       tree: &mut widget::Tree,
                       renderer: &iced::Renderer,
                       event: Event,
                       cursor| {
            let mut messages = Vec::new();
            let mut shell = Shell::new(
                &iced::window::Headless,
                iced_runtime::core::shell::Waker::new(|| {}),
                &mut messages,
            );
            element.as_widget_mut().update(
                tree,
                &event,
                Layout::new(&node),
                cursor,
                renderer,
                &mut shell,
                &bounds,
            );
        };
        deliver(
            &mut element,
            &mut tree,
            &renderer,
            Event::Window(iced::window::Event::RedrawRequested(now)),
            mouse::Cursor::Unavailable,
        );
        let cursor = mouse::Cursor::Available(Point::new(200.0, 150.0));
        deliver(
            &mut element,
            &mut tree,
            &renderer,
            Event::Mouse(mouse::Event::WheelScrolled {
                delta: mouse::ScrollDelta::Lines { x: 0.0, y: 2.0 },
            }),
            cursor,
        );
        deliver(
            &mut element,
            &mut tree,
            &renderer,
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Right)),
            cursor,
        );
        deliver(
            &mut element,
            &mut tree,
            &renderer,
            Event::Mouse(mouse::Event::CursorMoved {
                position: Point::new(220.0, 160.0),
            }),
            mouse::Cursor::Available(Point::new(220.0, 160.0)),
        );
        let render =
            |element: &Element<'_, ()>, tree: &widget::Tree, renderer: &mut iced::Renderer| {
                let extent = iced::Size::new(680, 540);
                renderer.reset(Rectangle::with_size(iced::Size::new(680.0, 540.0)));
                element.as_widget().draw(
                    tree,
                    renderer,
                    &crate::fluent_theme::app_theme(false),
                    &renderer::Style::default(),
                    Layout::new(&node),
                    mouse::Cursor::Unavailable,
                    &bounds,
                );
                renderer.screenshot(
                    &iced::widget::shader::Viewport::with_physical_size(extent, 1.0),
                    Color::WHITE,
                )
            };
        let before = render(&element, &tree, &mut renderer);
        let pixel = |image: &[u8], x: usize, y: usize| -> [u8; 3] {
            image[(y * 680 + x) * 4..(y * 680 + x) * 4 + 3]
                .try_into()
                .unwrap()
        };
        let green = |color: [u8; 3]| color[0] < 10 && color[1] > 150 && color[2] < 10;
        // Expected transformed positions come from the physical wheel/pan
        // gestures, and are observed in actual Labelled::draw GPU output.
        assert!(green(pixel(&before, 79, 98)));
        assert!(green(pixel(&before, 11, 30)));
        assert_eq!(pixel(&before, 5, 30), [255; 3]); // The second label is clipped.
        assert_eq!(pixel(&before, 111, 122), [0; 3]); // Its original position moved.
        assert_eq!(pixel(&before, 572, 28), [255; 3]); // Enabled FPS background.
        let glyph_pixels = (29..45)
            .flat_map(|y| (580..632).map(move |x| (x, y)))
            .filter(|&(x, y)| {
                pixel(&before, x, y)
                    .into_iter()
                    .all(|channel| channel < 160)
            })
            .count();
        assert!(
            glyph_pixels >= 12,
            "FPS glyphs must occupy the counter interior"
        );
        let mut replacement = view(program(false), source);
        tree.diff(replacement.as_widget_mut());
        deliver(
            &mut replacement,
            &mut tree,
            &renderer,
            Event::Window(iced::window::Event::RedrawRequested(now)),
            cursor,
        );
        let after = render(&replacement, &tree, &mut renderer);
        assert_eq!(pixel(&after, 79, 98), pixel(&before, 79, 98));
        assert_eq!(pixel(&after, 11, 30), pixel(&before, 11, 30));
        assert_eq!(pixel(&after, 572, 28), [0; 3]);
    }

    #[test]
    fn gallery_labels_share_source_scale_and_clip_with_images_and_hits() {
        let bounds = Rectangle {
            x: 15.0,
            y: -37.25,
            width: 600.0,
            height: 450.0,
        };
        let geometry = placement_geometry(
            bounds,
            (400, 400),
            super::super::Placement::GalleryGrid {
                first_row: 0,
                columns: 4,
                rows: 4,
                row_capacity: 4,
                row_origin: 0,
            },
            super::super::ViewTransform::FIT,
        )
        .unwrap();
        let annotation = crate::generated::AnnotationBox {
            first: crate::generated::AnnotationPoint { x: 100.0, y: 100.0 },
            second: crate::generated::AnnotationPoint { x: 200.0, y: 200.0 },
        };
        let label = label_bounds(geometry, [0, 0, 400, 400], &annotation, 53.0);
        assert_eq!(label.x, 165.0);
        assert_eq!(label.y, 112.75);
        assert_eq!(
            super::super::inverse_content_point(geometry, Point::new(label.x, label.y), (400, 400)),
            Some((100.0, 100.0))
        );
        let clip = Rectangle {
            x: 15.0,
            y: 0.0,
            width: 600.0,
            height: 450.0,
        };
        assert!(label.intersection(&clip).is_some());
        let mut clipped = annotation;
        clipped.first.y = 0.0;
        assert!(
            label_bounds(geometry, [0, 0, 400, 400], &clipped, 53.0)
                .intersection(&clip)
                .is_none()
        );
        clipped.first.y = 330.0;
        assert!(
            label_bounds(geometry, [0, 0, 400, 400], &clipped, 53.0)
                .intersection(&clip)
                .is_none()
        );
    }

    #[test]
    fn detail_uses_its_own_catalog_geometry_and_visibility_without_gallery_labels() {
        let mut snapshot = crate::view_model::test_support::explore_snapshot();
        snapshot.scene.categories = vec![crate::generated::ClassName {
            value: "人".into()
        }];
        snapshot.scene.palette = vec![crate::generated::AnnotationColor {
            hue: 120.0,
            saturation: 0.7,
            value: 0.8,
        }];
        snapshot.scene.objects = vec![crate::view_model::test_support::annotation_object(0)];
        snapshot.overlay.showboxes = false;
        snapshot.overlay.showlabels = true;
        assert!(snapshot.labels.is_empty());
        let collect = |source: Source| {
            let mut labels = Vec::new();
            source.visit(|category, bounds, name, color, count, _, _| {
                labels.push((
                    category,
                    bounds.clone(),
                    name.to_owned(),
                    color.clone(),
                    count,
                ))
            });
            labels
        };
        let detail = |scene: &crate::generated::AnnotationSceneContent,
                      overlay: &crate::generated::ExploreOverlay| {
            let mut snapshot = crate::view_model::test_support::explore_snapshot();
            snapshot.scene = scene.clone();
            snapshot.overlay = overlay.clone();
            Source::Detail(
                super::super::DetailContent::new(std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(
                        &snapshot,
                    )), None),
                overlay.showlabels,
            )
        };
        assert!(
            collect(Source::Gallery(
                std::sync::Arc::new(GalleryContent::new(std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(&snapshot)))),
                snapshot.overlay.showlabels
            ))
            .is_empty()
        );
        let labels = collect(detail(&snapshot.scene, &snapshot.overlay));
        assert_eq!(labels.len(), 1);
        assert_eq!(labels[0].2, "人");
        assert_eq!(labels[0].3, snapshot.scene.palette[0]);
        let mut upscale_scene = snapshot.scene.clone();
        upscale_scene.objects[0].box_.first.x = 123.0;
        assert_eq!(
            collect(detail(&upscale_scene, &snapshot.overlay))[0]
                .1
                .first
                .x,
            123.0
        );
        snapshot.overlay.classselection.mode = crate::generated::ExploreClassSelectionMode::None;
        assert!(collect(detail(&snapshot.scene, &snapshot.overlay)).is_empty());
        snapshot.overlay.classselection.mode = crate::generated::ExploreClassSelectionMode::Subset;
        snapshot.overlay.classselection.classes = vec![0];
        assert_eq!(collect(detail(&snapshot.scene, &snapshot.overlay)).len(), 1);
        snapshot.scene.objects[0].enabled = false;
        assert!(collect(detail(&snapshot.scene, &snapshot.overlay)).is_empty());
    }
}
