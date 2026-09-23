use crate::fluent_theme::Element;
use crate::generated::{ValidationOverlays, ValidationSampleIdentity};
use crate::presentation_surface::{self, Surface};
use crate::view::workflow::overlay_controls;
use iced::widget::{button, checkbox, column, container, row, text};
use iced::{Center, Fill};

pub const ATLAS_ID: &str = "validate.samples.atlas";
#[derive(Debug, Clone)]
pub enum Message {
    Atlas(AtlasInput),
    Select(ValidationSampleIdentity),
    Close,
    Fit,
    Original(bool),
    Labels(bool, bool),
    Overlays(ValidationOverlays),
    Upscale(crate::generated::UpscaleKernel),
    OpenAnnotation,
}
#[derive(Debug, Clone, PartialEq)]
struct AtlasSource {
    binding: (u64, u64),
    frame: crate::generated::VisualFrame,
    content: u64,
}
#[derive(Debug, Clone)]
pub struct AtlasInput {
    source: AtlasSource,
    kind: presentation_surface::SurfaceGestureKind,
    selected: Option<ValidationSampleIdentity>,
    pressed: bool,
}
#[derive(Default)]
struct AtlasInteraction {
    source: Option<AtlasSource>,
    pressed: Option<ValidationSampleIdentity>,
}
impl AtlasInteraction {
    fn synchronize(&mut self, source: Option<AtlasSource>) {
        // Publication revisions carry exact hit metadata, but do not end a hold
        // on the same retained atlas.
        let same_source = self
            .source
            .as_ref()
            .zip(source.as_ref())
            .is_some_and(|(old, new)| {
                old.binding == new.binding
                    && old.frame.source == new.frame.source
                    && old.content == new.content
            });
        if !same_source {
            self.pressed = None;
        }
        self.source = source;
    }
    fn input(&mut self, input: AtlasInput) -> Option<ValidationSampleIdentity> {
        if self.source.as_ref() != Some(&input.source) {
            return None;
        }
        match input.kind {
            presentation_surface::SurfaceGestureKind::Pointer if input.pressed => {
                let selected = input.selected?;
                if self.pressed.as_ref() == Some(&selected) {
                    return None;
                }
                self.pressed = Some(selected.clone());
                Some(selected)
            }
            presentation_surface::SurfaceGestureKind::Pointer
            | presentation_surface::SurfaceGestureKind::End
            | presentation_surface::SurfaceGestureKind::Cancel => {
                self.pressed = None;
                None
            }
            presentation_surface::SurfaceGestureKind::Viewport => None,
        }
    }
}
pub struct Component {
    ground_truth: bool,
    prediction: bool,
    fit_revision: u64,
    original: bool,
    atlas: std::cell::RefCell<AtlasInteraction>,
}
impl Default for Component {
    fn default() -> Self {
        Self {
            ground_truth: true,
            prediction: true,
            fit_revision: 0,
            original: true,
            atlas: Default::default(),
        }
    }
}
impl Component {
    pub fn update(&mut self, message: Message) -> Option<Message> {
        match message {
            Message::Atlas(input) => self.atlas.get_mut().input(input).map(Message::Select),
            Message::Labels(ground_truth, value) => {
                if ground_truth {
                    self.ground_truth = value;
                } else {
                    self.prediction = value;
                }
                None
            }
            Message::Original(value) => {
                self.original = value;
                None
            }
            Message::Fit => {
                self.fit_revision = self.fit_revision.wrapping_add(1);
                None
            }
            message => Some(message),
        }
    }
    pub fn controls<'a>(
        &self,
        model: &crate::view_model::ApplicationModel,
    ) -> Element<'a, Message> {
        crate::view::image_viewer::control_groups(self.control_groups(model))
    }
    fn control_groups<'a>(
        &self,
        model: &crate::view_model::ApplicationModel,
    ) -> Vec<Element<'a, Message>> {
        let Some(snapshot) = model.workflow.validation.as_ref() else {
            return vec![text("Waiting for validation").into()];
        };
        let overlays = snapshot.overlayselection.value.clone();
        let available = model.validation_navigation_available()
            && (snapshot.frame.revision == 0
                || snapshot.overlayselection.value == snapshot.overlays);
        let group = |ground_truth: bool| {
            let (boxes, masks, labels, layer, ids, name) = if ground_truth {
                (
                    overlays.groundtruthboxes,
                    overlays.groundtruthmasks,
                    self.ground_truth,
                    overlays.groundtruthlayer,
                    [
                        "validate.gt.labels",
                        "validate.gt.masks",
                        "validate.gt.boxes",
                    ],
                    "Groundtruth",
                )
            } else {
                (
                    overlays.predictionboxes,
                    overlays.predictionmasks,
                    self.prediction,
                    overlays.predictionlayer,
                    [
                        "validate.pred.labels",
                        "validate.pred.masks",
                        "validate.pred.boxes",
                    ],
                    "Detections",
                )
            };
            let full = overlays.clone();
            let fine = overlays.clone();
            column![
                container(
                    checkbox(layer)
                        .label(name)
                        .on_toggle_maybe(available.then_some(move |value| {
                            let mut next = full.clone();
                            if ground_truth {
                                next.groundtruthlayer = value;
                            } else {
                                next.predictionlayer = value;
                            }
                            Message::Overlays(next)
                        }))
                )
                .id(if ground_truth {
                    "validate.gt.layer"
                } else {
                    "validate.pred.layer"
                }),
                overlay_controls::view(labels, masks, boxes, true, available, ids).map(
                    move |message| {
                        let mut next = fine.clone();
                        match message {
                            overlay_controls::Message::Labels(value) => {
                                return Message::Labels(ground_truth, value);
                            }
                            overlay_controls::Message::Masks(value) => {
                                if ground_truth {
                                    next.groundtruthmasks = value;
                                } else {
                                    next.predictionmasks = value;
                                }
                            }
                            overlay_controls::Message::Boxes(value) => {
                                if ground_truth {
                                    next.groundtruthboxes = value;
                                } else {
                                    next.predictionboxes = value;
                                }
                            }
                        }
                        Message::Overlays(next)
                    }
                ),
            ]
            .spacing(3)
            .into()
        };
        let gt: Element<'a, Message> = group(true);
        let pred: Element<'a, Message> = group(false);
        vec![
            container(gt).id("validate.gt.group").into(),
            container(pred).id("validate.pred.group").into(),
        ]
    }
    pub fn atlas<'a>(
        &self,
        paired: Option<(
            Surface,
            std::sync::Arc<presentation_surface::labels::ValidationContent>,
        )>,
        settings: &crate::view::settings::SettingsModel,
        input: crate::workspace_input::Binding,
    ) -> Element<'a, Message> {
        let (surface, labels, local) = match paired.filter(|(_, content)| !content.metadata.detail)
        {
            Some((surface, content)) => {
                let source = AtlasSource {
                    binding: (surface.high, surface.low),
                    frame: content.metadata.frame.clone(),
                    content: content.metadata.contentidentity,
                };
                self.atlas.borrow_mut().synchronize(Some(source.clone()));
                let hit_metadata = content.metadata.clone();
                let local: std::sync::Arc<
                    dyn Fn(presentation_surface::SurfaceGesture) -> Option<Message> + Send + Sync,
                > = std::sync::Arc::new(move |gesture| {
                    if gesture.kind == presentation_surface::SurfaceGestureKind::Viewport {
                        return None;
                    }
                    Some(Message::Atlas(AtlasInput {
                        source: source.clone(),
                        kind: gesture.kind,
                        selected: hit(
                            &hit_metadata,
                            gesture.sample.content_x,
                            gesture.sample.content_y,
                        ),
                        pressed: gesture.sample.pressed,
                    }))
                });
                (
                    surface,
                    presentation_surface::labels::Source::Validation(
                        content,
                        self.ground_truth,
                        self.prediction,
                    ),
                    Some(local),
                )
            }
            None => {
                self.atlas.borrow_mut().synchronize(None);
                (
                    Surface::empty(),
                    presentation_surface::labels::Source::Hidden,
                    None,
                )
            }
        };
        container(presentation_surface::labels::view(
            presentation_surface::Program {
                surface,
                show_fps: crate::workspace_fps::enabled(settings),
                input: Some(input.for_source(
                    crate::generated::PresentationSourceKind::Validation,
                    0,
                    None,
                )),
                local,
                publish: None,
                placement: presentation_surface::Placement::FixedGrid {
                    columns: 2,
                    rows: 3,
                },
                control_id: ATLAS_ID,
            },
            labels,
        ))
        .id(ATLAS_ID)
        .width(Fill)
        .height(Fill)
        .style(crate::fluent_theme::container_workspace)
        .into()
    }
    pub fn detail<'a>(
        &self,
        surface: Surface,
        content: std::sync::Arc<presentation_surface::labels::ValidationContent>,
        model: &crate::view_model::ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        input: crate::workspace_input::Binding,
    ) -> Element<'a, Message> {
        let selected = content
            .metadata
            .selected
            .as_ref()
            .expect("paired validation detail identity");
        let (previous, next) = neighbors(&content.metadata);
        let available = model.validation_navigation_available();
        let mut shown = surface;
        shown.fit_revision = self.fit_revision;
        shown.configure_original(content.frame(), &content.metadata.frame, self.original);
        let image = crate::view::image_viewer::image(
            shown,
            presentation_surface::labels::Source::Validation(
                content.clone(),
                self.ground_truth,
                self.prediction,
            ),
            Some(input.for_source(content.frame().source.kind, 0, None)),
            crate::workspace_fps::enabled(settings),
            "validate.detail.image",
        );
        let fit = row![
            container(button("Fit").on_press(Message::Fit)).id("validate.detail.fit"),
            container(
                checkbox(self.original)
                    .label("Original")
                    .on_toggle(Message::Original)
            )
            .id("validate.detail.original"),
        ]
        .spacing(7)
        .align_y(Center);
        let source = crate::view::image_viewer::control_groups(
            std::iter::once(fit.into()).chain(self.control_groups(model)),
        );
        crate::view::image_viewer::panel(
            selected.datasetindex,
            image,
            source.into(),
            crate::view::image_viewer::PanelIds {
                image: "validate.detail.image",
                previous: "validate.detail.previous",
                next: "validate.detail.next",
                close: "validate.detail.close",
                annotate: "validate.detail.annotate",
                upscale: [
                    "validate.detail.upscale.basic",
                    "validate.detail.upscale.fast",
                    "validate.detail.upscale.neural",
                ],
            },
            previous.filter(|_| available).map(Message::Select),
            next.filter(|_| available).map(Message::Select),
            available.then_some(Message::Close),
            (model.annotation_import_available() && !settings.has_local_edits())
                .then_some(Message::OpenAnnotation),
            model.displayed_upscale_kernel(),
            model
                .requested_upscale
                .as_ref()
                .filter(|value| Some(value.kernel) != model.displayed_upscale_kernel())
                .map(|value| value.kernel),
            model.upscale_start_available(),
            Message::Upscale,
        )
    }
}
pub fn hit(
    metadata: &crate::generated::ValidationImageMetadata,
    x: f32,
    y: f32,
) -> Option<ValidationSampleIdentity> {
    let extent = &metadata.frame.extent;
    if metadata.detail
        || !x.is_finite()
        || !y.is_finite()
        || x < 0.0
        || y < 0.0
        || x >= extent.width as f32
        || y >= extent.height as f32
    {
        return None;
    }
    let column = (x * 2.0 / extent.width as f32) as usize;
    let row = (y * 3.0 / extent.height as f32) as usize;
    metadata
        .samples
        .get(row * 2 + column)
        .filter(|sample| sample.available)
        .map(|sample| sample.identity.clone())
}
fn neighbors(
    metadata: &crate::generated::ValidationImageMetadata,
) -> (
    Option<ValidationSampleIdentity>,
    Option<ValidationSampleIdentity>,
) {
    let Some(index) = metadata
        .samples
        .iter()
        .position(|sample| Some(&sample.identity) == metadata.selected.as_ref())
    else {
        return (None, None);
    };
    let previous = metadata.samples[..index]
        .iter()
        .rev()
        .find(|sample| sample.available);
    let next = metadata.samples[index + 1..]
        .iter()
        .find(|sample| sample.available);
    (
        previous.map(|sample| sample.identity.clone()),
        next.map(|sample| sample.identity.clone()),
    )
}
#[cfg(test)]
mod tests {
    use super::*;
    use presentation_surface::SurfaceGestureKind;

