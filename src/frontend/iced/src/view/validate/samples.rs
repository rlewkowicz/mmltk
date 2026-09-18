use crate::fluent_theme::Element;
use crate::generated::{ValidationOverlays, ValidationSampleIdentity};
use crate::presentation_surface::{self, Surface};
use crate::view::workflow::overlay_controls;
use iced::widget::{button, checkbox, column, container, row, text};
use iced::{Fill, Center};

pub const ATLAS_ID: &str = "validate.samples.atlas";
#[derive(Debug, Clone)]
pub enum Message {
    Select(ValidationSampleIdentity),
    Close,
    Fit,
    Labels(bool, bool),
    Overlays(ValidationOverlays),
    Upscale(crate::generated::UpscaleKernel),
    OpenAnnotation,
}
pub struct Component {
    ground_truth: bool,
    prediction: bool,
    fit_revision: u64,
}
impl Default for Component {
    fn default() -> Self {
        Self { ground_truth: true, prediction: true, fit_revision: 0 }
    }
}
impl Component {
    pub fn update(&mut self, message: &Message) -> bool {
        match message {
            Message::Labels(ground_truth, value) => {
                if *ground_truth { self.ground_truth = *value; } else { self.prediction = *value; }
                true
            }
            Message::Fit => { self.fit_revision = self.fit_revision.wrapping_add(1); true }
            _ => false,
        }
    }
    pub fn controls<'a>(&self, model: &crate::view_model::ApplicationModel) -> Element<'a, Message> {
        let Some(snapshot) = model.workflow.validation.as_ref()
            else { return text("Waiting for validation").into(); };
        let overlays = snapshot.overlayselection.value.clone();
        let available = model.settings_edit_available() && (snapshot.frame.revision == 0 || snapshot.overlayselection.value == snapshot.overlays)
            && !model.has_pending(crate::generated::ApplicationIntentEndpoint::ValidationSetOverlays);
        let group = |ground_truth: bool| {
            let (boxes, masks, labels, layer, ids, name) = if ground_truth {
                (overlays.groundtruthboxes, overlays.groundtruthmasks, self.ground_truth, overlays.groundtruthlayer,
                 ["validate.gt.labels", "validate.gt.masks", "validate.gt.boxes"], "GT labels")
            } else {
                (overlays.predictionboxes, overlays.predictionmasks, self.prediction, overlays.predictionlayer,
                 ["validate.pred.labels", "validate.pred.masks", "validate.pred.boxes"], "Det labels")
            };
            let full = overlays.clone();
            let fine = overlays.clone();
            column![
                container(checkbox(layer).label(name).on_toggle_maybe(available.then_some(move |value| {
                    let mut next = full.clone();
                    if ground_truth { next.groundtruthlayer = value; } else { next.predictionlayer = value; }
                    Message::Overlays(next)
                }))).id(if ground_truth { "validate.gt.layer" } else { "validate.pred.layer" }),
                overlay_controls::view(labels, masks, boxes, true, available, ids).map(move |message| {
                    let mut next = fine.clone();
                    match message {
                        overlay_controls::Message::Labels(value) => return Message::Labels(ground_truth, value),
                        overlay_controls::Message::Masks(value) => if ground_truth { next.groundtruthmasks = value; } else { next.predictionmasks = value; },
                        overlay_controls::Message::Boxes(value) => if ground_truth { next.groundtruthboxes = value; } else { next.predictionboxes = value; },
                    }
                    Message::Overlays(next)
                }),
            ].spacing(3).into()
        };
        let gt: Element<'a, Message> = group(true);
        let pred: Element<'a, Message> = group(false);
        row![gt, pred].spacing(12).align_y(Center).into()
    }
    pub fn atlas<'a>(&self, paired: Option<(Surface, std::sync::Arc<presentation_surface::labels::ValidationContent>)>,
        settings: &crate::view::settings::SettingsModel, input: crate::workspace_input::Binding) -> Element<'a, Message> {
        let (surface, labels, local) = match paired.filter(|(_, content)| !content.metadata.detail) {
            Some((surface, content)) => {
                let hit_metadata = content.metadata.clone();
                let local: std::sync::Arc<dyn Fn(presentation_surface::SurfaceGesture) -> Option<Message> + Send + Sync> = std::sync::Arc::new(move |gesture| {
                    if gesture.kind != presentation_surface::SurfaceGestureKind::Pointer || !gesture.sample.pressed { return None; }
                    hit(&hit_metadata, gesture.sample.content_x, gesture.sample.content_y).map(Message::Select)
                });
                (surface, presentation_surface::labels::Source::Validation(content, self.ground_truth, self.prediction), Some(local))
            }
            None => (Surface::empty(), presentation_surface::labels::Source::Hidden, None),
        };
        container(presentation_surface::labels::view(presentation_surface::Program {
            surface, show_fps: crate::workspace_fps::enabled(settings),
            input: Some(input.for_source(crate::generated::PresentationSourceKind::Validation, 0, None)),
            local, publish: None, placement: presentation_surface::Placement::FixedGrid { columns: 2, rows: 3 }, control_id: ATLAS_ID,
        }, labels)).id(ATLAS_ID).width(Fill).height(Fill).style(crate::fluent_theme::container_workspace).into()
    }
    pub fn detail<'a>(&self, surface: Surface, content: std::sync::Arc<presentation_surface::labels::ValidationContent>,
        model: &crate::view_model::ApplicationModel, settings: &crate::view::settings::SettingsModel,
        input: crate::workspace_input::Binding) -> Element<'a, Message> {
        let selected = content.metadata.selected.as_ref().expect("paired validation detail identity");
        let (previous, next) = neighbors(&content.metadata);
        let available = model.settings_edit_available()
            && !model.has_pending(crate::generated::ApplicationIntentEndpoint::ValidationSelectSample)
            && !model.has_pending(crate::generated::ApplicationIntentEndpoint::ValidationCloseDetail);
        let mut shown = surface;
        shown.fit_revision = self.fit_revision;
        let image = crate::view::image_viewer::image(shown,
            presentation_surface::labels::Source::Validation(content.clone(), self.ground_truth, self.prediction),
            Some(input.for_source(content.frame().source.kind, 0, None)), crate::workspace_fps::enabled(settings), "validate.detail.image");
        let source = row![container(button("Fit").on_press(Message::Fit)).id("validate.detail.fit"), self.controls(model)].spacing(7).align_y(Center);
        crate::view::image_viewer::panel(selected.datasetindex, image, source.into(), crate::view::image_viewer::PanelIds {
            image: "validate.detail.image", previous: "validate.detail.previous", next: "validate.detail.next", close: "validate.detail.close",
            annotate: "validate.detail.annotate", upscale: ["validate.detail.upscale.basic", "validate.detail.upscale.fast", "validate.detail.upscale.neural"],
        }, previous.filter(|_| available).map(Message::Select), next.filter(|_| available).map(Message::Select),
        available.then_some(Message::Close), (model.annotation_import_available() && !settings.has_local_edits()).then_some(Message::OpenAnnotation),
        model.displayed_upscale_kernel(), model.requested_upscale.as_ref().filter(|value| Some(value.kernel) != model.displayed_upscale_kernel()).map(|value| value.kernel),
        model.upscale_start_available(), Message::Upscale)
    }
}
pub fn hit(metadata: &crate::generated::ValidationImageMetadata, x: f32, y: f32) -> Option<ValidationSampleIdentity> {
    let extent = &metadata.frame.extent;
    if metadata.detail || !x.is_finite() || !y.is_finite() || x < 0.0 || y < 0.0 || x >= extent.width as f32 || y >= extent.height as f32 { return None; }
    let column = (x * 2.0 / extent.width as f32) as usize;
    let row = (y * 3.0 / extent.height as f32) as usize;
    metadata.samples.get(row * 2 + column).filter(|sample| sample.available).map(|sample| sample.identity.clone())
}
fn neighbors(metadata: &crate::generated::ValidationImageMetadata) -> (Option<ValidationSampleIdentity>, Option<ValidationSampleIdentity>) {
    let Some(index) = metadata.samples.iter().position(|sample| Some(&sample.identity) == metadata.selected.as_ref()) else { return (None, None) };
    let previous = metadata.samples[..index].iter().rev().find(|sample| sample.available);
    let next = metadata.samples[index + 1..].iter().find(|sample| sample.available);
    (previous.map(|sample| sample.identity.clone()), next.map(|sample| sample.identity.clone()))
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn grid_hits_and_navigation_use_only_paired_retained_identities() {
        let mut metadata = crate::view_model::test_support::validation_image_metadata();
        metadata.frame.extent.width = 512; metadata.frame.extent.height = 576;
        assert_eq!(hit(&metadata, 255.0, 191.0), Some(metadata.samples[0].identity.clone()));
        assert_eq!(hit(&metadata, 256.0, 0.0), Some(metadata.samples[1].identity.clone()));
        assert!(hit(&metadata, 0.0, 192.0).is_none());
        assert!(hit(&metadata, 512.0, 0.0).is_none());
        metadata.selected = Some(metadata.samples[0].identity.clone());
        assert_eq!(neighbors(&metadata), (None, Some(metadata.samples[1].identity.clone())));
        metadata.selected = Some(metadata.samples[1].identity.clone());
        assert_eq!(neighbors(&metadata), (Some(metadata.samples[0].identity.clone()), None));
    }
}
