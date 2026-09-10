use super::{PlacementGeometry, Program, Surface, ViewportOwner, placement_geometry};
use crate::fluent_theme::{Element, Theme};
use iced::advanced::Renderer as _;
use iced::advanced::text::Renderer as _;
use iced::advanced::{Layout, Shell, Widget, layout, mouse, renderer, text, widget};
use iced::{Color, Event, Fill, Point, Rectangle, Size};

fn label_bounds(
    geometry: PlacementGeometry,
    region: [u32; 4],
    bounds: &crate::generated::AnnotationBox,
    name: &str,
) -> Rectangle {
    Rectangle {
        x: geometry.x + (bounds.first.x - region[0] as f32) * geometry.width / region[2] as f32,
        y: geometry.y + (bounds.first.y - region[1] as f32) * geometry.height / region[3] as f32,
        width: (name.chars().count() as f32 * 7.5 + 8.0).max(20.0),
        height: 19.0,
    }
}

#[derive(Clone)]
pub(crate) enum Source {
    Gallery(std::sync::Arc<crate::generated::ExploreSnapshot>),
    Detail(super::DetailContent),
    Hidden,
}

impl Source {
    fn visit(
        &self,
        mut label: impl FnMut(
            u16,
            &crate::generated::AnnotationBox,
            &str,
            &crate::generated::AnnotationColor,
            usize,
            &crate::generated::ExploreOverlay,
        ),
    ) {
        match self {
            Self::Gallery(snapshot) if snapshot.overlay.showlabels => {
                for item in &snapshot.labels {
                    if let (Some(name), Some(color)) = (
                        snapshot.dataset.classnames.get(item.category as usize),
                        snapshot.dataset.palette.get(item.category as usize),
                    ) {
                        label(
                            item.category,
                            &item.box_,
                            &name.value,
                            color,
                            snapshot.dataset.classnames.len(),
                            &snapshot.overlay,
                        );
                    }
                }
            }
            Self::Detail(content) if content.overlay().showlabels => {
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
                        scene.categories.get(object.category as usize),
                        scene.palette.get(object.category as usize),
                    ) {
                        label(
                            object.category,
                            &object.box_,
                            &name.value,
                            color,
                            scene.categories.len(),
                            overlay,
                        );
                    }
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
    let surface = match &source {
        Source::Gallery(_) => {
            super::gallery::displayed().map_or(program.surface, |(surface, _)| surface)
        }
        Source::Detail(_) => super::drawable_detail(program.surface).map_or(program.surface, |(retained, _)| {
            if program.surface.frame == retained.frame
                && super::same_allocation(program.surface, retained)
            {
                program.surface
            } else {
                retained
            }
        }),
        Source::Hidden => program.surface,
    };
    let placement = program.placement;
    let transform_surface = program.surface;
    Element::new(Labelled {
        child: iced::widget::shader(program)
            .width(Fill)
            .height(Fill)
            .into(),
        surface,
        transform_surface,
        placement,
        source,
    })
}

struct Labelled<'a, Message> {
    child: Element<'a, Message>,
    surface: Surface,
    transform_surface: Surface,
    placement: super::Placement,
    source: Source,
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
        let state = tree.children[0].state.downcast_ref::<ViewportOwner>();
        let mut bounds = layout.bounds();
        let mut placement = self.placement;
        if let Source::Gallery(snapshot) = &self.source {
            bounds.y += super::gallery::row_offset(placement, snapshot, bounds.width);
            placement = super::gallery::placement(snapshot);
        }
        let Some(geometry) = placement_geometry(
            bounds,
            self.surface.content_extent(),
            placement,
            state.transform_for(self.transform_surface),
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
        super::gallery::observe(None, crate::fluent_theme::conformance(theme).dark);
        let region = self.surface.content_region();
        renderer.with_layer(clip, |renderer| {
            self.source
                .visit(|category, bounds, name, color, catalog_count, overlay| {
                    let rect = label_bounds(geometry, region, bounds, name);
                    if rect.intersection(&clip).is_none() {
                        return;
                    }
                    let color = class_color(color);
                    renderer.fill_quad(
                        renderer::Quad {
                            bounds: rect,
                            ..Default::default()
                        },
                        color,
                    );
                    renderer.fill_text(
                        text::Text {
                            content: name.to_owned(),
                            bounds: Size::new(rect.width - 6.0, 19.0),
                            size: iced::Pixels(12.0),
                            line_height: text::LineHeight::default(),
                            font: iced::Font::DEFAULT,
                            align_x: text::Alignment::Left,
                            align_y: iced::alignment::Vertical::Center,
                            shaping: text::Shaping::Advanced,
                            wrapping: text::Wrapping::None,
                            ellipsis: text::Ellipsis::default(),
                            hint_factor: None,
                        },
                        Point::new(rect.x + 3.0, rect.y + 9.5),
                        if color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722 > 0.55 {
                            Color::BLACK
                        } else {
                            Color::WHITE
                        },
                        clip,
                    );
                    if self.surface.integration
                        && let Some(frame) = self.surface.frame
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
                });
        });
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
            },
            super::super::ViewTransform::FIT,
        )
        .unwrap();
        let annotation = crate::generated::AnnotationBox {
            first: crate::generated::AnnotationPoint { x: 100.0, y: 100.0 },
            second: crate::generated::AnnotationPoint { x: 200.0, y: 200.0 },
        };
        let label = label_bounds(geometry, [0, 0, 400, 400], &annotation, "person");
        assert_eq!(label.x, 165.0);
        assert_eq!(label.y, 112.75);
        assert_eq!(
            super::super::inverse_content_point(geometry, Point::new(label.x, label.y), (400, 400)),
            Some((100, 100))
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
            label_bounds(geometry, [0, 0, 400, 400], &clipped, "person")
                .intersection(&clip)
                .is_none()
        );
        clipped.first.y = 330.0;
        assert!(
            label_bounds(geometry, [0, 0, 400, 400], &clipped, "person")
                .intersection(&clip)
                .is_none()
        );
    }

    #[test]
    fn detail_uses_its_own_catalog_geometry_and_visibility_without_gallery_labels() {
        let mut snapshot = crate::view_model::test_support::explore_snapshot();
        snapshot.scene.categories = vec![crate::generated::ArtifactClassName {
            value: "人".into(),
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
            source.visit(|category, bounds, name, color, count, _| {
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
            Source::Detail(super::super::DetailContent {
                explore: std::sync::Arc::new(snapshot),
                upscale: None,
            })
        };
        assert!(collect(Source::Gallery(std::sync::Arc::new(snapshot.clone()))).is_empty());
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