    #[test]
    fn rendered_groups_wrap_with_twenty_pixel_gaps_and_hidden_controls_keep_modal_bounds() {
        use iced::advanced::{Layout, layout, renderer::Headless, widget};
        let renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
            Default::default(),
            Some("wgpu"),
        ))
        .unwrap();
        let component = Component::default();
        let model = crate::view_model::test_support::bootstrapped();
        struct Bounds(std::collections::BTreeMap<String, iced::Rectangle>);
        impl widget::Operation for Bounds {
            fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn widget::Operation)) {
                operate(self);
            }
            fn container(&mut self, id: Option<&widget::Id>, bounds: iced::Rectangle) {
                for name in ["validate.gt.group", "validate.pred.group"] {
                    if id == Some(&widget::Id::from(name)) {
                        self.0.insert(name.to_owned(), bounds);
                    }
                }
            }
        }
        for width in [600.0, 300.0] {
            let mut controls = component.controls(&model);
            let mut tree = widget::Tree::new(&controls);
            tree.diff(&mut controls);
            let limits = layout::Limits::new(iced::Size::ZERO, iced::Size::new(width, 1000.0));
            let node = controls
                .as_widget_mut()
                .layout(&mut tree, &renderer, &limits);
            let mut bounds = Bounds(Default::default());
            controls
                .as_widget_mut()
                .operate(&mut tree, Layout::new(&node), &renderer, &mut bounds);
            let gt = bounds.0["validate.gt.group"];
            let det = bounds.0["validate.pred.group"];
            if width > 500.0 {
                assert_eq!(det.x - gt.x - gt.width, 20.0);
                assert_eq!(det.y, gt.y);
            } else {
                assert_eq!(det.y - gt.y - gt.height, 20.0);
                assert_eq!(det.x, gt.x);
            }
            let mut footprint: Element<'_, Message> =
                crate::view::image_viewer::control_footprint(component.controls(&model));
            let mut tree = widget::Tree::new(&footprint);
            tree.diff(&mut footprint);
            let hidden = footprint
                .as_widget_mut()
                .layout(&mut tree, &renderer, &limits);
            assert_eq!(hidden.size(), node.size());
            let mut hidden_ids = Bounds(Default::default());
            footprint.as_widget_mut().operate(
                &mut tree,
                Layout::new(&hidden),
                &renderer,
                &mut hidden_ids,
            );
            assert!(hidden_ids.0.is_empty());
        }
    }

    fn source(metadata: &crate::generated::ValidationImageMetadata) -> AtlasSource {
        AtlasSource {
            binding: (1, 2),
            frame: metadata.frame.clone(),
            content: metadata.contentidentity,
        }
    }
    fn pointer(metadata: &crate::generated::ValidationImageMetadata, x: f32, y: f32) -> AtlasInput {
        AtlasInput {
            source: source(metadata),
            kind: SurfaceGestureKind::Pointer,
            selected: hit(metadata, x, y),
            pressed: true,
        }
    }

    #[test]
    fn held_target_selects_once_and_keeps_exact_paired_identity() {
        let metadata = crate::view_model::test_support::validation_image_metadata();
        let mut component = Component::default();
        component
            .atlas
            .get_mut()
            .synchronize(Some(source(&metadata)));
        assert!(
            matches!(component.update(Message::Atlas(pointer(&metadata, 10.0, 10.0))),
            Some(Message::Select(identity)) if identity == metadata.samples[0].identity)
        );
        for x in 11..200 {
            assert!(
                component
                    .update(Message::Atlas(pointer(&metadata, x as f32, 10.0)))
                    .is_none()
            );
        }
        assert!(
            matches!(component.update(Message::Atlas(pointer(&metadata, 300.0, 10.0))),
            Some(Message::Select(identity)) if identity == metadata.samples[1].identity)
        );
        assert!(
            component
                .update(Message::Atlas(pointer(&metadata, 310.0, 10.0)))
                .is_none()
        );
        for (x, y) in [
            (0.0, 200.0),
            (-1.0, 0.0),
            (512.0, 0.0),
            (0.0, 576.0),
            (f32::NAN, 0.0),
            (0.0, f32::INFINITY),
        ] {
            assert!(
                component
                    .update(Message::Atlas(pointer(&metadata, x, y)))
                    .is_none()
            );
        }
        assert!(
            component
                .update(Message::Atlas(pointer(&metadata, 310.0, 10.0)))
                .is_none()
        );
    }

    #[test]
    fn release_end_and_cancel_allow_a_later_press() {
        let metadata = crate::view_model::test_support::validation_image_metadata();
        for kind in [
            SurfaceGestureKind::Pointer,
            SurfaceGestureKind::End,
            SurfaceGestureKind::Cancel,
        ] {
            let mut state = AtlasInteraction::default();
            state.synchronize(Some(source(&metadata)));
            let input = pointer(&metadata, 10.0, 10.0);
            assert!(state.input(input.clone()).is_some());
            assert!(
                state
                    .input(AtlasInput {
                        kind,
                        pressed: false,
                        ..input.clone()
                    })
                    .is_none()
            );
            assert!(state.input(input).is_some());
        }
    }

    #[test]
    fn publications_preserve_the_hold_but_replaced_sources_reject_stale_callbacks() {
        let mut metadata = crate::view_model::test_support::validation_image_metadata();
        let mut state = AtlasInteraction::default();
        state.synchronize(Some(source(&metadata)));
        let old = pointer(&metadata, 10.0, 10.0);
        assert!(state.input(old.clone()).is_some());
        metadata.frame.revision += 1;
        state.synchronize(Some(source(&metadata)));
        assert!(
            state
                .input(AtlasInput {
                    kind: SurfaceGestureKind::Cancel,
                    ..old.clone()
                })
                .is_none()
        );
        assert!(state.input(pointer(&metadata, 10.0, 10.0)).is_none());
        assert!(
            state
                .input(AtlasInput {
                    selected: Some(metadata.samples[1].identity.clone()),
                    ..old.clone()
                })
                .is_none()
        );
        for sample in &mut metadata.samples {
            sample.identity.generation += 1;
        }
        metadata.contentidentity += 1;
        state.synchronize(Some(source(&metadata)));
        assert!(state.input(old).is_none());
        let current = pointer(&metadata, 10.0, 10.0);
        assert!(state.input(current.clone()).is_some());
        state.synchronize(None);
        assert!(state.input(current.clone()).is_none());
        state.synchronize(Some(source(&metadata)));
        assert!(state.input(current.clone()).is_some());
        let mut replacement = source(&metadata);
        replacement.binding.0 += 1;
        state.synchronize(Some(replacement.clone()));
        assert!(state.input(current.clone()).is_none());
        assert!(
            state
                .input(AtlasInput {
                    source: replacement,
                    ..current
                })
                .is_some()
        );
    }

    #[test]
    fn grid_hits_and_navigation_use_only_paired_retained_identities() {
        let mut metadata = crate::view_model::test_support::validation_image_metadata();
        metadata.frame.extent.width = 512;
        metadata.frame.extent.height = 576;
        assert_eq!(
            hit(&metadata, 255.0, 191.0),
            Some(metadata.samples[0].identity.clone())
        );
        assert_eq!(
            hit(&metadata, 256.0, 0.0),
            Some(metadata.samples[1].identity.clone())
        );
        assert!(hit(&metadata, 0.0, 192.0).is_none());
        assert!(hit(&metadata, 512.0, 0.0).is_none());
        metadata.selected = Some(metadata.samples[0].identity.clone());
        assert_eq!(
            neighbors(&metadata),
            (None, Some(metadata.samples[1].identity.clone()))
        );
        metadata.selected = Some(metadata.samples[1].identity.clone());
        assert_eq!(
            neighbors(&metadata),
            (Some(metadata.samples[0].identity.clone()), None)
        );
    }
}
